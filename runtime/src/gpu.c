/* gpu.c — PS1 GPU hardware simulation (Phase 3, Step 1).
 *
 * Implements:
 *   - GPUSTAT register with correct bit semantics
 *   - GP1 commands 00h-08h (reset, display config)
 *   - VRAM storage (1024x512 x 16-bit)
 *   - GP0 command write — ABORTS (not yet implemented)
 *   - GPUREAD — returns last latched value
 *
 * Reference: nocash PSX specs, DuckStation src/core/gpu.cpp
 */

#include "gpu.h"
#include "gpu_sw_renderer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- VRAM ---- */
static uint16_t vram[1024 * 512];

/* ---- GP0 state machine ---- */

typedef enum {
    GP0_IDLE,
    GP0_COLLECTING,
    GP0_VRAM_WRITE
} Gp0State;

static Gp0State gp0_state;
static uint64_t gp0_write_count;
static uint64_t gp0_nop_count, gp0_fill_count, gp0_draw_count, gp0_env_count, gp0_copy_count;
static uint32_t gp0_cmd_buf[16];   /* max fixed-length command is 12 words */
static int      gp0_words_collected;
static int      gp0_words_needed;

/* VRAM write transfer state (CPU→VRAM, command 0xA0) */
static uint16_t vram_write_x, vram_write_y;   /* start coords */
static uint16_t vram_write_w, vram_write_h;   /* dimensions */
static uint16_t vram_write_col, vram_write_row; /* current offset */
static uint32_t vram_write_remaining;          /* words remaining */

/* VRAM read transfer state (VRAM→CPU, command 0xC0) */
static int      vram_read_active;
static uint16_t vram_read_x, vram_read_y;
static uint16_t vram_read_w, vram_read_h;
static uint16_t vram_read_col, vram_read_row;

/* ---- GPU internal state ---- */

/* Texture page / draw mode (set by GP0(E1h), reflected in GPUSTAT bits 0-10) */
static uint32_t texpage_x;         /* bits 0-3: texture page X base (N*64) */
static uint32_t texpage_y;         /* bit 4: texture page Y base (0 or 256) */
static uint32_t semi_transparency; /* bits 5-6 */
static uint32_t texpage_colors;    /* bits 7-8: 4bit/8bit/15bit */
static uint32_t dither_enabled;    /* bit 9 */
static uint32_t draw_to_display;   /* bit 10: drawing to display area allowed */
static uint32_t set_mask_bit;      /* bit 11 */
static uint32_t check_mask_bit;    /* bit 12 */
static uint32_t interlace_field;   /* bit 13 */
static uint32_t reverse_flag;      /* bit 14 */
static uint32_t texture_disable;   /* bit 15 */

/* Draw area (set by GP0(E3h)/GP0(E4h)) */
static uint32_t draw_area_left, draw_area_top;
static uint32_t draw_area_right, draw_area_bottom;

/* Draw offset (set by GP0(E5h)) */
static int32_t draw_offset_x, draw_offset_y;

/* Texture window raw value (set by GP0(E2h), readback via GP1(10h)) */
static uint32_t texture_window_value;

/* Display mode (set by GP1(08h), reflected in GPUSTAT bits 16-22) */
static uint32_t hres2;            /* bit 16: horizontal resolution 2 (368 mode) */
static uint32_t hres1;            /* bits 17-18: horizontal resolution 1 */
static uint32_t vres;             /* bit 19: vertical resolution (0=240, 1=480) */
static uint32_t video_mode;       /* bit 20: 0=NTSC, 1=PAL */
static uint32_t display_depth;    /* bit 21: 0=15bit, 1=24bit */
static uint32_t vertical_interlace; /* bit 22 */

/* Display enable (set by GP1(03h)) */
static uint32_t display_disabled; /* bit 23: 0=enabled, 1=disabled */

/* IRQ1 (set by GP0(1Fh), acked by GP1(02h)) */
static uint32_t irq1_flag;       /* bit 24 */

/* DMA direction (set by GP1(04h)) */
static uint32_t dma_direction;   /* bits 29-30 in GPUSTAT */

/* LCF (even/odd line in interlace, toggles per vblank) */
static uint32_t lcf;             /* bit 31 */

/* Display area start (GP1(05h)) */
static uint32_t display_area_x;
static uint32_t display_area_y;

/* Horizontal display range (GP1(06h)) */
static uint32_t h_display_x1;
static uint32_t h_display_x2;

/* Vertical display range (GP1(07h)) */
static uint32_t v_display_y1;
static uint32_t v_display_y2;

/* GPUREAD latch (GP1(10h) get-info result, or VRAM read data) */
static uint32_t gpuread_latch;

/* Vblank presentation callback */
static gpu_vblank_cb vblank_callback;

/* Shaded quad vertex capture (Phase 4.5 debug) — forward declarations
 * so gpu_vblank_tick can reference sq_cap_armed. */
#define SQ_CAP_MAX 32
static GpuSqCapEntry sq_cap_buf[SQ_CAP_MAX];
static int sq_cap_count;
static int sq_cap_armed;

/* GPUSTAT poll counter: in v4, recompiled code runs as native C without
 * per-instruction stepping. The BIOS contains tight VSYNC wait loops that
 * poll GPUSTAT bit 31 (LCF) waiting for it to toggle. LCF only changes
 * via gpu_vblank_tick(), which normally fires from psx_check_interrupts()
 * at dispatch boundaries. A polling loop within a single recompiled
 * function would spin forever.
 *
 * Fix: count GPUSTAT reads and trigger a vblank when the BIOS has polled
 * enough times, approximating the real hardware timing where the field
 * flips every ~33,868 GPU clocks (one NTSC frame). */
#define GPUSTAT_POLL_VBLANK_THRESHOLD 1000
static uint32_t gpustat_poll_count;

/* ---- Initialization ---- */

void gpu_init(void) {
    memset(vram, 0, sizeof(vram));
    sw_renderer_init(vram);

    /* Reset GP0 state machine */
    gp0_state = GP0_IDLE;
    gp0_words_collected = 0;
    gp0_words_needed = 0;
    vram_write_remaining = 0;
    vram_read_active = 0;

    /* Reset all state to power-on defaults */
    texpage_x = 0;
    texpage_y = 0;
    semi_transparency = 0;
    texpage_colors = 0;
    dither_enabled = 0;
    draw_to_display = 0;
    set_mask_bit = 0;
    check_mask_bit = 0;
    interlace_field = 0;
    reverse_flag = 0;
    texture_disable = 0;

    draw_area_left = 0;
    draw_area_top = 0;
    draw_area_right = 0;
    draw_area_bottom = 0;
    draw_offset_x = 0;
    draw_offset_y = 0;
    texture_window_value = 0;

    hres2 = 0;
    hres1 = 0;
    vres = 0;
    video_mode = 0;
    display_depth = 0;
    vertical_interlace = 0;

    display_disabled = 1;  /* display is OFF after reset */
    irq1_flag = 0;
    dma_direction = 0;
    lcf = 0;

    display_area_x = 0;
    display_area_y = 0;
    h_display_x1 = 0x200;
    h_display_x2 = 0xC00;
    v_display_y1 = 0x010;
    v_display_y2 = 0x100;

    gpuread_latch = 0;
    gpustat_poll_count = 0;
}

/* ---- GPUSTAT read (0x1F801814) ---- */

uint32_t gpu_read_gpustat(void) {
    /* Advance vblank when polled enough times from within a single function.
     * This handles BIOS VSYNC wait loops that poll LCF in tight loops. */
    gpustat_poll_count++;
    if (gpustat_poll_count >= GPUSTAT_POLL_VBLANK_THRESHOLD) {
        gpustat_poll_count = 0;
        gpu_vblank_tick();
    }

    uint32_t stat = 0;

    stat |= (texpage_x & 0xF);
    stat |= (texpage_y & 1) << 4;
    stat |= (semi_transparency & 3) << 5;
    stat |= (texpage_colors & 3) << 7;
    stat |= (dither_enabled & 1) << 9;
    stat |= (draw_to_display & 1) << 10;
    stat |= (set_mask_bit & 1) << 11;
    stat |= (check_mask_bit & 1) << 12;
    stat |= (interlace_field & 1) << 13;
    stat |= (reverse_flag & 1) << 14;
    stat |= (texture_disable & 1) << 15;
    stat |= (hres2 & 1) << 16;
    stat |= (hres1 & 3) << 17;
    stat |= (vres & 1) << 19;
    stat |= (video_mode & 1) << 20;
    stat |= (display_depth & 1) << 21;
    stat |= (vertical_interlace & 1) << 22;
    stat |= (display_disabled & 1) << 23;
    stat |= (irq1_flag & 1) << 24;

    /* Bit 25: DMA request — depends on DMA direction.
     * Direction 0: always 0
     * Direction 1: FIFO not full (always 1 for now — we process instantly)
     * Direction 2: same as bit 28 (ready to receive DMA block)
     * Direction 3: same as bit 27 (ready to send VRAM to CPU)
     */
    switch (dma_direction) {
        case 0: break; /* bit 25 = 0 */
        case 1: stat |= (1u << 25); break;
        case 2: stat |= (1u << 25); break; /* mirrors ready-to-receive */
        case 3: stat |= (1u << 25); break; /* mirrors ready-to-send */
    }

    /* Bit 26: ready to receive cmd word — 1 when not busy */
    stat |= (1u << 26);

    /* Bit 27: ready to send VRAM to CPU — 1 when VRAM read is active */
    if (vram_read_active)
        stat |= (1u << 27);

    /* Bit 28: ready to receive DMA block — 1 when not busy */
    stat |= (1u << 28);

    /* Bits 29-30: DMA direction */
    stat |= (dma_direction & 3) << 29;

    /* Bit 31: LCF — drawing even/odd lines in interlace mode */
    stat |= (lcf & 1) << 31;

    return stat;
}

/* ---- GPUREAD (0x1F801810 read) ---- */

uint32_t gpu_read_gpuread(void) {
    if (!vram_read_active)
        return gpuread_latch;

    /* Read two 16-bit pixels from VRAM and pack into one 32-bit word */
    uint32_t value = 0;
    for (int i = 0; i < 2; i++) {
        uint16_t rx = (vram_read_x + vram_read_col) % 1024;
        uint16_t ry = (vram_read_y + vram_read_row) % 512;
        value |= (uint32_t)vram[ry * 1024 + rx] << (i * 16);

        if (++vram_read_col == vram_read_w) {
            vram_read_col = 0;
            if (++vram_read_row == vram_read_h) {
                /* Transfer complete */
                vram_read_active = 0;
                break;
            }
        }
    }

    gpuread_latch = value;
    return value;
}

/* ---- GP0 command helpers ---- */

/* Convert 24-bit RGB888 to 16-bit RGB555 (PS1 VRAM format) */
static uint16_t rgb888_to_rgb555(uint32_t color24) {
    uint32_t r = (color24 >>  0) & 0xFF;
    uint32_t g = (color24 >>  8) & 0xFF;
    uint32_t b = (color24 >> 16) & 0xFF;
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* I_STAT owned by memory.c — needed to set IRQ_VBLANK from poll-triggered vblank */
extern uint32_t i_stat;

void gpu_vblank_tick(void) {
    lcf ^= 1;
    gpustat_poll_count = 0;
    i_stat |= (1u << 0); /* IRQ_VBLANK */
    if (vblank_callback) vblank_callback();
}

const uint16_t* gpu_get_vram(void) {
    return vram;
}

void gpu_get_display_info(GpuDisplayInfo* out) {
    out->display_x = display_area_x;
    out->display_y = display_area_y;
    out->disabled  = (int)display_disabled;

    /* Derive pixel width from horizontal display range and resolution mode.
     * The BIOS typically sets 256x240 or 320x240 NTSC. */
    uint32_t dots = (h_display_x2 > h_display_x1) ? (h_display_x2 - h_display_x1) : 0;

    /* Horizontal resolution in pixels depends on hres1/hres2 bits.
     * Common: hres1=0 → 256, hres1=1 → 320, hres1=2 → 512, hres1=3 → 640
     *         hres2=1 → 368 */
    uint32_t w;
    if (hres2) {
        w = 368;
    } else {
        static const uint32_t hres_lut[4] = { 256, 320, 512, 640 };
        w = hres_lut[hres1 & 3];
    }

    uint32_t h = (v_display_y2 > v_display_y1) ? (v_display_y2 - v_display_y1) : 240;
    if (vres) h *= 2; /* 480i */

    /* Clamp to sane maximums */
    if (w > 640) w = 640;
    if (h > 512) h = 512;

    out->width  = w;
    out->height = h;
}

void gpu_set_vblank_callback(gpu_vblank_cb cb) {
    vblank_callback = cb;
}

/* Sign-extend an N-bit value to int32_t */
static int32_t sign_extend(uint32_t val, int bits) {
    uint32_t sign_bit = 1u << (bits - 1);
    return (int32_t)((val ^ sign_bit) - sign_bit);
}

/* ---- Polygon rasterizer ---- */

/* Parse a vertex position word: signed 11-bit X and Y */
static void parse_vertex(uint32_t word, int32_t* x, int32_t* y) {
    *x = sign_extend(word & 0x7FFu, 11);
    *y = sign_extend((word >> 16) & 0x7FFu, 11);
}

/* Write a single pixel to VRAM with draw area clipping and mask bit handling */
static void raster_pixel(int32_t x, int32_t y, uint16_t color) {
    if (x < (int32_t)draw_area_left || x > (int32_t)draw_area_right) return;
    if (y < (int32_t)draw_area_top  || y > (int32_t)draw_area_bottom) return;
    uint32_t vx = (uint32_t)x & 1023u;
    uint32_t vy = (uint32_t)y & 511u;
    uint32_t idx = vy * 1024 + vx;
    if (check_mask_bit && (vram[idx] & 0x8000u)) return;
    vram[idx] = color | (set_mask_bit ? 0x8000u : 0u);
}

/* Rasterize a flat-shaded triangle using DDA scanline fill.
 * Vertices are in screen coordinates (draw offset already applied). */
static void raster_triangle(int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2,
                            uint16_t color)
{
    /* Sort vertices by Y (ascending). */
    int32_t tx, ty;
    if (y0 > y1) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }
    if (y1 > y2) { tx=x1; ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }
    if (y0 > y1) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }

    /* Reject degenerate (zero-height) or oversized triangles. */
    if (y0 == y2) return;
    if ((x2 - x0) > 1023 || (x0 - x2) > 1023) return;
    if ((y2 - y0) > 511) return;

    /* 64-bit fixed-point DDA (32.32) matching PS1 hardware behavior.
     * Reference: DuckStation gpu_sw_rasterizer.inl makefp_xy / makestep_xy */
    #define FP_ONE  (1LL << 32)
    #define MAKE_FP(v) (((int64_t)(v) << 32) + (FP_ONE - (1 << 11)))
    #define MAKE_STEP(dx, dy) \
        ((((int64_t)(dx) << 32) + ((dx) < 0 ? -((dy)-1) : (((dx) > 0) ? ((dy)-1) : 0))) / (dy))
    #define UNFP(fp) ((int32_t)((uint64_t)(fp) >> 32))

    /* Upper half: y0 to y1 */
    if (y1 > y0) {
        int32_t dy_long  = y2 - y0;
        int32_t dy_short = y1 - y0;
        int64_t base   = MAKE_FP(x0);
        int64_t step_long  = MAKE_STEP(x2 - x0, dy_long);
        int64_t step_short = MAKE_STEP(x1 - x0, dy_short);
        /* Determine which edge is left vs right */
        int64_t lx, rx, ls, rs;
        if (step_long < step_short) {
            lx = base; ls = step_long; rx = base; rs = step_short;
        } else {
            lx = base; ls = step_short; rx = base; rs = step_long;
        }
        for (int32_t y = y0; y < y1; y++) {
            int32_t xl = UNFP(lx);
            int32_t xr = UNFP(rx);
            for (int32_t x = xl; x < xr; x++)
                raster_pixel(x, y, color);
            lx += ls; rx += rs;
        }
    }

    /* Lower half: y1 to y2 */
    if (y2 > y1) {
        int32_t dy_long  = y2 - y0;
        int32_t dy_short = y2 - y1;
        /* Long edge continues from y0 to y2 */
        int64_t step_long  = MAKE_STEP(x2 - x0, dy_long);
        int64_t long_at_y1 = MAKE_FP(x0) + step_long * (y1 - y0);
        int64_t short_start = MAKE_FP(x1);
        int64_t step_short = MAKE_STEP(x2 - x1, dy_short);
        /* Determine left/right from the upper half's orientation */
        int64_t lx, rx, ls, rs;
        int64_t full_step_short_upper = (y1 > y0) ? MAKE_STEP(x1 - x0, y1 - y0) : 0;
        if (step_long < full_step_short_upper ||
            (y1 == y0 && long_at_y1 <= short_start)) {
            lx = long_at_y1; ls = step_long; rx = short_start; rs = step_short;
        } else {
            lx = short_start; ls = step_short; rx = long_at_y1; rs = step_long;
        }
        for (int32_t y = y1; y < y2; y++) {
            int32_t xl = UNFP(lx);
            int32_t xr = UNFP(rx);
            for (int32_t x = xl; x < xr; x++)
                raster_pixel(x, y, color);
            lx += ls; rx += rs;
        }
    }

    #undef FP_ONE
    #undef MAKE_FP
    #undef MAKE_STEP
    #undef UNFP
}

/* Execute mono triangle (GP0 0x20-0x23) */
static void gp0_exec_mono_tri(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t vx[3], vy[3];
    for (int i = 0; i < 3; i++) {
        parse_vertex(gp0_cmd_buf[1 + i], &vx[i], &vy[i]);
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
}

/* Execute mono quad (GP0 0x28-0x2B) — two triangles: (0,1,2) and (2,1,3) */
static void gp0_exec_mono_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t vx[4], vy[4];
    for (int i = 0; i < 4; i++) {
        parse_vertex(gp0_cmd_buf[1 + i], &vx[i], &vy[i]);
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
    sw_draw_flat_triangle(vx[2], vy[2], vx[1], vy[1], vx[3], vy[3], color);
}

/* Execute shaded triangle (GP0 0x30-0x33) — Gouraud shaded */
static void gp0_exec_shaded_tri(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[3], vy[3];
    uint16_t c[3];
    /* Layout: C0, V0, C1, V1, C2, V2 */
    for (int i = 0; i < 3; i++) {
        c[i] = rgb888_to_rgb555(gp0_cmd_buf[i * 2] & 0xFFFFFFu);
        parse_vertex(gp0_cmd_buf[1 + i * 2], &vx[i], &vy[i]);
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_gouraud_triangle(vx[0], vy[0], c[0],
                             vx[1], vy[1], c[1],
                             vx[2], vy[2], c[2]);
}

void gpu_arm_shaded_quad_capture(void) { sq_cap_armed = 1; sq_cap_count = 0; }
int  gpu_get_shaded_quad_capture(const GpuSqCapEntry** out) {
    sq_cap_armed = 0; /* disarm on read */
    *out = sq_cap_buf;
    return sq_cap_count;
}

/* Execute shaded quad (GP0 0x38-0x3B) */
static void gp0_exec_shaded_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[4], vy[4];
    uint16_t c[4];
    /* Layout: C0, V0, C1, V1, C2, V2, C3, V3 */
    for (int i = 0; i < 4; i++) {
        c[i] = rgb888_to_rgb555(gp0_cmd_buf[i * 2] & 0xFFFFFFu);
        parse_vertex(gp0_cmd_buf[1 + i * 2], &vx[i], &vy[i]);
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    /* Capture vertex data when armed. */
    if (sq_cap_armed && sq_cap_count < SQ_CAP_MAX) {
        GpuSqCapEntry* e = &sq_cap_buf[sq_cap_count++];
        for (int i = 0; i < 4; i++) {
            e->vx[i] = vx[i]; e->vy[i] = vy[i];
            e->color[i] = gp0_cmd_buf[i * 2] & 0xFFFFFFu;
        }
    }
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_gouraud_triangle(vx[0], vy[0], c[0],
                             vx[1], vy[1], c[1],
                             vx[2], vy[2], c[2]);
    sw_draw_gouraud_triangle(vx[2], vy[2], c[2],
                             vx[1], vy[1], c[1],
                             vx[3], vy[3], c[3]);
}

/* Helper: build texpage word from GPU state for SW renderer.
 * Format: bits 0-3 = X base, bit 4 = Y base, bits 5-6 = semi-trans, bits 7-8 = color depth */
static uint16_t current_texpage(void) {
    return (uint16_t)(texpage_x | (texpage_y << 4) |
                      (semi_transparency << 5) | (texpage_colors << 7));
}

/* Helper: set up SW renderer state before a textured draw.
 * semi_trans: whether the primitive has semi-transparency enabled (from opcode bit) */
static void setup_textured_draw(uint32_t color24, int semi_trans) {
    uint32_t r = (color24 >> 0) & 0xFF;
    uint32_t g = (color24 >> 8) & 0xFF;
    uint32_t b = (color24 >> 16) & 0xFF;
    int raw = (r == 0x80 && g == 0x80 && b == 0x80) ? 1 : 0;
    sw_set_color_modulation((int)r, (int)g, (int)b, raw);
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
}

/* Execute textured triangle (GP0 0x24-0x27) */
static void gp0_exec_textured_tri(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[3], vy[3];
    int u[3], v[3];
    /* Layout: color+v0, texcoord0+clut, v1, texcoord1+tpage, v2, texcoord2 */
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[3], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[5], &vx[2], &vy[2]);
    u[0] = gp0_cmd_buf[2] & 0xFF;        v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[4] & 0xFF;        v[1] = (gp0_cmd_buf[4] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[6] & 0xFF;        v[2] = (gp0_cmd_buf[6] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    /* Texpage from word 4 bits 16-31, or use current state */
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[4] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;

    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    setup_textured_draw(color24, semi_trans);
    sw_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
}

/* Execute textured quad (GP0 0x2C-0x2F) */
static void gp0_exec_textured_quad(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[4], vy[4];
    int u[4], v[4];
    /* Layout: color+v0, tc0+clut, v1, tc1+tpage, v2, tc2, v3, tc3 */
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[3], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[5], &vx[2], &vy[2]);
    parse_vertex(gp0_cmd_buf[7], &vx[3], &vy[3]);
    u[0] = gp0_cmd_buf[2] & 0xFF;  v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[4] & 0xFF;  v[1] = (gp0_cmd_buf[4] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[6] & 0xFF;  v[2] = (gp0_cmd_buf[6] >> 8) & 0xFF;
    u[3] = gp0_cmd_buf[8] & 0xFF;  v[3] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage = (uint16_t)(gp0_cmd_buf[4] >> 16) & 0x1FF;

    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    setup_textured_draw(color24, semi_trans);
    sw_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
    sw_draw_textured_triangle(vx[2], vy[2], u[2], v[2],
                              vx[1], vy[1], u[1], v[1],
                              vx[3], vy[3], u[3], v[3],
                              clut_x, clut_y, tpage);
}

/* Execute shaded textured triangle (GP0 0x34-0x37) */
static void gp0_exec_shaded_textured_tri(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[3], vy[3];
    int u[3], v[3];
    /* Layout: C0, V0, TC0+clut, C1, V1, TC1+tpage, C2, V2, TC2 */
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[4], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[7], &vx[2], &vy[2]);
    u[0] = gp0_cmd_buf[2] & 0xFF;  v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[5] & 0xFF;  v[1] = (gp0_cmd_buf[5] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[8] & 0xFF;  v[2] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage = (uint16_t)(gp0_cmd_buf[5] >> 16) & 0x1FF;

    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    setup_textured_draw(color24, semi_trans);
    sw_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
}

/* Execute shaded textured quad (GP0 0x3C-0x3F) */
static void gp0_exec_shaded_textured_quad(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[4], vy[4];
    int u[4], v[4];
    /* Layout: C0, V0, TC0+clut, C1, V1, TC1+tpage, C2, V2, TC2, C3, V3, TC3 */
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[4], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[7], &vx[2], &vy[2]);
    parse_vertex(gp0_cmd_buf[10], &vx[3], &vy[3]);
    u[0] = gp0_cmd_buf[2] & 0xFF;   v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[5] & 0xFF;   v[1] = (gp0_cmd_buf[5] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[8] & 0xFF;   v[2] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    u[3] = gp0_cmd_buf[11] & 0xFF;  v[3] = (gp0_cmd_buf[11] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage = (uint16_t)(gp0_cmd_buf[5] >> 16) & 0x1FF;

    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    setup_textured_draw(color24, semi_trans);
    sw_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
    sw_draw_textured_triangle(vx[2], vy[2], u[2], v[2],
                              vx[1], vy[1], u[1], v[1],
                              vx[3], vy[3], u[3], v[3],
                              clut_x, clut_y, tpage);
}

/* Execute mono line (GP0 0x40-0x47) — Bresenham */
static void gp0_exec_mono_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    parse_vertex(gp0_cmd_buf[2], &x1, &y1);
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_line(x0, y0, x1, y1, color);
}

/* Execute shaded line (GP0 0x50-0x57) — flat shaded for now */
static void gp0_exec_shaded_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    /* Shaded line: C0, V0, C1, V1 */
    parse_vertex(gp0_cmd_buf[3], &x1, &y1);
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_line(x0, y0, x1, y1, color);
}

/* Execute mono rectangle (GP0 0x60-0x63) */
static void gp0_exec_mono_rect(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    x0 += draw_offset_x; y0 += draw_offset_y;
    int w = gp0_cmd_buf[2] & 0xFFFFu;
    int h = (gp0_cmd_buf[2] >> 16) & 0xFFFFu;
    if (w > 1023) w = 1023;
    if (h > 511)  h = 511;
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_flat_rect(x0, y0, w, h, color);
}

/* Execute textured rectangle (GP0 0x64-0x67) */
static void gp0_exec_textured_rect(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    int w = gp0_cmd_buf[3] & 0x3FF;
    int h = (gp0_cmd_buf[3] >> 16) & 0x1FF;
    if (w > 1023) w = 1023;
    if (h > 511)  h = 511;

    setup_textured_draw(color24, semi_trans);
    sw_draw_textured_rect(x0, y0, w, h, u0, v0, clut_x, clut_y, current_texpage());
}

/* Execute 1x1 dot (GP0 0x68-0x6B) */
static void gp0_exec_mono_dot(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x, y;
    parse_vertex(gp0_cmd_buf[1], &x, &y);
    x += draw_offset_x; y += draw_offset_y;
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_flat_rect(x, y, 1, 1, color);
}

/* Execute 8x8 textured sprite (GP0 0x74-0x77) */
static void gp0_exec_textured_8x8(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    setup_textured_draw(color24, semi_trans);
    sw_draw_textured_rect(x0, y0, 8, 8, u0, v0, clut_x, clut_y, current_texpage());
}

/* Execute 8x8 sprite (GP0 0x70-0x73) */
static void gp0_exec_mono_8x8(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    x0 += draw_offset_x; y0 += draw_offset_y;
    sw_set_semi_transparency(semi_trans, (int)semi_transparency);
    sw_draw_flat_rect(x0, y0, 8, 8, color);
}

/* Execute 16x16 textured sprite (GP0 0x7C-0x7F) */
static void gp0_exec_textured_16x16(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    setup_textured_draw(color24, semi_trans);
    sw_draw_textured_rect(x0, y0, 16, 16, u0, v0, clut_x, clut_y, current_texpage());
}

/* ---- GP0 command execution ---- */

static void gp0_exec_nop(void) {
    /* 0x00 / 0x01 — nothing to do */
}

static void gp0_exec_fill_rect(void) {
    /* 0x02 — Fill Rectangle in VRAM
     * Word 0: 0x02BBGGRR (color)
     * Word 1: YstartXstart (X bits 0-9 aligned to 16, Y bits 16-24)
     * Word 2: YsizXsiz (W bits 0-9 rounded up to 16, H bits 16-24) */
    uint32_t color24 = gp0_cmd_buf[0] & 0x00FFFFFF;
    uint16_t color16 = rgb888_to_rgb555(color24);

    uint32_t dst_x = gp0_cmd_buf[1] & 0x3F0u;          /* X masked to 16-pixel alignment */
    uint32_t dst_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;
    uint32_t width = ((gp0_cmd_buf[2] & 0x3FFu) + 0xFu) & ~0xFu;  /* round up to 16 */
    uint32_t height = (gp0_cmd_buf[2] >> 16) & 0x1FFu;

    /* Fill ignores draw area, mask bits, and draw offset — writes directly to VRAM */
    for (uint32_t row = 0; row < height; row++) {
        uint32_t y = (dst_y + row) % 512;
        for (uint32_t col = 0; col < width; col++) {
            uint32_t x = (dst_x + col) % 1024;
            vram[y * 1024 + x] = color16;
        }
    }
}

static void gp0_exec_draw_mode(void) {
    /* 0xE1 — Draw Mode / Texpage
     * Bits 0-3: texpage X base
     * Bit 4: texpage Y base
     * Bits 5-6: semi-transparency mode
     * Bits 7-8: texture color mode
     * Bit 9: dither
     * Bit 10: draw to display area
     * Bit 11: texture disable (when allowed by GP1) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    texpage_x         = param & 0xF;
    texpage_y         = (param >> 4) & 1;
    semi_transparency = (param >> 5) & 3;
    texpage_colors    = (param >> 7) & 3;
    dither_enabled    = (param >> 9) & 1;
    draw_to_display   = (param >> 10) & 1;
    texture_disable   = (param >> 11) & 1;
    /* Sync semi-transparency to SW renderer — actual blending is per-primitive */
}

static void gp0_exec_texture_window(void) {
    /* 0xE2 — Texture Window
     * Bits 0-4: mask X (in 8-pixel steps)
     * Bits 5-9: mask Y
     * Bits 10-14: offset X
     * Bits 15-19: offset Y */
    texture_window_value = gp0_cmd_buf[0] & 0x000FFFFFu;
    sw_set_texture_window(texture_window_value);
}

static void gp0_exec_draw_area_tl(void) {
    /* 0xE3 — Set Drawing Area Top-Left
     * Bits 0-9: X
     * Bits 10-19: Y */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_area_left = param & 0x3FF;
    draw_area_top  = (param >> 10) & 0x3FF;
    sw_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
}

static void gp0_exec_draw_area_br(void) {
    /* 0xE4 — Set Drawing Area Bottom-Right
     * Bits 0-9: X
     * Bits 10-19: Y */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_area_right  = param & 0x3FF;
    draw_area_bottom = (param >> 10) & 0x3FF;
    sw_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
}

static void gp0_exec_draw_offset(void) {
    /* 0xE5 — Set Drawing Offset
     * Bits 0-10: X (signed 11-bit)
     * Bits 11-21: Y (signed 11-bit) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_offset_x = sign_extend(param & 0x7FFu, 11);
    draw_offset_y = sign_extend((param >> 11) & 0x7FFu, 11);
    sw_set_draw_offset(draw_offset_x, draw_offset_y);
}

static void gp0_exec_mask_bit(void) {
    /* 0xE6 — Mask Bit Setting
     * Bit 0: set mask bit when drawing (force bit 15 of pixels)
     * Bit 1: check mask bit (don't draw to pixels with bit 15 set) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    set_mask_bit   = param & 1;
    check_mask_bit = (param >> 1) & 1;
    sw_set_mask_bits((int)set_mask_bit, (int)check_mask_bit);
}

static void gp0_exec_cpu_to_vram(void) {
    /* 0xA0 — CPU→VRAM Copy (header: 3 words)
     * Word 0: command
     * Word 1: destination coords (X bits 0-9, Y bits 16-24)
     * Word 2: dimensions (W bits 0-9, H bits 16-24)
     * Followed by pixel data words */
    vram_write_x = gp0_cmd_buf[1] & 0x3FFu;
    vram_write_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;

    uint32_t w = gp0_cmd_buf[2] & 0x3FFu;
    uint32_t h = (gp0_cmd_buf[2] >> 16) & 0x1FFu;
    /* 0 means max dimension */
    vram_write_w = (w == 0) ? 0x400 : (uint16_t)w;
    vram_write_h = (h == 0) ? 0x200 : (uint16_t)h;

    vram_write_col = 0;
    vram_write_row = 0;

    uint32_t num_pixels = (uint32_t)vram_write_w * (uint32_t)vram_write_h;
    vram_write_remaining = (num_pixels + 1) / 2;

    if (vram_write_remaining > 0)
        gp0_state = GP0_VRAM_WRITE;
}

static void gp0_exec_vram_to_cpu(void) {
    /* 0xC0 — VRAM→CPU Copy (3 words)
     * Word 1: source coords
     * Word 2: dimensions
     * After this, data is read via GPUREAD */
    vram_read_x = gp0_cmd_buf[1] & 0x3FFu;
    vram_read_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;

    uint32_t w = gp0_cmd_buf[2] & 0x3FFu;
    uint32_t h = (gp0_cmd_buf[2] >> 16) & 0x1FFu;
    vram_read_w = (w == 0) ? 0x400 : (uint16_t)w;
    vram_read_h = (h == 0) ? 0x200 : (uint16_t)h;

    /* DuckStation uses (val-1)&mask+1 for VRAM→CPU, but the effect
     * is identical: 0→max. We use ReplaceZero like CPU→VRAM. */

    vram_read_col = 0;
    vram_read_row = 0;
    vram_read_active = 1;
}

/* Determine how many words a GP0 command requires (header only, not counting
 * variable-length data for 0xA0). Returns -1 for polylines (terminated by
 * sentinel). Returns 0 for unknown commands (will be fatal). */
static int gp0_command_word_count(uint8_t opcode) {
    switch (opcode) {
        /* NOP / control */
        case 0x00: return 1;
        case 0x01: return 1;  /* clear cache */
        case 0x02: return 3;  /* fill rect */
        case 0x1F: return 1;  /* IRQ request */

        /* Drawing commands — polygons */
        case 0x20: case 0x21: case 0x22: case 0x23: return 4;  /* mono tri */
        case 0x24: case 0x25: case 0x26: case 0x27: return 7;  /* textured tri */
        case 0x28: case 0x29: case 0x2A: case 0x2B: return 5;  /* mono quad */
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: return 9;  /* textured quad */
        case 0x30: case 0x31: case 0x32: case 0x33: return 6;  /* shaded tri */
        case 0x34: case 0x35: case 0x36: case 0x37: return 9;  /* shaded textured tri */
        case 0x38: case 0x39: case 0x3A: case 0x3B: return 8;  /* shaded quad */
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: return 12; /* shaded textured quad */

        /* Lines */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: return 3;  /* mono line */
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: return -1; /* mono polyline */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: return 4;  /* shaded line */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: return -1; /* shaded polyline */

        /* Rectangles */
        case 0x60: case 0x61: case 0x62: case 0x63: return 3;  /* variable rect */
        case 0x64: case 0x65: case 0x66: case 0x67: return 4;  /* variable textured rect */
        case 0x68: case 0x69: case 0x6A: case 0x6B: return 2;  /* 1x1 dot */
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: return 3;  /* 1x1 textured */
        case 0x70: case 0x71: case 0x72: case 0x73: return 2;  /* 8x8 rect */
        case 0x74: case 0x75: case 0x76: case 0x77: return 3;  /* 8x8 textured */
        case 0x78: case 0x79: case 0x7A: case 0x7B: return 2;  /* 16x16 rect */
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: return 3;  /* 16x16 textured */

        /* VRAM copy / transfer */
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: return 4;  /* VRAM→VRAM */

        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: return 3;  /* CPU→VRAM (header) */

        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF: return 3;  /* VRAM→CPU (header) */

        /* Environment */
        case 0xE0: return 1;  /* NOP */
        case 0xE1: return 1;  /* draw mode */
        case 0xE2: return 1;  /* texture window */
        case 0xE3: return 1;  /* draw area TL */
        case 0xE4: return 1;  /* draw area BR */
        case 0xE5: return 1;  /* draw offset */
        case 0xE6: return 1;  /* mask bits */

        default:
            /* 0x03-0x1E, 0xE7-0xEF, 0xFF: NOP (1 word) per DuckStation */
            if ((opcode >= 0x03 && opcode <= 0x1E) ||
                (opcode >= 0xE7 && opcode <= 0xEF) ||
                opcode == 0xFF) {
                return 1;
            }
            return 0;  /* unknown */
    }
}

/* Per-opcode execution counters (exposed via gpu_get_opcode_stats) */
static uint32_t gp0_opcode_count[256];

uint32_t gpu_get_opcode_count(uint8_t op) { return gp0_opcode_count[op]; }

/* Execute a fully-collected GP0 command */
static void gp0_execute_command(void) {
    uint8_t opcode = (gp0_cmd_buf[0] >> 24) & 0xFF;
    gp0_opcode_count[opcode]++;

    /* Categorize for diagnostics */
    if (opcode <= 0x01) gp0_nop_count++;
    else if (opcode == 0x02) gp0_fill_count++;
    else if (opcode >= 0x20 && opcode <= 0x7F) gp0_draw_count++;
    else if (opcode >= 0x80 && opcode <= 0xDF) gp0_copy_count++;
    else if (opcode >= 0xE1 && opcode <= 0xE6) gp0_env_count++;

    switch (opcode) {
        case 0x00:
        case 0x01:
            gp0_exec_nop();
            break;

        case 0x02:
            gp0_exec_fill_rect();
            break;

        case 0xE1:
            gp0_exec_draw_mode();
            break;

        case 0xE2:
            gp0_exec_texture_window();
            break;

        case 0xE3:
            gp0_exec_draw_area_tl();
            break;

        case 0xE4:
            gp0_exec_draw_area_br();
            break;

        case 0xE5:
            gp0_exec_draw_offset();
            break;

        case 0xE6:
            gp0_exec_mask_bit();
            break;

        /* Drawing commands — polygons */
        case 0x20: case 0x21: case 0x22: case 0x23:
            gp0_exec_mono_tri();
            break;
        case 0x24: case 0x25: case 0x26: case 0x27:
            gp0_exec_textured_tri();
            break;
        case 0x28: case 0x29: case 0x2A: case 0x2B:
            gp0_exec_mono_quad();
            break;
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            gp0_exec_textured_quad();
            break;
        case 0x30: case 0x31: case 0x32: case 0x33:
            gp0_exec_shaded_tri();
            break;
        case 0x34: case 0x35: case 0x36: case 0x37:
            gp0_exec_shaded_textured_tri();
            break;
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            gp0_exec_shaded_quad();
            break;
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            gp0_exec_shaded_textured_quad();
            break;

        /* Lines */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            gp0_exec_mono_line();
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            gp0_exec_shaded_line();
            break;

        /* Rectangles */
        case 0x60: case 0x61: case 0x62: case 0x63:
            gp0_exec_mono_rect();
            break;
        case 0x64: case 0x65: case 0x66: case 0x67:
            gp0_exec_textured_rect();
            break;
        case 0x68: case 0x69: case 0x6A: case 0x6B:
            gp0_exec_mono_dot();
            break;
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
            /* 1x1 textured dot: cmd, vertex, texcoord+clut (no size word) */
            uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
            int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
            int32_t x0, y0;
            parse_vertex(gp0_cmd_buf[1], &x0, &y0);
            x0 += draw_offset_x; y0 += draw_offset_y;
            int u0 = gp0_cmd_buf[2] & 0xFF;
            int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
            uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
            uint16_t clut_x = (clut & 0x3F) * 16;
            uint16_t clut_y = (clut >> 6) & 0x1FF;
            setup_textured_draw(color24, semi_trans);
            sw_draw_textured_rect(x0, y0, 1, 1, u0, v0, clut_x, clut_y, current_texpage());
            break;
        }
        case 0x70: case 0x71: case 0x72: case 0x73:
            gp0_exec_mono_8x8();
            break;
        case 0x74: case 0x75: case 0x76: case 0x77:
            gp0_exec_textured_8x8();
            break;
        case 0x78: case 0x79: case 0x7A: case 0x7B: {
            /* 16x16 mono sprite */
            int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
            uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
            int32_t x0, y0;
            parse_vertex(gp0_cmd_buf[1], &x0, &y0);
            x0 += draw_offset_x; y0 += draw_offset_y;
            sw_set_semi_transparency(semi_trans, (int)semi_transparency);
            sw_draw_flat_rect(x0, y0, 16, 16, color);
            break;
        }
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            gp0_exec_textured_16x16();
            break;

        /* VRAM→VRAM copy */
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
            /* Word 1: source coords, Word 2: dest coords, Word 3: dimensions */
            int src_x = gp0_cmd_buf[1] & 0x3FF;
            int src_y = (gp0_cmd_buf[1] >> 16) & 0x1FF;
            int dst_x = gp0_cmd_buf[2] & 0x3FF;
            int dst_y = (gp0_cmd_buf[2] >> 16) & 0x1FF;
            int w = gp0_cmd_buf[3] & 0x3FF;
            int h = (gp0_cmd_buf[3] >> 16) & 0x1FF;
            if (w == 0) w = 0x400;
            if (h == 0) h = 0x200;
            sw_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
            break;
        }

        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            gp0_exec_cpu_to_vram();
            break;

        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            gp0_exec_vram_to_cpu();
            break;

        default:
            /* NOP range: 0x03-0x1E, 0xE0, 0xE7-0xEF, 0xFF */
            if ((opcode >= 0x03 && opcode <= 0x1E) ||
                opcode == 0xE0 ||
                (opcode >= 0xE7 && opcode <= 0xEF) ||
                opcode == 0xFF) {
                break;  /* NOP — silently consume */
            }

            /* Any other command (drawing, VRAM-to-VRAM, etc.) is not yet
             * implemented. Fatal exit so we know exactly what's needed next. */
            {
                FILE* cf = fopen("psx_crash.txt", "w");
                if (cf) {
                    fprintf(cf, "GPU GP0 unimplemented command 0x%02X (word 0x%08X)\n",
                            opcode, gp0_cmd_buf[0]);
                    fclose(cf);
                }
            }
            exit(1);
    }
}

/* ---- GP0 write (0x1F801810 write) — command state machine ---- */

uint64_t gpu_get_gp0_count(void) { return gp0_write_count; }

void gpu_get_gp0_stats(uint64_t* nop, uint64_t* fill, uint64_t* draw, uint64_t* env, uint64_t* copy) {
    *nop = gp0_nop_count; *fill = gp0_fill_count;
    *draw = gp0_draw_count; *env = gp0_env_count; *copy = gp0_copy_count;
}

void gpu_get_draw_area(GpuDrawArea* out) {
    out->left = draw_area_left;
    out->top = draw_area_top;
    out->right = draw_area_right;
    out->bottom = draw_area_bottom;
    out->offset_x = draw_offset_x;
    out->offset_y = draw_offset_y;
}

uint16_t gpu_vram_peek(int x, int y) {
    if (x < 0 || x >= 1024 || y < 0 || y >= 512) return 0;
    return vram[y * 1024 + x];
}

void gpu_write_gp0(uint32_t val) {
    gp0_write_count++;

    /* State: consuming pixel data for CPU→VRAM transfer */
    if (gp0_state == GP0_VRAM_WRITE) {
        /* Each word contains two 16-bit pixels (low halfword first) */
        for (int i = 0; i < 2; i++) {
            uint16_t pixel = (uint16_t)(val >> (i * 16));
            uint16_t wx = (vram_write_x + vram_write_col) % 1024;
            uint16_t wy = (vram_write_y + vram_write_row) % 512;

            /* Respect mask bit settings */
            if (check_mask_bit && (vram[wy * 1024 + wx] & 0x8000))
                goto next_pixel;

            if (set_mask_bit)
                pixel |= 0x8000;

            vram[wy * 1024 + wx] = pixel;

        next_pixel:
            if (++vram_write_col == vram_write_w) {
                vram_write_col = 0;
                if (++vram_write_row == vram_write_h) {
                    /* Transfer complete */
                    gp0_state = GP0_IDLE;
                    vram_write_remaining = 0;
                    return;
                }
            }
        }

        if (--vram_write_remaining == 0)
            gp0_state = GP0_IDLE;

        return;
    }

    /* State: collecting words for a multi-word command */
    if (gp0_state == GP0_COLLECTING) {
        gp0_cmd_buf[gp0_words_collected++] = val;
        if (gp0_words_collected >= gp0_words_needed) {
            gp0_state = GP0_IDLE;
            gp0_execute_command();
        }
        return;
    }

    /* State: IDLE — this is the first word of a new command */
    uint8_t opcode = (val >> 24) & 0xFF;
    int word_count = gp0_command_word_count(opcode);

    if (word_count == 0) {
        /* Unknown command — fatal */
        FILE* cf = fopen("psx_crash.txt", "w");
        if (cf) {
            fprintf(cf, "GPU GP0 unknown command 0x%02X (word 0x%08X)\n", opcode, val);
            fclose(cf);
        }
        exit(1);
    }

    if (word_count < 0) {
        /* Polyline — not yet implemented */
        FILE* cf = fopen("psx_crash.txt", "w");
        if (cf) {
            fprintf(cf, "GPU GP0 polyline command 0x%02X not implemented\n", opcode);
            fclose(cf);
        }
        exit(1);
    }

    gp0_cmd_buf[0] = val;

    if (word_count == 1) {
        gp0_words_collected = 1;
        gp0_words_needed = 1;
        gp0_execute_command();
    } else {
        gp0_state = GP0_COLLECTING;
        gp0_words_collected = 1;
        gp0_words_needed = word_count;
    }
}

/* ---- GP1 write (0x1F801814 write) ---- */

static void gp1_reset(void) {
    /* GP1(00h): Reset GPU — clears FIFO, resets all state, display off */
    gpu_init();
}

static void gp1_reset_command_buffer(void) {
    /* GP1(01h): Reset command buffer — clears FIFO, aborts current command */
    gp0_state = GP0_IDLE;
    gp0_words_collected = 0;
    gp0_words_needed = 0;
    vram_write_remaining = 0;
}

static void gp1_ack_irq1(void) {
    /* GP1(02h): Acknowledge IRQ1 */
    irq1_flag = 0;
}

static void gp1_display_enable(uint32_t val) {
    /* GP1(03h): Display enable — bit 0: 0=on, 1=off */
    display_disabled = val & 1;
}

static void gp1_dma_direction(uint32_t val) {
    /* GP1(04h): DMA direction — bits 0-1 */
    dma_direction = val & 3;
}

static void gp1_display_area_start(uint32_t val) {
    /* GP1(05h): Start of display area in VRAM
     * bits 0-9: X (in halfwords, 0-1023)
     * bits 10-18: Y (0-511) */
    display_area_x = val & 0x3FF;
    display_area_y = (val >> 10) & 0x1FF;
}

static void gp1_h_display_range(uint32_t val) {
    /* GP1(06h): Horizontal display range
     * bits 0-11: X1
     * bits 12-23: X2 */
    h_display_x1 = val & 0xFFF;
    h_display_x2 = (val >> 12) & 0xFFF;
}

static void gp1_v_display_range(uint32_t val) {
    /* GP1(07h): Vertical display range
     * bits 0-9: Y1
     * bits 10-19: Y2 */
    v_display_y1 = val & 0x3FF;
    v_display_y2 = (val >> 10) & 0x3FF;
}

static void gp1_display_mode(uint32_t val) {
    /* GP1(08h): Display mode
     * bit 0-1: horizontal resolution 1 (0=256, 1=320, 2=512, 3=640)
     * bit 2: vertical resolution (0=240, 1=480)
     * bit 3: video mode (0=NTSC, 1=PAL)
     * bit 4: display area color depth (0=15bit, 1=24bit)
     * bit 5: vertical interlace (0=off, 1=on)
     * bit 6: horizontal resolution 2 (0=normal, 1=368)
     * bit 7: "reverseflag" */
    hres1 = val & 3;
    vres = (val >> 2) & 1;
    video_mode = (val >> 3) & 1;
    display_depth = (val >> 4) & 1;
    vertical_interlace = (val >> 5) & 1;
    hres2 = (val >> 6) & 1;
    reverse_flag = (val >> 7) & 1;
}

static void gp1_get_info(uint32_t val) {
    /* GP1(10h): Get GPU info — writes result to GPUREAD latch.
     * DuckStation masks subcommand to 3 bits (val & 0x07). */
    uint32_t which = val & 0x07;
    switch (which) {
        case 0: case 1: case 6: case 7:
            /* Leave GPUREAD latch unchanged (per DuckStation) */
            break;
        case 2: /* texture window */
            gpuread_latch = texture_window_value;
            break;
        case 3: /* draw area top-left */
            gpuread_latch = draw_area_left | (draw_area_top << 10);
            break;
        case 4: /* draw area bottom-right */
            gpuread_latch = draw_area_right | (draw_area_bottom << 10);
            break;
        case 5: /* draw offset */
            gpuread_latch = ((uint32_t)draw_offset_x & 0x7FFu) |
                            (((uint32_t)draw_offset_y & 0x7FFu) << 11);
            break;
    }
}

void gpu_write_gp1(uint32_t val) {
    uint32_t cmd = (val >> 24) & 0x3F;

    switch (cmd) {
        case 0x00: gp1_reset(); break;
        case 0x01: gp1_reset_command_buffer(); break;
        case 0x02: gp1_ack_irq1(); break;
        case 0x03: gp1_display_enable(val); break;
        case 0x04: gp1_dma_direction(val); break;
        case 0x05: gp1_display_area_start(val); break;
        case 0x06: gp1_h_display_range(val); break;
        case 0x07: gp1_v_display_range(val); break;
        case 0x08: gp1_display_mode(val); break;
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            gp1_get_info(val); break;
        default:
            fprintf(stderr, "GPU GP1 unknown command 0x%02X (word 0x%08X)\n", cmd, val);
            fflush(stderr);
            exit(1);
    }
}
