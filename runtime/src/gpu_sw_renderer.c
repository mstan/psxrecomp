/*
 * gpu_sw_renderer.c — PS1 software rasterizer
 *
 * Operates on a 1024x512 uint16_t VRAM array (15-bit color + mask bit).
 * All drawing is clipped to the current draw area.
 *
 * Texture formats: 4-bit CLUT, 8-bit CLUT, 15-bit direct.
 * Supports: semi-transparency (4 modes), mask bit, texture window,
 *           color modulation for textured primitives.
 */

#include "gpu_sw_renderer.h"
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static uint16_t *g_vram;

/* Draw area clipping rectangle */
static int g_clip_x1, g_clip_y1, g_clip_x2, g_clip_y2;

/* Draw offset */
static int g_offset_x, g_offset_y;

/* Semi-transparency state */
static int g_semi_trans_enabled;
static int g_semi_trans_mode; /* 0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4 */

/* Mask bit state */
static int g_mask_set_bit;   /* force bit 15 on written pixels */
static int g_mask_check_bit; /* skip write if dest bit 15 is set */

/* Texture window (from GP0(E2h)) */
static uint8_t g_tw_mask_x, g_tw_mask_y;
static uint8_t g_tw_off_x, g_tw_off_y;

/* Color modulation for textured primitives */
static uint8_t g_mod_r, g_mod_g, g_mod_b;
static int g_raw_texture; /* 1 = skip modulation */

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int min_i(int a, int b) { return a < b ? a : b; }
static inline int max_i(int a, int b) { return a > b ? a : b; }

static inline uint16_t vram_get(int x, int y) {
    x &= (VRAM_WIDTH - 1);
    y &= (VRAM_HEIGHT - 1);
    return g_vram[y * VRAM_WIDTH + x];
}

static inline int inside_draw_area(int x, int y) {
    return (x >= g_clip_x1 && x <= g_clip_x2 &&
            y >= g_clip_y1 && y <= g_clip_y2);
}

/* ------------------------------------------------------------------ */
/* Semi-transparency blending                                         */
/* ------------------------------------------------------------------ */

static inline uint16_t blend_pixels(uint16_t back, uint16_t front, int mode) {
    int br = (back >>  0) & 0x1F;
    int bg = (back >>  5) & 0x1F;
    int bb = (back >> 10) & 0x1F;
    int fr = (front >>  0) & 0x1F;
    int fg = (front >>  5) & 0x1F;
    int fb = (front >> 10) & 0x1F;
    int r, g, b;

    switch (mode) {
    case 0: /* B/2+F/2 */
        r = (br + fr) >> 1;
        g = (bg + fg) >> 1;
        b = (bb + fb) >> 1;
        break;
    case 1: /* B+F */
        r = br + fr; if (r > 31) r = 31;
        g = bg + fg; if (g > 31) g = 31;
        b = bb + fb; if (b > 31) b = 31;
        break;
    case 2: /* B-F */
        r = br - fr; if (r < 0) r = 0;
        g = bg - fg; if (g < 0) g = 0;
        b = bb - fb; if (b < 0) b = 0;
        break;
    case 3: /* B+F/4 */
        r = br + (fr >> 2); if (r > 31) r = 31;
        g = bg + (fg >> 2); if (g > 31) g = 31;
        b = bb + (fb >> 2); if (b > 31) b = 31;
        break;
    default:
        r = fr; g = fg; b = fb;
        break;
    }
    return (uint16_t)(r | (g << 5) | (b << 10));
}

/* ------------------------------------------------------------------ */
/* Pixel write — central function with mask + semi-trans              */
/* ------------------------------------------------------------------ */

/* Write an opaque (untextured) pixel with semi-transparency if enabled */
static inline void draw_pixel_opaque(int x, int y, uint16_t color) {
    if (x < 0 || x >= VRAM_WIDTH || y < 0 || y >= VRAM_HEIGHT) return;
    if (!inside_draw_area(x, y)) return;

    int idx = y * VRAM_WIDTH + x;

    /* Mask bit check: don't overwrite if dest has bit 15 set */
    if (g_mask_check_bit && (g_vram[idx] & 0x8000)) return;

    /* Semi-transparency: for untextured primitives, always blend when flag set */
    if (g_semi_trans_enabled) {
        color = blend_pixels(g_vram[idx], color, g_semi_trans_mode);
    }

    /* Mask bit set: force bit 15 on written pixel */
    if (g_mask_set_bit) color |= 0x8000;

    g_vram[idx] = color;
}

/* Write a textured pixel — semi-trans only if texel bit 15 is set */
static inline void draw_pixel_textured(int x, int y, uint16_t texel) {
    if (x < 0 || x >= VRAM_WIDTH || y < 0 || y >= VRAM_HEIGHT) return;
    if (!inside_draw_area(x, y)) return;

    /* Transparent texel (0x0000) is always skipped */
    if (texel == 0x0000) return;

    int idx = y * VRAM_WIDTH + x;

    /* Mask bit check */
    if (g_mask_check_bit && (g_vram[idx] & 0x8000)) return;

    /* Color modulation: multiply texel by vertex color unless raw texture */
    uint16_t color;
    if (!g_raw_texture) {
        int tr = (texel >>  0) & 0x1F;
        int tg = (texel >>  5) & 0x1F;
        int tb = (texel >> 10) & 0x1F;
        /* PS1 formula: (texel * color * 2) / 256, with 5-bit texel scaled to 8-bit.
         * Simplified: (tr * mod_r) >> 4, clamped to 31 */
        int r = (tr * g_mod_r) >> 4; if (r > 31) r = 31;
        int g = (tg * g_mod_g) >> 4; if (g > 31) g = 31;
        int b = (tb * g_mod_b) >> 4; if (b > 31) b = 31;
        color = (uint16_t)(r | (g << 5) | (b << 10));
    } else {
        color = texel & 0x7FFF;
    }

    /* Semi-transparency: for textured primitives, only blend if texel has bit 15 */
    if (g_semi_trans_enabled && (texel & 0x8000)) {
        color = blend_pixels(g_vram[idx], color, g_semi_trans_mode);
    }

    /* Mask bit set */
    if (g_mask_set_bit) color |= 0x8000;

    g_vram[idx] = color;
}

/* ------------------------------------------------------------------ */
/* Texture lookup with texture window                                 */
/* ------------------------------------------------------------------ */

static uint16_t texel_fetch(int u, int v, uint16_t texpage,
                            uint16_t clut_x, uint16_t clut_y) {
    /* Apply texture window:
     * texcoord = (texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
     * Mask and Offset are in 8-pixel steps. */
    if (g_tw_mask_x | g_tw_mask_y) {
        u = (u & ~(g_tw_mask_x * 8)) | ((g_tw_off_x & g_tw_mask_x) * 8);
        v = (v & ~(g_tw_mask_y * 8)) | ((g_tw_off_y & g_tw_mask_y) * 8);
    }

    int tpx = (texpage & 0xF) * 64;
    int tpy = ((texpage >> 4) & 1) * 256;
    int depth = (texpage >> 7) & 3;

    switch (depth) {
    case 0: { /* 4-bit CLUT */
        int vram_x = tpx + (u / 4);
        int vram_y = tpy + v;
        uint16_t texel_word = vram_get(vram_x, vram_y);
        int shift = (u & 3) * 4;
        int index = (texel_word >> shift) & 0xF;
        return vram_get(clut_x + index, clut_y);
    }
    case 1: { /* 8-bit CLUT */
        int vram_x = tpx + (u / 2);
        int vram_y = tpy + v;
        uint16_t texel_word = vram_get(vram_x, vram_y);
        int shift = (u & 1) * 8;
        int index = (texel_word >> shift) & 0xFF;
        return vram_get(clut_x + index, clut_y);
    }
    case 2:   /* 15-bit direct */
    case 3: {
        return vram_get(tpx + u, tpy + v);
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Public API: init                                                   */
/* ------------------------------------------------------------------ */

void sw_renderer_init(uint16_t *vram) {
    g_vram = vram;
    g_clip_x1 = 0;
    g_clip_y1 = 0;
    g_clip_x2 = VRAM_WIDTH - 1;
    g_clip_y2 = VRAM_HEIGHT - 1;
    g_offset_x = 0;
    g_offset_y = 0;
    g_semi_trans_enabled = 0;
    g_semi_trans_mode = 0;
    g_mask_set_bit = 0;
    g_mask_check_bit = 0;
    g_tw_mask_x = g_tw_mask_y = 0;
    g_tw_off_x = g_tw_off_y = 0;
    g_mod_r = g_mod_g = g_mod_b = 16; /* neutral = 128/8 = 16 (no modulation) */
    g_raw_texture = 0;
}

/* ------------------------------------------------------------------ */
/* Draw state setters                                                 */
/* ------------------------------------------------------------------ */

void sw_set_semi_transparency(int enabled, int mode) {
    g_semi_trans_enabled = enabled;
    g_semi_trans_mode = mode & 3;
}

void sw_set_mask_bits(int set_bit, int check_bit) {
    g_mask_set_bit = set_bit;
    g_mask_check_bit = check_bit;
}

void sw_set_texture_window(uint32_t raw) {
    g_tw_mask_x = (uint8_t)(raw & 0x1F);
    g_tw_mask_y = (uint8_t)((raw >> 5) & 0x1F);
    g_tw_off_x  = (uint8_t)((raw >> 10) & 0x1F);
    g_tw_off_y  = (uint8_t)((raw >> 15) & 0x1F);
}

void sw_set_color_modulation(int r, int g, int b, int raw_texture) {
    /* Convert 8-bit vertex color to modulation factor.
     * PS1 formula: result = (texel_5bit * color_8bit) >> 4
     * When color_8bit=128 (0x80, neutral gray), result = texel * 8 = texel << 3 ≈ original.
     * We store color_8bit >> 3 so the multiply in draw_pixel_textured is >> 4.
     * Actually: store raw 8-bit value / 8 = >> 3 to get 5-bit-range factor.
     *
     * Simpler approach: store the 8-bit color, shift later.
     * result_5bit = (texel_5bit * color_8bit + 127) >> 7, clamped to 31
     * But that's slower. Let's use: (texel * (color >> 3)) >> 4
     * At color=128: factor=16, (31*16)>>4 = 31 — perfect.
     * At color=255: factor=31, (31*31)>>4 = 60 → clamp 31 — saturated.
     * At color=0: factor=0, result=0 — black.
     */
    g_mod_r = (uint8_t)(r >> 3);
    g_mod_g = (uint8_t)(g >> 3);
    g_mod_b = (uint8_t)(b >> 3);
    g_raw_texture = raw_texture;
}

/* ------------------------------------------------------------------ */
/* Draw area / offset                                                 */
/* ------------------------------------------------------------------ */

void sw_set_draw_area(int x1, int y1, int x2, int y2) {
    g_clip_x1 = clamp_i(x1, 0, VRAM_WIDTH - 1);
    g_clip_y1 = clamp_i(y1, 0, VRAM_HEIGHT - 1);
    g_clip_x2 = clamp_i(x2, 0, VRAM_WIDTH - 1);
    g_clip_y2 = clamp_i(y2, 0, VRAM_HEIGHT - 1);
}

void sw_get_draw_area(int *x1, int *y1, int *x2, int *y2) {
    *x1 = g_clip_x1; *y1 = g_clip_y1;
    *x2 = g_clip_x2; *y2 = g_clip_y2;
}

void sw_set_draw_offset(int x, int y) {
    g_offset_x = x;
    g_offset_y = y;
}

/* ------------------------------------------------------------------ */
/* Fill rectangle (GP0(02h) — directly in VRAM, no draw offset)       */
/* ------------------------------------------------------------------ */

void sw_fill_rect(int x, int y, int w, int h, uint16_t color) {
    /* Fill rect ignores draw area, mask bits, and semi-transparency.
     * It writes directly to VRAM with coordinates wrapping. */
    int x0 = x & (VRAM_WIDTH - 1);
    int y0 = y & (VRAM_HEIGHT - 1);

    for (int row = 0; row < h; row++) {
        int py = (y0 + row) & (VRAM_HEIGHT - 1);
        for (int col = 0; col < w; col++) {
            int px = (x0 + col) & (VRAM_WIDTH - 1);
            g_vram[py * VRAM_WIDTH + px] = color;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Copy rectangle (VRAM -> VRAM)                                      */
/* ------------------------------------------------------------------ */

void sw_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    uint16_t row_buf[VRAM_WIDTH];

    for (int row = 0; row < h; row++) {
        int sy = (src_y + row) & (VRAM_HEIGHT - 1);
        int dy = (dst_y + row) & (VRAM_HEIGHT - 1);

        for (int col = 0; col < w; col++) {
            int sx = (src_x + col) & (VRAM_WIDTH - 1);
            row_buf[col] = g_vram[sy * VRAM_WIDTH + sx];
        }
        for (int col = 0; col < w; col++) {
            int dx = (dst_x + col) & (VRAM_WIDTH - 1);
            uint16_t pix = row_buf[col];
            /* Copy applies mask bit settings */
            if (g_mask_check_bit && (g_vram[dy * VRAM_WIDTH + dx] & 0x8000))
                continue;
            if (g_mask_set_bit) pix |= 0x8000;
            g_vram[dy * VRAM_WIDTH + dx] = pix;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Flat-shaded triangle (scanline rasterization)                      */
/* ------------------------------------------------------------------ */

void sw_draw_flat_triangle(int x0, int y0, int x1, int y1,
                           int x2, int y2, uint16_t color) {
    /* Sort vertices by Y coordinate */
    if (y0 > y1) { int t; t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t; }
    if (y0 > y2) { int t; t=x0; x0=x2; x2=t; t=y0; y0=y2; y2=t; }
    if (y1 > y2) { int t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }

    int dy_total = y2 - y0;
    if (dy_total == 0) return;

    for (int y = y0; y <= y2; y++) {
        if (y < g_clip_y1 || y > g_clip_y2) continue;

        int second_half = (y >= y1);
        int seg_height = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_height == 0) seg_height = 1;

        float alpha = (float)(y - y0) / (float)dy_total;
        float beta;
        if (second_half)
            beta = (float)(y - y1) / (float)seg_height;
        else
            beta = (float)(y - y0) / (float)seg_height;

        int xa = x0 + (int)((float)(x2 - x0) * alpha);
        int xb;
        if (second_half)
            xb = x1 + (int)((float)(x2 - x1) * beta);
        else
            xb = x0 + (int)((float)(x1 - x0) * beta);

        if (xa > xb) { int t = xa; xa = xb; xb = t; }

        int sx = max_i(xa, g_clip_x1);
        int ex = min_i(xb, g_clip_x2);

        for (int x = sx; x <= ex; x++) {
            draw_pixel_opaque(x, y, color);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Gouraud-shaded triangle (scanline rasterization with color interp) */
/* ------------------------------------------------------------------ */

void sw_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                              int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2) {
    /* Extract 5-bit color components for each vertex */
    int r0 = (c0 >>  0) & 0x1F, g0 = (c0 >>  5) & 0x1F, b0 = (c0 >> 10) & 0x1F;
    int r1 = (c1 >>  0) & 0x1F, g1 = (c1 >>  5) & 0x1F, b1 = (c1 >> 10) & 0x1F;
    int r2 = (c2 >>  0) & 0x1F, g2 = (c2 >>  5) & 0x1F, b2 = (c2 >> 10) & 0x1F;

    /* Sort vertices by Y coordinate, keeping colors in sync */
    if (y0 > y1) {
        int t; t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t;
        t=r0; r0=r1; r1=t; t=g0; g0=g1; g1=t; t=b0; b0=b1; b1=t;
    }
    if (y0 > y2) {
        int t; t=x0; x0=x2; x2=t; t=y0; y0=y2; y2=t;
        t=r0; r0=r2; r2=t; t=g0; g0=g2; g2=t; t=b0; b0=b2; b2=t;
    }
    if (y1 > y2) {
        int t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t;
        t=r1; r1=r2; r2=t; t=g1; g1=g2; g2=t; t=b1; b1=b2; b2=t;
    }

    int dy_total = y2 - y0;
    if (dy_total == 0) return;

    for (int y = y0; y <= y2; y++) {
        if (y < g_clip_y1 || y > g_clip_y2) continue;

        int second_half = (y >= y1);
        int seg_height = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_height == 0) seg_height = 1;

        float alpha = (float)(y - y0) / (float)dy_total;
        float beta;
        if (second_half)
            beta = (float)(y - y1) / (float)seg_height;
        else
            beta = (float)(y - y0) / (float)seg_height;

        /* Interpolate X along edges */
        int xa = x0 + (int)((float)(x2 - x0) * alpha);
        int xb;
        if (second_half)
            xb = x1 + (int)((float)(x2 - x1) * beta);
        else
            xb = x0 + (int)((float)(x1 - x0) * beta);

        /* Interpolate colors along edges (in 5-bit space) */
        int ra = r0 + (int)((float)(r2 - r0) * alpha);
        int ga = g0 + (int)((float)(g2 - g0) * alpha);
        int ba = b0 + (int)((float)(b2 - b0) * alpha);
        int rb, gb, bb;
        if (second_half) {
            rb = r1 + (int)((float)(r2 - r1) * beta);
            gb = g1 + (int)((float)(g2 - g1) * beta);
            bb = b1 + (int)((float)(b2 - b1) * beta);
        } else {
            rb = r0 + (int)((float)(r1 - r0) * beta);
            gb = g0 + (int)((float)(g1 - g0) * beta);
            bb = b0 + (int)((float)(b1 - b0) * beta);
        }

        /* Ensure xa < xb, swap colors too */
        if (xa > xb) {
            int t;
            t = xa; xa = xb; xb = t;
            t = ra; ra = rb; rb = t;
            t = ga; ga = gb; gb = t;
            t = ba; ba = bb; bb = t;
        }

        int sx = max_i(xa, g_clip_x1);
        int ex = min_i(xb, g_clip_x2);
        int span = xb - xa;

        for (int x = sx; x <= ex; x++) {
            /* Interpolate color across the scanline */
            uint16_t color;
            if (span > 0) {
                float t = (float)(x - xa) / (float)span;
                int r = ra + (int)((float)(rb - ra) * t);
                int g = ga + (int)((float)(gb - ga) * t);
                int b = ba + (int)((float)(bb - ba) * t);
                if (r < 0) r = 0; if (r > 31) r = 31;
                if (g < 0) g = 0; if (g > 31) g = 31;
                if (b < 0) b = 0; if (b > 31) b = 31;
                color = (uint16_t)(r | (g << 5) | (b << 10));
            } else {
                color = (uint16_t)(ra | (ga << 5) | (ba << 10));
            }
            draw_pixel_opaque(x, y, color);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Textured triangle                                                  */
/* ------------------------------------------------------------------ */

void sw_draw_textured_triangle(int x0, int y0, int u0, int v0,
                               int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage) {
    /* Sort by Y, keeping UV in sync */
    if (y0 > y1) {
        int t;
        t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t;
        t=u0; u0=u1; u1=t; t=v0; v0=v1; v1=t;
    }
    if (y0 > y2) {
        int t;
        t=x0; x0=x2; x2=t; t=y0; y0=y2; y2=t;
        t=u0; u0=u2; u2=t; t=v0; v0=v2; v2=t;
    }
    if (y1 > y2) {
        int t;
        t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t;
        t=u1; u1=u2; u2=t; t=v1; v1=v2; v2=t;
    }

    int dy_total = y2 - y0;
    if (dy_total == 0) return;

    for (int y = y0; y <= y2; y++) {
        if (y < g_clip_y1 || y > g_clip_y2) continue;

        int second_half = (y >= y1);
        int seg_height = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_height == 0) seg_height = 1;

        float alpha = (float)(y - y0) / (float)dy_total;
        float beta;
        if (second_half)
            beta = (float)(y - y1) / (float)seg_height;
        else
            beta = (float)(y - y0) / (float)seg_height;

        int xa = x0 + (int)((float)(x2 - x0) * alpha);
        int xb;
        if (second_half)
            xb = x1 + (int)((float)(x2 - x1) * beta);
        else
            xb = x0 + (int)((float)(x1 - x0) * beta);

        float ua = u0 + (float)(u2 - u0) * alpha;
        float va = v0 + (float)(v2 - v0) * alpha;
        float ub, vb;
        if (second_half) {
            ub = u1 + (float)(u2 - u1) * beta;
            vb = v1 + (float)(v2 - v1) * beta;
        } else {
            ub = u0 + (float)(u1 - u0) * beta;
            vb = v0 + (float)(v1 - v0) * beta;
        }

        if (xa > xb) {
            int t = xa; xa = xb; xb = t;
            float tf;
            tf = ua; ua = ub; ub = tf;
            tf = va; va = vb; vb = tf;
        }

        int span = xb - xa;
        if (span == 0) span = 1;

        int sx = max_i(xa, g_clip_x1);
        int ex = min_i(xb, g_clip_x2);

        for (int x = sx; x <= ex; x++) {
            float t_val = (float)(x - xa) / (float)span;
            int u = (int)(ua + (ub - ua) * t_val) & 0xFF;
            int v_coord = (int)(va + (vb - va) * t_val) & 0xFF;

            uint16_t texel = texel_fetch(u, v_coord, texpage, clut_x, clut_y);
            draw_pixel_textured(x, y, texel);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Flat rectangle                                                     */
/* ------------------------------------------------------------------ */

void sw_draw_flat_rect(int x, int y, int w, int h, uint16_t color) {
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < g_clip_y1 || py > g_clip_y2) continue;

        int sx = max_i(x, g_clip_x1);
        int ex = min_i(x + w - 1, g_clip_x2);

        for (int px = sx; px <= ex; px++) {
            draw_pixel_opaque(px, py, color);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Textured rectangle                                                 */
/* ------------------------------------------------------------------ */

void sw_draw_textured_rect(int x, int y, int w, int h,
                           int u, int v,
                           uint16_t clut_x, uint16_t clut_y,
                           uint16_t texpage) {
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < g_clip_y1 || py > g_clip_y2) continue;

        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < g_clip_x1 || px > g_clip_x2) continue;

            int tu = (u + col) & 0xFF;
            int tv = (v + row) & 0xFF;
            uint16_t texel = texel_fetch(tu, tv, texpage, clut_x, clut_y);
            draw_pixel_textured(px, py, texel);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Line (Bresenham)                                                   */
/* ------------------------------------------------------------------ */

void sw_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        draw_pixel_opaque(x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ------------------------------------------------------------------ */
/* VRAM pixel access                                                  */
/* ------------------------------------------------------------------ */

void sw_vram_write(int x, int y, uint16_t pixel) {
    x &= (VRAM_WIDTH - 1);
    y &= (VRAM_HEIGHT - 1);
    g_vram[y * VRAM_WIDTH + x] = pixel;
}

uint16_t sw_vram_read(int x, int y) {
    return vram_get(x, y);
}

/* ------------------------------------------------------------------ */
/* Bulk VRAM transfers                                                */
/* ------------------------------------------------------------------ */

void sw_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    int idx = 0;
    for (int row = 0; row < h; row++) {
        int py = (y + row) & (VRAM_HEIGHT - 1);
        for (int col = 0; col < w; col++) {
            int px = (x + col) & (VRAM_WIDTH - 1);
            g_vram[py * VRAM_WIDTH + px] = data[idx++];
        }
    }
}

void sw_vram_transfer_out(int x, int y, int w, int h, uint16_t *data) {
    int idx = 0;
    for (int row = 0; row < h; row++) {
        int py = (y + row) & (VRAM_HEIGHT - 1);
        for (int col = 0; col < w; col++) {
            int px = (x + col) & (VRAM_WIDTH - 1);
            data[idx++] = g_vram[py * VRAM_WIDTH + px];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Display output — convert 15-bit VRAM to 32-bit RGBA                */
/* ------------------------------------------------------------------ */

int sw_render_display(uint32_t *out_pixels, int out_pitch,
                      int disp_x, int disp_y, int disp_w, int disp_h) {
    int count = 0;

    for (int row = 0; row < disp_h; row++) {
        int vy = (disp_y + row) & (VRAM_HEIGHT - 1);
        uint32_t *dst = (uint32_t *)((uint8_t *)out_pixels + row * out_pitch);

        for (int col = 0; col < disp_w; col++) {
            int vx = (disp_x + col) & (VRAM_WIDTH - 1);
            uint16_t pix = g_vram[vy * VRAM_WIDTH + vx];

            int r5 = (pix >>  0) & 0x1F;
            int g5 = (pix >>  5) & 0x1F;
            int b5 = (pix >> 10) & 0x1F;

            uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
            uint8_t g = (uint8_t)((g5 << 3) | (g5 >> 2));
            uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));

            dst[col] = (uint32_t)r |
                       ((uint32_t)g << 8) |
                       ((uint32_t)b << 16) |
                       0xFF000000u;
            count++;
        }
    }

    return count;
}
