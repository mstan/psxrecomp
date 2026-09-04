#ifndef PSX_GPU_RENDER_H
#define PSX_GPU_RENDER_H

/* Renderer facade.  gpu.c (command processing) and main.cpp (present) call
 * these gr_* entry points instead of a specific backend.  The facade
 * dispatches to a selected backend: the software rasterizer (gpu_sw_renderer.c,
 * the default + fallback) or a hardware OpenGL backend (gpu_gl_renderer.c).
 *
 * The gr_* signatures mirror the software renderer's interface exactly, so a
 * backend is just a table of the same functions.  Select the backend with
 * gr_set_backend() BEFORE gr_init(); gr_backend() reports the EFFECTIVE backend
 * (a requested backend that fails to initialize falls back to software). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GR_BACKEND_SOFTWARE = 0,
    GR_BACKEND_OPENGL   = 1,
    GR_BACKEND_VULKAN   = 2
} GrBackend;

void      gr_set_backend(GrBackend backend);  /* call before gr_init() */
GrBackend gr_backend(void);                   /* effective backend after init */

/* Lifecycle / global state */
void gr_init(uint16_t *vram);
void gr_set_scale(int scale);
int  gr_scale(void);
void gr_set_texture_filter(int bilinear);
int  gr_texture_filter(void);

/* Per-primitive draw state */
void gr_set_semi_transparency(int enabled, int mode);
void gr_set_mask_bits(int set_bit, int check_bit);
void gr_set_texture_window(uint32_t raw);
void gr_set_color_modulation(int r, int g, int b, int raw_texture);

/* Sub-pixel vertex override for the NEXT triangle ([video] geometry_correction).
 * x/y are signed 16.16 screen coordinates carrying the projection fraction the
 * GTE discarded; the integer positions still passed to gr_draw_*_triangle
 * remain authoritative for anything that must stay faithful (dirty-rect
 * tracking, the software path's native VRAM write). enabled == 0 clears the
 * override. Backends that do not implement it ignore the call, so the integer
 * path is what they draw. The override is consumed by the next triangle. */
void gr_set_precise_triangle(int enabled,
                             int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2);
/* Perspective-correct UV weights for the NEXT triangle ([video]
 * perspective_texturing). q[i] is the normalized 1/z homogeneous weight at
 * vertex i. enabled == 0 restores the PS1's affine UV interpolation. */
void gr_set_perspective_triangle(int enabled, float q0, float q1, float q2);

/* Primitives */
void gr_fill_rect(int x, int y, int w, int h, uint16_t color);
void gr_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
void gr_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                           uint16_t color);
void gr_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                              int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2);
void gr_draw_textured_triangle(int x0, int y0, int u0, int v0,
                               int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage);
void gr_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0,
                                      uint32_t color0,
                                      int x1, int y1, int u1, int v1,
                                      uint32_t color1,
                                      int x2, int y2, int u2, int v2,
                                      uint32_t color2,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage, int raw_texture);
void gr_draw_flat_rect(int x, int y, int w, int h, uint16_t color);
void gr_draw_textured_rect(int x, int y, int w, int h,
                           int u, int v,
                           uint16_t clut_x, uint16_t clut_y,
                           uint16_t texpage);
void gr_draw_textured_rect_scaled(int x, int y, int w, int h,
                                  int u0, int v0, int u1, int v1,
                                  uint16_t clut_x, uint16_t clut_y,
                                  uint16_t texpage);
void gr_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void gr_draw_shaded_line(int x0, int y0, uint16_t c0,
                         int x1, int y1, uint16_t c1);

/* Display readout (present path) */
int gr_render_display(uint32_t *out_pixels, int out_pitch,
                      int disp_x, int disp_y, int disp_w, int disp_h);
int gr_render_display_hires(uint32_t *out_pixels, int out_pitch,
                            int disp_x, int disp_y, int disp_w, int disp_h);

/* VRAM transfers */
void gr_vram_write(int x, int y, uint16_t pixel);
uint16_t gr_vram_read(int x, int y);
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data);
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *data);

/* Draw area / offset */
void gr_set_draw_area(int x1, int y1, int x2, int y2);
void gr_get_draw_area(int *x1, int *y1, int *x2, int *y2);
void gr_set_draw_offset(int x, int y);

/* Native-wide compositor. gr_wide_supported() reports whether the active
 * backend implements it; if not, the others are no-ops and the caller keeps
 * the canonical present path. */
int  gr_wide_supported(void);
void gr_wide_configure(int wide_w, int offset);
void gr_wide_set_target(int base_x);
void gr_wide_disable_target(void);
void gr_wide_clear(int base_x, int y, int h, uint16_t color);
/* sides: bit 0 = left synthetic margin, bit 1 = right. */
void gr_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides);
int  gr_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh, int base_x);
int  gr_render_wide_display(uint32_t *out, int pitch, int base_x,
                            int disp_y, int disp_h);

/* ---- Backend vtable -----------------------------------------------------
 * A backend supplies the same set of functions.  gpu_gl_renderer.c (Phase 2)
 * provides gl_backend_get(); until it does, requesting OpenGL falls back to
 * software.  Members mirror the gr_* / sw_* signatures 1:1. */
typedef struct GpuRenderBackend {
    const char *name;
    void (*init)(uint16_t *vram);
    void (*set_scale)(int scale);
    int  (*scale)(void);
    void (*set_texture_filter)(int bilinear);
    int  (*texture_filter)(void);
    void (*set_semi_transparency)(int enabled, int mode);
    void (*set_mask_bits)(int set_bit, int check_bit);
    void (*set_texture_window)(uint32_t raw);
    void (*set_color_modulation)(int r, int g, int b, int raw_texture);
    /* Optional (NULL = unsupported, facade no-ops): sub-pixel vertex override
     * and perspective UV weights for the next triangle. See gr_* above. */
    void (*set_precise_triangle)(int enabled,
                                 int32_t x0, int32_t y0,
                                 int32_t x1, int32_t y1,
                                 int32_t x2, int32_t y2);
    void (*set_perspective_triangle)(int enabled, float q0, float q1, float q2);
    void (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    void (*copy_rect)(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
    void (*draw_flat_triangle)(int x0, int y0, int x1, int y1, int x2, int y2,
                               uint16_t color);
    void (*draw_gouraud_triangle)(int x0, int y0, uint16_t c0,
                                  int x1, int y1, uint16_t c1,
                                  int x2, int y2, uint16_t c2);
    void (*draw_textured_triangle)(int x0, int y0, int u0, int v0,
                                   int x1, int y1, int u1, int v1,
                                   int x2, int y2, int u2, int v2,
                                   uint16_t clut_x, uint16_t clut_y,
                                   uint16_t texpage);
    void (*draw_shaded_textured_triangle)(int x0, int y0, int u0, int v0,
                                          uint32_t color0,
                                          int x1, int y1, int u1, int v1,
                                          uint32_t color1,
                                          int x2, int y2, int u2, int v2,
                                          uint32_t color2,
                                          uint16_t clut_x, uint16_t clut_y,
                                          uint16_t texpage, int raw_texture);
    void (*draw_flat_rect)(int x, int y, int w, int h, uint16_t color);
    void (*draw_textured_rect)(int x, int y, int w, int h, int u, int v,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage);
    void (*draw_textured_rect_scaled)(int x, int y, int w, int h,
                                      int u0, int v0, int u1, int v1,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage);
    void (*draw_line)(int x0, int y0, int x1, int y1, uint16_t color);
    void (*draw_shaded_line)(int x0, int y0, uint16_t c0,
                             int x1, int y1, uint16_t c1);
    int  (*render_display)(uint32_t *out, int pitch,
                           int dx, int dy, int dw, int dh);
    int  (*render_display_hires)(uint32_t *out, int pitch,
                                 int dx, int dy, int dw, int dh);
    void (*vram_write)(int x, int y, uint16_t pixel);
    uint16_t (*vram_read)(int x, int y);
    void (*vram_transfer_in)(int x, int y, int w, int h, const uint16_t *data);
    void (*vram_transfer_out)(int x, int y, int w, int h, uint16_t *data);
    void (*set_draw_area)(int x1, int y1, int x2, int y2);
    void (*get_draw_area)(int *x1, int *y1, int *x2, int *y2);
    void (*set_draw_offset)(int x, int y);
    /* Native-wide compositor (optional; NULL on backends without it — the
     * facade then reports gr_wide_supported() == 0 and the caller keeps the
     * canonical present). */
    void (*wide_configure)(int wide_w, int offset);
    void (*wide_set_target)(int base_x);
    void (*wide_disable_target)(void);
    void (*wide_clear)(int base_x, int y, int h, uint16_t color);
    void (*wide_clear_margins)(int base_x, int y, int h, uint16_t color, int sides);
    int  (*render_wide_display)(uint32_t *out, int pitch, int base_x,
                                int disp_y, int disp_h);
    /* Dump the ENTIRE wide compositor surface for base_x (all double-buffer
     * bands + both margins), g_wide_w x VRAM_H. Debug/inspection only; returns
     * pixel count, writes width/height to ow/oh. NULL if unsupported. */
    int  (*wide_dump_full)(uint32_t *out, int cap_pixels, int *ow, int *oh,
                           int base_x);
} GpuRenderBackend;

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_RENDER_H */
