#ifndef PSX_GPU_SW_RENDERER_H
#define PSX_GPU_SW_RENDERER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum internal-resolution supersampling factor (linear, per axis). */
#define SW_MAX_INTERNAL_SCALE 4

/* Initialize software renderer */
void sw_renderer_init(uint16_t* vram);

/* Internal-resolution supersampling (SSAA).
 * scale == 1 : disabled, renderer behaves exactly as native VRAM only.
 * scale  > 1 : maintain an S*-scaled mirror of VRAM; the display reads the
 *              mirror (sw_render_display_hires) and the present path
 *              downsamples to the window. Clamped to SW_MAX_INTERNAL_SCALE.
 * Must be called once after sw_renderer_init, before drawing begins. */
void sw_renderer_set_scale(int scale);
int  sw_renderer_scale(void);

/* Visual-only high-resolution vertex override for the next triangle. Values
 * are signed 16.16 screen coordinates; native VRAM still uses the integer GP0
 * positions, while the supersampled mirror consumes these fractions. */
void sw_set_precise_triangle(int enabled,
                             int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2);

/* Perspective-correct UV override for the next triangle. q values are the
 * homogeneous interpolation weights associated with each screen vertex. */
void sw_set_perspective_triangle(int enabled,
                                 float q0, float q1, float q2);
uint32_t sw_perspective_triangle_count(void);

/* Texture filtering. 0 = nearest (native PSX look, default), 1 = bilinear
 * (smooths textures/2D backgrounds; blends in RGB after the CLUT lookup,
 * transparency-aware). Independent of supersampling. */
void sw_set_texture_filter(int bilinear);
int  sw_texture_filter(void);

/* Draw state — must be set by gpu.c before each primitive */
void sw_set_semi_transparency(int enabled, int mode);
void sw_set_mask_bits(int set_bit, int check_bit);
void sw_set_texture_window(uint32_t raw);
void sw_set_color_modulation(int r, int g, int b, int raw_texture);

/* Drawing primitives — called from gpu.c command processing */
void sw_fill_rect(int x, int y, int w, int h, uint16_t color);
void sw_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
void sw_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                           uint16_t color);
void sw_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                              int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2);
void sw_draw_textured_triangle(int x0, int y0, int u0, int v0,
                               int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage);
void sw_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0,
                                      uint32_t color0,
                                      int x1, int y1, int u1, int v1,
                                      uint32_t color1,
                                      int x2, int y2, int u2, int v2,
                                      uint32_t color2,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage, int raw_texture);
void sw_draw_flat_rect(int x, int y, int w, int h, uint16_t color);
void sw_draw_textured_rect(int x, int y, int w, int h,
                           int u, int v,
                           uint16_t clut_x, uint16_t clut_y,
                           uint16_t texpage);
void sw_draw_textured_rect_scaled(int x, int y, int w, int h,
                                  int u0, int v0, int u1, int v1,
                                  uint16_t clut_x, uint16_t clut_y,
                                  uint16_t texpage);
void sw_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void sw_draw_shaded_line(int x0, int y0, uint16_t c0,
                         int x1, int y1, uint16_t c1);

/* Copy display region to a 32-bit RGBA buffer for SDL presentation.
   Returns number of pixels written. */
int sw_render_display(uint32_t* out_pixels, int out_pitch,
                      int disp_x, int disp_y, int disp_w, int disp_h);

/* Like sw_render_display, but reads the hi-res mirror and emits the region
   at (disp_w*scale x disp_h*scale). Falls back to sw_render_display when
   supersampling is disabled. Returns number of pixels written. */
int sw_render_display_hires(uint32_t* out_pixels, int out_pitch,
                            int disp_x, int disp_y, int disp_w, int disp_h);

/* VRAM transfer (CPU->VRAM, VRAM->CPU) */
void sw_vram_write(int x, int y, uint16_t pixel);
uint16_t sw_vram_read(int x, int y);
void sw_vram_transfer_in(int x, int y, int w, int h, const uint16_t* data);
void sw_vram_transfer_out(int x, int y, int w, int h, uint16_t* data);

/* Set draw area and draw offset */
void sw_set_draw_area(int x1, int y1, int x2, int y2);
void sw_get_draw_area(int *x1, int *y1, int *x2, int *y2);
void sw_set_draw_offset(int x, int y);

/* Native-wide compositor (see gpu_sw_renderer.c). Canonical VRAM stays faithful;
 * framebuffer draws are mirrored into independent wide surfaces keyed by buffer
 * base_x. wide_w<=0 disables. set_target selects/clears the mirror per back
 * buffer; render_wide_display is the present source for the displayed buffer. */
void sw_wide_configure(int wide_w, int offset);
void sw_wide_set_target(int base_x);
void sw_wide_disable_target(void);
void sw_wide_clear(int base_x, int y, int h, uint16_t color);
void sw_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides);
/* MMX6 native-wide reveal-column tile: rasterize a 16x16 textured tile into the
 * wide surface ONLY (canonical VRAM + hi-res mirror untouched). See definition. */
void sw_wide_emit_tile(int band_y, int x, int y, int u, int v,
                       uint16_t clut_x, uint16_t clut_y, uint16_t texpage,
                       uint32_t color24, int semi_trans, int semi_mode, int raw_texture);
int  sw_render_wide_display(uint32_t* out_pixels, int out_pitch, int base_x,
                            int disp_y, int disp_h);
int  sw_wide_dump_full(uint32_t* out, int cap_pixels, int* ow, int* oh, int base_x);
int  sw_wide_width(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_SW_RENDERER_H */
