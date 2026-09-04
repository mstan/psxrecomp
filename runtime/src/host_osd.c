/* host_osd.c — top-left toast messages for host hotkeys / savestate results.
 *
 * Visual OSD is gated on RECOMP_LAUNCHER (recomp-ui bundled). Volume state
 * remains available for headless / launcher-less builds. */

#include "host_osd.h"
#include "psx_rewind.h"
#include "psx_savestate_menu.h"
#include "psx_sdl.h"

#include <stdio.h>
#include <string.h>

#if defined(RECOMP_LAUNCHER)
#define HOST_OSD_VISUAL 1
#else
#define HOST_OSD_VISUAL 0
#endif

#define OSD_MAX_CHARS  64
#define OSD_PAD_X      2
#define OSD_PAD_Y      2
#define OSD_SCALE      2
#define OSD_DEFAULT_MS 2000
#define OSD_GLYPH_W    8
#define OSD_GLYPH_H    8

/* Public-domain 8x8 ASCII (32..126), row bitmasks LSB = left pixel.
 * Sourced from the font8x8_basic set (https://github.com/d7samurai/font8x8). */
static const uint8_t FONT8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x7F,0x00}, /* E */
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x06,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x06,0x06,0x06,0x06,0x06,0x06,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x06,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0f,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

static char     s_msg[OSD_MAX_CHARS];
static Uint32   s_expire_ms;
static int      s_active;
static int      s_img_dirty = 1;

static char     s_status_msg[OSD_MAX_CHARS];
static int      s_status_active;
static int      s_status_dirty = 1;

static int      s_volume = 100;          /* host master 0..100 */
static int      s_vol_pct = 100;         /* last shown bar fill */
static Uint32   s_vol_expire_ms;
static int      s_vol_active;
static int      s_vol_dirty = 1;

static int      s_needs_clear;

#define OSD_IMG_W  ((OSD_PAD_X * 2 + OSD_MAX_CHARS * OSD_GLYPH_W) * OSD_SCALE)
#define OSD_IMG_H  ((OSD_PAD_Y * 2 + OSD_GLYPH_H) * OSD_SCALE)
static uint32_t s_img[OSD_IMG_W * OSD_IMG_H];
static int      s_img_w;
static int      s_img_h;

/* Vertical bar: outer 16x120 @ 2x → 32x240. Fill grows from the bottom. */
#define VOL_BAR_W   (16 * OSD_SCALE)
#define VOL_BAR_H   (120 * OSD_SCALE)
#define VOL_BORDER  (2 * OSD_SCALE)
static uint32_t s_vol_img[VOL_BAR_W * VOL_BAR_H];
static int      s_vol_w = VOL_BAR_W;
static int      s_vol_h = VOL_BAR_H;

#ifndef PSX_SDL_NO_RENDER
static SDL_Texture *s_sdl_tex;
static int          s_sdl_tw;
static int          s_sdl_th;
static SDL_Texture *s_sdl_vol_tex;
static int          s_sdl_vol_tw;
static int          s_sdl_vol_th;
static SDL_Texture *s_sdl_rw_tex;
static int          s_sdl_rw_tw;
static int          s_sdl_rw_th;
static SDL_Texture *s_sdl_ssm_tex;
static int          s_sdl_ssm_tw;
static int          s_sdl_ssm_th;
static SDL_Renderer *s_sdl_ren;
#endif

static int clamp_pct(int p) {
    if (p < 0) return 0;
    if (p > 100) return 100;
    return p;
}

static int msg_visible(void) {
    if (!s_active) return 0;
    if ((int32_t)(SDL_GetTicks() - s_expire_ms) >= 0) {
        s_active = 0;
        s_msg[0] = '\0';
        s_needs_clear = 1;
        s_img_dirty = 1;
        return 0;
    }
    return 1;
}

static int vol_visible(void) {
    if (!s_vol_active) return 0;
    if ((int32_t)(SDL_GetTicks() - s_vol_expire_ms) >= 0) {
        s_vol_active = 0;
        s_needs_clear = 1;
        s_vol_dirty = 1;
        return 0;
    }
    return 1;
}

static void rasterize_text(const char *msg) {
    int n = (int)strlen(msg);
    if (n > OSD_MAX_CHARS) n = OSD_MAX_CHARS;
    s_img_w = (OSD_PAD_X * 2 + n * OSD_GLYPH_W) * OSD_SCALE;
    s_img_h = (OSD_PAD_Y * 2 + OSD_GLYPH_H) * OSD_SCALE;
    if (s_img_w < 1) s_img_w = 1;
    if (s_img_h < 1) s_img_h = 1;
    if (s_img_w > OSD_IMG_W) s_img_w = OSD_IMG_W;
    if (s_img_h > OSD_IMG_H) s_img_h = OSD_IMG_H;

    for (int i = 0; i < s_img_w * s_img_h; i++)
        s_img[i] = 0xFF202020u;

    for (int ci = 0; ci < n; ci++) {
        unsigned char ch = (unsigned char)msg[ci];
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = FONT8X8[ch - 32];
        for (int row = 0; row < OSD_GLYPH_H; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < OSD_GLYPH_W; col++) {
                if (!(bits & (1u << col))) continue;
                int x0 = (OSD_PAD_X + ci * OSD_GLYPH_W + col) * OSD_SCALE;
                int y0 = (OSD_PAD_Y + row) * OSD_SCALE;
                for (int dy = 0; dy < OSD_SCALE; dy++) {
                    for (int dx = 0; dx < OSD_SCALE; dx++) {
                        int x = x0 + dx, y = y0 + dy;
                        if ((unsigned)x < (unsigned)s_img_w &&
                            (unsigned)y < (unsigned)s_img_h)
                            s_img[y * s_img_w + x] = 0xFFFFFFFFu;
                    }
                }
            }
        }
    }
    s_img_dirty = 0;
}

static void rasterize_volume(void) {
    s_vol_w = VOL_BAR_W;
    s_vol_h = VOL_BAR_H;
    for (int i = 0; i < s_vol_w * s_vol_h; i++)
        s_vol_img[i] = 0xFF202020u;

    /* Border (white). */
    for (int y = 0; y < s_vol_h; y++) {
        for (int x = 0; x < s_vol_w; x++) {
            const int edge = (x < VOL_BORDER || x >= s_vol_w - VOL_BORDER ||
                              y < VOL_BORDER || y >= s_vol_h - VOL_BORDER);
            if (edge) s_vol_img[y * s_vol_w + x] = 0xFFE0E0E0u;
        }
    }

    const int inner_w = s_vol_w - 2 * VOL_BORDER;
    const int inner_h = s_vol_h - 2 * VOL_BORDER;
    int fill_h = (inner_h * clamp_pct(s_vol_pct) + 50) / 100;
    if (fill_h < 0) fill_h = 0;
    if (fill_h > inner_h) fill_h = inner_h;
    const int y0 = VOL_BORDER + (inner_h - fill_h);
    for (int y = y0; y < VOL_BORDER + inner_h; y++) {
        for (int x = VOL_BORDER; x < VOL_BORDER + inner_w; x++)
            s_vol_img[y * s_vol_w + x] = 0xFFFFFFFFu;
    }
    s_vol_dirty = 0;
}

#ifndef PSX_SDL_NO_RENDER
static void sdl_blit_argb(SDL_Renderer *renderer, SDL_Texture **tex,
                          int *tw, int *th, const uint32_t *px, int w, int h,
                          int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!*tex || *tw != w || *th != h || renderer != s_sdl_ren) {
        if (*tex) SDL_DestroyTexture(*tex);
        *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, w, h);
        *tw = w;
        *th = h;
        if (*tex) SDL_SetTextureBlendMode(*tex, SDL_BLENDMODE_BLEND);
    }
    if (!*tex) return;
    void *locked = NULL;
    int pitch = 0;
#if defined(PSX_SDL3)
    const int lock_ok = SDL_LockTexture(*tex, NULL, &locked, &pitch);
#else
    const int lock_ok = (SDL_LockTexture(*tex, NULL, &locked, &pitch) == 0);
#endif
    if (lock_ok && locked) {
        uint8_t *dst = (uint8_t *)locked;
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * (size_t)pitch,
                   px + (size_t)y * (size_t)w,
                   (size_t)w * sizeof(uint32_t));
        SDL_UnlockTexture(*tex);
    }
    if (dst_w < 1) dst_w = w;
    if (dst_h < 1) dst_h = h;
    SDL_Rect dst = { dst_x, dst_y, dst_w, dst_h };
#if defined(PSX_SDL3)
    (void)psx_sdl_render_copy(renderer, *tex, NULL, &dst);
#else
    SDL_RenderCopy(renderer, *tex, NULL, &dst);
#endif
}

/* Present uses SDL_RenderSetLogicalSize — position/size in logical pixels,
 * not window/output pixels (GetRenderOutputSize mixed spaces and broke scale). */
static void sdl_logical_size(SDL_Renderer *renderer, int *lw, int *lh) {
    int w = 0, h = 0;
#if defined(PSX_SDL3)
    {
        SDL_RendererLogicalPresentation mode =
            SDL_LOGICAL_PRESENTATION_DISABLED;
        if (!SDL_GetRenderLogicalPresentation(renderer, &w, &h, &mode) ||
            w <= 0 || h <= 0)
            SDL_GetRenderOutputSize(renderer, &w, &h);
    }
#else
    SDL_RenderGetLogicalSize(renderer, &w, &h);
    if (w <= 0 || h <= 0)
        SDL_GetRendererOutputSize(renderer, &w, &h);
#endif
    if (lw) *lw = w > 0 ? w : 640;
    if (lh) *lh = h > 0 ? h : 480;
}
#endif

void host_osd_push(const char *msg, int duration_ms) {
#if !HOST_OSD_VISUAL
    (void)msg;
    (void)duration_ms;
    return;
#else
    if (!msg || !msg[0]) return;
    if (duration_ms <= 0) duration_ms = OSD_DEFAULT_MS;
    snprintf(s_msg, sizeof(s_msg), "%s", msg);
    s_expire_ms = SDL_GetTicks() + (Uint32)duration_ms;
    s_active = 1;
    s_needs_clear = 0;
    s_img_dirty = 1;
#endif
}

void host_osd_set_status(const char *msg) {
#if !HOST_OSD_VISUAL
    (void)msg;
    return;
#else
    if (!msg || !msg[0]) {
        if (s_status_active) s_needs_clear = 1;
        s_status_msg[0] = '\0';
        s_status_active = 0;
        s_status_dirty = 1;
        return;
    }
    snprintf(s_status_msg, sizeof(s_status_msg), "%s", msg);
    s_status_active = 1;
    s_status_dirty = 1;
    s_needs_clear = 0;
#endif
}

void host_osd_show_volume(int percent, int duration_ms) {
#if !HOST_OSD_VISUAL
    (void)percent;
    (void)duration_ms;
    return;
#else
    if (duration_ms <= 0) duration_ms = OSD_DEFAULT_MS;
    s_vol_pct = clamp_pct(percent);
    s_vol_expire_ms = SDL_GetTicks() + (Uint32)duration_ms;
    s_vol_active = 1;
    s_needs_clear = 0;
    s_vol_dirty = 1;
#endif
}

int host_volume_get(void) {
    return s_volume;
}

void host_volume_set(int percent) {
    s_volume = clamp_pct(percent);
}

int host_volume_adjust(int delta) {
    s_volume = clamp_pct(s_volume + delta);
#if HOST_OSD_VISUAL
    host_osd_show_volume(s_volume, OSD_DEFAULT_MS);
#endif
    return s_volume;
}

int host_osd_needs_present(void) {
    if (psx_rewind_needs_present()) return 1;
    if (psx_savestate_menu_needs_present()) return 1;
#if !HOST_OSD_VISUAL
    return 0;
#else
    if (msg_visible() || s_status_active || vol_visible()) return 1;
    return s_needs_clear;
#endif
}

int host_osd_image(const uint32_t **pixels, int *w, int *h) {
#if !HOST_OSD_VISUAL
    if (pixels) *pixels = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
#else
    const int show_msg = msg_visible();
    if (!show_msg && !s_status_active) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (show_msg) {
        if (s_img_dirty) rasterize_text(s_msg);
    } else {
        if (s_status_dirty || s_img_dirty) {
            rasterize_text(s_status_msg);
            s_status_dirty = 0;
        }
    }
    if (pixels) *pixels = s_img;
    if (w) *w = s_img_w;
    if (h) *h = s_img_h;
    return 1;
#endif
}

int host_osd_volume_image(const uint32_t **pixels, int *w, int *h) {
#if !HOST_OSD_VISUAL
    if (pixels) *pixels = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
#else
    if (!vol_visible()) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (s_vol_dirty) rasterize_volume();
    if (pixels) *pixels = s_vol_img;
    if (w) *w = s_vol_w;
    if (h) *h = s_vol_h;
    return 1;
#endif
}

void host_osd_present_done(void) {
#if !HOST_OSD_VISUAL
    return;
#else
    if (!s_active && !s_status_active && !s_vol_active) s_needs_clear = 0;
#endif
}

void host_osd_draw_sdl(struct SDL_Renderer *renderer) {
#ifndef PSX_SDL_NO_RENDER
    if (!renderer) return;
    s_sdl_ren = renderer;
#if HOST_OSD_VISUAL
    {
        const uint32_t *px;
        int w, h;
        int lw = 640, lh = 480;
        int ui, margin;
        sdl_logical_size(renderer, &lw, &lh);
        /* Bitmaps authored for ~480-tall logical; scale with supersampling. */
        ui = lh / 480;
        if (ui < 1) ui = 1;
        if (ui > 8) ui = 8;
        margin = 8 * ui;
        if (host_osd_image(&px, &w, &h) && px)
            sdl_blit_argb(renderer, &s_sdl_tex, &s_sdl_tw, &s_sdl_th, px, w, h,
                          margin, margin, w * ui, h * ui);
        if (host_osd_volume_image(&px, &w, &h) && px) {
            const int dw = w * ui, dh = h * ui;
            int x = (lw > dw + margin) ? (lw - dw - margin) : margin;
            int y = (lh > dh) ? ((lh - dh) / 2) : margin;
            sdl_blit_argb(renderer, &s_sdl_vol_tex, &s_sdl_vol_tw, &s_sdl_vol_th,
                          px, w, h, x, y, dw, dh);
        }
    }
    host_osd_present_done();
#endif
    {
        const uint32_t *px = NULL;
        int w = 0, h = 0;
        if (psx_rewind_overlay_image(&px, &w, &h) && px) {
            int lw = 640, lh = 480;
            float slide;
            int dw, dh, y;
            sdl_logical_size(renderer, &lw, &lh);
            slide = psx_rewind_slide();
            dw = lw;
            dh = (lh * h) / 480;
            if (dh < 8) dh = h;
            y = lh - (int)((float)dh * slide + 0.5f);
            sdl_blit_argb(renderer, &s_sdl_rw_tex, &s_sdl_rw_tw, &s_sdl_rw_th,
                          px, w, h, 0, y, dw, dh);
        }
        if (psx_savestate_menu_overlay_image(&px, &w, &h) && px) {
            int lw = 640, lh = 480;
            sdl_logical_size(renderer, &lw, &lh);
            sdl_blit_argb(renderer, &s_sdl_ssm_tex, &s_sdl_ssm_tw,
                          &s_sdl_ssm_th, px, w, h, 0, 0, lw, lh);
        }
    }
#else
    (void)renderer;
#endif /* PSX_SDL_NO_RENDER */
}
