#include "gpu_render.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int stub_scale = 1;
static int stub_texture_filter = 0;
static int ng_available = 0;

static void stub_init(uint16_t* vram) { (void)vram; }
static void stub_set_scale(int scale) { stub_scale = scale; }
static int stub_get_scale(void) { return stub_scale; }
static void stub_set_texture_filter(int bilinear) { stub_texture_filter = bilinear; }
static int stub_get_texture_filter(void) { return stub_texture_filter; }
static void stub_set_semi_transparency(int enabled, int mode) { (void)enabled; (void)mode; }
static void stub_set_mask_bits(int set_bit, int check_bit) { (void)set_bit; (void)check_bit; }
static void stub_set_texture_window(uint32_t raw) { (void)raw; }
static void stub_set_color_modulation(int r, int g, int b, int raw_texture) { (void)r; (void)g; (void)b; (void)raw_texture; }
static void stub_set_precise_triangle(int enabled, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2) { (void)enabled; (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; }
static void stub_set_perspective_triangle(int enabled, float q0, float q1, float q2) { (void)enabled; (void)q0; (void)q1; (void)q2; }
static void stub_fill_rect(int x, int y, int w, int h, uint16_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
static void stub_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) { (void)src_x; (void)src_y; (void)dst_x; (void)dst_y; (void)w; (void)h; }
static void stub_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) { (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)color; }
static void stub_draw_gouraud_triangle(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1, int x2, int y2, uint16_t c2) { (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1; (void)x2; (void)y2; (void)c2; }
static void stub_draw_textured_triangle(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1, int x2, int y2, int u2, int v2, uint16_t clut_x, uint16_t clut_y, uint16_t texpage) { (void)x0; (void)y0; (void)u0; (void)v0; (void)x1; (void)y1; (void)u1; (void)v1; (void)x2; (void)y2; (void)u2; (void)v2; (void)clut_x; (void)clut_y; (void)texpage; }
static void stub_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0, uint32_t color0, int x1, int y1, int u1, int v1, uint32_t color1, int x2, int y2, int u2, int v2, uint32_t color2, uint16_t clut_x, uint16_t clut_y, uint16_t texpage, int raw_texture) { (void)x0; (void)y0; (void)u0; (void)v0; (void)color0; (void)x1; (void)y1; (void)u1; (void)v1; (void)color1; (void)x2; (void)y2; (void)u2; (void)v2; (void)color2; (void)clut_x; (void)clut_y; (void)texpage; (void)raw_texture; }
static void stub_draw_flat_rect(int x, int y, int w, int h, uint16_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
static void stub_draw_textured_rect(int x, int y, int w, int h, int u, int v, uint16_t clut_x, uint16_t clut_y, uint16_t texpage) { (void)x; (void)y; (void)w; (void)h; (void)u; (void)v; (void)clut_x; (void)clut_y; (void)texpage; }
static void stub_draw_textured_rect_scaled(int x, int y, int w, int h, int u0, int v0, int u1, int v1, uint16_t clut_x, uint16_t clut_y, uint16_t texpage) { (void)x; (void)y; (void)w; (void)h; (void)u0; (void)v0; (void)u1; (void)v1; (void)clut_x; (void)clut_y; (void)texpage; }
static void stub_draw_line(int x0, int y0, int x1, int y1, uint16_t color) { (void)x0; (void)y0; (void)x1; (void)y1; (void)color; }
static void stub_draw_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1) { (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1; }
static int stub_render_display(uint32_t* out, int pitch, int dx, int dy, int dw, int dh) { (void)out; (void)pitch; (void)dx; (void)dy; (void)dw; (void)dh; return 0; }
static int stub_render_display_hires(uint32_t* out, int pitch, int dx, int dy, int dw, int dh) { (void)out; (void)pitch; (void)dx; (void)dy; (void)dw; (void)dh; return 0; }
static void stub_vram_write(int x, int y, uint16_t pixel) { (void)x; (void)y; (void)pixel; }
static uint16_t stub_vram_read(int x, int y) { (void)x; (void)y; return 0; }
static void stub_vram_transfer_in(int x, int y, int w, int h, const uint16_t* data) { (void)x; (void)y; (void)w; (void)h; (void)data; }
static void stub_vram_transfer_out(int x, int y, int w, int h, uint16_t* data) { (void)x; (void)y; (void)w; (void)h; (void)data; }
static void stub_set_draw_area(int x1, int y1, int x2, int y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
static void stub_get_draw_area(int* x1, int* y1, int* x2, int* y2) { if (x1) *x1 = 0; if (y1) *y1 = 0; if (x2) *x2 = 0; if (y2) *y2 = 0; }
static void stub_set_draw_offset(int x, int y) { (void)x; (void)y; }
static void stub_wide_configure(int wide_w, int offset) { (void)wide_w; (void)offset; }
static void stub_wide_set_target(int base_x) { (void)base_x; }
static void stub_wide_disable_target(void) {}
static void stub_wide_clear(int base_x, int y, int h, uint16_t color) { (void)base_x; (void)y; (void)h; (void)color; }
static void stub_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) { (void)base_x; (void)y; (void)h; (void)color; (void)sides; }
static int stub_render_wide_display(uint32_t* out, int pitch, int base_x, int disp_y, int disp_h) { (void)out; (void)pitch; (void)base_x; (void)disp_y; (void)disp_h; return 0; }
static int stub_wide_dump_full(uint32_t* out, int cap_pixels, int* ow, int* oh, int base_x) { (void)out; (void)cap_pixels; (void)base_x; if (ow) *ow = 0; if (oh) *oh = 0; return 0; }

#define SW_ALIAS(name) void sw_##name
void sw_renderer_init(uint16_t* vram) { stub_init(vram); }
void sw_renderer_set_scale(int scale) { stub_set_scale(scale); }
int sw_renderer_scale(void) { return stub_get_scale(); }
void sw_set_texture_filter(int bilinear) { stub_set_texture_filter(bilinear); }
int sw_texture_filter(void) { return stub_get_texture_filter(); }
void sw_set_semi_transparency(int enabled, int mode) { stub_set_semi_transparency(enabled, mode); }
void sw_set_mask_bits(int set_bit, int check_bit) { stub_set_mask_bits(set_bit, check_bit); }
void sw_set_texture_window(uint32_t raw) { stub_set_texture_window(raw); }
void sw_set_color_modulation(int r, int g, int b, int raw_texture) { stub_set_color_modulation(r, g, b, raw_texture); }
void sw_set_precise_triangle(int enabled, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2) { stub_set_precise_triangle(enabled, x0, y0, x1, y1, x2, y2); }
void sw_set_perspective_triangle(int enabled, float q0, float q1, float q2) { stub_set_perspective_triangle(enabled, q0, q1, q2); }
void sw_fill_rect(int x, int y, int w, int h, uint16_t color) { stub_fill_rect(x, y, w, h, color); }
void sw_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) { stub_copy_rect(src_x, src_y, dst_x, dst_y, w, h); }
void sw_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) { stub_draw_flat_triangle(x0, y0, x1, y1, x2, y2, color); }
void sw_draw_gouraud_triangle(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1, int x2, int y2, uint16_t c2) { stub_draw_gouraud_triangle(x0, y0, c0, x1, y1, c1, x2, y2, c2); }
void sw_draw_textured_triangle(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1, int x2, int y2, int u2, int v2, uint16_t clut_x, uint16_t clut_y, uint16_t texpage) { stub_draw_textured_triangle(x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2, clut_x, clut_y, texpage); }
void sw_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0, uint32_t color0, int x1, int y1, int u1, int v1, uint32_t color1, int x2, int y2, int u2, int v2, uint32_t color2, uint16_t clut_x, uint16_t clut_y, uint16_t texpage, int raw_texture) { stub_draw_shaded_textured_triangle(x0, y0, u0, v0, color0, x1, y1, u1, v1, color1, x2, y2, u2, v2, color2, clut_x, clut_y, texpage, raw_texture); }
void sw_draw_flat_rect(int x, int y, int w, int h, uint16_t color) { stub_draw_flat_rect(x, y, w, h, color); }
void sw_draw_textured_rect(int x, int y, int w, int h, int u, int v, uint16_t clut_x, uint16_t clut_y, uint16_t texpage) { stub_draw_textured_rect(x, y, w, h, u, v, clut_x, clut_y, texpage); }
void sw_draw_textured_rect_scaled(int x, int y, int w, int h, int u0, int v0, int u1, int v1, uint16_t clut_x, uint16_t clut_y, uint16_t texpage) { stub_draw_textured_rect_scaled(x, y, w, h, u0, v0, u1, v1, clut_x, clut_y, texpage); }
void sw_draw_line(int x0, int y0, int x1, int y1, uint16_t color) { stub_draw_line(x0, y0, x1, y1, color); }
void sw_draw_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1) { stub_draw_shaded_line(x0, y0, c0, x1, y1, c1); }
int sw_render_display(uint32_t* out, int pitch, int dx, int dy, int dw, int dh) { return stub_render_display(out, pitch, dx, dy, dw, dh); }
int sw_render_display_hires(uint32_t* out, int pitch, int dx, int dy, int dw, int dh) { return stub_render_display_hires(out, pitch, dx, dy, dw, dh); }
void sw_vram_write(int x, int y, uint16_t pixel) { stub_vram_write(x, y, pixel); }
uint16_t sw_vram_read(int x, int y) { return stub_vram_read(x, y); }
void sw_vram_transfer_in(int x, int y, int w, int h, const uint16_t* data) { stub_vram_transfer_in(x, y, w, h, data); }
void sw_vram_transfer_out(int x, int y, int w, int h, uint16_t* data) { stub_vram_transfer_out(x, y, w, h, data); }
void sw_set_draw_area(int x1, int y1, int x2, int y2) { stub_set_draw_area(x1, y1, x2, y2); }
void sw_get_draw_area(int* x1, int* y1, int* x2, int* y2) { stub_get_draw_area(x1, y1, x2, y2); }
void sw_set_draw_offset(int x, int y) { stub_set_draw_offset(x, y); }
void sw_wide_configure(int wide_w, int offset) { stub_wide_configure(wide_w, offset); }
void sw_wide_set_target(int base_x) { stub_wide_set_target(base_x); }
void sw_wide_disable_target(void) { stub_wide_disable_target(); }
void sw_wide_clear(int base_x, int y, int h, uint16_t color) { stub_wide_clear(base_x, y, h, color); }
void sw_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) { stub_wide_clear_margins(base_x, y, h, color, sides); }
int sw_render_wide_display(uint32_t* out, int pitch, int base_x, int disp_y, int disp_h) { return stub_render_wide_display(out, pitch, base_x, disp_y, disp_h); }
int sw_wide_dump_full(uint32_t* out, int cap_pixels, int* ow, int* oh, int base_x) { return stub_wide_dump_full(out, cap_pixels, ow, oh, base_x); }

static const GpuRenderBackend NG_BACKEND = {
    "ng-test",
    stub_init,
    stub_set_scale,
    stub_get_scale,
    stub_set_texture_filter,
    stub_get_texture_filter,
    stub_set_semi_transparency,
    stub_set_mask_bits,
    stub_set_texture_window,
    stub_set_color_modulation,
    stub_set_precise_triangle,
    stub_set_perspective_triangle,
    stub_fill_rect,
    stub_copy_rect,
    stub_draw_flat_triangle,
    stub_draw_gouraud_triangle,
    stub_draw_textured_triangle,
    stub_draw_shaded_textured_triangle,
    stub_draw_flat_rect,
    stub_draw_textured_rect,
    stub_draw_textured_rect_scaled,
    stub_draw_line,
    stub_draw_shaded_line,
    stub_render_display,
    stub_render_display_hires,
    stub_vram_write,
    stub_vram_read,
    stub_vram_transfer_in,
    stub_vram_transfer_out,
    stub_set_draw_area,
    stub_get_draw_area,
    stub_set_draw_offset,
    stub_wide_configure,
    stub_wide_set_target,
    stub_wide_disable_target,
    stub_wide_clear,
    stub_wide_clear_margins,
    stub_render_wide_display,
    stub_wide_dump_full,
};

const GpuRenderBackend* gl_backend_get(void) { return NULL; }
const GpuRenderBackend* vk_backend_get(void) { return NULL; }
const GpuRenderBackend* ng_backend_get(void) { return ng_available ? &NG_BACKEND : NULL; }

int main(void) {
    assert(GR_BACKEND_NOGRAPHICS == 3);
    assert(gr_backend() == GR_BACKEND_SOFTWARE);

    gr_set_backend(GR_BACKEND_NOGRAPHICS);
    assert(gr_backend() == GR_BACKEND_SOFTWARE);

    ng_available = 1;
    gr_set_backend(GR_BACKEND_NOGRAPHICS);
    assert(gr_backend() == GR_BACKEND_NOGRAPHICS);
    gr_set_scale(3);
    assert(gr_scale() == 3);
    gr_set_texture_filter(1);
    assert(gr_texture_filter() == 1);

    ng_available = 0;
    gr_set_backend(GR_BACKEND_NOGRAPHICS);
    assert(gr_backend() == GR_BACKEND_SOFTWARE);

    puts("PASS: NoGraphics renderer facade effective backend routing");
    return 0;
}
