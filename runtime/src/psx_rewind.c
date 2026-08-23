/* psx_rewind.c — local snap ring + filmstrip overlay. */

#include "psx_rewind.h"

#include "boot_state.h"
#include "cdrom.h"
#include "gpu.h"
#include "host_keymap.h"
#include "host_osd.h"
#include "interrupts.h"
#include "mdec.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_netplay_rb.h"
#include "psx_ram.h"
#include "psx_scheduler.h"
#include "savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "psx_sdl.h"

/* Amortized snapshot compression (logic near do_capture below). */
#define RW_ZSLICE (1u * 1024u * 1024u)
static struct {
    int      active;
    uint32_t tick;
    uint8_t *raw;   size_t raw_len; size_t in_pos;
    uint8_t *out;   size_t out_cap;
    z_stream zs;
} s_zpend;
static void rewind_pending_abort(void);
static int  rewind_pump_compress(size_t budget);

#if defined(PSX_HAS_RBENGINE_SNAP)
#include "retcomm_rbengine/snap_ring.h"
#endif

#define RW_THUMB_W     128
#define RW_THUMB_H      96
#define RW_MAX_DEPTH   200
#define RW_DEF_DEPTH    50
#define RW_DEF_INTERVAL 15
/* Soft defaults when 8 MB RAM is live and the user has not set prefs. */
#define RW_DEF_DEPTH_8MB    50
#define RW_DEF_INTERVAL_8MB 30
/* Match netplay §96 FMV media snaps (default 4; MEDIA_KF uses 2). */
#define RW_DEF_FMV_INTERVAL 4
/* ...but NOT at 8 MB, for the same reason RW_DEF_INTERVAL_8MB exists, only more
 * so. An 8 MB snapshot is ~10 MiB raw, and do_capture streams the deflate at
 * RW_ZSLICE (1 MiB) per vblank — so a single capture needs ~10 vblanks to drain
 * and the next one is skipped while s_zpend is active. Asking for one every 4
 * frames therefore never achieves 4: it just pins the compressor at 100% duty
 * on the emu thread for the whole movie. Measured on the WipEout 3 ntscfull8
 * intro: 17% of FMV frame rate, for rewind granularity the pump cannot deliver.
 * PSX_REWIND_FMV_INTERVAL still overrides for anyone who wants denser snaps. */
#define RW_DEF_FMV_INTERVAL_8MB 30
#define RW_FMV_MDEC_HYSTERESIS 8u
/* Latch sources, for PSX_REWIND_DIAG attribution. */
#define RW_FMV_SRC_DEPTH24 1
#define RW_FMV_SRC_MDEC    2
#define RW_FMV_SRC_XA      3
/* Cap the streaming deflate at ~1/RW_FMV_DUTY of the emulation thread. */
#define RW_FMV_DUTY 3u
#define RW_PANEL_W     640
#define RW_PANEL_H     176
#define RW_SLIDE_MS    180u

#if defined(PSX_HAS_RBENGINE_SNAP)
/* Public-domain 8x8 ASCII 32..90 (subset of font8x8_basic). */
static const uint8_t FONT8[59][8] = {
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
};
#endif

#if !defined(PSX_HAS_RBENGINE_SNAP)

void psx_rewind_set_depth(uint32_t depth) { (void)depth; }
void psx_rewind_set_interval(uint32_t interval) { (void)interval; }
void psx_rewind_configure(uint32_t bios_checksum, uint32_t entry_pc)
{
    (void)bios_checksum;
    (void)entry_pc;
}
void psx_rewind_shutdown(void) {}
int  psx_rewind_enabled(void) { return 0; }
int  psx_rewind_is_open(void) { return 0; }
int  psx_rewind_needs_present(void) { return 0; }
void psx_rewind_note_frame(void) {}
void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc)
{
    (void)cpu;
    (void)resume_pc;
}
int  psx_rewind_toggle(void) { return 0; }
int  psx_rewind_cancel(void) { return 0; }
int  psx_rewind_accept(void) { return 0; }
void psx_rewind_move(int delta) { (void)delta; }
void psx_rewind_nav_held(int left_down, int right_down, int accept_down,
                         int cancel_down, uint32_t now_ms)
{
    (void)left_down; (void)right_down; (void)accept_down;
    (void)cancel_down; (void)now_ms;
}
void psx_rewind_present_tick(uint32_t now_ms) { (void)now_ms; }
int  psx_rewind_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (pixels) *pixels = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}
float psx_rewind_slide(void) { return 0.f; }

#else /* PSX_HAS_RBENGINE_SNAP */

typedef struct RwThumb {
    uint32_t tick;
    uint32_t px[RW_THUMB_W * RW_THUMB_H];
} RwThumb;

static int s_enabled = -1;
static uint32_t s_bios;
static uint32_t s_entry;
static uint32_t s_interval = RW_DEF_INTERVAL;
static uint32_t s_fmv_interval = RW_DEF_FMV_INTERVAL;
static uint32_t s_depth = RW_DEF_DEPTH;
static int s_depth_pref = -1; /* -1 = unset; else from settings.toml / launcher */
static int s_interval_pref = -1;
static uint32_t s_frame;
/* Raw (pre-deflate) size of the last snapshot — sizes the pump-drain floor. */
static size_t   s_last_raw_len;
/* Completed captures, for the PSX_REWIND_DIAG rate line. */
static uint32_t s_capture_count;
/* Set when PSX_REWIND_FMV_INTERVAL was given explicitly — bypasses the floor. */
static int      s_fmv_interval_explicit;
/* Free-running vblank counter and the last value the deflate advanced on. */
static uint32_t s_vblank_seq;
static uint32_t s_pump_seq = 0xffffffffu;
static uint32_t s_last_capture_frame = 0xffffffffu;
static int s_capture_due;
static int s_configured;

static RbeSnapRing *s_ring;
static RwThumb *s_thumbs;
static uint32_t *s_ticks;
static uint32_t s_count;
static int s_sel;

static int s_open;
static float s_slide;
static uint32_t s_anim_t0;
static int s_anim_dir; /* +1 opening, -1 closing, 0 idle */
static uint32_t s_panel[RW_PANEL_W * RW_PANEL_H];
static int s_panel_dirty = 1;

static int s_load_pending;
static uint32_t s_load_tick;

static int s_left_was, s_right_was, s_acc_was, s_can_was;
static uint32_t s_rep_next;
static int s_rep_dir;

static uint32_t env_u32(const char *name, uint32_t def, uint32_t lo, uint32_t hi)
{
    const char *e = getenv(name);
    unsigned v;
    if (!e || !e[0])
        return def;
    v = (unsigned)atoi(e);
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

/* UI offers 50/100/150/200; nearest match keeps hand-edited toml values sane. */
static uint32_t normalize_rewind_depth(uint32_t v)
{
    static const uint32_t opts[4] = {50u, 100u, 150u, 200u};
    uint32_t best = opts[0];
    uint32_t best_d = v > best ? v - best : best - v;
    unsigned i;
    for (i = 1; i < 4; i++) {
        uint32_t d = v > opts[i] ? v - opts[i] : opts[i] - v;
        if (d < best_d) {
            best_d = d;
            best = opts[i];
        }
    }
    return best;
}

/* UI offers 1/4/8/12/15/30 (30 softens 8 MB capture cost). */
static uint32_t normalize_rewind_interval(uint32_t v)
{
    static const uint32_t opts[6] = {1u, 4u, 8u, 12u, 15u, 30u};
    uint32_t best = opts[0];
    uint32_t best_d = v > best ? v - best : best - v;
    unsigned i;
    for (i = 1; i < 6; i++) {
        uint32_t d = v > opts[i] ? v - opts[i] : opts[i] - v;
        if (d < best_d) {
            best_d = d;
            best = opts[i];
        }
    }
    return best;
}

void psx_rewind_set_depth(uint32_t depth)
{
    s_depth_pref = (int)normalize_rewind_depth(depth);
    /* Apply immediately if prefs are already resolved (reconfigure path). */
    if (s_enabled >= 0)
        s_depth = (uint32_t)s_depth_pref;
}

void psx_rewind_set_interval(uint32_t interval)
{
    s_interval_pref = (int)normalize_rewind_interval(interval);
    if (s_enabled >= 0)
        s_interval = (uint32_t)s_interval_pref;
}

static int rewind_wanted(void)
{
    const char *e;
    if (s_enabled < 0) {
        e = getenv("PSX_REWIND");
        if (e && e[0] == '0')
            s_enabled = 0;
        else
            s_enabled = 1;
        {
            uint32_t iv_def;
            if (s_interval_pref > 0)
                iv_def = (uint32_t)s_interval_pref;
            else
                iv_def = RW_DEF_INTERVAL;
            /* 8 MB soft default as a FLOOR, not an else-branch. main.cpp
             * always pushes g_rewind_interval (settings.toml always carries
             * rewind_interval, and it defaults to RW_DEF_INTERVAL), so
             * s_interval_pref is always > 0 and the old else-if made
             * RW_DEF_INTERVAL_8MB unreachable dead code — 8 MB captured
             * every 15 frames, ~4x the bytes of a 2 MB snap, measured at
             * 5-7% of frame time in WipEout 3 ntscfull8. PSX_REWIND_INTERVAL
             * still overrides below for anyone who wants denser snaps. */
            if (psx_ram_8mb_active() && iv_def < RW_DEF_INTERVAL_8MB)
                iv_def = RW_DEF_INTERVAL_8MB;
            s_interval = normalize_rewind_interval(
                env_u32("PSX_REWIND_INTERVAL", iv_def, 1u, 60u));
        }
        {
            uint32_t fmv_def = RW_DEF_FMV_INTERVAL;
            if (psx_ram_8mb_active() && fmv_def < RW_DEF_FMV_INTERVAL_8MB)
                fmv_def = RW_DEF_FMV_INTERVAL_8MB;
            s_fmv_interval =
                env_u32("PSX_REWIND_FMV_INTERVAL", fmv_def, 1u, 60u);
            /* An explicit env pick wins over the drain floor, the same way the
             * 8 MB defaults above are floors on the DEFAULT only. */
            s_fmv_interval_explicit = getenv("PSX_REWIND_FMV_INTERVAL") ? 1 : 0;
        }
        {
            uint32_t depth_def;
            if (s_depth_pref > 0)
                depth_def = (uint32_t)s_depth_pref;
            else if (psx_ram_8mb_active())
                depth_def = RW_DEF_DEPTH_8MB;
            else
                depth_def = RW_DEF_DEPTH;
            s_depth = normalize_rewind_depth(
                env_u32("PSX_REWIND_DEPTH", depth_def, 4u, RW_MAX_DEPTH));
        }
    }
    return s_enabled;
}

/* Same heuristic as netplay rb_fmv_media_active — denser snaps while media runs.
 * Returns the latching source so PSX_REWIND_DIAG can name it: XA in particular
 * stays asserted for a game that streams its BGM off the disc, which is NOT a
 * movie and must not be treated as one (see rewind_capture_interval). */
static int rewind_fmv_media_active(void)
{
    if (gpu_display_is_depth24())
        return RW_FMV_SRC_DEPTH24;
    if (mdec_recently_active(RW_FMV_MDEC_HYSTERESIS))
        return RW_FMV_SRC_MDEC;
    if (cdrom_xa_stream_active())
        return RW_FMV_SRC_XA;
    return 0;
}

/* Vblanks the streaming pump needs to drain one snapshot through deflate, at
 * RW_ZSLICE per frame. Capturing faster than this cannot work: do_capture
 * refuses while s_zpend.active, so the only thing a shorter interval buys is a
 * deflate that never stops running on the emulation thread. */
static uint32_t rewind_drain_frames(void)
{
    size_t len = s_last_raw_len ? s_last_raw_len : (size_t)(2u << 20);
    uint32_t frames = (uint32_t)((len + RW_ZSLICE - 1u) / RW_ZSLICE);
    return frames ? frames : 1u;
}

static uint32_t rewind_capture_interval(void)
{
    uint32_t fmv;
    if (!rewind_fmv_media_active())
        return s_interval;
    /* FMV densifies toward s_fmv_interval but never sparser than the user pick. */
    fmv = s_fmv_interval;
    /* ...and never denser than the pump can actually sustain. RW_DEF_FMV_INTERVAL
     * is 4, but a 2 MB-mode snapshot is ~3.7 MB (RAM + VRAM + SPU RAM), which is
     * ~4 vblanks of RW_ZSLICE on its own — so "every 4 frames" pinned deflate at
     * 100% duty on the emulation thread for as long as the heuristic latched.
     * Street Fighter Alpha 3 streams its BGM as XA, so cdrom_xa_stream_active()
     * held it latched through ordinary gameplay and cost ~3.5 ms of every 16.7 ms
     * frame. RW_DEF_FMV_INTERVAL_8MB=30 was the same realisation applied to 8 MB
     * only; deriving the floor from the measured snapshot size covers every mode.
     * RW_FMV_DUTY keeps the pump under ~1/3 duty. */
    if (!s_fmv_interval_explicit) {
        const uint32_t floor_iv = rewind_drain_frames() * RW_FMV_DUTY;
        if (fmv < floor_iv)
            fmv = floor_iv;
    }
    return s_interval < fmv ? s_interval : fmv;
}

static int resume_pc_ok(uint32_t pc)
{
    return pc != 0u && (pc & 3u) == 0u && psx_is_dispatchable(pc) &&
           pc != 0x80000080u && pc != 0xbfc00180u && pc != 0x80000000u;
}

/* Mid-FMV / present-edge IRQ paths often pass hint=0 — same latch walk as
 * savestate_resolve_resume_pc / selfcheck sc_pick_snap_pc. */
static uint32_t rewind_resolve_resume_pc(const CPUState *cpu, uint32_t hint)
{
    const uint32_t cands[6] = {
        hint,
        cpu ? cpu->pc : 0u,
        psx_compiled_irq_resume_pc(),
        psx_last_irq_check_pc(),
        psx_netplay_rb_sticky_bb_pc(),
        cpu ? cpu->gpr[31] : 0u,
    };
    int i;
    for (i = 0; i < 6; ++i) {
        if (resume_pc_ok(cands[i]))
            return cands[i];
    }
    return 0u;
}

static void capture_thumb(uint32_t *dst)
{
    GpuDisplayInfo di;
    uint32_t dw, dh, x, y;
    gpu_get_display_info(&di);
    dw = di.width ? di.width : 320u;
    dh = di.height ? di.height : 240u;
    if (di.disabled) {
        memset(dst, 0, (size_t)RW_THUMB_W * RW_THUMB_H * sizeof(uint32_t));
        return;
    }
    for (y = 0; y < RW_THUMB_H; y++) {
        uint32_t sy = y * dh / RW_THUMB_H;
        for (x = 0; x < RW_THUMB_W; x++) {
            uint32_t sx = x * dw / RW_THUMB_W;
            dst[y * RW_THUMB_W + x] =
                gpu_display_pixel_argb(&di, sx, sy) | 0xFF000000u;
        }
    }
}

static void list_drop_after(uint32_t tick)
{
    uint32_t i, n = 0;
    for (i = 0; i < s_count; i++) {
        if (s_ticks[i] <= tick) {
            if (n != i) {
                s_ticks[n] = s_ticks[i];
                s_thumbs[n] = s_thumbs[i];
            }
            n++;
        }
    }
    s_count = n;
    if (s_sel >= (int)s_count)
        s_sel = s_count ? (int)s_count - 1 : 0;
    s_panel_dirty = 1;
}

static void list_push(uint32_t tick, const uint32_t *thumb)
{
    uint32_t i;
    for (i = 0; i < s_count; i++) {
        if (s_ticks[i] == tick) {
            memcpy(s_thumbs[i].px, thumb, sizeof(s_thumbs[i].px));
            s_thumbs[i].tick = tick;
            s_panel_dirty = 1;
            return;
        }
    }
    if (s_count >= s_depth) {
        memmove(s_ticks, s_ticks + 1, (s_count - 1u) * sizeof(s_ticks[0]));
        memmove(s_thumbs, s_thumbs + 1, (s_count - 1u) * sizeof(s_thumbs[0]));
        s_count--;
        if (s_sel > 0)
            s_sel--;
    }
    s_ticks[s_count] = tick;
    s_thumbs[s_count].tick = tick;
    memcpy(s_thumbs[s_count].px, thumb, sizeof(s_thumbs[0].px));
    s_count++;
    s_sel = (int)s_count - 1;
    s_panel_dirty = 1;
}

void psx_rewind_configure(uint32_t bios_checksum, uint32_t entry_pc)
{
    if (!rewind_wanted())
        return;
    psx_rewind_shutdown();
    s_bios = bios_checksum;
    s_entry = entry_pc;
    s_ring = rbe_snap_ring_create(s_depth);
    s_thumbs = (RwThumb *)calloc(s_depth, sizeof(RwThumb));
    s_ticks = (uint32_t *)calloc(s_depth, sizeof(uint32_t));
    if (!s_ring || !s_thumbs || !s_ticks) {
        psx_rewind_shutdown();
        fprintf(stderr, "psxrecomp: rewind alloc failed — disabled\n");
        s_enabled = 0;
        return;
    }
    s_configured = 1;
    s_frame = 0;
    s_last_capture_frame = 0xffffffffu;
    s_count = 0;
    s_sel = 0;
    fprintf(stderr,
            "psxrecomp: rewind on (interval=%u fmv=%u depth=%u ~%.1fs) — "
            "F8 / View / L3\n",
            (unsigned)s_interval, (unsigned)s_fmv_interval, (unsigned)s_depth,
            (double)s_interval * (double)s_depth / 60.0);
}

void psx_rewind_shutdown(void)
{
    rewind_pending_abort();
    if (s_ring)
        rbe_snap_ring_destroy(s_ring);
    free(s_thumbs);
    free(s_ticks);
    s_ring = NULL;
    s_thumbs = NULL;
    s_ticks = NULL;
    s_count = 0;
    s_configured = 0;
    s_open = 0;
    s_slide = 0.f;
    s_anim_dir = 0;
    s_load_pending = 0;
    s_capture_due = 0;
}

int psx_rewind_enabled(void)
{
    return rewind_wanted() && s_configured && s_ring != NULL;
}

int psx_rewind_is_open(void)
{
    return s_open;
}

int psx_rewind_needs_present(void)
{
    return s_open || s_slide > 0.01f || s_anim_dir != 0;
}

/* Async VRAM readback pre-arm (GL backend). Kicked one frame before a capture
 * is due so the GPU->PBO copy overlaps a full frame; consumed in do_capture.
 * 0 on non-GL backends / missing extension — capture falls back to the
 * synchronous gr_vram_transfer_out path. */
extern int gl_renderer_vram_readback_begin(void);
extern int gl_renderer_vram_readback_finish(uint16_t *dst);
static int       s_vram_prearmed;
static uint16_t *s_vram_async;   /* 1 MiB, lazily allocated */

void psx_rewind_note_frame(void)
{
    uint32_t iv;
    if (!psx_rewind_enabled())
        return;
    /* Paces the streaming deflate (psx_rewind_poll). Counted BEFORE the
     * open/netplay bail: s_frame is the ring tick and must not advance while
     * the filmstrip is up or netplay owns the state, but an already-in-flight
     * compression still has to drain — throttling it on s_frame would strand
     * s_zpend.active (and its ~7 MB of buffers) until the guest resumed. */
    s_vblank_seq++;
    if (s_open || psx_netplay_determinism_active())
        return;
    s_frame++;
    iv = rewind_capture_interval();
    /* PSX_REWIND_DIAG=1: one line per second naming the densify latch and the
     * interval it resolved to. This is how the SFA3 "XA BGM reads as FMV"
     * case is identified — src=xa with a densified interval during a fight. */
    {
        static int diag = -1;
        static uint32_t last_s, cap_at_last;
        if (diag < 0) diag = getenv("PSX_REWIND_DIAG") ? 1 : 0;
        if (diag) {
            uint32_t now_s = (uint32_t)SDL_GetTicks() / 1000u;
            if (now_s != last_s) {
                static const char *src_name[4] = {
                    "none", "depth24", "mdec", "xa"
                };
                int src = rewind_fmv_media_active();
                fprintf(stderr,
                    "[REWIND] f=%u src=%s interval=%u (user=%u fmv=%u "
                    "drain=%u) raw=%zu captures/s=%u\n",
                    s_frame, src_name[src & 3], iv, s_interval, s_fmv_interval,
                    rewind_drain_frames(), s_last_raw_len,
                    s_capture_count - cap_at_last);
                last_s = now_s;
                cap_at_last = s_capture_count;
            }
        }
    }
    if (s_last_capture_frame == 0xffffffffu ||
        s_frame - s_last_capture_frame >= iv)
        s_capture_due = 1;
    else if (iv >= 2 && s_frame - s_last_capture_frame == iv - 1u &&
             !s_zpend.active && !s_vram_prearmed)
        s_vram_prearmed = gl_renderer_vram_readback_begin();
}

/* ---- Amortized snapshot compression --------------------------------------
 * The old capture ran boot_state_save_buffer (serialize + zlib, Z_BEST_SPEED)
 * synchronously on the emulation thread. At 8 MB main RAM that is ~9.5 MiB
 * through deflate ≈ 20-30 ms in ONE frame, every capture interval — a 2 Hz
 * judder that a 1-second FPS average completely hides (measured with
 * PSX_HITCH_DIAG: guest=30-45 ms at exactly f=32/62/92 with interval=30;
 * rewind off ⇒ zero guest spikes >25 ms).
 *
 * Now the capture frame only RAW-serializes (boot_state_save_buffer_raw,
 * pure memcpy work, ~2-3 ms) and the deflate is streamed in ~1 MiB slices on
 * subsequent vblanks (~2 ms each) via rewind_pump_compress(). The finished
 * blob is an "RWZ1" envelope [magic][u32 raw_len][zlib stream] stored in the
 * ring; do_load transparently unwraps it. No worker thread: everything stays
 * on the emulation thread, so ring ordering and savestate interactions are
 * unchanged — do_load just finishes any in-flight compression first. While a
 * compression is in flight new captures wait (FMV densify at interval=4 will
 * effectively capture every ~raw_len/slice frames instead — acceptable).
 * (s_zpend / RW_ZSLICE are declared with the file-top forward decls so
 * psx_rewind_note_frame's prearm check can see them.) */

static void rewind_pending_abort(void)
{
    if (!s_zpend.active) return;
    deflateEnd(&s_zpend.zs);
    free(s_zpend.raw);
    free(s_zpend.out);
    memset(&s_zpend, 0, sizeof s_zpend);
}

/* Advance the in-flight compression by at most `budget` input bytes.
 * Returns 1 while still pending, 0 when idle/finished. The z_stream keeps its
 * own output cursor across calls; only the input is sliced. avail_out is the
 * full compressBound up front, so the terminal Z_FINISH always completes in a
 * single deflate call. */
static int rewind_pump_compress(size_t budget)
{
    if (!s_zpend.active) return 0;
    for (;;) {
        size_t remain = s_zpend.raw_len - s_zpend.in_pos;
        size_t chunk  = remain;
        int    flush  = Z_FINISH;
        if (chunk > budget) { chunk = budget; flush = Z_NO_FLUSH; }
        s_zpend.zs.next_in  = s_zpend.raw + s_zpend.in_pos;
        s_zpend.zs.avail_in = (uInt)chunk;
        int rc = deflate(&s_zpend.zs, flush);
        size_t consumed = chunk - s_zpend.zs.avail_in;
        s_zpend.in_pos += consumed;
        if (flush == Z_FINISH) {
            if (rc == Z_STREAM_END) {
                size_t zbytes  = (s_zpend.out_cap - 8u) - s_zpend.zs.avail_out;
                size_t out_len = 8u + zbytes;
                uint8_t *blob  = s_zpend.out;
                uint32_t tick  = s_zpend.tick;
                deflateEnd(&s_zpend.zs);
                free(s_zpend.raw);
                memset(&s_zpend, 0, sizeof s_zpend);
                if (!rbe_snap_ring_store(s_ring, tick, blob, out_len))
                    free(blob);
                return 0;
            }
            rewind_pending_abort();  /* bound-sized out: cannot happen */
            return 0;
        }
        if (rc != Z_OK) { rewind_pending_abort(); return 0; }
        budget -= consumed;
        if (budget == 0 || consumed == 0)
            return 1;
    }
}

static int do_capture(CPUState *cpu, uint32_t resume_pc)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    uint32_t thumb[RW_THUMB_W * RW_THUMB_H];
    CPUState snap;
    uint32_t tick = s_frame;
    uint32_t pc;

    if (!cpu || !s_ring)
        return 0;
    if (s_zpend.active)   /* previous snapshot still compressing */
        return 0;
    pc = rewind_resolve_resume_pc(cpu, resume_pc);
    if (!resume_pc_ok(pc))
        return 0;
    snap = *cpu;
    snap.pc = pc;
    {
        /* PSX_HITCH_DIAG: time the raw serialize itself — the zlib was only
         * part of the historical capture spike; the VRAM readback inside the
         * serializer is the other suspect. */
        static int diag = -1;
        if (diag < 0) diag = getenv("PSX_HITCH_DIAG") ? 1 : 0;
        unsigned long long t0 = diag ? SDL_GetPerformanceCounter() : 0;
        /* Consume the VRAM readback kicked one frame ago: the serializer then
         * memcpys from the mapped copy instead of draining the GL pipeline
         * with a synchronous glReadPixels (measured 8-17 ms per capture). */
        int vram_async = 0;
        if (s_vram_prearmed) {
            s_vram_prearmed = 0;
            if (!s_vram_async)
                s_vram_async = (uint16_t *)malloc(1024u * 512u * 2u);
            if (s_vram_async &&
                gl_renderer_vram_readback_finish(s_vram_async)) {
                boot_state_set_vram_override(s_vram_async);
                vram_async = 1;
            }
        }
        int ok = boot_state_save_buffer_raw(&snap, s_bios, s_entry, &blob, &len);
        if (vram_async)
            boot_state_set_vram_override(NULL);
        if (diag) {
            double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                        (double)SDL_GetPerformanceFrequency();
            if (ms > 4.0)
                fprintf(stderr, "[HITCH] rewind raw-serialize %.1fms (len=%zu)\n",
                        ms, len);
        }
        if (!ok || !blob || !len)
            return 0;
        s_last_raw_len = len;
    }
    /* Stage the streaming deflate; the envelope header goes in up front. */
    uLong bound = compressBound((uLong)len);
    uint8_t *out = (uint8_t *)malloc(8u + (size_t)bound);
    if (!out) { free(blob); return 0; }
    memcpy(out, "RWZ1", 4);
    out[4] = (uint8_t)(len);
    out[5] = (uint8_t)(len >> 8);
    out[6] = (uint8_t)(len >> 16);
    out[7] = (uint8_t)(len >> 24);
    memset(&s_zpend.zs, 0, sizeof s_zpend.zs);
    if (deflateInit(&s_zpend.zs, Z_BEST_SPEED) != Z_OK) {
        free(blob); free(out);
        return 0;
    }
    s_zpend.active   = 1;
    s_zpend.tick     = tick;
    s_zpend.raw      = blob;
    s_zpend.raw_len  = len;
    s_zpend.in_pos   = 0;
    s_zpend.out      = out;
    s_zpend.out_cap  = 8u + (size_t)bound;
    s_zpend.zs.next_out  = out + 8;
    s_zpend.zs.avail_out = (uInt)bound;
    capture_thumb(thumb);
    list_push(tick, thumb);
    s_last_capture_frame = s_frame;
    s_capture_due = 0;
    s_capture_count++;
    return 1;
}

static int do_load(CPUState *cpu, uint32_t tick)
{
    size_t size = 0;
    const uint8_t *data;

    if (!cpu || !s_ring)
        return 0;
    if (!psx_hle_scheduler_enabled()) {
        fprintf(stderr, "psxrecomp: rewind load needs HLE scheduler\n");
        return 0;
    }
    data = rbe_snap_ring_peek(s_ring, tick, &size);
    if (!data || !size)
        return 0;
    /* "RWZ1" envelope from the amortized capture: [magic][u32 raw_len][zlib].
     * Unwrap before handing to the section loader. Pre-envelope blobs (plain
     * boot_state streams) pass through unchanged. */
    if (size > 12 && memcmp(data, "RWZ1", 4) == 0) {
        size_t raw_len = (size_t)data[4] | ((size_t)data[5] << 8) |
                         ((size_t)data[6] << 16) | ((size_t)data[7] << 24);
        uint8_t *raw = raw_len ? (uint8_t *)malloc(raw_len) : NULL;
        uLongf dl = (uLongf)raw_len;
        if (!raw ||
            uncompress(raw, &dl, (const Bytef *)data + 8,
                       (uLong)(size - 8)) != Z_OK ||
            dl != raw_len) {
            free(raw);
            return 0;
        }
        int ok = boot_state_load_buffer(raw, raw_len, s_bios, s_entry, cpu);
        free(raw);
        if (!ok)
            return 0;
    } else if (!boot_state_load_buffer(data, size, s_bios, s_entry, cpu)) {
        return 0;
    }
    if (!resume_pc_ok(cpu->pc))
        return 0;
    rbe_snap_ring_drop_after(s_ring, tick);
    list_drop_after(tick);
    s_frame = tick;
    s_last_capture_frame = tick;
    psx_cycles_resync_after_restore(cpu);
    interrupts_resync_after_restore();
    cdrom_accelerate_after_savestate();
    psx_frontend_on_savestate_loaded();
    host_osd_push("Rewind loaded", 1200);
    psx_scheduler_resume_at(cpu->pc);
    return 1; /* not reached on success */
}

void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc)
{
    if (!psx_rewind_enabled())
        return;
    if (s_load_pending) {
        uint32_t tick = s_load_tick;
        s_load_pending = 0;
        s_open = 0;
        s_anim_dir = -1;
        /* Finish any in-flight snapshot compression synchronously so its tick
         * is in the ring (the thumbnail list already references it) before
         * the load peeks/drops. User-initiated, so the few ms are fine. */
        while (rewind_pump_compress((size_t)-1)) {}
        (void)do_load(cpu, tick);
        return;
    }
    /* Amortize the pending snapshot deflate: ~1 MiB (~2 ms) per vblank
     * instead of the whole ~9.5 MiB (~20-30 ms) in the capture frame.
     *
     * "per vblank" was the intent but nothing enforced it: psx_rewind_poll is
     * called from psx_check_interrupts, and while the fast paths there gate on
     * (++s_fast_maintenance & 0x3FFF) the general slow path calls it at EVERY
     * interrupt check. So the 1 MiB slice ran hundreds of times per frame and
     * the whole snapshot drained in one or two frames — re-creating the very
     * spike this streaming path exists to remove, only now charged to guest
     * time instead of host (SFA3: [HITCH] guest=30-45 ms, gprofng put zlib at
     * 31% of the emulation thread). Stamp the pump to one slice per guest
     * frame so the deflate is actually spread across the capture interval. */
    if (s_vblank_seq != s_pump_seq) {
        s_pump_seq = s_vblank_seq;
        (void)rewind_pump_compress(RW_ZSLICE);
    }
    if (s_capture_due && !s_open && !psx_netplay_determinism_active())
        (void)do_capture(cpu, resume_pc);
}

int psx_rewind_toggle(void)
{
    if (!psx_rewind_enabled())
        return 0;
    if (psx_netplay_determinism_active()) {
        host_osd_push("Rewind off during netplay", 1500);
        return 0;
    }
    if (s_open) {
        s_open = 0;
        s_anim_dir = -1;
        s_anim_t0 = 0u;
        return 1;
    }
    s_open = 1;
    s_anim_dir = 1;
    s_anim_t0 = 0u;
    /* Swallow currently-held Back/L3/A so open doesn't instantly cancel. */
    s_left_was = s_right_was = s_acc_was = s_can_was = 1;
    s_rep_dir = 0;
    if (s_count)
        s_sel = (int)s_count - 1;
    s_panel_dirty = 1;
    host_osd_push(s_count ? "Rewind" : "Rewind (no snaps yet)", 1200);
    return 1;
}

int psx_rewind_cancel(void)
{
    if (!s_open)
        return 0;
    s_open = 0;
    s_anim_dir = -1;
    s_anim_t0 = 0u;
    return 1;
}

int psx_rewind_accept(void)
{
    if (!s_open || !s_count)
        return 0;
    s_load_tick = s_ticks[s_sel];
    s_load_pending = 1;
    s_open = 0;
    s_anim_dir = -1;
    s_anim_t0 = 0u;
    return 1;
}

void psx_rewind_move(int delta)
{
    int n;
    if (!s_open || !s_count || delta == 0)
        return;
    n = (int)s_count;
    s_sel += delta;
    if (s_sel < 0)
        s_sel = 0;
    if (s_sel >= n)
        s_sel = n - 1;
    s_panel_dirty = 1;
}

void psx_rewind_nav_held(int left_down, int right_down, int accept_down,
                         int cancel_down, uint32_t now_ms)
{
    int left_edge, right_edge, acc_edge, can_edge;
    int dir;

    if (!s_open)
        return;

    left_edge = left_down && !s_left_was;
    right_edge = right_down && !s_right_was;
    acc_edge = accept_down && !s_acc_was;
    can_edge = cancel_down && !s_can_was;
    s_left_was = left_down;
    s_right_was = right_down;
    s_acc_was = accept_down;
    s_can_was = cancel_down;

    if (can_edge) {
        psx_rewind_cancel();
        return;
    }
    if (acc_edge) {
        psx_rewind_accept();
        return;
    }
    if (left_edge) {
        psx_rewind_move(-1);
        s_rep_dir = -1;
        s_rep_next = now_ms + 280u;
    } else if (right_edge) {
        psx_rewind_move(+1);
        s_rep_dir = +1;
        s_rep_next = now_ms + 280u;
    } else {
        dir = left_down ? -1 : (right_down ? +1 : 0);
        if (dir && dir == s_rep_dir && now_ms >= s_rep_next) {
            psx_rewind_move(dir);
            s_rep_next = now_ms + 70u;
        } else if (!dir) {
            s_rep_dir = 0;
        }
    }
}

void psx_rewind_present_tick(uint32_t now_ms)
{
    float t;
    if (s_anim_dir == 0)
        return;
    if (s_anim_t0 == 0u)
        s_anim_t0 = now_ms ? now_ms : 1u;
    t = (float)(now_ms - s_anim_t0) / (float)RW_SLIDE_MS;
    if (t < 0.f)
        t = 0.f;
    if (t > 1.f)
        t = 1.f;
    /* ease-out */
    t = 1.f - (1.f - t) * (1.f - t);
    if (s_anim_dir > 0)
        s_slide = t;
    else
        s_slide = 1.f - t;
    if ((s_anim_dir > 0 && s_slide >= 0.999f) ||
        (s_anim_dir < 0 && s_slide <= 0.001f)) {
        s_slide = s_anim_dir > 0 ? 1.f : 0.f;
        s_anim_dir = 0;
        s_anim_t0 = 0u;
    }
}

float psx_rewind_slide(void)
{
    return s_slide;
}

static void blit_thumb(uint32_t *dst, int dw, int x0, int y0, int tw, int th,
                       const uint32_t *src, int sel)
{
    int x, y, dx, dy;
    uint32_t border = sel ? 0xFFFFD24Du : 0xFF3A3A3Au;
    int bw = sel ? 3 : 1;
    for (y = -bw; y < th + bw; y++) {
        dy = y0 + y;
        if (dy < 0 || dy >= RW_PANEL_H)
            continue;
        for (x = -bw; x < tw + bw; x++) {
            dx = x0 + x;
            if (dx < 0 || dx >= dw)
                continue;
            if (x < 0 || y < 0 || x >= tw || y >= th)
                dst[dy * dw + dx] = border;
        }
    }
    for (y = 0; y < th; y++) {
        dy = y0 + y;
        if (dy < 0 || dy >= RW_PANEL_H)
            continue;
        for (x = 0; x < tw; x++) {
            int sx = x * RW_THUMB_W / tw;
            int sy = y * RW_THUMB_H / th;
            dx = x0 + x;
            if (dx < 0 || dx >= dw)
                continue;
            dst[dy * dw + dx] = src[sy * RW_THUMB_W + sx];
        }
    }
}

static void glyph_fill_rect(uint32_t *dst, int dw, int x0, int y0,
                            int w, int h, uint32_t col)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > dw) w = dw - x0;
    if (y0 + h > RW_PANEL_H) h = RW_PANEL_H - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            dst[y * dw + x] = col;
}

static void glyph_fill_disc(uint32_t *dst, int dw, int cx, int cy,
                            int r, uint32_t col)
{
    int x, y;
    const int rr = r * r;
    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            if (x * x + y * y > rr) continue;
            glyph_fill_rect(dst, dw, cx + x, cy + y, 1, 1, col);
        }
    }
}

static void glyph_line(uint32_t *dst, int dw, int x0, int y0, int x1, int y1,
                       int thickness, uint32_t col)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int inset = thickness / 2;

    for (;;) {
        glyph_fill_rect(dst, dw, x0 - inset, y0 - inset, thickness, thickness,
                        col);
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = err * 2;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static void draw_psx_button(uint32_t *dst, int dw, int x, int y, char kind)
{
    switch (kind) {
    case 'x':
        glyph_line(dst, dw, x + 4, y + 4, x + 14, y + 14, 2, 0xFF5FA8FFu);
        glyph_line(dst, dw, x + 14, y + 4, x + 4, y + 14, 2, 0xFF5FA8FFu);
        break;
    case 'o':
        glyph_fill_disc(dst, dw, x + 9, y + 9, 8, 0xFFFF6B6Bu);
        glyph_fill_disc(dst, dw, x + 9, y + 9, 5, 0xE0101218u);
        break;
    default:
        glyph_fill_rect(dst, dw, x + 7, y + 2, 5, 15, 0xFFD7DCE6u);
        glyph_fill_rect(dst, dw, x + 2, y + 7, 15, 5, 0xFFD7DCE6u);
        break;
    }
}

static void draw_char(uint32_t *dst, int dw, int x0, int y0, char c, uint32_t col)
{
    const uint8_t *g;
    int x, y;
    if (c < 32 || c > 90)
        c = '?';
    g = FONT8[(int)c - 32];
    for (y = 0; y < 8; y++) {
        uint8_t row = g[y];
        int dy = y0 + y;
        if (dy < 0 || dy >= RW_PANEL_H)
            continue;
        for (x = 0; x < 8; x++) {
            int dx;
            if ((row & (1u << x)) == 0)
                continue;
            dx = x0 + x;
            if (dx < 0 || dx >= dw)
                continue;
            dst[dy * dw + dx] = col;
        }
    }
}

static void draw_text(uint32_t *dst, int dw, int x0, int y0, const char *s,
                      uint32_t col)
{
    int x = x0;
    if (!s)
        return;
    while (*s) {
        char c = *s++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        draw_char(dst, dw, x, y0, c, col);
        x += 8;
    }
}

static void rasterize_panel(void)
{
    uint32_t *d = s_panel;
    int i, n, card_w, card_h, gap, x, y, sel_w, sel_h;
    int fy;
    int center_x, origin;
    char buf[96];
    char hk[32];

    for (i = 0; i < RW_PANEL_W * RW_PANEL_H; i++)
        d[i] = 0xE0101218u;

    draw_text(d, RW_PANEL_W, 12, 8, "REWIND", 0xFFFFD24Du);
    if (s_count) {
        snprintf(buf, sizeof(buf), "%d / %u   F%u",
                 s_sel + 1, (unsigned)s_count,
                 (unsigned)s_ticks[s_sel]);
        draw_text(d, RW_PANEL_W, 120, 8, buf, 0xFFE8E8E8u);
    } else {
        draw_text(d, RW_PANEL_W, 120, 8, "NO SNAPSHOTS YET", 0xFF888888u);
    }
    host_keymap_label(HOST_KEYMAP_REWIND, hk, sizeof(hk));
    fy = RW_PANEL_H - 22;
    draw_psx_button(d, RW_PANEL_W, 12, fy, 'd');
    draw_text(d, RW_PANEL_W, 36, fy + 5, "SEEK", 0xFFE2E5EBu);
    draw_psx_button(d, RW_PANEL_W, 116, fy, 'x');
    draw_text(d, RW_PANEL_W, 140, fy + 5, "LOAD", 0xFFE2E5EBu);
    draw_psx_button(d, RW_PANEL_W, 216, fy, 'o');
    draw_text(d, RW_PANEL_W, 240, fy + 5, "CLOSE", 0xFFE2E5EBu);
    snprintf(buf, sizeof(buf), "%s MENU", hk[0] ? hk : "F8");
    draw_text(d, RW_PANEL_W, 512, fy + 5, buf, 0xFFAAAAAAu);

    n = (int)s_count;
    if (n <= 0) {
        s_panel_dirty = 0;
        return;
    }

    card_w = 96;
    card_h = 72;
    sel_w = 120;
    sel_h = 90;
    gap = 10;
    center_x = RW_PANEL_W / 2;
    origin = center_x - (s_sel * (card_w + gap)) - sel_w / 2;

    y = 28;
    for (i = 0; i < n; i++) {
        int w = (i == s_sel) ? sel_w : card_w;
        int h = (i == s_sel) ? sel_h : card_h;
        int yy = (i == s_sel) ? (y - 8) : y;
        x = origin + i * (card_w + gap);
        if (i > s_sel)
            x += (sel_w - card_w);
        if (x + w < 0 || x >= RW_PANEL_W)
            continue;
        blit_thumb(d, RW_PANEL_W, x, yy, w, h, s_thumbs[i].px, i == s_sel);
    }
    s_panel_dirty = 0;
}

int psx_rewind_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (!psx_rewind_needs_present() || s_slide <= 0.01f) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (s_panel_dirty)
        rasterize_panel();
    if (pixels) *pixels = s_panel;
    if (w) *w = RW_PANEL_W;
    if (h) *h = RW_PANEL_H;
    return 1;
}

#endif /* PSX_HAS_RBENGINE_SNAP */
