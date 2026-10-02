#ifndef AC3_HW_UI_H
#define AC3_HW_UI_H

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SDL_Window *window;
    uint32_t api_version;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfamily;
    VkQueue queue;
    VkRenderPass pass;
    uint32_t min_images, images;
} hw_ui_vk_t;

int  hw_ui_init(const hw_ui_vk_t *vk);
void hw_ui_shutdown(void);
int  hw_ui_event(const SDL_Event *ev);
int  hw_ui_open(void);
void hw_ui_set_open(int open);
void hw_ui_set_image_count(uint32_t min_images);
void hw_ui_frame(void);
void hw_ui_draw(VkCommandBuffer cb);

void  ac3_render_set_fullscreen(int on);
void  ac3_render_set_vsync(int on);
void  ac3_render_request_scale(int scale);
void  ac3_render_request_restart(int english);
float ac3_render_fps(void);
void  ac3_audio_set_volume(float v);
void  hw_set_perspective(int on);
void  hw_set_pgxp(int on);
int   hw_scale(void);
void  hw_frame_stats(uint32_t *prims, uint32_t *precise);

int         ac3_lang_english(void);
const char *ac3_lang_patch_dir(void);
int         ac3_disc_english_active(void);
int         ac3_en_find_patch(int disc, const char *dir, char *out, size_t n);
void        ac3_settings_set(const char *key, const char *value);
int         ac3_settings_save(void);

#ifdef __cplusplus
}
#endif

#endif
