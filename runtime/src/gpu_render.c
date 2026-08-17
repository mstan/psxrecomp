/* gpu_render.c — renderer facade / backend dispatch.
 *
 * gpu.c and main.cpp call the gr_* functions; this file forwards each to the
 * selected backend's vtable.  The software rasterizer is the default and the
 * fallback.  The OpenGL backend (gpu_gl_renderer.c) is selected via
 * gr_set_backend(GR_BACKEND_OPENGL) and supplies its table through
 * gl_backend_get(); if that returns NULL (GL unavailable / init failed) we
 * stay on software so a misconfigured [video] renderer never bricks boot.
 *
 * The software backend table points straight at the existing sw_* functions,
 * so the software path is byte-for-byte unchanged. */

#include "gpu_render.h"
#include "gpu_sw_renderer.h"
#include "gpu_uv.h"
#include "tex_pack.h"
#include <stdio.h>

static const GpuRenderBackend SW_BACKEND = {
    .name                          = "software",
    .init                          = sw_renderer_init,
    .set_scale                     = sw_renderer_set_scale,
    .scale                         = sw_renderer_scale,
    .set_texture_filter            = sw_set_texture_filter,
    .texture_filter                = sw_texture_filter,
    .set_semi_transparency         = sw_set_semi_transparency,
    .set_mask_bits                 = sw_set_mask_bits,
    .set_texture_window            = sw_set_texture_window,
    .set_color_modulation          = sw_set_color_modulation,
    .set_precise_triangle          = sw_set_precise_triangle,
    .set_perspective_triangle      = sw_set_perspective_triangle,
    .fill_rect                     = sw_fill_rect,
    .copy_rect                     = sw_copy_rect,
    .draw_flat_triangle            = sw_draw_flat_triangle,
    .draw_gouraud_triangle         = sw_draw_gouraud_triangle,
    .draw_textured_triangle        = sw_draw_textured_triangle,
    .draw_shaded_textured_triangle = sw_draw_shaded_textured_triangle,
    .draw_flat_rect                = sw_draw_flat_rect,
    .draw_textured_rect            = sw_draw_textured_rect,
    .draw_textured_rect_scaled     = sw_draw_textured_rect_scaled,
    .draw_line                     = sw_draw_line,
    .draw_shaded_line              = sw_draw_shaded_line,
    .render_display                = sw_render_display,
    .render_display_hires          = sw_render_display_hires,
    .vram_write                    = sw_vram_write,
    .vram_read                     = sw_vram_read,
    .vram_transfer_in              = sw_vram_transfer_in,
    .vram_transfer_out             = sw_vram_transfer_out,
    .set_draw_area                 = sw_set_draw_area,
    .get_draw_area                 = sw_get_draw_area,
    .set_draw_offset               = sw_set_draw_offset,
    .wide_configure                = sw_wide_configure,
    .wide_set_target               = sw_wide_set_target,
    .wide_disable_target           = sw_wide_disable_target,
    .wide_clear                    = sw_wide_clear,
    .wide_clear_margins            = sw_wide_clear_margins,
    .render_wide_display           = sw_render_wide_display,
    .wide_dump_full                = sw_wide_dump_full,
};

/* Supplied by gpu_gl_renderer.c; returns NULL until the GL backend is ready. */
extern const GpuRenderBackend *gl_backend_get(void);
/* Supplied by gpu_vk_renderer.c; returns NULL until the Vulkan backend is
 * ready (init succeeded). When Vulkan isn't compiled in it returns NULL. */
extern const GpuRenderBackend *vk_backend_get(void);

static const GpuRenderBackend *g_b         = &SW_BACKEND;
static GrBackend               g_effective = GR_BACKEND_SOFTWARE;

void gr_set_backend(GrBackend backend) {
    if (backend == GR_BACKEND_OPENGL) {
        const GpuRenderBackend *gl = gl_backend_get();
        if (gl) {
            g_b = gl;
            g_effective = GR_BACKEND_OPENGL;
            fprintf(stdout, "psxrecomp: renderer = opengl (%s)\n", gl->name);
            return;
        }
        fprintf(stdout, "psxrecomp: renderer = opengl requested but unavailable "
                        "— falling back to software\n");
    } else if (backend == GR_BACKEND_VULKAN) {
        const GpuRenderBackend *vk = vk_backend_get();
        if (vk) {
            g_b = vk;
            g_effective = GR_BACKEND_VULKAN;
            fprintf(stdout, "psxrecomp: renderer = vulkan (%s)\n", vk->name);
            return;
        }
        fprintf(stdout, "psxrecomp: renderer = vulkan requested but unavailable "
                        "— falling back to software\n");
    }
    g_b = &SW_BACKEND;
    g_effective = GR_BACKEND_SOFTWARE;
}

GrBackend gr_backend(void) { return g_effective; }

/* ---- Dispatch wrappers (one line each; forward to the active backend) ---- */
void gr_init(uint16_t *vram)                         { g_b->init(vram); }
void gr_set_scale(int scale)                         { g_b->set_scale(scale); }
int  gr_scale(void)                                  { return g_b->scale(); }
void gr_set_texture_filter(int bilinear)             { g_b->set_texture_filter(bilinear); }
int  gr_texture_filter(void)                         { return g_b->texture_filter(); }
void gr_set_semi_transparency(int e, int m)          { g_b->set_semi_transparency(e, m); }
void gr_set_mask_bits(int s, int c)                  { g_b->set_mask_bits(s, c); }
void gr_set_texture_window(uint32_t raw)             { tex_pack_set_texture_window(raw); g_b->set_texture_window(raw); }
void gr_set_color_modulation(int r, int g, int b, int raw) { g_b->set_color_modulation(r, g, b, raw); }
void gr_set_precise_triangle(int enabled, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2) {
    if (g_b->set_precise_triangle)
        g_b->set_precise_triangle(enabled, x0, y0, x1, y1, x2, y2);
}
void gr_set_perspective_triangle(int enabled, float q0, float q1, float q2) {
    if (g_b->set_perspective_triangle)
        g_b->set_perspective_triangle(enabled, q0, q1, q2);
}
void gr_fill_rect(int x, int y, int w, int h, uint16_t c)  {
    tex_pack_invalidate(x, y, w, h);
    g_b->fill_rect(x, y, w, h, c);
}
void gr_copy_rect(int sx, int sy, int dx, int dy, int w, int h) {
    tex_pack_invalidate(dx, dy, w, h);
    g_b->copy_rect(sx, sy, dx, dy, w, h);
}
void gr_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    g_b->draw_flat_triangle(x0, y0, x1, y1, x2, y2, c);
}
void gr_draw_gouraud_triangle(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2) {
    g_b->draw_gouraud_triangle(x0, y0, c0, x1, y1, c1, x2, y2, c2);
}
/* HD texture identity. Hooked here rather than in a backend so software, GL and
 * Vulkan all feed the same tracker, and so the uv-limit model stays the single
 * one in gpu_uv.h — the backends derive their own `lim` from these same helpers
 * and the same (pre-compensation) uv values, so the rect we match against is the
 * rect they sample. No-op unless a pack or the dumper is enabled. */
/* Arm (or clear) the backend's replacement slot for the primitive about to be
 * drawn. Same one-shot contract as set_precise_triangle: set before, cleared
 * after, so a primitive without a replacement can never inherit the previous
 * one's. Cheap no-op when no pack is loaded — the lookup exits on the first
 * test — and entirely absent for backends that do not implement it. */
static int tex_pack_arm(const int lim[4], uint16_t clut_x, uint16_t clut_y,
                        uint16_t texpage,
                        int sx0, int sy0, int sx1, int sy1) {
    if (!g_b->set_replacement) return 0;
    TexPackRepl r;
    if (!tex_pack_lookup_replacement(lim, clut_x, clut_y, texpage, &r)) return 0;
    /* Record WHICH primitive got substituted and where it landed, not just how
     * many. Counters alone could not distinguish "the HUD is being replaced by
     * the wrong image" from "replacement damages state the HUD needs". */
    tex_pack_note_armed(r.id, sx0, sy0, sx1, sy1);
    g_b->set_replacement(&r);
    return 1;
}

static void tex_pack_disarm(int armed) {
    if (armed && g_b->set_replacement) g_b->set_replacement(NULL);
}

static int tex_pack_note_tri(int x0, int y0, int u0, int v0,
                             int x1, int y1, int u1, int v1,
                             int x2, int y2, int u2, int v2,
                             uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    if (!tex_pack_active()) return 0;
    const int xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
    const int us[3] = { u0, u1, u2 }, vs[3] = { v0, v1, v2 };
    int lim[4];
    psx_uv_tri_limits(xs, ys, us, vs, lim);
    tex_pack_on_textured_prim(lim, clut_x, clut_y, texpage);
    int sx0 = xs[0], sx1 = xs[0], sy0 = ys[0], sy1 = ys[0];
    for (int i = 1; i < 3; i++) {
        if (xs[i] < sx0) sx0 = xs[i];
        if (xs[i] > sx1) sx1 = xs[i];
        if (ys[i] < sy0) sy0 = ys[i];
        if (ys[i] > sy1) sy1 = ys[i];
    }
    return tex_pack_arm(lim, clut_x, clut_y, texpage, sx0, sy0, sx1, sy1);
}

void gr_draw_textured_triangle(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    const int armed = tex_pack_note_tri(x0, y0, u0, v0, x1, y1, u1, v1,
                                        x2, y2, u2, v2, clut_x, clut_y, texpage);
    g_b->draw_textured_triangle(x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2,
                                clut_x, clut_y, texpage);
    tex_pack_disarm(armed);
}
void gr_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0, uint32_t c0,
                                      int x1, int y1, int u1, int v1, uint32_t c1,
                                      int x2, int y2, int u2, int v2, uint32_t c2,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage, int raw) {
    const int armed = tex_pack_note_tri(x0, y0, u0, v0, x1, y1, u1, v1,
                                        x2, y2, u2, v2, clut_x, clut_y, texpage);
    g_b->draw_shaded_textured_triangle(x0, y0, u0, v0, c0, x1, y1, u1, v1, c1,
                                       x2, y2, u2, v2, c2, clut_x, clut_y, texpage, raw);
    tex_pack_disarm(armed);
}
void gr_draw_flat_rect(int x, int y, int w, int h, uint16_t c) { g_b->draw_flat_rect(x, y, w, h, c); }
/* Rect prims carry their uv corners directly; u1/v1 are the EXCLUSIVE corner,
 * which for the unscaled form is u+w / v+h (matching glb_draw_textured_rect). */
static int tex_pack_note_rect(int u0, int v0, int u1, int v1,
                              uint16_t clut_x, uint16_t clut_y, uint16_t texpage,
                              int sx, int sy, int sw, int sh) {
    if (!tex_pack_active()) return 0;
    int lim[4];
    psx_uv_rect_limits(u0, v0, u1, v1, lim);
    tex_pack_on_textured_prim(lim, clut_x, clut_y, texpage);
    return tex_pack_arm(lim, clut_x, clut_y, texpage, sx, sy, sx + sw, sy + sh);
}

void gr_draw_textured_rect(int x, int y, int w, int h, int u, int v,
                           uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    const int armed = tex_pack_note_rect(u, v, u + w, v + h, clut_x, clut_y, texpage,
                                         x, y, w, h);
    g_b->draw_textured_rect(x, y, w, h, u, v, clut_x, clut_y, texpage);
    tex_pack_disarm(armed);
}
void gr_draw_textured_rect_scaled(int x, int y, int w, int h, int u0, int v0, int u1, int v1,
                                  uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    const int armed = tex_pack_note_rect(u0, v0, u1, v1, clut_x, clut_y, texpage,
                                         x, y, w, h);
    g_b->draw_textured_rect_scaled(x, y, w, h, u0, v0, u1, v1, clut_x, clut_y, texpage);
    tex_pack_disarm(armed);
}
void gr_draw_line(int x0, int y0, int x1, int y1, uint16_t c) { g_b->draw_line(x0, y0, x1, y1, c); }
void gr_draw_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1) {
    g_b->draw_shaded_line(x0, y0, c0, x1, y1, c1);
}
int gr_render_display(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    return g_b->render_display(o, p, dx, dy, dw, dh);
}
int gr_render_display_hires(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    return g_b->render_display_hires(o, p, dx, dy, dw, dh);
}
void gr_vram_write(int x, int y, uint16_t pixel)     { tex_pack_invalidate(x, y, 1, 1); g_b->vram_write(x, y, pixel); }
uint16_t gr_vram_read(int x, int y)                  { return g_b->vram_read(x, y); }
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *d)  {
    /* The staged rect `d` is the hash preimage: CRC32 over it is the texture
     * hash a Beetle-format pack is keyed by. */
    tex_pack_on_upload(x, y, w, h, d);
    g_b->vram_transfer_in(x, y, w, h, d);
}
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *d)       { g_b->vram_transfer_out(x, y, w, h, d); }
void gr_set_draw_area(int x1, int y1, int x2, int y2){ g_b->set_draw_area(x1, y1, x2, y2); }
void gr_get_draw_area(int *x1, int *y1, int *x2, int *y2) { g_b->get_draw_area(x1, y1, x2, y2); }
void gr_set_draw_offset(int x, int y)                { g_b->set_draw_offset(x, y); }

/* Native-wide compositor — present only on backends that supply it. */
int  gr_wide_supported(void) { return g_b->render_wide_display != 0; }
void gr_wide_configure(int wide_w, int offset) {
    if (g_b->wide_configure) g_b->wide_configure(wide_w, offset);
}
void gr_wide_set_target(int base_x) {
    if (g_b->wide_set_target) g_b->wide_set_target(base_x);
}
void gr_wide_disable_target(void) {
    if (g_b->wide_disable_target) g_b->wide_disable_target();
}
void gr_wide_clear(int base_x, int y, int h, uint16_t color) {
    if (g_b->wide_clear) g_b->wide_clear(base_x, y, h, color);
}
void gr_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    if (g_b->wide_clear_margins)
        g_b->wide_clear_margins(base_x, y, h, color, sides);
}
int gr_render_wide_display(uint32_t *out, int pitch, int base_x,
                           int disp_y, int disp_h) {
    if (g_b->render_wide_display)
        return g_b->render_wide_display(out, pitch, base_x, disp_y, disp_h);
    return 0;
}
int gr_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh, int base_x) {
    if (g_b->wide_dump_full)
        return g_b->wide_dump_full(out, cap_pixels, ow, oh, base_x);
    return 0;
}
