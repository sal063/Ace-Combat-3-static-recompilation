#include "hw_ocean.h"
#include "ac3_shaders_fx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HS hw_state
#define ON HW_OCEAN_N
#define NC HW_OCEAN_CASCADES

static const float PATCH_M[NC] = { 500.0f, 85.0f, 17.0f };
static const float KBAND[NC + 1] = { 0.0f, 0.296f, 1.48f, 20.0f };
static const float ROT_DEG[NC] = { 0.0f, 8.0f, -13.0f };
static const float WIND_DIR[2] = { 0.8253f, 0.5646f };
#define WIND_MS     10.0
#define SIG_HEIGHT  2.0
#define CHOPPY      1.2f
#define REPEAT_S    200.0
#define FOAM_LIFE_S 2.0
#define UNITS_PER_M 5.0f

#define LODS  18
#define CELL0 1.5
#define KRANGE 4.0
#define EARTH_INV_2R (1.0 / (2.0 * 6.371e6 * 5.0))

_Static_assert(2 + (NC * HW_OCEAN_MIPS + 3) / 4 <= HW_OCEAN_PARAMS,
               "the roughness table does not fit hw_fx_common.glsl's ocean[]");

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view, store; } oimg_t;

static struct {
    int made, ready, images_ready, first;
    oimg_t h0, P, Q, disp, deriv;
    VkSampler samp;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline p_spec, p_fft, p_fin;
    VkBuffer stage; VkDeviceMemory stage_mem; void *stage_map;
    VkBuffer idx; VkDeviceMemory idx_mem; void *idx_map;
    u32 nidx;
    float params[HW_OCEAN_PARAMS][4];
    double last_time;
} O;

#define VKOK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "ac3: ocean: %s failed (%d) at %s:%d\n", #expr, (int)_r, \
            __FILE__, __LINE__); return 0; } } while (0)

static u32 rng_state;
static double rnd01(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return ((double)(rng_state >> 8) + 0.5) / 16777216.0;
}
static void gauss2(double *a, double *b) {
    double u = rnd01(), v = rnd01();
    double r = sqrt(-2.0 * log(u));
    *a = r * cos(6.283185307179586 * v);
    *b = r * sin(6.283185307179586 * v);
}

static void make_spectrum(float *out) {
    const double g = 9.81, Lw = WIND_MS * WIND_MS / g;
    static double re[NC][ON * ON], im[NC][ON * ON], kk[NC][ON * ON];
    double var = 0.0;
    for (int c = 0; c < NC; c++) {
        double a = ROT_DEG[c] * 3.14159265358979 / 180.0;
        double wx = cos(a) * WIND_DIR[0] - sin(a) * WIND_DIR[1];
        double wz = sin(a) * WIND_DIR[0] + cos(a) * WIND_DIR[1];
        double dk = 6.283185307179586 / PATCH_M[c];
        rng_state = 0x9E3779B9u * (u32)(c + 1) + 12345u;
        for (int j = 0; j < ON; j++)
            for (int i = 0; i < ON; i++) {
                int o = j * ON + i;
                double kx = dk * (i - ON / 2), kz = dk * (j - ON / 2);
                double k = sqrt(kx * kx + kz * kz);
                double ga, gb;
                gauss2(&ga, &gb);
                re[c][o] = im[c][o] = 0.0;
                kk[c][o] = k;
                if (i == 0 || j == 0 || k <= 0.0 || k < KBAND[c] || k >= KBAND[c + 1]) continue;
                double d = (kx * wx + kz * wz) / k;
                double P = exp(-1.0 / (k * Lw * k * Lw)) / (k * k * k * k) * d * d *
                           exp(-(k * 0.04) * (k * 0.04));
                if (d < 0.0) P *= 0.07;
                double amp = sqrt(P * dk * dk * 0.5);
                re[c][o] = amp * ga;
                im[c][o] = amp * gb;
                var += 2.0 * (re[c][o] * re[c][o] + im[c][o] * im[c][o]);
            }
    }
    double scale = var > 0.0 ? (SIG_HEIGHT / 4.0) / sqrt(var) : 0.0;
    double slope_total = 0.0;
    float lost[NC][HW_OCEAN_MIPS];
    for (int c = 0; c < NC; c++) {
        double band_lost[HW_OCEAN_MIPS] = { 0 };
        for (int o = 0; o < ON * ON; o++) {
            re[c][o] *= scale;
            im[c][o] *= scale;
        }
        for (int j = 0; j < ON; j++)
            for (int i = 0; i < ON; i++) {
                int o = j * ON + i;
                int n = ((ON - j) & (ON - 1)) * ON + ((ON - i) & (ON - 1));
                float *t = out + ((size_t)c * ON * ON + (size_t)o) * 4;
                t[0] = (float)re[c][o];
                t[1] = (float)im[c][o];
                t[2] = (float)re[c][n];
                t[3] = (float)-im[c][n];
                double s = 2.0 * kk[c][o] * kk[c][o] * (re[c][o] * re[c][o] + im[c][o] * im[c][o]);
                slope_total += s;
                for (int m = 0; m < HW_OCEAN_MIPS; m++) {
                    double kcut = 0.6 * 3.14159265358979 * ON / (PATCH_M[c] * (double)(1 << m));
                    if (kk[c][o] > kcut) band_lost[m] += s;
                }
            }
        for (int m = 0; m < HW_OCEAN_MIPS; m++) lost[c][m] = (float)band_lost[m];
    }
    double cox = 0.003 + 0.00512 * WIND_MS;
    double base = cox - slope_total;
    if (base < 0.006) base = 0.006;
    memset(O.params, 0, sizeof O.params);
    for (int c = 0; c < NC; c++) O.params[0][c] = PATCH_M[c] * UNITS_PER_M;
    O.params[0][3] = CHOPPY;
    O.params[1][0] = (float)base;
    O.params[1][1] = UNITS_PER_M;
    for (int c = 0; c < NC; c++)
        for (int m = 0; m < HW_OCEAN_MIPS; m++) {
            int k = c * HW_OCEAN_MIPS + m;
            O.params[2 + k / 4][k % 4] = lost[c][m];
        }
    printf("ocean: Hs %.1f m, slope variance %.4f in the waves, %.4f below them\n",
           SIG_HEIGHT, slope_total, base);
    if (getenv("AC3_OCEAN_DEBUG"))
        for (int c = 0; c < NC; c++)
            printf("  cascade %d lost: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n", c,
                   lost[c][0], lost[c][1], lost[c][2], lost[c][3], lost[c][4], lost[c][5],
                   lost[c][6], lost[c][7], lost[c][8]);
}

static int oimage(VkFormat fmt, u32 mips, VkImageUsageFlags usage, oimg_t *o) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = ON; ii.extent.height = ON; ii.extent.depth = 1;
    ii.mipLevels = mips; ii.arrayLayers = NC;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(HS.dev, &ii, NULL, &o->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(HS.dev, o->img, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = hw_find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return 0;
    VKOK(vkAllocateMemory(HS.dev, &ai, NULL, &o->mem));
    VKOK(vkBindImageMemory(HS.dev, o->img, o->mem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = o->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = NC;
    VKOK(vkCreateImageView(HS.dev, &vi, NULL, &o->store));
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        vi.subresourceRange.levelCount = mips;
        VKOK(vkCreateImageView(HS.dev, &vi, NULL, &o->view));
    }
    return 1;
}

static void oimage_free(oimg_t *o) {
    if (o->view) vkDestroyImageView(HS.dev, o->view, NULL);
    if (o->store) vkDestroyImageView(HS.dev, o->store, NULL);
    if (o->img) vkDestroyImage(HS.dev, o->img, NULL);
    if (o->mem) vkFreeMemory(HS.dev, o->mem, NULL);
    memset(o, 0, sizeof *o);
}

static VkPipeline compute_pipe(const uint32_t *code, size_t bytes) {
    VkShaderModule m = hw_shader(code, bytes);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = O.layout;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateComputePipelines(HS.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    vkDestroyShaderModule(HS.dev, m, NULL);
    return p;
}

static int init_compute(void) {
    u32 nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(HS.phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(HS.phys, &nq, qp);
    if (HS.qfamily >= nq || !(qp[HS.qfamily].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        printf("ocean: the queue cannot compute; calm water\n");
        return 0;
    }
    VkDescriptorSetLayoutBinding b[5];
    memset(b, 0, sizeof b);
    for (int i = 0; i < 5; i++) {
        b[i].binding = (u32)i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 5;
    li.pBindings = b;
    VKOK(vkCreateDescriptorSetLayout(HS.dev, &li, NULL, &O.dsl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VKOK(vkCreateDescriptorPool(HS.dev, &pi, NULL, &O.pool));
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = O.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &O.dsl;
    VKOK(vkAllocateDescriptorSets(HS.dev, &ai, &O.set));
    VkDescriptorImageInfo ii[5] = {
        { VK_NULL_HANDLE, O.h0.store, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, O.P.store, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, O.Q.store, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, O.disp.store, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, O.deriv.store, VK_IMAGE_LAYOUT_GENERAL },
    };
    VkWriteDescriptorSet w[5];
    memset(w, 0, sizeof w);
    for (int i = 0; i < 5; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = O.set;
        w[i].dstBinding = (u32)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(HS.dev, 5, w, 0, NULL);
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &O.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VKOK(vkCreatePipelineLayout(HS.dev, &pl, NULL, &O.layout));
    O.p_spec = compute_pipe(spv_ocean_spectrum_comp, sizeof spv_ocean_spectrum_comp);
    O.p_fft = compute_pipe(spv_ocean_fft_comp, sizeof spv_ocean_fft_comp);
    O.p_fin = compute_pipe(spv_ocean_finalize_comp, sizeof spv_ocean_finalize_comp);
    return O.p_spec && O.p_fft && O.p_fin;
}

int hw_ocean_init(void) {
    memset(&O, 0, sizeof O);
    const VkImageUsageFlags st = VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags tex = st | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!oimage(VK_FORMAT_R32G32B32A32_SFLOAT, 1, st | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &O.h0) ||
        !oimage(VK_FORMAT_R32G32B32A32_SFLOAT, 1, st, &O.P) ||
        !oimage(VK_FORMAT_R32G32B32A32_SFLOAT, 1, st, &O.Q) ||
        !oimage(VK_FORMAT_R16G16B16A16_SFLOAT, HW_OCEAN_MIPS, tex, &O.disp) ||
        !oimage(VK_FORMAT_R16G16B16A16_SFLOAT, HW_OCEAN_MIPS, tex, &O.deriv))
        return 0;
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = (float)(HW_OCEAN_MIPS - 1);
    VKOK(vkCreateSampler(HS.dev, &si, NULL, &O.samp));
    VkDeviceSize sb = (VkDeviceSize)NC * ON * ON * 16;
    if (!hw_make_host_buffer(sb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &O.stage, &O.stage_mem, &O.stage_map))
        return 0;
    make_spectrum((float *)O.stage_map);

    const u32 Q = HW_OCEAN_QUADS, V = Q + 1;
    O.nidx = Q * Q * 6;
    if (!hw_make_host_buffer((VkDeviceSize)O.nidx * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                             &O.idx, &O.idx_mem, &O.idx_map))
        return 0;
    u16 *ix = (u16 *)O.idx_map;
    for (u32 j = 0; j < Q; j++)
        for (u32 i = 0; i < Q; i++) {
            u16 a = (u16)(j * V + i), b = (u16)(a + 1), c = (u16)(a + V), d = (u16)(c + 1);
            *ix++ = a; *ix++ = c; *ix++ = b;
            *ix++ = b; *ix++ = c; *ix++ = d;
        }
    O.made = 1;
    O.ready = init_compute();
    O.first = 1;
    return 1;
}

void hw_ocean_shutdown(void) {
    if (!HS.dev) return;
    VkPipeline ps[3] = { O.p_spec, O.p_fft, O.p_fin };
    for (int i = 0; i < 3; i++) if (ps[i]) vkDestroyPipeline(HS.dev, ps[i], NULL);
    if (O.layout) vkDestroyPipelineLayout(HS.dev, O.layout, NULL);
    if (O.pool) vkDestroyDescriptorPool(HS.dev, O.pool, NULL);
    if (O.dsl) vkDestroyDescriptorSetLayout(HS.dev, O.dsl, NULL);
    if (O.samp) vkDestroySampler(HS.dev, O.samp, NULL);
    if (O.stage) hw_free_host_buffer(&O.stage, &O.stage_mem, &O.stage_map);
    if (O.idx) hw_free_host_buffer(&O.idx, &O.idx_mem, &O.idx_map);
    oimg_t *im[5] = { &O.h0, &O.P, &O.Q, &O.disp, &O.deriv };
    for (int i = 0; i < 5; i++) oimage_free(im[i]);
    memset(&O, 0, sizeof O);
}

int hw_ocean_ready(void) { return O.made && O.ready; }
VkImageView hw_ocean_disp_view(void) { return O.disp.view; }
VkImageView hw_ocean_deriv_view(void) { return O.deriv.view; }
VkSampler hw_ocean_sampler(void) { return O.samp; }
void hw_ocean_params(float out[HW_OCEAN_PARAMS][4]) { memcpy(out, O.params, sizeof O.params); }

VkBuffer hw_ocean_indices(u32 *count) {
    *count = O.nidx;
    return O.idx;
}

static void mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkAccessFlags sa,
                        VkPipelineStageFlags ds, VkAccessFlags da) {
    VkMemoryBarrier m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    m.srcAccessMask = sa;
    m.dstAccessMask = da;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 1, &m, 0, NULL, 0, NULL);
}

static void to_general(VkCommandBuffer cb, VkImage img, u32 mips) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = mips;
    b.subresourceRange.layerCount = NC;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

#define ST_READERS (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
#define ST_CS      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
#define ST_XF      VK_PIPELINE_STAGE_TRANSFER_BIT
#define A_SRW      (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
#define A_XRW      (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)

void hw_ocean_simulate(VkCommandBuffer cb, double time) {
    if (!O.made) return;
    if (!O.images_ready) {
        oimg_t *im[5] = { &O.h0, &O.P, &O.Q, &O.disp, &O.deriv };
        for (int i = 0; i < 5; i++) to_general(cb, im[i]->img, i >= 3 ? HW_OCEAN_MIPS : 1);
        VkClearColorValue z = { { 0, 0, 0, 0 } };
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, HW_OCEAN_MIPS, 0, NC };
        vkCmdClearColorImage(cb, O.disp.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        vkCmdClearColorImage(cb, O.deriv.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        VkBufferImageCopy r;
        memset(&r, 0, sizeof r);
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = NC;
        r.imageExtent.width = ON; r.imageExtent.height = ON; r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, O.stage, O.h0.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        mem_barrier(cb, ST_XF, VK_ACCESS_TRANSFER_WRITE_BIT, ST_CS | ST_XF | ST_READERS,
                    A_SRW | A_XRW);
        O.images_ready = 1;
    }
    if (time < 0.0 || !O.ready) return;

    mem_barrier(cb, ST_READERS | ST_XF | ST_CS, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                ST_CS, A_SRW);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, O.layout, 0, 1, &O.set, 0, NULL);

    struct { float patch_m[4]; float time, n, w0, pad; } sp;
    for (int c = 0; c < 4; c++) sp.patch_m[c] = c < NC ? PATCH_M[c] : 1.0f;
    sp.time = (float)fmod(time, REPEAT_S);
    sp.n = (float)ON;
    sp.w0 = (float)(6.283185307179586 / REPEAT_S);
    sp.pad = 0.0f;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, O.p_spec);
    vkCmdPushConstants(cb, O.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof sp, &sp);
    vkCmdDispatch(cb, ON / 16, ON / 16, NC);
    mem_barrier(cb, ST_CS, VK_ACCESS_SHADER_WRITE_BIT, ST_CS, A_SRW);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, O.p_fft);
    for (int col = 0; col < 2; col++) {
        s32 fp[2] = { col, NC };
        vkCmdPushConstants(cb, O.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof fp, fp);
        vkCmdDispatch(cb, 1, ON, 2 * NC);
        mem_barrier(cb, ST_CS, VK_ACCESS_SHADER_WRITE_BIT, ST_CS, A_SRW);
    }

    double dt = time - O.last_time;
    if (dt < 0.0 || dt > 1.0) dt = 1.0 / 30.0;
    O.last_time = time;
    struct { float choppy, decay, first, pad; } fp = {
        CHOPPY, (float)exp(-dt / FOAM_LIFE_S), O.first ? 1.0f : 0.0f, 0.0f };
    O.first = 0;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, O.p_fin);
    vkCmdPushConstants(cb, O.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof fp, &fp);
    vkCmdDispatch(cb, ON / 16, ON / 16, NC);
    mem_barrier(cb, ST_CS, VK_ACCESS_SHADER_WRITE_BIT, ST_XF, A_XRW);

    for (u32 m = 1; m < HW_OCEAN_MIPS; m++) {
        VkImageBlit bl;
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.mipLevel = m - 1;
        bl.dstSubresource.mipLevel = m;
        bl.srcSubresource.layerCount = bl.dstSubresource.layerCount = NC;
        bl.srcOffsets[1].x = ON >> (m - 1); bl.srcOffsets[1].y = ON >> (m - 1); bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = ON >> m;       bl.dstOffsets[1].y = ON >> m;       bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, O.disp.img, VK_IMAGE_LAYOUT_GENERAL, O.disp.img, VK_IMAGE_LAYOUT_GENERAL,
                       1, &bl, VK_FILTER_LINEAR);
        vkCmdBlitImage(cb, O.deriv.img, VK_IMAGE_LAYOUT_GENERAL, O.deriv.img, VK_IMAGE_LAYOUT_GENERAL,
                       1, &bl, VK_FILTER_LINEAR);
        mem_barrier(cb, ST_XF, VK_ACCESS_TRANSFER_WRITE_BIT, ST_XF, A_XRW);
    }
    mem_barrier(cb, ST_XF | ST_CS, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                ST_READERS, VK_ACCESS_SHADER_READ_BIT);
}

typedef struct {
    double cx, cy, cz, alt, sea_y, horizon;
    float rot[9];
    float hw, hh, nx, ny;
    hw_ocean_node_t *out;
    u32 n, max;
} sel_t;

static double node_size(int L) { return 32.0 * CELL0 * ldexp(1.0, L); }
static double node_range(int L) { return KRANGE * node_size(L); }

static double rect_h2(const sel_t *s, double x0, double z0, double size) {
    double dx = fabs(s->cx - (x0 + size * 0.5)) - size * 0.5;
    double dz = fabs(s->cz - (z0 + size * 0.5)) - size * 0.5;
    if (dx < 0) dx = 0;
    if (dz < 0) dz = 0;
    return dx * dx + dz * dz;
}

static int visible(const sel_t *s, double x0, double z0, double size) {
    double half = size * 0.5;
    double hx = x0 + half - s->cx, hz = z0 + half - s->cz;
    double D = sqrt(hx * hx + hz * hz);
    double R = half * 1.4143 + 80.0 + EARTH_INV_2R * (2.0 * D * half * 1.4143 + 2.0 * half * half);
    double rel[3] = { hx, s->sea_y + D * D * EARTH_INV_2R - s->cy, hz };
    double v[3];
    for (int i = 0; i < 3; i++)
        v[i] = s->rot[3 * i] * rel[0] + s->rot[3 * i + 1] * rel[1] + s->rot[3 * i + 2] * rel[2];
    if (v[2] < -R) return 0;
    if ((v[0] - s->hw * v[2]) * s->nx > R || (-v[0] - s->hw * v[2]) * s->nx > R) return 0;
    if ((v[1] - s->hh * v[2]) * s->ny > R || (-v[1] - s->hh * v[2]) * s->ny > R) return 0;
    return 1;
}

static void emit(sel_t *s, double x0, double z0, double cell, double range) {
    if (s->n >= s->max) return;
    if (rect_h2(s, x0, z0, cell * HW_OCEAN_QUADS) > s->horizon * s->horizon) return;
    if (!visible(s, x0, z0, cell * HW_OCEAN_QUADS)) return;
    hw_ocean_node_t *o = &s->out[s->n++];
    o->x = (float)x0; o->z = (float)z0; o->cell = (float)cell; o->range = (float)range;
}

static int select_node(sel_t *s, int L, double x0, double z0) {
    double size = node_size(L), r = node_range(L);
    double h2 = rect_h2(s, x0, z0, size);
    double d2 = h2 + s->alt * s->alt;
    if (d2 >= r * r) return 0;
    if (h2 > s->horizon * s->horizon || !visible(s, x0, z0, size)) return 1;
    double q = size * 0.5, cell = size / (2.0 * HW_OCEAN_QUADS);
    double rc = L > 0 ? node_range(L - 1) : 0.0;
    int split = L > 0 && d2 < rc * rc;
    for (int i = 0; i < 4; i++) {
        double qx = x0 + (i & 1) * q, qz = z0 + (i >> 1) * q;
        if (!split || !select_node(s, L - 1, qx, qz)) emit(s, qx, qz, cell, r);
    }
    return 1;
}

u32 hw_ocean_select(const hw_scene_t *sc, float horizon, hw_ocean_node_t *out, u32 max) {
    sel_t s;
    memset(&s, 0, sizeof s);
    s.cx = sc->cam[0]; s.cy = sc->cam[1]; s.cz = sc->cam[2];
    s.sea_y = sc->sea_y;
    s.alt = s.sea_y - s.cy;
    if (s.alt < 1.0) s.alt = 1.0;
    s.horizon = horizon;
    memcpy(s.rot, sc->rot, sizeof s.rot);
    s.hw = (float)(sc->area[2] - sc->area[0] + 1) * 0.5f * 1.15f / (sc->H * sc->ws);
    s.hh = (float)(sc->area[3] - sc->area[1] + 1) * 0.5f * 1.15f / sc->H;
    s.nx = 1.0f / sqrtf(1.0f + s.hw * s.hw);
    s.ny = 1.0f / sqrtf(1.0f + s.hh * s.hh);
    s.out = out;
    s.max = max;
    int top = LODS - 1;
    double size = node_size(top);
    double bx = floor(s.cx / size) * size, bz = floor(s.cz / size) * size;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
            select_node(&s, top, bx + i * size, bz + j * size);
    return s.n;
}
