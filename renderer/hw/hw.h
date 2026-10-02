#ifndef AC3_HW_H
#define AC3_HW_H

#include "gpu_sw.h"
#include "pgxp.h"
#include "hw_world.h"
#include "hw_settings.h"
#include <vulkan/vulkan.h>

#define HW_FRAMES 2

typedef struct {
    int scale;
    int perspective;
} hw_config_t;

typedef struct {
    int ok;
    ac3_pgxp_vtx_t v;
} hw_pv_t;

int  hw_init(VkPhysicalDevice phys, VkDevice dev, uint32_t qfamily,
             const hw_config_t *cfg);
void hw_shutdown(void);
int  hw_scale(void);
void hw_set_perspective(int on);
void hw_set_pgxp(int on);

void hw_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t pv[4]);
void hw_rect(const gpu_sw_state_t *st, const gpu_rect_t *r);
void hw_line(const gpu_sw_state_t *st, const gpu_line_t *l);
void hw_fill(const PSX_RECT *r, u16 pixel);
void hw_copy(const gpu_sw_state_t *st, const PSX_RECT *src, s32 dx, s32 dy);
void hw_upload(const PSX_RECT *r, const u16 *vram);

void hw_record(VkCommandBuffer cb, int slot);

void hw_frame_stats(u32 *prims, u32 *precise);

int  hw_display_init(VkRenderPass pass);
void hw_display_draw(VkCommandBuffer cb, const gpu_display_t *d,
                     const VkViewport *vp);

int  hw_readback(VkQueue q, VkCommandPool pool, int x, int y, int w, int h,
                 u8 *rgba);
int  hw_readback_depth(VkQueue q, VkCommandPool pool, int x, int y, int w, int h,
                       float *z);

extern u32 hw_fx_flags;
#define FX_FAR    1u
#define FX_OCEAN  2u
#define FX_CLOUDS 4u
#define FX_FOREST 8u
#define FX_ATMO   16u
#define FX_GI     32u
#define FX_SUN    64u
void hw_fx_begin(const hw_scene_t *s);
void hw_fx_end(const hw_scene_t *s);
void hw_fx_palette(s32 dx, s32 dy, s32 w, s32 h, s32 sx, s32 sy);
int  hw_fx_ocean_drawn(void);
int  hw_fx_ready(void);
void hw_effect_depth_test(int on);
void hw_draw_order(u32 order);

#endif
