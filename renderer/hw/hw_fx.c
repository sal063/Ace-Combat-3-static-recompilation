#include "hw_private.h"
#include "hw_ocean.h"
#include "ac3_shaders.h"
#include "ac3_shaders_fx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HS hw_state

u32 hw_fx_flags = FX_FAR | FX_OCEAN | FX_CLOUDS | FX_FOREST | FX_ATMO | FX_GI | FX_SUN;

typedef struct {
    float vtw[12], wtv[12];
    float cam[4], proj[4], ofs[4], area[4], sun[4], sun_col[4], fog[4];
    float map[4], cloud[4], wind[4], water[4], misc[4];
    float pal[4], pal_src[4];
    float hdr[4];
    float ocean[HW_OCEAN_PARAMS][4];
    float sky[32][4];
    float prev_wtv[12];
    float prev_cam[4], prev_proj[4], prev_area[4], cloud_tc[4];
} fx_ubo_t;

#define UBO_STRIDE 2048
_Static_assert(sizeof(fx_ubo_t) <= UBO_STRIDE, "the effects' uniforms outgrew their slot");
#define MAX_SCENES 8
#define NOISE_N    64
#define NOISE_MIPS 7
#define WEATHER_N  256
#define WEATHER_MIPS 9
#define CLOUD_HISTORY 0.9f
#define Z_FAR      2.0e6f
#define TILE_CELLS 44
#define MAX_INST   ((2 * TILE_CELLS + 1) * (2 * TILE_CELLS + 1))
#define MESH_CAP   (1u << 17)

typedef struct { u32 first_vtx, nvtx, first_inst, ninst; } tile_draw_t;

#define BLOOM_MIPS 6
typedef struct { float src_texel[2], uv_max[2], radius, first; } bloom_push_t;

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } fx_img_t;

static struct {
    int ready;
    fx_img_t depth, cloud, cloudz, grid, height, colors, top, noise, weather;
    fx_img_t cloud_raw, cloud_hist;
    fx_img_t hdr, sunvis;
    VkImage bloom; VkDeviceMemory bloom_mem; VkImageView bloom_view[BLOOM_MIPS];
    u32 CW, CH, HDW, HDH;
    VkRenderPass rp_hdr, rp_bup, rp_sunvis;
    VkFramebuffer fb_hdr, fb_bloom[BLOOM_MIPS], fb_sunvis;
    VkSampler s_lin;
    VkDescriptorSetLayout dsl_b;
    VkDescriptorPool pool_b;
    VkDescriptorSet set_b[BLOOM_MIPS + 1];
    VkPipelineLayout layout_b;
    VkPipeline p_bdown, p_bup, p_sunvis, p_comp;
    VkSampler s_near, s_rep, s_mip;
    VkRenderPass rp_prime, rp_trees, rp_cloud, rp_post, rp_world, rp_cres;
    VkFramebuffer fb_prime, fb_trees, fb_cloud, fb_post, fb_world, fb_cres;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set[HW_FRAMES];
    VkPipelineLayout layout;
    VkPipeline p_bg, p_prime, p_tree, p_cloud, p_post, p_tile, p_ocean, p_cres;
    struct {
        int valid;
        u32 frame;
        float rot[9], cam[3], H, ws, ofs[2];
        s32 area[4];
    } cl_last;
    VkBuffer mesh; VkDeviceMemory mesh_mem; void *mesh_map;
    u32 mesh_version, mesh_n;
    VkBuffer inst[HW_FRAMES]; VkDeviceMemory inst_mem[HW_FRAMES]; void *inst_map[HW_FRAMES];
    VkBuffer ubo[HW_FRAMES]; VkDeviceMemory ubo_mem[HW_FRAMES]; void *ubo_map[HW_FRAMES];
    VkBuffer stage; VkDeviceMemory stage_mem; void *stage_map;
    int images_ready;
    u32 map_version;
    hw_scene_t scenes[MAX_SCENES];
    u8 kind[MAX_SCENES];
    s32 pal[MAX_SCENES][6], pend_pal[6];
    u32 nscenes;
    tile_draw_t draws[2][512];
    u32 ndraws[2], inst_first[2];
    int tiles_scene[2];
    VkBuffer onode[HW_FRAMES]; VkDeviceMemory onode_mem[HW_FRAMES]; void *onode_map[HW_FRAMES];
    u32 nnodes[2];
    int ocean_scene[2];
    u8 ocean_on[MAX_SCENES];
} F;

#define VKOK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "ac3: fx: %s failed (%d) at %s:%d\n", #expr, (int)_r, \
            __FILE__, __LINE__); return 0; } } while (0)

static int fx_image_mips(VkImageType type, VkFormat fmt, u32 w, u32 h, u32 d, u32 mips,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect, fx_img_t *o) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = type;
    ii.format = fmt;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = d;
    ii.mipLevels = mips; ii.arrayLayers = 1;
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
    vi.viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.layerCount = 1;
    VKOK(vkCreateImageView(HS.dev, &vi, NULL, &o->view));
    return 1;
}

static int fx_image(VkImageType type, VkFormat fmt, u32 w, u32 h, u32 d,
                    VkImageUsageFlags usage, VkImageAspectFlags aspect, fx_img_t *o) {
    return fx_image_mips(type, fmt, w, h, d, 1, usage, aspect, o);
}

static void fx_image_free(fx_img_t *o) {
    if (o->view) vkDestroyImageView(HS.dev, o->view, NULL);
    if (o->img) vkDestroyImage(HS.dev, o->img, NULL);
    if (o->mem) vkFreeMemory(HS.dev, o->mem, NULL);
    memset(o, 0, sizeof *o);
}

static void layout_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                           VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                           VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static u32 nhash(u32 x, u32 y, u32 z, u32 seed) {
    u32 h = x * 374761393u + y * 668265263u + z * 2246822519u + seed * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float grad3(u32 h, float x, float y, float z) {
    switch (h % 12u) {
    case 0: return x + y;   case 1: return -x + y;  case 2: return x - y;   case 3: return -x - y;
    case 4: return x + z;   case 5: return -x + z;  case 6: return x - z;   case 7: return -x - z;
    case 8: return y + z;   case 9: return -y + z;  case 10: return y - z;  default: return -y - z;
    }
}

static float fade5(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

static float perlin3(float x, float y, float z, int period, u32 seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    float u = fade5(xf), v = fade5(yf), w = fade5(zf);
    float n[8];
    for (int k = 0; k < 8; k++) {
        int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
        u32 h = nhash((u32)(((xi + dx) % period + period) % period),
                      (u32)(((yi + dy) % period + period) % period),
                      (u32)(((zi + dz) % period + period) % period), seed);
        n[k] = grad3(h, xf - dx, yf - dy, zf - dz);
    }
    float x00 = n[0] + u * (n[1] - n[0]), x10 = n[2] + u * (n[3] - n[2]);
    float x01 = n[4] + u * (n[5] - n[4]), x11 = n[6] + u * (n[7] - n[6]);
    float y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
    return y0 + w * (y1 - y0);
}

static float worley3(float x, float y, float z, int period, u32 seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float best = 9.0f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int cx = xi + dx, cy = yi + dy, cz = zi + dz;
                u32 h = nhash((u32)((cx % period + period) % period),
                              (u32)((cy % period + period) % period),
                              (u32)((cz % period + period) % period), seed);
                float px = cx + (float)(h & 1023u) / 1023.0f;
                float py = cy + (float)((h >> 10) & 1023u) / 1023.0f;
                float pz = cz + (float)((h >> 20) & 1023u) / 1023.0f;
                float d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
                if (d < best) best = d;
            }
    float d = sqrtf(best);
    return d > 1.0f ? 1.0f : d;
}

static u8 unorm8(float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return (u8)(v * 255.0f + 0.5f);
}

static void make_noise(u8 *vol, u8 *weather) {
    for (int z = 0; z < NOISE_N; z++)
        for (int y = 0; y < NOISE_N; y++)
            for (int x = 0; x < NOISE_N; x++) {
                float fx = (float)x / NOISE_N, fy = (float)y / NOISE_N, fz = (float)z / NOISE_N;
                float p = 0, amp = 1, norm = 0;
                for (int o = 0; o < 4; o++) {
                    int f = 4 << o;
                    p += perlin3(fx * f, fy * f, fz * f, f, 11u + (u32)o) * amp;
                    norm += amp;
                    amp *= 0.5f;
                }
                p = p / norm * 0.8f + 0.5f;
                float w4 = 1.0f - worley3(fx * 4, fy * 4, fz * 4, 4, 21u);
                float w8 = 1.0f - worley3(fx * 8, fy * 8, fz * 8, 8, 22u);
                float w16 = 1.0f - worley3(fx * 16, fy * 16, fz * 16, 16, 23u);
                float w32 = 1.0f - worley3(fx * 32, fy * 32, fz * 32, 32, 24u);
                float wf = w4 * 0.625f + w8 * 0.25f + w16 * 0.125f;
                float pw = (p - (wf - 1.0f)) / (2.0f - wf);
                u8 *o = vol + (((size_t)z * NOISE_N + y) * NOISE_N + x) * 4;
                o[0] = unorm8(pw);
                o[1] = unorm8(w8);
                o[2] = unorm8(w16);
                o[3] = unorm8(w32);
            }
    for (int y = 0; y < WEATHER_N; y++)
        for (int x = 0; x < WEATHER_N; x++) {
            float fx = (float)x / WEATHER_N, fy = (float)y / WEATHER_N;
            float p = 0, amp = 1, norm = 0;
            for (int o = 0; o < 5; o++) {
                int f = 3 << o;
                p += perlin3(fx * f, fy * f, 0.37f, f, 31u + (u32)o) * amp;
                norm += amp;
                amp *= 0.55f;
            }
            p = p / norm;
            float v = p * 1.6f + 0.5f;
            u8 *o = weather + ((size_t)y * WEATHER_N + x) * 4;
            o[0] = unorm8(v);
            o[1] = o[2] = 0;
            o[3] = 255;
        }
}

static size_t mip_bytes(u32 n, int is3d, u32 levels) {
    size_t total = 0;
    for (u32 l = 1; l < levels; l++) {
        size_t m = n >> l;
        total += m * m * (is3d ? m : 1) * 4;
    }
    return total;
}

static void make_mips(const u8 *src, u32 n, int is3d, u32 levels, u8 *dst) {
    const u8 *prev = src;
    u32 pn = n;
    for (u32 l = 1; l < levels; l++) {
        u32 m = pn / 2, md = is3d ? m : 1, kz = is3d ? 2u : 1u;
        for (u32 z = 0; z < md; z++)
            for (u32 y = 0; y < m; y++)
                for (u32 x = 0; x < m; x++)
                    for (u32 c = 0; c < 4; c++) {
                        u32 sum = 0;
                        for (u32 dz = 0; dz < kz; dz++)
                            for (u32 dy = 0; dy < 2; dy++)
                                for (u32 dx = 0; dx < 2; dx++)
                                    sum += prev[((((size_t)(2 * z + dz) * pn) + 2 * y + dy) * pn +
                                                 2 * x + dx) * 4 + c];
                        u32 cnt = 4 * kz;
                        dst[(((size_t)z * m + y) * m + x) * 4 + c] = (u8)((sum + cnt / 2) / cnt);
                    }
        prev = dst;
        dst += (size_t)m * m * md * 4;
        pn = m;
    }
}

static VkRenderPass make_pass(int ncolor, const VkFormat *cf, const VkAttachmentLoadOp *cload,
                              const VkImageLayout *cinit, const VkImageLayout *cfinal,
                              int depth, VkAttachmentLoadOp dload, VkImageLayout dinit) {
    VkAttachmentDescription at[4];
    VkAttachmentReference cref[3], dref;
    memset(at, 0, sizeof at);
    int n = 0;
    for (int i = 0; i < ncolor; i++, n++) {
        at[n].format = cf[i];
        at[n].samples = VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = cload[i];
        at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[n].initialLayout = cinit[i];
        at[n].finalLayout = cfinal[i];
        cref[i].attachment = (u32)n;
        cref[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (depth) {
        at[n].format = VK_FORMAT_D32_SFLOAT;
        at[n].samples = VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = dload;
        at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[n].initialLayout = dinit;
        at[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref.attachment = (u32)n;
        dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        n++;
    }
    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = (u32)ncolor;
    sub.pColorAttachments = ncolor ? cref : NULL;
    sub.pDepthStencilAttachment = depth ? &dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    const VkPipelineStageFlags gfx = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    const VkPipelineStageFlags all = gfx | VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkAccessFlags gacc = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags acc = gacc | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = all; dep[0].dstStageMask = gfx;
    dep[0].srcAccessMask = acc; dep[0].dstAccessMask = gacc;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = gfx; dep[1].dstStageMask = all;
    dep[1].srcAccessMask = gacc; dep[1].dstAccessMask = acc;
    VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ri.attachmentCount = (u32)n;
    ri.pAttachments = at;
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = 2;
    ri.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(HS.dev, &ri, NULL, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
    return rp;
}

static VkFramebuffer make_fb(VkRenderPass rp, const VkImageView *views, u32 n, u32 w, u32 h) {
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = rp;
    fi.attachmentCount = n;
    fi.pAttachments = views;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(HS.dev, &fi, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    return fb;
}

typedef struct {
    const uint32_t *vs; size_t vs_bytes;
    const uint32_t *fs; size_t fs_bytes;
    VkRenderPass rp;
    int ncolor;
    VkColorComponentFlags mask[3];
    int depth_test, depth_write;
    VkCompareOp depth_op;
    const VkPipelineVertexInputStateCreateInfo *vin;
    VkPipelineLayout layout;
    int additive;
} pipe_desc_t;

static VkPipeline make_pipe(const pipe_desc_t *d) {
    VkShaderModule vs = hw_shader(d->vs, d->vs_bytes), fs = hw_shader(d->fs, d->fs_bytes);
    if (!vs || !fs) return VK_NULL_HANDLE;
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb[3];
    memset(cb, 0, sizeof cb);
    for (int i = 0; i < d->ncolor; i++) {
        cb[i].colorWriteMask = d->mask[i];
        if (d->additive) {
            cb[i].blendEnable = VK_TRUE;
            cb[i].srcColorBlendFactor = cb[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb[i].srcAlphaBlendFactor = cb[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb[i].colorBlendOp = cb[i].alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = (u32)d->ncolor;
    cbs.pAttachments = d->ncolor ? cb : NULL;
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = d->depth_test ? VK_TRUE : VK_FALSE;
    dss.depthWriteEnable = d->depth_write ? VK_TRUE : VK_FALSE;
    dss.depthCompareOp = d->depth_op;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = d->vin ? d->vin : &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDepthStencilState = &dss;
    gp.pDynamicState = &ds;
    gp.layout = d->layout ? d->layout : F.layout;
    gp.renderPass = d->rp;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(HS.dev, VK_NULL_HANDLE, 1, &gp, NULL, &p);
    vkDestroyShaderModule(HS.dev, vs, NULL);
    vkDestroyShaderModule(HS.dev, fs, NULL);
    return p;
}

static int init_descriptors(void) {
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKOK(vkCreateSampler(HS.dev, &si, NULL, &F.s_near));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    VKOK(vkCreateSampler(HS.dev, &si, NULL, &F.s_lin));
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VKOK(vkCreateSampler(HS.dev, &si, NULL, &F.s_rep));
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VKOK(vkCreateSampler(HS.dev, &si, NULL, &F.s_mip));

#define NBIND 20
    VkDescriptorSetLayoutBinding b[NBIND];
    memset(b, 0, sizeof b);
    for (int i = 0; i < NBIND; i++) {
        b[i].binding = (u32)i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = NBIND;
    li.pBindings = b;
    VKOK(vkCreateDescriptorSetLayout(HS.dev, &li, NULL, &F.dsl));

    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, HW_FRAMES },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (NBIND - 1) * HW_FRAMES },
    };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = HW_FRAMES;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    VKOK(vkCreateDescriptorPool(HS.dev, &pi, NULL, &F.pool));
    VkDescriptorSetLayout ls[HW_FRAMES];
    for (int i = 0; i < HW_FRAMES; i++) ls[i] = F.dsl;
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = F.pool;
    dai.descriptorSetCount = HW_FRAMES;
    dai.pSetLayouts = ls;
    VKOK(vkAllocateDescriptorSets(HS.dev, &dai, F.set));

    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (int f = 0; f < HW_FRAMES; f++) {
        VkDescriptorBufferInfo bi = { F.ubo[f], 0, sizeof(fx_ubo_t) };
        VkDescriptorImageInfo ii[NBIND - 1] = {
            { F.s_near, HS.read_view, RO },     { F.s_near, HS.z_view, RO },
            { F.s_near, F.cloud.view, RO },    { F.s_near, F.cloudz.view, RO },
            { F.s_near, F.grid.view, RO },     { F.s_near, F.height.view, RO },
            { F.s_near, F.colors.view, RO },   { F.s_near, F.top.view, RO },
            { F.s_rep, F.noise.view, RO },     { F.s_rep, F.weather.view, RO },
            { F.s_near, F.hdr.view, RO },      { F.s_lin, F.bloom_view[0], RO },
            { F.s_near, F.sunvis.view, RO },
            { hw_ocean_sampler(), hw_ocean_disp_view(), VK_IMAGE_LAYOUT_GENERAL },
            { hw_ocean_sampler(), hw_ocean_deriv_view(), VK_IMAGE_LAYOUT_GENERAL },
            { F.s_near, F.cloud_raw.view, RO }, { F.s_lin, F.cloud_hist.view, RO },
            { F.s_mip, F.noise.view, RO },     { F.s_mip, F.weather.view, RO },
        };
        VkWriteDescriptorSet w[NBIND];
        memset(w, 0, sizeof w);
        for (int i = 0; i < NBIND; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = F.set[f];
            w[i].dstBinding = (u32)i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = b[i].descriptorType;
            if (i == 0) w[i].pBufferInfo = &bi;
            else w[i].pImageInfo = &ii[i - 1];
        }
        vkUpdateDescriptorSets(HS.dev, NBIND, w, 0, NULL);
    }

    VkDescriptorSetLayoutBinding bb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lb = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lb.bindingCount = 1;
    lb.pBindings = &bb;
    VKOK(vkCreateDescriptorSetLayout(HS.dev, &lb, NULL, &F.dsl_b));
    VkDescriptorPoolSize pb = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, BLOOM_MIPS + 1 };
    VkDescriptorPoolCreateInfo pbi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pbi.maxSets = BLOOM_MIPS + 1;
    pbi.poolSizeCount = 1;
    pbi.pPoolSizes = &pb;
    VKOK(vkCreateDescriptorPool(HS.dev, &pbi, NULL, &F.pool_b));
    VkDescriptorSetLayout lbs[BLOOM_MIPS + 1];
    for (int i = 0; i <= BLOOM_MIPS; i++) lbs[i] = F.dsl_b;
    VkDescriptorSetAllocateInfo dbi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dbi.descriptorPool = F.pool_b;
    dbi.descriptorSetCount = BLOOM_MIPS + 1;
    dbi.pSetLayouts = lbs;
    VKOK(vkAllocateDescriptorSets(HS.dev, &dbi, F.set_b));
    for (int i = 0; i <= BLOOM_MIPS; i++) {
        VkDescriptorImageInfo di = { F.s_lin, i == 0 ? F.hdr.view : F.bloom_view[i - 1], RO };
        VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wd.dstSet = F.set_b[i];
        wd.descriptorCount = 1;
        wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wd.pImageInfo = &di;
        vkUpdateDescriptorSets(HS.dev, 1, &wd, 0, NULL);
    }
    VkPushConstantRange pbr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bloom_push_t) };
    VkPipelineLayoutCreateInfo plb = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plb.setLayoutCount = 1;
    plb.pSetLayouts = &F.dsl_b;
    plb.pushConstantRangeCount = 1;
    plb.pPushConstantRanges = &pbr;
    VKOK(vkCreatePipelineLayout(HS.dev, &plb, NULL, &F.layout_b));

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &F.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VKOK(vkCreatePipelineLayout(HS.dev, &pli, NULL, &F.layout));
    return 1;
}

static int bloom_image(void) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent.width = F.HDW / 2; ii.extent.height = F.HDH / 2; ii.extent.depth = 1;
    ii.mipLevels = BLOOM_MIPS; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(HS.dev, &ii, NULL, &F.bloom));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(HS.dev, F.bloom, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = hw_find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return 0;
    VKOK(vkAllocateMemory(HS.dev, &ai, NULL, &F.bloom_mem));
    VKOK(vkBindImageMemory(HS.dev, F.bloom, F.bloom_mem, 0));
    for (int m = 0; m < BLOOM_MIPS; m++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = F.bloom;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel = (u32)m;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VKOK(vkCreateImageView(HS.dev, &vi, NULL, &F.bloom_view[m]));
    }
    return 1;
}

int hw_fx_init(void) {
    memset(&F, 0, sizeof F);
    F.CW = HS.W / 2; F.CH = HS.Hs / 2;
    F.HDW = 512u * (u32)HS.S; F.HDH = 256u * (u32)HS.S;
    const VkImageUsageFlags up = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageAspectFlags C = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, HS.W, HS.Hs, 1,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &F.depth) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, F.CW, F.CH, 1,
                  rt | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, C, &F.cloud) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, F.CW, F.CH, 1, rt, C, &F.cloud_raw) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, F.CW, F.CH, 1, up, C, &F.cloud_hist) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32_SFLOAT, F.CW, F.CH, 1, rt, C, &F.cloudz) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R16_UINT, HW_MAP_N, HW_MAP_N, 1, up, C, &F.grid) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, HW_ATLAS_W, HW_ATLAS_H, 1, up, C, &F.height) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, HW_ATLAS_W, HW_ATLAS_H, 1, up, C, &F.colors) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, 512, 1, 1, up, C, &F.top) ||
        !fx_image_mips(VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, NOISE_N, NOISE_N, NOISE_N,
                       NOISE_MIPS, up, C, &F.noise) ||
        !fx_image_mips(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, WEATHER_N, WEATHER_N, 1,
                       WEATHER_MIPS, up, C, &F.weather) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, F.HDW, F.HDH, 1, rt, C, &F.hdr) ||
        !fx_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R16_SFLOAT, 1, 1, 1, rt, C, &F.sunvis) ||
        !bloom_image())
        return 0;
    for (int f = 0; f < HW_FRAMES; f++)
        if (!hw_make_host_buffer(UBO_STRIDE * MAX_SCENES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 &F.ubo[f], &F.ubo_mem[f], &F.ubo_map[f])) return 0;
    VkDeviceSize stage = (VkDeviceSize)HW_MAP_N * HW_MAP_N * 2 +
                         (VkDeviceSize)HW_ATLAS_W * HW_ATLAS_H * 8 + 512 * 4 +
                         (VkDeviceSize)NOISE_N * NOISE_N * NOISE_N * 4 +
                         (VkDeviceSize)WEATHER_N * WEATHER_N * 4 +
                         mip_bytes(NOISE_N, 1, NOISE_MIPS) + mip_bytes(WEATHER_N, 0, WEATHER_MIPS);
    if (!hw_make_host_buffer(stage, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             &F.stage, &F.stage_mem, &F.stage_map)) return 0;
    if (!hw_make_host_buffer((VkDeviceSize)MESH_CAP * sizeof(hw_tile_vtx_t),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &F.mesh, &F.mesh_mem, &F.mesh_map))
        return 0;
    for (int f = 0; f < HW_FRAMES; f++)
        if (!hw_make_host_buffer((VkDeviceSize)MAX_INST * 2 * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 &F.inst[f], &F.inst_mem[f], &F.inst_map[f]) ||
            !hw_make_host_buffer((VkDeviceSize)HW_OCEAN_MAX_NODES * 2 * sizeof(hw_ocean_node_t),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 &F.onode[f], &F.onode_mem[f], &F.onode_map[f])) return 0;
    if (!hw_ocean_init()) return 0;
    if (!init_descriptors()) return 0;

    const VkImageLayout ATT = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkFormat rgba8 = VK_FORMAT_R8G8B8A8_UNORM;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD, dc = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkImageLayout att[2] = { ATT, ATT }, und[2] = { VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageLayout ro[2] = { RO, RO };
    VkFormat cf[2] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32_SFLOAT };
    VkAttachmentLoadOp dcs[2] = { dc, dc };
    F.rp_prime = make_pass(0, NULL, NULL, NULL, NULL, 1, VK_ATTACHMENT_LOAD_OP_CLEAR,
                           VK_IMAGE_LAYOUT_UNDEFINED);
    F.rp_trees = make_pass(1, &rgba8, &load, att, att, 1, VK_ATTACHMENT_LOAD_OP_LOAD,
                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    F.rp_cloud = make_pass(2, cf, dcs, und, ro, 0, dc, VK_IMAGE_LAYOUT_UNDEFINED);
    F.rp_post = make_pass(1, &rgba8, &load, att, att, 0, dc, VK_IMAGE_LAYOUT_UNDEFINED);
    VkFormat wf[2] = { rgba8, VK_FORMAT_R32_SFLOAT };
    VkAttachmentLoadOp loads[2] = { load, load };
    F.rp_world = make_pass(2, wf, loads, att, att, 1, VK_ATTACHMENT_LOAD_OP_CLEAR,
                           VK_IMAGE_LAYOUT_UNDEFINED);
    VkFormat f16 = VK_FORMAT_R16G16B16A16_SFLOAT, r16 = VK_FORMAT_R16_SFLOAT;
    F.rp_hdr = make_pass(1, &f16, &dc, und, ro, 0, dc, VK_IMAGE_LAYOUT_UNDEFINED);
    F.rp_bup = make_pass(1, &f16, &load, ro, ro, 0, dc, VK_IMAGE_LAYOUT_UNDEFINED);
    F.rp_sunvis = make_pass(1, &r16, &dc, und, ro, 0, dc, VK_IMAGE_LAYOUT_UNDEFINED);
    F.rp_cres = make_pass(1, &f16, &dc, und, ro, 0, dc, VK_IMAGE_LAYOUT_UNDEFINED);
    if (!F.rp_prime || !F.rp_trees || !F.rp_cloud || !F.rp_post || !F.rp_world ||
        !F.rp_hdr || !F.rp_bup || !F.rp_sunvis || !F.rp_cres) return 0;
    VkImageView v_trees[2] = { HS.target_view, F.depth.view };
    VkImageView v_cloud[2] = { F.cloud_raw.view, F.cloudz.view };
    F.fb_prime = make_fb(F.rp_prime, &F.depth.view, 1, HS.W, HS.Hs);
    F.fb_trees = make_fb(F.rp_trees, v_trees, 2, HS.W, HS.Hs);
    F.fb_cloud = make_fb(F.rp_cloud, v_cloud, 2, F.CW, F.CH);
    F.fb_cres = make_fb(F.rp_cres, &F.cloud.view, 1, F.CW, F.CH);
    F.fb_post = make_fb(F.rp_post, &HS.target_view, 1, HS.W, HS.Hs);
    VkImageView v_world[3] = { HS.target_view, HS.z_view, F.depth.view };
    F.fb_world = make_fb(F.rp_world, v_world, 3, HS.W, HS.Hs);
    F.fb_hdr = make_fb(F.rp_hdr, &F.hdr.view, 1, F.HDW, F.HDH);
    F.fb_sunvis = make_fb(F.rp_sunvis, &F.sunvis.view, 1, 1, 1);
    for (int m = 0; m < BLOOM_MIPS; m++) {
        F.fb_bloom[m] = make_fb(F.rp_hdr, &F.bloom_view[m], 1, (F.HDW / 2) >> m, (F.HDH / 2) >> m);
        if (!F.fb_bloom[m]) return 0;
    }
    if (!F.fb_prime || !F.fb_trees || !F.fb_cloud || !F.fb_post || !F.fb_world ||
        !F.fb_hdr || !F.fb_sunvis || !F.fb_cres) return 0;

    const VkColorComponentFlags RGB = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT;
    const VkColorComponentFlags RGBA = RGB | VK_COLOR_COMPONENT_A_BIT;
    pipe_desc_t d;
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_bg_frag; d.fs_bytes = sizeof spv_hw_bg_frag;
    d.rp = F.rp_world; d.ncolor = 2; d.mask[0] = RGB; d.mask[1] = VK_COLOR_COMPONENT_R_BIT;
    d.depth_test = 1; d.depth_write = 1; d.depth_op = VK_COMPARE_OP_GREATER;
    F.p_bg = make_pipe(&d);
    VkVertexInputBindingDescription tb[2] = {
        { 0, sizeof(hw_tile_vtx_t), VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, 8, VK_VERTEX_INPUT_RATE_INSTANCE },
    };
    VkVertexInputAttributeDescription ta[6] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 20 },
        { 3, 0, VK_FORMAT_R32_UINT, 24 },
        { 4, 0, VK_FORMAT_R8G8B8A8_UNORM, 28 },
        { 5, 1, VK_FORMAT_R32G32_SFLOAT, 0 },
    };
    VkPipelineVertexInputStateCreateInfo tvin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    tvin.vertexBindingDescriptionCount = 2;
    tvin.pVertexBindingDescriptions = tb;
    tvin.vertexAttributeDescriptionCount = 6;
    tvin.pVertexAttributeDescriptions = ta;
    memset(&d, 0, sizeof d);
    d.vs = spv_hw_tile_vert; d.vs_bytes = sizeof spv_hw_tile_vert;
    d.fs = spv_hw_tile_frag; d.fs_bytes = sizeof spv_hw_tile_frag;
    d.rp = F.rp_world; d.ncolor = 2; d.mask[0] = RGB; d.mask[1] = VK_COLOR_COMPONENT_R_BIT;
    d.depth_test = 1; d.depth_write = 1; d.depth_op = VK_COMPARE_OP_GREATER;
    d.vin = &tvin;
    F.p_tile = make_pipe(&d);
    VkVertexInputBindingDescription ob = { 0, sizeof(hw_ocean_node_t), VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription oa = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo ovin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    ovin.vertexBindingDescriptionCount = 1;
    ovin.pVertexBindingDescriptions = &ob;
    ovin.vertexAttributeDescriptionCount = 1;
    ovin.pVertexAttributeDescriptions = &oa;
    memset(&d, 0, sizeof d);
    d.vs = spv_hw_ocean_vert; d.vs_bytes = sizeof spv_hw_ocean_vert;
    d.fs = spv_hw_ocean_frag; d.fs_bytes = sizeof spv_hw_ocean_frag;
    d.rp = F.rp_world; d.ncolor = 2; d.mask[0] = RGB; d.mask[1] = VK_COLOR_COMPONENT_R_BIT;
    d.depth_test = 1; d.depth_write = 1; d.depth_op = VK_COMPARE_OP_GREATER;
    d.vin = &ovin;
    F.p_ocean = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_prime_frag; d.fs_bytes = sizeof spv_hw_prime_frag;
    d.rp = F.rp_prime; d.ncolor = 0; d.depth_test = 1; d.depth_write = 1;
    d.depth_op = VK_COMPARE_OP_ALWAYS;
    F.p_prime = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_hw_tree_vert; d.vs_bytes = sizeof spv_hw_tree_vert;
    d.fs = spv_hw_tree_frag; d.fs_bytes = sizeof spv_hw_tree_frag;
    d.rp = F.rp_trees; d.ncolor = 1; d.mask[0] = RGB; d.depth_test = 1; d.depth_write = 1;
    d.depth_op = VK_COMPARE_OP_GREATER;
    F.p_tree = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_cloud_frag; d.fs_bytes = sizeof spv_hw_cloud_frag;
    d.rp = F.rp_cloud; d.ncolor = 2; d.mask[0] = RGBA;
    d.mask[1] = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    F.p_cloud = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_cloud_resolve_frag; d.fs_bytes = sizeof spv_hw_cloud_resolve_frag;
    d.rp = F.rp_cres; d.ncolor = 1; d.mask[0] = RGBA;
    F.p_cres = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_post_frag; d.fs_bytes = sizeof spv_hw_post_frag;
    d.rp = F.rp_hdr; d.ncolor = 1; d.mask[0] = RGBA;
    F.p_post = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_bloom_down_frag; d.fs_bytes = sizeof spv_hw_bloom_down_frag;
    d.rp = F.rp_hdr; d.ncolor = 1; d.mask[0] = RGBA; d.layout = F.layout_b;
    F.p_bdown = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_bloom_up_frag; d.fs_bytes = sizeof spv_hw_bloom_up_frag;
    d.rp = F.rp_bup; d.ncolor = 1; d.mask[0] = RGBA; d.layout = F.layout_b; d.additive = 1;
    F.p_bup = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_sunvis_frag; d.fs_bytes = sizeof spv_hw_sunvis_frag;
    d.rp = F.rp_sunvis; d.ncolor = 1; d.mask[0] = VK_COLOR_COMPONENT_R_BIT;
    F.p_sunvis = make_pipe(&d);
    memset(&d, 0, sizeof d);
    d.vs = spv_psx_vert; d.vs_bytes = sizeof spv_psx_vert;
    d.fs = spv_hw_composite_frag; d.fs_bytes = sizeof spv_hw_composite_frag;
    d.rp = F.rp_post; d.ncolor = 1; d.mask[0] = RGB;
    F.p_comp = make_pipe(&d);
    if (!F.p_bg || !F.p_prime || !F.p_tree || !F.p_cloud || !F.p_post || !F.p_tile ||
        !F.p_ocean || !F.p_bdown || !F.p_bup || !F.p_sunvis || !F.p_comp || !F.p_cres) return 0;

    u8 *st = (u8 *)F.stage_map + (size_t)HW_MAP_N * HW_MAP_N * 2 +
             (size_t)HW_ATLAS_W * HW_ATLAS_H * 8 + 512 * 4;
    u8 *wst = st + (size_t)NOISE_N * NOISE_N * NOISE_N * 4;
    u8 *mst = wst + (size_t)WEATHER_N * WEATHER_N * 4;
    make_noise(st, wst);
    make_mips(st, NOISE_N, 1, NOISE_MIPS, mst);
    make_mips(wst, WEATHER_N, 0, WEATHER_MIPS, mst + mip_bytes(NOISE_N, 1, NOISE_MIPS));
    F.ready = 1;
    return 1;
}

void hw_fx_shutdown(void) {
    if (!HS.dev) return;
    VkPipeline ps[12] = { F.p_bg, F.p_prime, F.p_tree, F.p_cloud, F.p_post, F.p_tile,
                          F.p_ocean, F.p_bdown, F.p_bup, F.p_sunvis, F.p_comp, F.p_cres };
    for (int i = 0; i < 12; i++) if (ps[i]) vkDestroyPipeline(HS.dev, ps[i], NULL);
    for (int m = 0; m < BLOOM_MIPS; m++) {
        if (F.fb_bloom[m]) vkDestroyFramebuffer(HS.dev, F.fb_bloom[m], NULL);
        if (F.bloom_view[m]) vkDestroyImageView(HS.dev, F.bloom_view[m], NULL);
    }
    if (F.bloom) vkDestroyImage(HS.dev, F.bloom, NULL);
    if (F.bloom_mem) vkFreeMemory(HS.dev, F.bloom_mem, NULL);
    if (F.fb_hdr) vkDestroyFramebuffer(HS.dev, F.fb_hdr, NULL);
    if (F.fb_sunvis) vkDestroyFramebuffer(HS.dev, F.fb_sunvis, NULL);
    VkRenderPass rp3[3] = { F.rp_hdr, F.rp_bup, F.rp_sunvis };
    for (int i = 0; i < 3; i++) if (rp3[i]) vkDestroyRenderPass(HS.dev, rp3[i], NULL);
    if (F.layout_b) vkDestroyPipelineLayout(HS.dev, F.layout_b, NULL);
    if (F.pool_b) vkDestroyDescriptorPool(HS.dev, F.pool_b, NULL);
    if (F.dsl_b) vkDestroyDescriptorSetLayout(HS.dev, F.dsl_b, NULL);
    if (F.s_lin) vkDestroySampler(HS.dev, F.s_lin, NULL);
    fx_image_free(&F.hdr);
    fx_image_free(&F.sunvis);
    VkFramebuffer fbs[6] = { F.fb_prime, F.fb_trees, F.fb_cloud, F.fb_post, F.fb_world, F.fb_cres };
    for (int i = 0; i < 6; i++) if (fbs[i]) vkDestroyFramebuffer(HS.dev, fbs[i], NULL);
    VkRenderPass rps[6] = { F.rp_prime, F.rp_trees, F.rp_cloud, F.rp_post, F.rp_world, F.rp_cres };
    for (int i = 0; i < 6; i++) if (rps[i]) vkDestroyRenderPass(HS.dev, rps[i], NULL);
    if (F.mesh) hw_free_host_buffer(&F.mesh, &F.mesh_mem, &F.mesh_map);
    for (int f = 0; f < HW_FRAMES; f++) {
        if (F.inst[f]) hw_free_host_buffer(&F.inst[f], &F.inst_mem[f], &F.inst_map[f]);
        if (F.onode[f]) hw_free_host_buffer(&F.onode[f], &F.onode_mem[f], &F.onode_map[f]);
    }
    hw_ocean_shutdown();
    if (F.layout) vkDestroyPipelineLayout(HS.dev, F.layout, NULL);
    if (F.pool) vkDestroyDescriptorPool(HS.dev, F.pool, NULL);
    if (F.dsl) vkDestroyDescriptorSetLayout(HS.dev, F.dsl, NULL);
    if (F.s_near) vkDestroySampler(HS.dev, F.s_near, NULL);
    if (F.s_rep) vkDestroySampler(HS.dev, F.s_rep, NULL);
    if (F.s_mip) vkDestroySampler(HS.dev, F.s_mip, NULL);
    for (int f = 0; f < HW_FRAMES; f++)
        if (F.ubo[f]) hw_free_host_buffer(&F.ubo[f], &F.ubo_mem[f], &F.ubo_map[f]);
    if (F.stage) hw_free_host_buffer(&F.stage, &F.stage_mem, &F.stage_map);
    fx_img_t *im[11] = { &F.depth, &F.cloud, &F.cloudz, &F.grid, &F.height, &F.colors,
                         &F.top, &F.noise, &F.weather, &F.cloud_raw, &F.cloud_hist };
    for (int i = 0; i < 11; i++) fx_image_free(im[i]);
    memset(&F, 0, sizeof F);
}

static void queue(u8 op, const hw_scene_t *s) {
    if (!F.ready || F.nscenes >= MAX_SCENES) return;
    F.scenes[F.nscenes] = *s;
    F.kind[F.nscenes] = op;
    memcpy(F.pal[F.nscenes], F.pend_pal, sizeof F.pend_pal);
    cmd_t *c = hw_new_cmd(op);
    if (!c) return;
    c->first = F.nscenes++;
}

void hw_fx_palette(s32 dx, s32 dy, s32 w, s32 h, s32 sx, s32 sy) {
    s32 p[6] = { dx, dy, w, h, sx, sy };
    memcpy(F.pend_pal, p, sizeof p);
}

void hw_fx_begin(const hw_scene_t *s) {
    queue(OP_FX_BEGIN, s);
}

void hw_fx_end(const hw_scene_t *s) {
    queue(OP_FX_END, s);
    hw_mark_dirty(s->area[0], s->area[1], s->area[2] + 1, s->area[3] + 1);
}

int hw_fx_ready(void) { return F.ready; }

void hw_fx_frame_done(void) {
    F.nscenes = 0;
}

static void cloud_history(fx_ubo_t *u, const hw_scene_t *s) {
    int ok = F.cl_last.valid && s->frame > F.cl_last.frame && s->frame - F.cl_last.frame <= 8 &&
             fabsf(s->H - F.cl_last.H) <= 0.01f * s->H && fabsf(s->ws - F.cl_last.ws) <= 0.01f;
    float moved = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = s->cam[i] - F.cl_last.cam[i];
        moved += d * d;
    }
    if (moved > 5000.0f * 5000.0f) ok = 0;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) u->prev_wtv[4 * i + k] = F.cl_last.rot[3 * k + i];
    for (int i = 0; i < 3; i++) u->prev_cam[i] = F.cl_last.cam[i];
    u->prev_proj[0] = F.cl_last.H;
    u->prev_proj[1] = F.cl_last.ws;
    u->prev_proj[2] = F.cl_last.ofs[0];
    u->prev_proj[3] = F.cl_last.ofs[1];
    for (int i = 0; i < 4; i++) u->prev_area[i] = (float)F.cl_last.area[i];
    u->cloud_tc[0] = (float)(s->frame & 1023u);
    u->cloud_tc[1] = ok ? CLOUD_HISTORY : 0.0f;
    F.cl_last.valid = 1;
    F.cl_last.frame = s->frame;
    memcpy(F.cl_last.rot, s->rot, sizeof F.cl_last.rot);
    memcpy(F.cl_last.cam, s->cam, sizeof F.cl_last.cam);
    F.cl_last.H = s->H;
    F.cl_last.ws = s->ws;
    F.cl_last.ofs[0] = s->ofs[0];
    F.cl_last.ofs[1] = s->ofs[1];
    memcpy(F.cl_last.area, s->area, sizeof F.cl_last.area);
}

static void fill_ubo(fx_ubo_t *u, const hw_scene_t *s, u32 si, float tile_half) {
    memset(u, 0, sizeof *u);
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) {
            u->vtw[4 * i + k] = s->rot[3 * i + k];
            u->wtv[4 * i + k] = s->rot[3 * k + i];
        }
    u->cam[0] = s->cam[0]; u->cam[1] = s->cam[1]; u->cam[2] = s->cam[2]; u->cam[3] = s->time;
    u->proj[0] = s->H; u->proj[1] = s->ws; u->proj[2] = (float)HS.S; u->proj[3] = s->sea_y;
    u->ofs[0] = s->ofs[0]; u->ofs[1] = s->ofs[1];
    u->ofs[2] = (float)s->area[0]; u->ofs[3] = (float)s->area[1];
    u->area[0] = (float)(s->area[2] - s->area[0] + 1);
    u->area[1] = (float)(s->area[3] - s->area[1] + 1);
    float horizon[3];
    hw_world_sky_at(s, 0.1f, horizon);
    float lum = horizon[0] * 0.3f + horizon[1] * 0.59f + horizon[2] * 0.11f;
    float inten = lum * 2.4f;
    if (inten < 0.15f) inten = 0.15f;
    if (inten > 1.6f) inten = 1.6f;
    float up = -s->sun[1];
    if (up < 0.05f) inten *= up <= -0.05f ? 0.0f : (up + 0.05f) / 0.1f;
    for (int i = 0; i < 3; i++) {
        u->sun[i] = s->sun[i];
        u->sun_col[i] = s->sun_col[i];
        u->fog[i] = s->fog_col[i];
        u->water[i] = (float)((hw_map.water_rgb >> (8 * i)) & 255u) / 255.0f;
    }
    u->sun[3] = inten;
    u->fog[3] = 1.0f / 110000.0f;
    u->map[0] = hw_map.valid ? 1.0f : 0.0f;
    u->map[1] = hw_map.top_y;
    u->map[2] = 300.0f;
    u->map[3] = 1.4e6f;
    u->cloud[0] = s->sea_y - 7500.0f;
    u->cloud[1] = s->sea_y - 13500.0f;
    u->cloud[2] = 0.36f;
    u->cloud[3] = 1.0f;
    u->wind[0] = s->time * 30.0f; u->wind[1] = s->time * 12.0f;
    u->wind[2] = 0.8253f; u->wind[3] = 0.5646f;
    u->water[3] = hw_map.valid && hw_map.has_sea ? 1.0f : 0.0f;
    u->misc[0] = 1.0f / (float)HS.W; u->misc[1] = 1.0f / (float)HS.Hs;
    u->misc[2] = Z_FAR;
    u32 flags = hw_fx_flags;
    if (!hw_map.valid) flags &= ~(FX_FAR | FX_FOREST);
    u->misc[3] = (float)flags;
    for (int i = 0; i < HW_SKY_BINS; i++)
        for (int c = 0; c < 3; c++) u->sky[i][c] = s->sky[i][c];
    for (int i = 0; i < 4; i++) u->pal[i] = (float)F.pal[si][i];
    u->pal_src[0] = (float)F.pal[si][4];
    u->pal_src[1] = (float)F.pal[si][5];
    u->pal_src[3] = tile_half;
    u->hdr[0] = (float)F.HDW; u->hdr[1] = (float)F.HDH;
    u->hdr[2] = 0.5f;
    u->hdr[3] = 1.0f;
    hw_ocean_params(u->ocean);
    u->ocean[1][3] = F.ocean_on[si] ? 1.0f : 0.0f;
}

int hw_fx_ocean_drawn(void) {
    return F.ready && (hw_fx_flags & FX_OCEAN) && (hw_fx_flags & FX_FAR) && hw_map.valid &&
           hw_map.has_sea && hw_ocean_ready();
}

static float ocean_horizon(const hw_scene_t *s) {
    double alt = (double)s->sea_y - (double)s->cam[1];
    if (alt < 1.0) alt = 1.0;
    double h = sqrt(2.0 * 6.371e6 * 5.0 * alt) * 1.15 + 20000.0;
    if (h > 2.2e6) h = 2.2e6;
    return (float)h;
}

static u32 gather_tiles(const hw_scene_t *s, float *inst, tile_draw_t *draws) {
    static u16 cell_type[MAX_INST];
    static float cell_org[MAX_INST][2];
    static u32 count[512], start[512];
    memset(count, 0, sizeof count);
    int ctx = (int)floorf(s->cam[0] / HW_TILE_SIZE) + 128;
    int ctz = (int)floorf(s->cam[2] / HW_TILE_SIZE) + 128;
    float hw_ = (float)(s->area[2] - s->area[0] + 1) * 0.5f * 1.15f / (s->H * s->ws);
    float hh = (float)(s->area[3] - s->area[1] + 1) * 0.5f * 1.15f / s->H;
    float nx = 1.0f / sqrtf(1.0f + hw_ * hw_), ny = 1.0f / sqrtf(1.0f + hh * hh);
    const float R = 2600.0f;
    u32 n = 0;
    for (int dz = -TILE_CELLS; dz <= TILE_CELLS; dz++)
        for (int dx = -TILE_CELLS; dx <= TILE_CELLS; dx++) {
            int cx = ctx + dx, cz = ctz + dz;
            int wx = cx < 0 ? (cx & 7) : cx >= HW_MAP_N ? ((cx & 7) | 0xF8) : cx;
            int wz = cz < 0 ? (cz & 7) : cz >= HW_MAP_N ? ((cz & 7) | 0xF8) : cz;
            u16 t = hw_map.grid[wz * HW_MAP_N + wx];
            if (!hw_map.mesh_count[t] || hw_map.tile_water[t]) continue;
            float ox = (float)(cx - 128) * HW_TILE_SIZE + HW_TILE_SIZE * 0.5f;
            float oz = (float)(cz - 128) * HW_TILE_SIZE + HW_TILE_SIZE * 0.5f;
            float rel[3] = { ox - s->cam[0], hw_map.tile_top[t] * 0.5f + hw_map.sea_y * 0.5f - s->cam[1],
                             oz - s->cam[2] };
            float v[3];
            for (int i = 0; i < 3; i++)
                v[i] = s->rot[3 * i] * rel[0] + s->rot[3 * i + 1] * rel[1] + s->rot[3 * i + 2] * rel[2];
            if (v[2] < -R) continue;
            if ((v[0] - hw_ * v[2]) * nx > R || (-v[0] - hw_ * v[2]) * nx > R) continue;
            if ((v[1] - hh * v[2]) * ny > R || (-v[1] - hh * v[2]) * ny > R) continue;
            cell_type[n] = t;
            cell_org[n][0] = ox;
            cell_org[n][1] = oz;
            count[t]++;
            n++;
        }
    u32 nd = 0, acc = 0;
    for (int t = 0; t < 512; t++) {
        start[t] = acc;
        if (!count[t]) continue;
        draws[nd].first_vtx = hw_map.mesh_first[t];
        draws[nd].nvtx = hw_map.mesh_count[t];
        draws[nd].first_inst = acc;
        draws[nd].ninst = count[t];
        nd++;
        acc += count[t];
    }
    for (u32 i = 0; i < n; i++) {
        u32 k = start[cell_type[i]]++;
        inst[2 * k] = cell_org[i][0];
        inst[2 * k + 1] = cell_org[i][1];
    }
    return nd;
}

static void upload_mips(VkCommandBuffer cb, fx_img_t *im, VkDeviceSize off0, VkDeviceSize offm,
                        u32 n, int is3d, u32 levels) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = levels;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy r[16];
    memset(r, 0, sizeof r);
    VkDeviceSize off = offm;
    for (u32 l = 0; l < levels && l < 16; l++) {
        u32 m = n >> l;
        r[l].bufferOffset = l ? off : off0;
        r[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r[l].imageSubresource.mipLevel = l;
        r[l].imageSubresource.layerCount = 1;
        r[l].imageExtent.width = m;
        r[l].imageExtent.height = m;
        r[l].imageExtent.depth = is3d ? m : 1;
        if (l) off += (VkDeviceSize)m * m * (is3d ? m : 1) * 4;
    }
    vkCmdCopyBufferToImage(cb, F.stage, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, r);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static void upload_region(VkCommandBuffer cb, fx_img_t *im, VkDeviceSize off, u32 w, u32 h, u32 d,
                          int first) {
    layout_barrier(cb, im->img, VK_IMAGE_ASPECT_COLOR_BIT,
                   first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r;
    memset(&r, 0, sizeof r);
    r.bufferOffset = off;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = d;
    vkCmdCopyBufferToImage(cb, F.stage, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    layout_barrier(cb, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
}

void hw_fx_prepare(VkCommandBuffer cb, int slot) {
    if (!F.ready) return;
    VkDeviceSize o_grid = 0;
    VkDeviceSize o_height = o_grid + (VkDeviceSize)HW_MAP_N * HW_MAP_N * 2;
    VkDeviceSize o_colors = o_height + (VkDeviceSize)HW_ATLAS_W * HW_ATLAS_H * 4;
    VkDeviceSize o_top = o_colors + (VkDeviceSize)HW_ATLAS_W * HW_ATLAS_H * 4;
    VkDeviceSize o_noise = o_top + 512 * 4;
    VkDeviceSize o_weather = o_noise + (VkDeviceSize)NOISE_N * NOISE_N * NOISE_N * 4;
    if (!F.images_ready) {
        VkDeviceSize o_mips = o_weather + (VkDeviceSize)WEATHER_N * WEATHER_N * 4;
        upload_mips(cb, &F.noise, o_noise, o_mips, NOISE_N, 1, NOISE_MIPS);
        upload_mips(cb, &F.weather, o_weather, o_mips + mip_bytes(NOISE_N, 1, NOISE_MIPS),
                    WEATHER_N, 0, WEATHER_MIPS);
        memset(F.stage_map, 0, (size_t)o_noise);
        upload_region(cb, &F.grid, o_grid, HW_MAP_N, HW_MAP_N, 1, 1);
        upload_region(cb, &F.height, o_height, HW_ATLAS_W, HW_ATLAS_H, 1, 1);
        upload_region(cb, &F.colors, o_colors, HW_ATLAS_W, HW_ATLAS_H, 1, 1);
        upload_region(cb, &F.top, o_top, 512, 1, 1, 1);
        fx_img_t *cl[4] = { &F.cloud, &F.cloudz, &F.cloud_raw, &F.cloud_hist };
        for (int i = 0; i < 4; i++) {
            layout_barrier(cb, cl[i]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue z = { { 0, 0, 0, 1 } };
            VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cb, cl[i]->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &rg);
            layout_barrier(cb, cl[i]->img, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        VkImage hi[2] = { F.hdr.img, F.sunvis.img };
        for (int i = 0; i < 3; i++) {
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = i < 2 ? hi[i] : F.bloom;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = i < 2 ? 1 : BLOOM_MIPS;
            b.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
        }
        F.images_ready = 1;
    }
    if (hw_map.valid && hw_map.version != F.map_version) {
        u8 *st = (u8 *)F.stage_map;
        memcpy(st + o_grid, hw_map.grid, (size_t)HW_MAP_N * HW_MAP_N * 2);
        memcpy(st + o_height, hw_map.height, (size_t)HW_ATLAS_W * HW_ATLAS_H * 4);
        memcpy(st + o_colors, hw_map.color, (size_t)HW_ATLAS_W * HW_ATLAS_H * 4);
        memcpy(st + o_top, hw_map.tile_top, 512 * 4);
        upload_region(cb, &F.grid, o_grid, HW_MAP_N, HW_MAP_N, 1, 0);
        upload_region(cb, &F.height, o_height, HW_ATLAS_W, HW_ATLAS_H, 1, 0);
        upload_region(cb, &F.colors, o_colors, HW_ATLAS_W, HW_ATLAS_H, 1, 0);
        upload_region(cb, &F.top, o_top, 512, 1, 1, 0);
        F.map_version = hw_map.version;
    }
    if (hw_map.valid && hw_map.mesh && F.mesh_version != hw_map.version) {
        vkDeviceWaitIdle(HS.dev);
        F.mesh_n = hw_map.mesh_n < MESH_CAP ? hw_map.mesh_n : 0;
        if (F.mesh_n) memcpy(F.mesh_map, hw_map.mesh, (size_t)F.mesh_n * sizeof(hw_tile_vtx_t));
        F.mesh_version = hw_map.version;
    }
    int sim = 0;
    double sim_time = 0.0;
    for (u32 i = 0; i < F.nscenes && !sim; i++)
        if (F.kind[i] == OP_FX_BEGIN && (hw_fx_flags & FX_OCEAN) && hw_map.valid && hw_map.has_sea) {
            sim = 1;
            sim_time = F.scenes[i].time;
        }
    hw_ocean_simulate(cb, sim ? sim_time : -1.0);
    int no = 0;
    F.ocean_scene[0] = F.ocean_scene[1] = -1;
    memset(F.ocean_on, 0, sizeof F.ocean_on);
    for (u32 i = 0; i < F.nscenes; i++)
        if (F.kind[i] == OP_FX_BEGIN && no < 2 && hw_fx_ocean_drawn()) {
            hw_ocean_node_t *nodes = (hw_ocean_node_t *)F.onode_map[slot] + (size_t)no * HW_OCEAN_MAX_NODES;
            F.nnodes[no] = hw_ocean_select(&F.scenes[i], ocean_horizon(&F.scenes[i]), nodes,
                                           HW_OCEAN_MAX_NODES);
            F.ocean_scene[no] = (int)i;
            if (getenv("AC3_OCEAN_DEBUG")) printf("ocean: %u patches\n", F.nnodes[no]);
            F.ocean_on[i] = 1;
            no++;
        }
    for (u32 i = 0, last = 0; i < F.nscenes; i++) {
        if (F.kind[i] == OP_FX_BEGIN) last = F.ocean_on[i];
        else F.ocean_on[i] = (u8)last;
    }
    int nb = 0;
    F.tiles_scene[0] = F.tiles_scene[1] = -1;
    for (u32 i = 0; i < F.nscenes; i++) {
        float half = 0.0f;
        if (F.kind[i] == OP_FX_BEGIN && nb < 2 && (hw_fx_flags & FX_FAR) && hw_map.valid &&
            F.mesh_n && F.pal[i][2] > 0) {
            float *inst = (float *)F.inst_map[slot] + (size_t)nb * MAX_INST * 2;
            F.ndraws[nb] = gather_tiles(&F.scenes[i], inst, F.draws[nb]);
            F.inst_first[nb] = (u32)nb * MAX_INST;
            F.tiles_scene[nb] = (int)i;
            half = (float)TILE_CELLS * HW_TILE_SIZE - HW_TILE_SIZE;
            nb++;
        }
        fx_ubo_t *u = (fx_ubo_t *)((u8 *)F.ubo_map[slot] + UBO_STRIDE * i);
        fill_ubo(u, &F.scenes[i], i, half);
        if (F.kind[i] == OP_FX_END) {
            if (hw_fx_flags & FX_CLOUDS) cloud_history(u, &F.scenes[i]);
            else F.cl_last.valid = 0;
        }
    }
}

static VkRect2D area_rect(const hw_scene_t *s, int div) {
    s32 x0 = s->area[0], y0 = s->area[1], x1 = s->area[2] + 1, y1 = s->area[3] + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W) x1 = VRAM_W;
    if (y1 > VRAM_H) y1 = VRAM_H;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    VkRect2D r;
    r.offset.x = x0 * HS.S / div;
    r.offset.y = y0 * HS.S / div;
    r.extent.width = (u32)((x1 * HS.S + div - 1) / div - r.offset.x);
    r.extent.height = (u32)((y1 * HS.S + div - 1) / div - r.offset.y);
    return r;
}

static void bind(VkCommandBuffer cb, VkPipeline p, int slot, u32 scene) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    u32 off = UBO_STRIDE * scene;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.layout, 0, 1, &F.set[slot], 1, &off);
}

static void begin_rp(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, VkRect2D area,
                     u32 w, u32 h, const VkClearValue *clear, u32 nclear) {
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = area;
    bi.clearValueCount = nclear;
    bi.pClearValues = clear;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
}

#define L_ATT  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
#define L_RO   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
#define A_ATT  (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
#define S_ATT  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
#define S_SH   (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)

static void record_begin(VkCommandBuffer cb, int slot, u32 si) {
    const hw_scene_t *s = &F.scenes[si];
    VkRect2D sc = area_rect(s, 1);
    if (!sc.extent.width || !sc.extent.height) return;
    VkClearValue cv[3];
    memset(cv, 0, sizeof cv);
    cv[2].depthStencil.depth = 0.0f;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = F.rp_world;
    bi.framebuffer = F.fb_world;
    bi.renderArea = sc;
    bi.clearValueCount = 3;
    bi.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0, 0, (float)HS.W, (float)HS.Hs, 0, 1 };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkClearAttachment a = { VK_IMAGE_ASPECT_COLOR_BIT, 1, { { { 1.0e9f, 0, 0, 0 } } } };
    VkClearRect cr = { sc, 0, 1 };
    vkCmdClearAttachments(cb, 1, &a, 1, &cr);
    int no = F.ocean_scene[0] == (int)si ? 0 : F.ocean_scene[1] == (int)si ? 1 : -1;
    if (no >= 0 && F.nnodes[no]) {
        bind(cb, F.p_ocean, slot, si);
        VkDeviceSize off = (VkDeviceSize)no * HW_OCEAN_MAX_NODES * sizeof(hw_ocean_node_t);
        vkCmdBindVertexBuffers(cb, 0, 1, &F.onode[slot], &off);
        u32 nidx;
        VkBuffer ib = hw_ocean_indices(&nidx);
        vkCmdBindIndexBuffer(cb, ib, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, nidx, F.nnodes[no], 0, 0, 0);
    }
    int nb = F.tiles_scene[0] == (int)si ? 0 : F.tiles_scene[1] == (int)si ? 1 : -1;
    if (nb >= 0 && F.ndraws[nb]) {
        bind(cb, F.p_tile, slot, si);
        VkBuffer bufs[2] = { F.mesh, F.inst[slot] };
        VkDeviceSize offs[2] = { 0, (VkDeviceSize)F.inst_first[nb] * 8 };
        vkCmdBindVertexBuffers(cb, 0, 2, bufs, offs);
        for (u32 i = 0; i < F.ndraws[nb]; i++) {
            const tile_draw_t *d = &F.draws[nb][i];
            vkCmdDraw(cb, d->nvtx, d->ninst, d->first_vtx, d->first_inst);
        }
    }
    if ((hw_fx_flags & FX_FAR) && hw_map.valid) {
        bind(cb, F.p_bg, slot, si);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
}

static void record_end(VkCommandBuffer cb, int slot, u32 si) {
    const hw_scene_t *s = &F.scenes[si];
    VkRect2D sc = area_rect(s, 1);
    if (!sc.extent.width || !sc.extent.height) return;
    hw_barrier(cb, HS.zimg, L_ATT, L_RO, A_ATT, VK_ACCESS_SHADER_READ_BIT, S_ATT, S_SH);

    if ((hw_fx_flags & FX_FOREST) && hw_map.valid && hw_map.forest_samples > 0) {
        VkClearValue cv;
        cv.depthStencil.depth = 0.0f;
        cv.depthStencil.stencil = 0;
        begin_rp(cb, F.rp_prime, F.fb_prime, sc, HS.W, HS.Hs, &cv, 1);
        bind(cb, F.p_prime, slot, si);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        begin_rp(cb, F.rp_trees, F.fb_trees, sc, HS.W, HS.Hs, NULL, 0);
        bind(cb, F.p_tree, slot, si);
        float rings[2][4] = { { 44.0f, 292.0f, 0.0f, 6300.0f }, { 88.0f, 296.0f, 6300.0f, 12800.0f } };
        for (int r = 0; r < 2; r++) {
            vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, 16, rings[r]);
            u32 n = (u32)rings[r][1];
            vkCmdDraw(cb, 42, n * n, 0, 0);
        }
        vkCmdEndRenderPass(cb);
    }

    VkImageCopy g;
    memset(&g, 0, sizeof g);
    g.srcSubresource.aspectMask = g.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    g.srcSubresource.layerCount = g.dstSubresource.layerCount = 1;
    g.srcOffset.x = g.dstOffset.x = sc.offset.x;
    g.srcOffset.y = g.dstOffset.y = sc.offset.y;
    g.extent.width = sc.extent.width;
    g.extent.height = sc.extent.height;
    g.extent.depth = 1;
    hw_barrier(cb, HS.target, L_ATT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, A_ATT,
               VK_ACCESS_TRANSFER_READ_BIT, S_ATT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    hw_barrier(cb, HS.read, L_RO, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, S_SH, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImage(cb, HS.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, HS.read,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &g);
    hw_barrier(cb, HS.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, L_ATT,
               VK_ACCESS_TRANSFER_READ_BIT, A_ATT, VK_PIPELINE_STAGE_TRANSFER_BIT, S_ATT);
    hw_barrier(cb, HS.read, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, L_RO, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, S_SH);

    if (hw_fx_flags & FX_CLOUDS) {
        VkRect2D hc = area_rect(s, 2);
        if (hc.offset.x + (s32)hc.extent.width > (s32)F.CW) hc.extent.width = F.CW - (u32)hc.offset.x;
        if (hc.offset.y + (s32)hc.extent.height > (s32)F.CH) hc.extent.height = F.CH - (u32)hc.offset.y;
        begin_rp(cb, F.rp_cloud, F.fb_cloud, hc, F.CW, F.CH, NULL, 0);
        bind(cb, F.p_cloud, slot, si);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        begin_rp(cb, F.rp_cres, F.fb_cres, hc, F.CW, F.CH, NULL, 0);
        bind(cb, F.p_cres, slot, si);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        const VkPipelineStageFlags XF = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const VkImageLayout XS = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, XD = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        hw_barrier(cb, F.cloud.img, L_RO, XS, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, S_SH, XF);
        hw_barrier(cb, F.cloud_hist.img, L_RO, XD, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   S_SH, XF);
        VkImageCopy hcp;
        memset(&hcp, 0, sizeof hcp);
        hcp.srcSubresource.aspectMask = hcp.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        hcp.srcSubresource.layerCount = hcp.dstSubresource.layerCount = 1;
        hcp.srcOffset.x = hcp.dstOffset.x = hc.offset.x;
        hcp.srcOffset.y = hcp.dstOffset.y = hc.offset.y;
        hcp.extent.width = hc.extent.width;
        hcp.extent.height = hc.extent.height;
        hcp.extent.depth = 1;
        vkCmdCopyImage(cb, F.cloud.img, XS, F.cloud_hist.img, XD, 1, &hcp);
        hw_barrier(cb, F.cloud.img, XS, L_RO, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, XF, S_SH);
        hw_barrier(cb, F.cloud_hist.img, XD, L_RO, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   XF, S_SH);
    }

    VkRect2D hs = { { 0, 0 }, { sc.extent.width, sc.extent.height } };
    if (hs.extent.width > F.HDW) hs.extent.width = F.HDW;
    if (hs.extent.height > F.HDH) hs.extent.height = F.HDH;
    begin_rp(cb, F.rp_hdr, F.fb_hdr, hs, F.HDW, F.HDH, NULL, 0);
    bind(cb, F.p_post, slot, si);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    VkRect2D one = { { 0, 0 }, { 1, 1 } };
    begin_rp(cb, F.rp_sunvis, F.fb_sunvis, one, 1, 1, NULL, 0);
    bind(cb, F.p_sunvis, slot, si);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    bloom_push_t bp;
    bp.uv_max[0] = (float)hs.extent.width / (float)F.HDW;
    bp.uv_max[1] = (float)hs.extent.height / (float)F.HDH;
    bp.radius = 1.0f;
    for (int m = 0; m < BLOOM_MIPS; m++) {
        u32 w = (F.HDW / 2) >> m, h = (F.HDH / 2) >> m;
        VkRect2D r = { { 0, 0 }, { w, h } };
        begin_rp(cb, F.rp_hdr, F.fb_bloom[m], r, w, h, NULL, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.p_bdown);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.layout_b, 0, 1, &F.set_b[m], 0, NULL);
        bp.src_texel[0] = 1.0f / (float)(w * 2);
        bp.src_texel[1] = 1.0f / (float)(h * 2);
        bp.first = m == 0 ? 1.0f : 0.0f;
        vkCmdPushConstants(cb, F.layout_b, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    }
    for (int m = BLOOM_MIPS - 2; m >= 0; m--) {
        u32 w = (F.HDW / 2) >> m, h = (F.HDH / 2) >> m;
        VkRect2D r = { { 0, 0 }, { w, h } };
        begin_rp(cb, F.rp_bup, F.fb_bloom[m], r, w, h, NULL, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.p_bup);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, F.layout_b, 0, 1, &F.set_b[m + 2], 0, NULL);
        bp.src_texel[0] = 1.0f / (float)(w / 2);
        bp.src_texel[1] = 1.0f / (float)(h / 2);
        bp.first = 0.0f;
        vkCmdPushConstants(cb, F.layout_b, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    }

    begin_rp(cb, F.rp_post, F.fb_post, sc, HS.W, HS.Hs, NULL, 0);
    bind(cb, F.p_comp, slot, si);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    const VkImageLayout XS = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, XD = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkPipelineStageFlags XF = VK_PIPELINE_STAGE_TRANSFER_BIT;
    hw_barrier(cb, HS.zimg, L_RO, XS, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, S_SH, XF);
    hw_barrier(cb, HS.zsnap, L_RO, XD, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, S_SH, XF);
    VkImageCopy zc;
    memset(&zc, 0, sizeof zc);
    zc.srcSubresource.aspectMask = zc.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    zc.srcSubresource.layerCount = zc.dstSubresource.layerCount = 1;
    zc.srcOffset.x = zc.dstOffset.x = sc.offset.x;
    zc.srcOffset.y = zc.dstOffset.y = sc.offset.y;
    zc.extent.width = sc.extent.width;
    zc.extent.height = sc.extent.height;
    zc.extent.depth = 1;
    vkCmdCopyImage(cb, HS.zimg, XS, HS.zsnap, XD, 1, &zc);
    hw_barrier(cb, HS.zsnap, XD, L_RO, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, XF, S_SH);
    hw_barrier(cb, HS.zimg, XS, L_ATT, VK_ACCESS_TRANSFER_READ_BIT, A_ATT, XF, S_ATT);
}

void hw_fx_record(VkCommandBuffer cb, int slot, const cmd_t *c) {
    if (!F.ready || c->first >= F.nscenes) return;
    if (c->op == OP_FX_BEGIN) record_begin(cb, slot, c->first);
    else record_end(cb, slot, c->first);
}
