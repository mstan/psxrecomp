#ifndef PSX_GPU_SW_RENDERER_H
#define PSX_GPU_SW_RENDERER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize software renderer */
void sw_renderer_init(uint16_t* vram);

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
void sw_draw_flat_rect(int x, int y, int w, int h, uint16_t color);
void sw_draw_textured_rect(int x, int y, int w, int h,
                           int u, int v,
                           uint16_t clut_x, uint16_t clut_y,
                           uint16_t texpage);
void sw_draw_line(int x0, int y0, int x1, int y1, uint16_t color);

/* Copy display region to a 32-bit RGBA buffer for SDL presentation.
   Returns number of pixels written. */
int sw_render_display(uint32_t* out_pixels, int out_pitch,
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

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_SW_RENDERER_H */
