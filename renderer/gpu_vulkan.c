#include "gpu_backend.h"
#include "gpu_sw.h"
#include "ac3_capture.h"
#include "ac3_shaders.h"
#include "spu_core.h"
#ifdef AC3_ENHANCED
#include "hw/hw_glue.h"
#include "hw/hw_ui.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES_IN_FLIGHT 2
#define VRAM_BYTES     (GPU_VRAM_W * GPU_VRAM_H * 2)

typedef struct {
    int32_t origin[2];
    int32_t size[2];
    float   view_pos[2];
    float   view_size[2];
    uint32_t rgb24;
} display_push_t;

static struct {
    int ready;
    SDL_Window *window;

    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfamily;
    VkQueue queue;

    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkColorSpaceKHR swap_space;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage *images;
    VkImageView *views;
    VkFramebuffer *framebuffers;
    VkSemaphore *sem_render;

    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline pipeline;

    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet desc_set;

    VkImage vram_img;
    VkDeviceMemory vram_mem;
    VkImageView vram_view;
    VkSampler vram_sampler;
    VkBuffer vram_staging[FRAMES_IN_FLIGHT];
    VkDeviceMemory vram_staging_mem[FRAMES_IN_FLIGHT];
    void *vram_staging_map[FRAMES_IN_FLIGHT];

    VkCommandPool cmd_pool;
    VkCommandBuffer cmd[FRAMES_IN_FLIGHT];
    VkSemaphore sem_acquire[FRAMES_IN_FLIGHT];
    VkFence fence[FRAMES_IN_FLIGHT];
    uint32_t frame;

    uint32_t prims_at_last_frame;

    int quit;
    uint32_t min_images;
    int want_recreate, pending_scale, paused;
    int restart, restart_english;
    Uint64 fps_t0;
    uint32_t fps_n;
    float fps;
} R;

int ac3_render_quit_requested(void) { return R.quit; }

extern void ac3_input_event(const SDL_Event *ev);
extern void ac3_input_poll(void);
extern void ac3_input_open(void);
extern void ac3_input_suppress(int on);
extern void ac3_audio_set_volume(float v);

#define VKCHECK(expr) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
        fprintf(stderr, "ac3: vulkan: %s failed (%d) at %s:%d\n", \
                #expr, (int)_r, __FILE__, __LINE__); \
        return 0; \
    } \
} while (0)

static uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(R.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props,
                       VkBuffer *buf, VkDeviceMemory *mem) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(R.dev, &bi, NULL, buf));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(R.dev, *buf, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        fprintf(stderr, "ac3: vulkan: no memory type for buffer\n");
        return 0;
    }
    VKCHECK(vkAllocateMemory(R.dev, &ai, NULL, mem));
    VKCHECK(vkBindBufferMemory(R.dev, *buf, *mem, 0));
    return 1;
}

static int shot_pending;
static char shot_path[260];

static uint32_t shot_min_prims;

void ac3_render_screenshot(const char *path) {
    snprintf(shot_path, sizeof shot_path, "%s", path);
    shot_pending = 1;
}

void ac3_render_screenshot_when_drawing(const char *path, uint32_t min_prims) {
    snprintf(shot_path, sizeof shot_path, "%s", path);
    shot_min_prims = min_prims ? min_prims : 1;
}

static void write_screenshot(uint32_t image) {
    VkDeviceSize bytes = (VkDeviceSize)R.extent.width * R.extent.height * 4;
    VkBuffer buf; VkDeviceMemory mem;
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf, &mem))
        return;

    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = R.cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(R.dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = R.images[image];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    VkBufferImageCopy rg = { 0 };
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = R.extent.width;
    rg.imageExtent.height = R.extent.height;
    rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, R.images[image],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);

    VkImageMemoryBarrier back = b;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 0, NULL, 1, &back);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(R.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(R.queue);

    void *map = NULL;
    if (vkMapMemory(R.dev, mem, 0, bytes, 0, &map) == VK_SUCCESS) {
        uint32_t npx = R.extent.width * R.extent.height;
        const uint8_t *p = (const uint8_t *)map;
        uint8_t *rgb = (uint8_t *)malloc((size_t)npx * 3);
        if (rgb) {
            for (uint32_t i = 0; i < npx; i++) {
                rgb[i * 3 + 0] = p[i * 4 + 2];
                rgb[i * 3 + 1] = p[i * 4 + 1];
                rgb[i * 3 + 2] = p[i * 4 + 0];
            }
            size_t n = strlen(shot_path);
            int ok;
            if (n > 4 && strcmp(shot_path + n - 4, ".png") == 0) {
                ok = ac3_write_png_rgb(shot_path, R.extent.width,
                                       R.extent.height, rgb);
            } else {
                FILE *f = fopen(shot_path, "wb");
                ok = f != NULL;
                if (f) {
                    fprintf(f, "P6\n%u %u\n255\n",
                            R.extent.width, R.extent.height);
                    fwrite(rgb, 1, (size_t)npx * 3, f);
                    fclose(f);
                }
            }
            if (ok)
                printf("screenshot -> %s (%ux%u)\n", shot_path,
                       R.extent.width, R.extent.height);
            free(rgb);
        }
        vkUnmapMemory(R.dev, mem);
    }
    vkFreeCommandBuffers(R.dev, R.cmd_pool, 1, &cb);
    vkDestroyBuffer(R.dev, buf, NULL);
    vkFreeMemory(R.dev, mem, NULL);
}

static VkShaderModule make_shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(R.dev, &ci, NULL, &m) != VK_SUCCESS) {
        fprintf(stderr, "ac3: vulkan: shader module failed\n");
        return VK_NULL_HANDLE;
    }
    return m;
}

static int init_instance(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "ac3: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        fprintf(stderr, "ac3: SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
        return 0;
    }
#ifdef AC3_ENHANCED
    R.window = SDL_CreateWindow("Ace Combat 3 -- recompiled (enhanced)",
                                ac3_hd.widescreen ? 1600 : 1280,
                                ac3_hd.widescreen ? 900 : 960,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
#else
    R.window = SDL_CreateWindow("Ace Combat 3 -- recompiled",
                                1280, 960, SDL_WINDOW_VULKAN);
#endif
    if (!R.window) {
        fprintf(stderr, "ac3: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }

    uint32_t next = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&next);
    if (!sdl_ext) {
        fprintf(stderr, "ac3: SDL_Vulkan_GetInstanceExtensions failed: %s\n",
                SDL_GetError());
        return 0;
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ac3";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = next;
    ci.ppEnabledExtensionNames = sdl_ext;
    VKCHECK(vkCreateInstance(&ci, NULL, &R.instance));

    if (!SDL_Vulkan_CreateSurface(R.window, R.instance, NULL, &R.surface)) {
        fprintf(stderr, "ac3: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

static int init_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(R.instance, &n, NULL);
    if (!n) { fprintf(stderr, "ac3: no Vulkan device\n"); return 0; }
    VkPhysicalDevice *devs = malloc(n * sizeof *devs);
    vkEnumeratePhysicalDevices(R.instance, &n, devs);

    R.phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n && R.phys == VK_NULL_HANDLE; i++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        VkQueueFamilyProperties *qs = malloc(qn * sizeof *qs);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qs);
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, R.surface, &present);
            if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                R.phys = devs[i];
                R.qfamily = q;
                break;
            }
        }
        free(qs);
    }
    free(devs);
    if (R.phys == VK_NULL_HANDLE) {
        fprintf(stderr, "ac3: no graphics+present queue\n");
        return 0;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(R.phys, &props);
    printf("renderer: %s\n", props.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = R.qfamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = dext;
#ifdef AC3_ENHANCED
    VkPhysicalDeviceFeatures feat = { 0 };
    feat.independentBlend = VK_TRUE;
    di.pEnabledFeatures = &feat;
#endif
    VKCHECK(vkCreateDevice(R.phys, &di, NULL, &R.dev));
    vkGetDeviceQueue(R.dev, R.qfamily, 0, &R.queue);
    return 1;
}

static int pick_surface_format(void) {
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.phys, R.surface, &fn, NULL);
    if (!fn) return 0;
    VkSurfaceFormatKHR *fmts = malloc(fn * sizeof *fmts);
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.phys, R.surface, &fn, fmts);
    R.swap_format = fmts[0].format;
    R.swap_space = fmts[0].colorSpace;
    for (uint32_t i = 0; i < fn; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            R.swap_format = fmts[i].format;
            R.swap_space = fmts[i].colorSpace;
            break;
        }
    free(fmts);
    return 1;
}

static int init_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R.phys, R.surface, &caps));

    R.extent = caps.currentExtent;
    if (R.extent.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(R.window, &w, &h);
        R.extent.width = (uint32_t)w;
        R.extent.height = (uint32_t)h;
    }
    if (!R.extent.width || !R.extent.height) return -1;

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = R.surface;
    sci.minImageCount = want;
    sci.imageFormat = R.swap_format;
    sci.imageColorSpace = R.swap_space;
    sci.imageExtent = R.extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    R.min_images = want;
    {
        const char *pm = SDL_getenv("AC3_PRESENT_MODE");
#ifdef AC3_ENHANCED
        if (!pm && !ac3_ui.vsync) {
            u32 nmodes = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(R.phys, R.surface, &nmodes, NULL);
            VkPresentModeKHR modes[16];
            if (nmodes > 16) nmodes = 16;
            vkGetPhysicalDeviceSurfacePresentModesKHR(R.phys, R.surface, &nmodes, modes);
            pm = "immediate";
            for (u32 i = 0; i < nmodes; i++)
                if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) pm = "mailbox";
        }
#endif
        if (pm) {
            VkPresentModeKHR mode = sci.presentMode;
            if (!SDL_strcasecmp(pm, "immediate"))    mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (!SDL_strcasecmp(pm, "mailbox")) mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (!SDL_strcasecmp(pm, "fifo"))    mode = VK_PRESENT_MODE_FIFO_KHR;
            else printf("renderer: unknown AC3_PRESENT_MODE '%s', keeping fifo\n", pm);

            u32 nmodes = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(R.phys, R.surface, &nmodes, NULL);
            VkPresentModeKHR *modes = malloc(nmodes * sizeof *modes);
            if (modes) {
                vkGetPhysicalDeviceSurfacePresentModesKHR(R.phys, R.surface,
                                                          &nmodes, modes);
                for (u32 i = 0; i < nmodes; i++)
                    if (modes[i] == mode) { sci.presentMode = mode; break; }
                free(modes);
            }
            static int told;
            if (!told++) {
                if (sci.presentMode != mode)
                    printf("renderer: present mode '%s' not supported, keeping fifo\n", pm);
                else if (mode != VK_PRESENT_MODE_FIFO_KHR)
                    printf("renderer: present mode %s (vsync off)\n", pm);
            }
        }
    }
    sci.clipped = VK_TRUE;
    VKCHECK(vkCreateSwapchainKHR(R.dev, &sci, NULL, &R.swapchain));

    vkGetSwapchainImagesKHR(R.dev, R.swapchain, &R.image_count, NULL);
    R.images = calloc(R.image_count, sizeof *R.images);
    R.views = calloc(R.image_count, sizeof *R.views);
    R.framebuffers = calloc(R.image_count, sizeof *R.framebuffers);
    R.sem_render = calloc(R.image_count, sizeof *R.sem_render);
    if (!R.images || !R.views || !R.framebuffers || !R.sem_render) return 0;
    vkGetSwapchainImagesKHR(R.dev, R.swapchain, &R.image_count, R.images);

    for (uint32_t i = 0; i < R.image_count; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = R.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = R.swap_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VKCHECK(vkCreateImageView(R.dev, &vi, NULL, &R.views[i]));

        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = R.pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &R.views[i];
        fi.width = R.extent.width;
        fi.height = R.extent.height;
        fi.layers = 1;
        VKCHECK(vkCreateFramebuffer(R.dev, &fi, NULL, &R.framebuffers[i]));

        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VKCHECK(vkCreateSemaphore(R.dev, &si, NULL, &R.sem_render[i]));
    }
    return 1;
}

static void destroy_swapchain(void) {
    for (uint32_t i = 0; i < R.image_count; i++) {
        if (R.framebuffers && R.framebuffers[i])
            vkDestroyFramebuffer(R.dev, R.framebuffers[i], NULL);
        if (R.views && R.views[i]) vkDestroyImageView(R.dev, R.views[i], NULL);
        if (R.sem_render && R.sem_render[i])
            vkDestroySemaphore(R.dev, R.sem_render[i], NULL);
    }
    free(R.images); free(R.views); free(R.framebuffers); free(R.sem_render);
    R.images = NULL; R.views = NULL; R.framebuffers = NULL; R.sem_render = NULL;
    R.image_count = 0;
    if (R.swapchain) vkDestroySwapchainKHR(R.dev, R.swapchain, NULL);
    R.swapchain = VK_NULL_HANDLE;
}

static int recreate_swapchain(void) {
    vkDeviceWaitIdle(R.dev);
    destroy_swapchain();
    int ok = init_swapchain() == 1;
#ifdef AC3_ENHANCED
    if (ok) hw_ui_set_image_count(R.min_images);
#endif
    return ok;
}

static int init_pass(void) {
    VkAttachmentDescription at = { 0 };
    at.format = R.swap_format;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep = { 0 };
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ri.attachmentCount = 1;
    ri.pAttachments = &at;
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = 1;
    ri.pDependencies = &dep;
    VKCHECK(vkCreateRenderPass(R.dev, &ri, NULL, &R.pass));
    return 1;
}

static int init_vram(void) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16_UINT;
    ii.extent.width = GPU_VRAM_W;
    ii.extent.height = GPU_VRAM_H;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(R.dev, &ii, NULL, &R.vram_img));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(R.dev, R.vram_img, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(R.dev, &ai, NULL, &R.vram_mem));
    VKCHECK(vkBindImageMemory(R.dev, R.vram_img, R.vram_mem, 0));

    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = R.vram_img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16_UINT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VKCHECK(vkCreateImageView(R.dev, &vi, NULL, &R.vram_view));

    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VKCHECK(vkCreateSampler(R.dev, &si, NULL, &R.vram_sampler));

    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (!make_buffer(VRAM_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &R.vram_staging[i], &R.vram_staging_mem[i]))
            return 0;
        VKCHECK(vkMapMemory(R.dev, R.vram_staging_mem[i], 0, VRAM_BYTES, 0,
                            &R.vram_staging_map[i]));
    }
    return 1;
}

static int init_descriptors(void) {
    VkDescriptorSetLayoutBinding b = { 0 };
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo li =
        { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings = &b;
    VKCHECK(vkCreateDescriptorSetLayout(R.dev, &li, NULL, &R.set_layout));

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VKCHECK(vkCreateDescriptorPool(R.dev, &pi, NULL, &R.desc_pool));

    VkDescriptorSetAllocateInfo dai =
        { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = R.desc_pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &R.set_layout;
    VKCHECK(vkAllocateDescriptorSets(R.dev, &dai, &R.desc_set));

    VkDescriptorImageInfo dii = { 0 };
    dii.sampler = R.vram_sampler;
    dii.imageView = R.vram_view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = R.desc_set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(R.dev, 1, &w, 0, NULL);
    return 1;
}

static int init_pipeline(void) {
    VkShaderModule vs = make_shader(spv_psx_vert, sizeof spv_psx_vert);
    VkShaderModule fs = make_shader(spv_psx_frag, sizeof spv_psx_frag);
    if (!vs || !fs) return 0;

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(display_push_t) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &R.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(R.dev, &pli, NULL, &R.layout));

    VkPipelineVertexInputStateCreateInfo vin =
        { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo ia =
        { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp =
        { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs =
        { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms =
        { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cb = { 0 };
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs =
        { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds =
        { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &ds;
    gp.layout = R.layout;
    gp.renderPass = R.pass;
    VKCHECK(vkCreateGraphicsPipelines(R.dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                      &R.pipeline));
    vkDestroyShaderModule(R.dev, vs, NULL);
    vkDestroyShaderModule(R.dev, fs, NULL);
    return 1;
}

static int init_frames(void) {
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = R.qfamily;
    VKCHECK(vkCreateCommandPool(R.dev, &cpi, NULL, &R.cmd_pool));

    VkCommandBufferAllocateInfo cbi =
        { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = R.cmd_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = FRAMES_IN_FLIGHT;
    VKCHECK(vkAllocateCommandBuffers(R.dev, &cbi, R.cmd));

    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VKCHECK(vkCreateSemaphore(R.dev, &si, NULL, &R.sem_acquire[i]));
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHECK(vkCreateFence(R.dev, &fi, NULL, &R.fence[i]));
    }
    return 1;
}

static void vk_init(void) {
    if (!init_instance() || !init_device() || !pick_surface_format() ||
        !init_pass() || init_swapchain() == 0 || !init_vram() ||
        !init_descriptors() || !init_pipeline() || !init_frames()) {
        fprintf(stderr, "ac3: renderer init failed; continuing headless\n");
        R.ready = 0;
        return;
    }
#ifdef AC3_ENHANCED
    {
        hw_config_t cfg = { ac3_hd.scale, ac3_hd.perspective };
        if (hw_init(R.phys, R.dev, R.qfamily, &cfg) && hw_display_init(R.pass))
            hw_glue_attach();
        else
            fprintf(stderr, "ac3: enhanced renderer unavailable; native picture\n");
        hw_ui_vk_t uv = { R.window, VK_API_VERSION_1_1, R.instance, R.phys, R.dev, R.qfamily,
                          R.queue, R.pass, R.min_images, R.image_count };
        if (!hw_ui_init(&uv)) fprintf(stderr, "ac3: the settings menu could not be set up\n");
        if (ac3_ui.fullscreen) SDL_SetWindowFullscreen(R.window, true);
        ac3_audio_set_volume(ac3_ui.volume);
    }
#endif
    R.ready = 1;
    ac3_input_open();
    printf("renderer: %ux%u window, native VRAM scan-out\n",
           R.extent.width, R.extent.height);
}

static void vk_shutdown(void) {
    if (!R.ready) return;
    vkDeviceWaitIdle(R.dev);
    R.ready = 0;
}

static void pump_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) R.quit = 1;
#ifdef AC3_ENHANCED
        if (hw_ui_event(&ev)) continue;
#endif
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)
            R.quit = 1;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F9 &&
            !ev.key.repeat) {
            ac3_capture_request();
#ifdef AC3_ENHANCED
            hw_rec_arm();
#endif
        }
#ifdef AC3_ENHANCED
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F11 &&
            !ev.key.repeat)
            SDL_SetWindowFullscreen(R.window,
                !(SDL_GetWindowFlags(R.window) & SDL_WINDOW_FULLSCREEN));
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F7 &&
            !ev.key.repeat)
            hw_glue_toggle_fx();
#endif
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F10 &&
            !ev.key.repeat)
            ac3_capture_on_video();
        ac3_input_event(&ev);
    }
#ifdef AC3_ENHANCED
    ac3_input_suppress(hw_ui_open());
#endif
    ac3_input_poll();
}

static Uint64 field_deadline_ns;

static void pace_field(void) {
    static int disabled = -1;
    if (disabled < 0) {
        const char *s = SDL_getenv("AC3_NO_FRAME_LIMIT");
        disabled = (s && *s && *s != '0');
    }
#ifdef AC3_ENHANCED
    if (!ac3_ui.frame_limit && !R.paused) {
        field_deadline_ns = 0;
        return;
    }
#endif
    if (disabled) return;

    const Uint64 period = 1000000000ull * (Uint64)SPU_FIELD_NUM /
                          ((Uint64)SPU_FIELD_DEN * (Uint64)SPU_SAMPLE_RATE);

    Uint64 now = SDL_GetTicksNS();
    if (!field_deadline_ns) { field_deadline_ns = now + period; return; }

    if (now < field_deadline_ns) SDL_DelayPrecise(field_deadline_ns - now);
    field_deadline_ns += period;

    now = SDL_GetTicksNS();
    if (field_deadline_ns + 4 * period < now) field_deadline_ns = now + period;
}

#ifdef AC3_ENHANCED
static int frames_without_3d = 1000;
static int display_wide(int rgb24) {
    return hw_glue_widescreen_active() && !rgb24 && frames_without_3d <= 8;
}
#endif

static VkViewport display_viewport(float num, float den) {
    float fw = (float)R.extent.width, fh = (float)R.extent.height;
    float w = fw, h = fh;
    if (fw * den > fh * num) w = SDL_floorf(fh * num / den);
    else                     h = SDL_floorf(fw * den / num);
    VkViewport v = { SDL_floorf((fw - w) * 0.5f), SDL_floorf((fh - h) * 0.5f),
                     w, h, 0.0f, 1.0f };
    return v;
}

#ifdef AC3_ENHANCED
static int hw_recorded;

static void hw_offscreen_flush(void) {
    uint32_t f = R.frame % FRAMES_IN_FLIGHT;
    vkWaitForFences(R.dev, 1, &R.fence[f], VK_TRUE, UINT64_MAX);
    vkResetFences(R.dev, 1, &R.fence[f]);
    VkCommandBuffer cb = R.cmd[f];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    hw_record(cb, (int)f);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(R.queue, 1, &si, R.fence[f]);
    R.frame++;
}
#endif

static uint32_t present_field(void) {
    if (R.want_recreate) {
        R.want_recreate = 0;
        recreate_swapchain();
    }
    if (!R.swapchain && !recreate_swapchain()) return UINT32_MAX;
#ifdef AC3_ENHANCED
    hw_ui_frame();
#endif

    uint32_t f = R.frame % FRAMES_IN_FLIGHT;
    vkWaitForFences(R.dev, 1, &R.fence[f], VK_TRUE, UINT64_MAX);

    uint32_t image = 0;
    VkResult acq = vkAcquireNextImageKHR(R.dev, R.swapchain, UINT64_MAX,
                                         R.sem_acquire[f], VK_NULL_HANDLE, &image);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return UINT32_MAX; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return UINT32_MAX;

    vkResetFences(R.dev, 1, &R.fence[f]);

    gpu_display_t d;
    gpu_display_state(&d);
    if (ac3_vram) memcpy(R.vram_staging_map[f], ac3_vram, VRAM_BYTES);

    VkCommandBuffer cb = R.cmd[f];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    {
        VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = R.vram_img;
        to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_dst.subresourceRange.levelCount = 1;
        to_dst.subresourceRange.layerCount = 1;
        to_dst.srcAccessMask = 0;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 0, NULL, 1, &to_dst);

        VkBufferImageCopy region = { 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = GPU_VRAM_W;
        region.imageExtent.height = GPU_VRAM_H;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, R.vram_staging[f], R.vram_img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier to_read = to_dst;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 1, &to_read);
    }

    VkClearValue black = { { { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = R.pass;
    rp.framebuffer = R.framebuffers[image];
    rp.renderArea.extent = R.extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &black;
#ifdef AC3_ENHANCED
    int hd = 0;
    if (gpu_sw_hooks) {
        hw_record(cb, (int)f);
        hw_recorded = 1;
        if (!R.paused) {
            if (hw_glue_widescreen_active() && ac3_pgxp_projections >= 32)
                frames_without_3d = 0;
            else if (frames_without_3d < 100000) frames_without_3d++;
        }
        hd = !d.rgb24;
    }
    int wide = display_wide(d.rgb24);
    float anum = wide ? 16.0f : 4.0f, aden = wide ? 9.0f : 3.0f;
#else
    float anum = 4.0f, aden = 3.0f;
#endif
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

    if (d.enabled && d.w > 0 && d.h > 0) {
        VkViewport vpt = display_viewport(anum, aden);
#ifdef AC3_ENHANCED
        if (hd) {
            hw_display_draw(cb, &d, &vpt);
            goto displayed;
        }
#endif
        VkRect2D sc = { { (int32_t)vpt.x, (int32_t)vpt.y },
                        { (uint32_t)vpt.width, (uint32_t)vpt.height } };
        vkCmdSetViewport(cb, 0, 1, &vpt);
        vkCmdSetScissor(cb, 0, 1, &sc);
        display_push_t push = {
            { d.x, d.y }, { d.w, d.h },
            { vpt.x, vpt.y }, { vpt.width, vpt.height },
            d.rgb24 ? 1u : 0u,
        };
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout,
                                0, 1, &R.desc_set, 0, NULL);
        vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof push, &push);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
#ifdef AC3_ENHANCED
displayed:
    hw_ui_draw(cb);
#endif
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &R.sem_acquire[f];
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &R.sem_render[image];
    vkQueueSubmit(R.queue, 1, &si, R.fence[f]);

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &R.sem_render[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &R.swapchain;
    pi.pImageIndices = &image;
    VkResult pr = vkQueuePresentKHR(R.queue, &pi);
    R.frame++;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
        if (shot_pending) {
            vkQueueWaitIdle(R.queue);
            write_screenshot(image);
            shot_pending = 0;
        }
        recreate_swapchain();
        return UINT32_MAX;
    }
    return image;
}

#ifdef AC3_ENHANCED
static void count_fps(void) {
    Uint64 now = SDL_GetTicksNS();
    if (!R.fps_t0) R.fps_t0 = now;
    R.fps_n++;
    if (now - R.fps_t0 >= 500000000ull) {
        R.fps = (float)R.fps_n * 1.0e9f / (float)(now - R.fps_t0);
        R.fps_n = 0;
        R.fps_t0 = now;
    }
}

static int hw_start(int scale) {
    hw_config_t cfg = { scale, ac3_hd.perspective };
    if (!hw_init(R.phys, R.dev, R.qfamily, &cfg)) return 0;
    if (!hw_display_init(R.pass)) {
        hw_shutdown();
        return 0;
    }
    PSX_RECT all = { 0, 0, GPU_VRAM_W, GPU_VRAM_H };
    hw_upload(&all, ac3_vram);
    return 1;
}

static void apply_scale_change(void) {
    int s = R.pending_scale, old = hw_scale();
    R.pending_scale = 0;
    if (s == old) return;
    vkDeviceWaitIdle(R.dev);
    hw_shutdown();
    if (hw_start(s)) return;
    fprintf(stderr, "ac3: could not switch to %dx resolution\n", s);
    ac3_hd.scale = old;
    if (!hw_start(old)) {
        fprintf(stderr, "ac3: enhanced renderer lost; native picture\n");
        gpu_sw_hooks = NULL;
    }
}

void ac3_render_set_fullscreen(int on) {
    if (R.window) SDL_SetWindowFullscreen(R.window, on != 0);
}

void ac3_render_set_vsync(int on) {
    ac3_ui.vsync = on ? 1 : 0;
    R.want_recreate = 1;
}

void ac3_render_request_scale(int scale) {
    if (scale >= 1 && scale <= HW_MAX_SCALE) R.pending_scale = scale;
}

void ac3_render_request_restart(int english) {
    R.restart = 1;
    R.restart_english = english;
    R.quit = 1;
}

float ac3_render_fps(void) { return R.fps; }

int ac3_render_restart_pending(int *english) {
    if (english) *english = R.restart_english;
    return R.restart;
}

int ac3_render_relaunch(int argc, char **argv, int english) {
    if (R.dev) vkDeviceWaitIdle(R.dev);
    hw_ui_shutdown();
    SDL_Quit();
    const char **args = calloc((size_t)argc + 2, sizeof *args);
    if (!args) return 1;
    int n = 0;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--english") && strcmp(argv[i], "--japanese")) args[n++] = argv[i];
    args[n++] = english ? "--english" : "--japanese";
    args[n] = NULL;
    printf("\nrestarting in %s\n\n", english ? "English" : "Japanese");
    fflush(stdout);
    SDL_Process *p = SDL_CreateProcess(args, false);
    free(args);
    if (!p) {
        fprintf(stderr, "ac3: could not restart: %s\n", SDL_GetError());
        return 1;
    }
    int code = 0;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    return code;
}
#endif

static void vk_frame_begin(void) { }

static void vk_frame_end(void) {
    if (!R.ready) return;
    pump_events();
#ifdef AC3_ENHANCED
    hw_recorded = 0;
#endif

    gpu_display_t d;
    gpu_display_state(&d);
    uint32_t prims = gpu_sw_prim_count();
    if (shot_min_prims && d.enabled &&
        prims - R.prims_at_last_frame >= shot_min_prims) {
        shot_pending = 1;
        shot_min_prims = 0;
    }

    uint32_t image = present_field();
#ifdef AC3_ENHANCED
    if (!hw_recorded && gpu_sw_hooks) hw_offscreen_flush();
#endif
    if (image != UINT32_MAX && shot_pending) {
        printf("shot frame: %u primitives, display %s %d,%d %dx%d%s\n",
               prims - R.prims_at_last_frame, d.enabled ? "on" : "off",
               d.x, d.y, d.w, d.h, d.rgb24 ? " 24bpp" : "");
        vkQueueWaitIdle(R.queue);
        write_screenshot(image);
        shot_pending = 0;
    }
    R.prims_at_last_frame = prims;
#ifdef AC3_ENHANCED
    hw_glue_frame_end(&d);
    count_fps();
    if (R.pending_scale) apply_scale_change();
    while (!R.quit && hw_ui_open() && ac3_ui.pause_on_menu) {
        R.paused = 1;
        pace_field();
        pump_events();
        hw_recorded = 0;
        present_field();
        if (!hw_recorded && gpu_sw_hooks) hw_offscreen_flush();
        if (R.pending_scale) apply_scale_change();
    }
    R.paused = 0;
#endif
    pace_field();
}

static void vk_set_draw_env(const PSX_DRAWENV *e) { (void)e; }
static void vk_set_disp_env(const PSX_DISPENV *e) { (void)e; }
static void vk_set_draw_area(s16 x0, s16 y0, s16 x1, s16 y1) {
    (void)x0; (void)y0; (void)x1; (void)y1;
}
static void vk_set_draw_offset(s16 x, s16 y) { (void)x; (void)y; }
static void vk_set_draw_mode(u16 t, u16 m, u16 o) { (void)t; (void)m; (void)o; }
static void vk_set_mask(int set_bit, int check_bit) { (void)set_bit; (void)check_bit; }
static void vk_poly(const gpu_poly_t *p) { (void)p; }
static void vk_rect(const gpu_rect_t *r) { (void)r; }
static void vk_line(const gpu_line_t *l) { (void)l; }
static void vk_load_image(const PSX_RECT *r, const void *d) { (void)r; (void)d; }
static void vk_store_image(const PSX_RECT *r, void *d) { (void)r; (void)d; }
static void vk_move_image(const PSX_RECT *r, s16 x, s16 y) {
    (void)r; (void)x; (void)y;
}
static void vk_fill_rect(const PSX_RECT *r, u8 a, u8 b, u8 c) {
    (void)r; (void)a; (void)b; (void)c;
}

const gpu_backend_t gpu_backend_vulkan = {
    "vulkan",
    vk_init, vk_shutdown,
    vk_frame_begin, vk_frame_end,
    vk_set_draw_env, vk_set_disp_env,
    vk_set_draw_area, vk_set_draw_offset, vk_set_draw_mode, vk_set_mask,
    vk_poly, vk_rect, vk_line,
    vk_load_image, vk_store_image, vk_move_image, vk_fill_rect,
};
