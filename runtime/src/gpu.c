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
#include "gpu_render.h"
#include "text_xlate.h"
#include "crash_trace.h"
#include "debug_server.h"
#include "cpu_state.h"
#include "event_ring.h"
#include "color_lut.h"
#include "ws_cull_detect.h"
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
    GP0_VRAM_WRITE,
    GP0_POLYLINE_MONO,    /* collecting mono polyline vertices */
    GP0_POLYLINE_SHADED   /* collecting shaded polyline color+vertex pairs */
} Gp0State;

static Gp0State gp0_state;
static uint64_t gp0_write_count;
static uint64_t gp0_nop_count, gp0_fill_count, gp0_draw_count, gp0_env_count, gp0_copy_count;
static uint32_t gp0_cmd_buf[16];   /* max fixed-length command is 12 words */
static int      gp0_words_collected;
static int      gp0_words_needed;
static uint32_t gp0_next_source_addr = 0xFFFFFFFFu;
static uint32_t gp0_cmd_source_addr  = 0xFFFFFFFFu;

/* ---- Widescreen proportion correction --------------------------------------
 * Active only when [video] aspect_ratio != 4:3 AND the game's [widescreen]
 * block opts in (see config_loader.h). Two mechanisms, both driven from the
 * game.toml so nothing here is game-specific:
 *
 *  1. Tagged character/billboard prims. The recompiler emits a
 *     psx_ws_sprite_tag(cpu) call at the entry of each configured
 *     sprite-tag function ($a0 = prim pointer, scratchpad holds the prim's
 *     GTE-projected anchor). The game computes the prim's screen-pixel
 *     offsets/widths itself — which our GTE X-squash never sees — so tagged
 *     prims get every X re-squashed around their own anchor at execution
 *     time, restoring correct proportions on the stretched present.
 *
 *  2. Untagged textured rects (SPRT family). These never went through the
 *     GTE at all (HUD, menus — pure screen space), so they're squashed
 *     around the display centre, presenting at native proportions.
 *     Untextured TILEs are never touched: full-screen fades/flashes must
 *     keep covering the whole frame.
 *
 * The tag table is keyed by the prim's guest address (the DMA linked-list
 * walk reports each word's source address) and stamped with the frame
 * counter, so stale entries age out without an explicit clear. */
static int32_t  ws_xnum = 1, ws_xden = 1;   /* X squash factor; 1/1 = off */
static uint32_t ws_anchor_addr = 0;          /* scratchpad addr of anchor SXY */
static int      ws_hud_sprt = 0;             /* edge-anchor untagged HUD SPRTs */

/* Wide-aspect mode: 0 = off (4:3 identity), 1 = squash (legacy hack — compress
 * a wider FOV into the 320 frame, present stretched), 2 = native-wide (render
 * the wider FOV into an actually wider frame and present 1:1; the GTE is NOT
 * squashed — the frame is widened via draw_offset/draw_area and the display
 * read). ws_cfg_num/den hold the configured aspect that drives the wide extent
 * in native-wide mode. */
static int      ws_mode    = 0;
static int      ws_cfg_num = 4, ws_cfg_den = 3;

#define WS_TAG_BUCKETS 4096                  /* power of two */
#define WS_TAG_PROBES  8
#define WS_FMV_HYSTERESIS 30                 /* frames a colour MDEC decode pins 4:3 */
typedef struct { uint32_t key; uint32_t stamp; int32_t anchor_x; } WsTag;
static WsTag    ws_tags[WS_TAG_BUCKETS];
static uint32_t ws_last_tag_stamp = (uint32_t)-1000; /* frame of newest tag */
static uint32_t ws_last_3d_stamp  = (uint32_t)-1000; /* frame of newest shaded prim (diagnostic) */
extern uint64_t s_frame_count;               /* defined in debug_server.c */
extern int      mdec_recently_active(uint32_t within_frames);  /* mdec.c */

static int ws_configured(void) { return ws_xnum != ws_xden; }

/* Any wide mode engaged (squash or native-wide). Drives the FMV/menu 4:3
 * pillarbox + the game-vs-2D detector, which both modes share. */
static int ws_engaged(void) { return ws_mode != 0; }

/* Forward decls: defined later but used by psx_ws_backdrop_x above them. */
static int32_t ws_scale_about(int32_t x, int32_t ax);
static int32_t ws_disp_w(void);
static void ws_clear_all_reveal_margins(void);

/* Gameplay vs full-2D screen. Character/billboard prims tag (psx_ws_sprite_tag)
 * within the last couple of frames => the actor render funnel is running =>
 * gameplay. A menu/title/save screen draws no tagged prims. The 2-frame window
 * makes this frame-stable regardless of per-prim ordering within a frame. */
/* Full-2D widescreen ([widescreen] full_2d). A pure 2D sprite game (e.g. MMX6)
 * never emits the sprite-tag hook the 3D gameplay detector keys on, so every
 * in-game frame would be misclassified as a "full-2D menu" and pillarboxed 4:3.
 * When this is set, treat every in-game frame as gameplay so native-wide engages
 * and the 2D scene presents widescreen. Set once from game.toml at widescreen-
 * engage (gpu_ws_set_full_2d); PSX_WS_FORCE_2D=1 forces it on for testing. */
static int ws_full_2d = 0;
void gpu_ws_set_full_2d(int on) { ws_full_2d = on ? 1 : 0; }
static int ws_clear_reveal = 0;
static int g_mmx6_void_sides = 0;
static uint32_t g_mmx6_void_generation = 1;
void gpu_ws_set_clear_reveal(int on) { ws_clear_reveal = on ? 1 : 0; }

/* GTE-activity gameplay detector ([widescreen] gte_game_mode) — the generic
 * 3D-title analog of the sprite-tag stamp. A fully-3D game (e.g. Ape Escape)
 * has no per-prim tag helper to hook, but every gameplay frame projects a
 * meaningful number of vertices through RTPS/RTPT, while a full-2D screen
 * (save/options/memory-card) projects none (or a token few). gte.cpp notes
 * every projection here; when a frame accumulates >= the threshold the frame
 * is stamped as gameplay, with the same 2-frame hysteresis the tag path uses.
 * Off unless the game opts in, so existing titles are untouched. */
static int      ws_gte_game_mode_cfg = 0;
static uint32_t ws_gte_frame = (uint32_t)-1;
static uint32_t ws_gte_count = 0;
static uint32_t ws_last_gte_stamp = (uint32_t)-1000;
/* Any frame that projects a handful of vertices is "3D" (a low threshold so a
 * sparse close-up cutscene frame still counts — the flicker was frames dipping
 * below a high 16-vert bar and pillarboxing for a frame or two). */
#define WS_GTE_GAME_MODE_MIN_VERTS 3u
/* STICKY: stay in native-wide for ~0.75s after the last 3D frame, so brief
 * low-poly frames in a real-time 3D cutscene never flip to a 4:3 pillarbox (the
 * intro-cutscene flicker). Only a genuine full-2D screen — no GTE projection for
 * this many consecutive frames (save/options/memory-card) — reverts to 4:3. */
#define WS_GTE_GAME_MODE_HYSTERESIS 45u
void gpu_ws_set_gte_game_mode(int on) { ws_gte_game_mode_cfg = on ? 1 : 0; }

/* World-scale 3D signal for the 2D-only-scene classifier (sprite-tag titles).
 * Shaded-prim presence proved to be a FALSE world signal: task-clear /
 * new-task title cards are gouraud-shaded screen-space tiles animated over
 * many consecutive frames, which defeated both the shaded test and its
 * sustained guard (room re-stretched whenever a card played). Projection
 * volume can't be fooled that way: a real 3D world projects hundreds of
 * verts per frame through RTPS/RTPT, a room interior projects only its
 * character anchors (a handful), and screen-space overlays project ZERO.
 * Sustained rule as elsewhere: 2+ consecutive frames over threshold, so an
 * isolated projection burst can't flip a 2D scene wide. */
#define WS_WORLD3D_MIN_VERTS 48u
static uint32_t ws_gte_prev_verts     = 0;  /* final count of last completed frame */
static uint32_t ws_last_world3d_stamp = (uint32_t)-1000;
static uint32_t ws_sust_world3d_stamp = (uint32_t)-1000;

/* Natural-overhang world signal — the classifier's actual input. GTE volume
 * above proved to be a SECOND false world signal: the task-found/task-clear
 * jingle projects world-scale vert counts inside a room (stamped world3d at
 * ≥48 while the room idles at 4). What can't be faked is CONTENT: a real 3D
 * scene always submits polygons whose raw vertex X extent crosses outside
 * the canonical display window (that overhang is the very content native-
 * wide reveals — the working outdoor reveal proves it exists), while rooms,
 * dialogs, HUDs and jingle popups compose everything inside it. Polygons
 * only (sprites/rects slide in from edges routinely — the task icon enters
 * from the right edge), ≥3 prims per frame, sustained 2+ consecutive
 * frames. Counted at gp0_execute_command via prim_sx_extent (raw SX,
 * pre-draw_offset, so our native-wide offset injection can't feed back). */
/* Depth qualifier: only polys crossing DEEP past the canonical edge count.
 * Census-measured (SCUS-94236): outdoor terrain = 55/frame at >=24px (depths
 * to 128px); the task-jingle sparkles poke <=20px and its sliding icon is a
 * single prim at 34px — so >=24px depth + >=4 prims excludes every observed
 * screen-edge effect with ~14x headroom to the real world's count. */
#define WS_OVERHANG_DEEP_PX   24
#define WS_OVERHANG_MIN_PRIMS 4u
static uint32_t ws_ovh_frame      = (uint32_t)-1;
static uint32_t ws_ovh_count      = 0;
static uint32_t ws_ovh_prev       = 0;  /* final count of last completed frame */
static uint32_t ws_last_ovh_stamp = (uint32_t)-1000;
static uint32_t ws_sust_ovh_stamp = (uint32_t)-1000;

void psx_ws_note_gte_project(int nverts) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f != ws_gte_frame) {
        if (f == ws_gte_frame + 1u) ws_gte_prev_verts = ws_gte_count;
        else                        ws_gte_prev_verts = 0;
        ws_gte_frame = f; ws_gte_count = 0;
    }
    ws_gte_count += (uint32_t)nverts;
    if (ws_gte_game_mode_cfg && ws_gte_count >= WS_GTE_GAME_MODE_MIN_VERTS)
        ws_last_gte_stamp = f;
    if (ws_gte_count >= WS_WORLD3D_MIN_VERTS && ws_last_world3d_stamp != f) {
        if (f == ws_last_world3d_stamp + 1u) ws_sust_world3d_stamp = f;
        ws_last_world3d_stamp = f;
    }
}

/* Full-2D tile-engine mode (MMX6): config [widescreen] full_2d, or the PSX_WS_FORCE_2D
 * test override. Distinct from ws_game_mode (which also fires on the 3D sprite-tag path,
 * e.g. Tomba) — only true full-2D games get the BG tile-budget reveal cap. */
static int ws_full_2d_mode(void) {
    static int env = -1;
    if (env < 0) { const char *e = getenv("PSX_WS_FORCE_2D"); env = (e && e[0] == '1') ? 1 : 0; }
    return ws_full_2d || env;
}
static int ws_game_mode(void) {
    if (ws_full_2d_mode()) return 1;
    if (ws_gte_game_mode_cfg &&
        (uint32_t)s_frame_count - ws_last_gte_stamp <= WS_GTE_GAME_MODE_HYSTERESIS) return 1;
    return (uint32_t)s_frame_count - ws_last_tag_stamp <= 2;
}

/* True when the current frame is presented at native 4:3 (NOT stretched), so
 * ALL squash must be suppressed and the content rendered pixel-native:
 *   - FMV video (24-bit, or streamed 15-bit colour MDEC), and
 *   - full-2D screens (menus/title/save — no character billboards this frame).
 * Coupling squash to this exact predicate keeps content and present in lock-
 * step: we squash IFF we stretch. Per-prim center-squash mangles composite 2D
 * UI (dialog boxes built from tiled cap/middle pieces), so such screens get
 * zero squash + a 4:3 pillarbox instead. The FMV check is cached per frame;
 * game_mode is a cheap live check. */
/* 2D-only gameplay scenes (sprite-tag titles). A room interior or a sky-only
 * fall is gameplay-classified (the character prims tag every frame) but has
 * no world-scale GTE projection — there is no 3D world and nothing beyond
 * the canonical frame to reveal, so wide presents can only stretch flat art
 * (the backdrop stretch fires wholesale because the background phase never
 * ends). Present those scenes native 4:3 instead: the canonical buffer is
 * always faithful. The signal is natural polygon OVERHANG past the canonical
 * window (ws_sust_ovh_stamp) — content that would actually be revealed.
 * Shaded-prim presence and GTE projection volume both proved to be false
 * world signals (title-card letter tiles; the task-jingle's GTE effect) —
 * see the overhang block for the full lineage. Scoped to the tag-classified
 * (2.5D) mechanism: full-2D titles (MMX6) are 2D-only by definition and
 * genuinely reveal more, and GTE-detector titles (Ape) already classify 2D
 * screens by projection count. The hysteresis rides out 1-2 frame gaps; a
 * real scene change crosses it in ~0.1 s. */
#define WS_2D_SCENE_HYSTERESIS 6u
static int ws_2d_only_scene(void) {
    if (ws_full_2d_mode() || ws_gte_game_mode_cfg) return 0;
    return (uint32_t)s_frame_count - ws_sust_ovh_stamp > WS_2D_SCENE_HYSTERESIS;
}

int gpu_ws_present_native_43(void) {
    if (!ws_engaged()) return 0;
    if (!ws_game_mode()) return 1;                 /* full-2D screen */
    if (ws_2d_only_scene()) return 1;              /* 2D-only gameplay scene */
    static uint32_t fmv_frame_cache = 0xFFFFFFFFu;
    static int      fmv_cached = 0;
    uint32_t f = (uint32_t)s_frame_count;
    if (f != fmv_frame_cache) {
        fmv_frame_cache = f;
        GpuDisplayInfo di; gpu_get_display_info(&di);
        fmv_cached = di.depth24 || mdec_recently_active(WS_FMV_HYSTERESIS);
    }
    return fmv_cached;
}

/* Squash applies only when configured AND the frame is being stretched. */
static int ws_active(void) { return ws_configured() && !gpu_ws_present_native_43(); }

/* ----- Native-wide (mode 2) ------------------------------------------------
 * Render the wider FOV into an actually wider frame instead of squashing it.
 * Engaged on game frames only (FMV/menu frames pillarbox 4:3 like squash). The
 * frame grows symmetrically: each side by OFFSET display-pixels, total by
 * EXTRA = 2*OFFSET. OFFSET = round(W*(3*num-4*den)/(8*den)) where W is the live
 * display width — derived from the aspect so it generalises past 16:9 and
 * tracks display-mode changes; EXTRA is forced even so OFFSET is integral
 * (e.g. W=320 @ 16:9 -> OFFSET=53, EXTRA=106, frame 320 -> 426). The shift is
 * applied via the GPU draw_offset (so 3D and 2D move together and stay
 * aligned), the draw-area widens by EXTRA so the shifted content is not clipped
 * on the right, and the present widens the display read by EXTRA. 0 when
 * native-wide is inactive (4:3 / boot / squash mode / FMV / full-2D). */
int ws_native_wide_active(void) {
    return ws_mode == 2 && !gpu_ws_present_native_43();
}
static int ws_nw_offset(void) {
    if (!ws_native_wide_active()) return 0;
    int numr = 3 * ws_cfg_num - 4 * ws_cfg_den;   /* > 0 for aspects wider than 4:3 */
    if (numr <= 0) return 0;
    int w = (int)ws_disp_w();
    int v = (w * numr + 4 * ws_cfg_den) / (8 * ws_cfg_den);   /* round-to-nearest */
    return v;
}
int ws_nw_extra(void) { return 2 * ws_nw_offset(); }

/* Per-side X cull margin in screen/world units (the game's draw classifier
 * works in objX-camX where 1 unit ~= 1 native-4:3 screen pixel). The squash
 * shows a half-view of 160/s pixels (s = squash factor = ws_xnum/ws_xden), so
 * the visible edge moves out by 160*(1/s - 1) = 160*(xden-xnum)/xnum. Widening
 * the cull window by this restores the original off-screen margin at the new
 * edge. 0 whenever squash is inactive (4:3/boot/menu/FMV) → original cull. */
/* Diagnostic override (8C): when >= 0, psx_ws_x_margin() returns this value
 * unconditionally so a probe can sweep the cull margin live (0 = force 4:3
 * cull while still stretching; large = over-draw) at a fixed camera position.
 * -1 = normal computed margin. */
static int ws_margin_override = -1;
static int ws_cull_guard_pixels = 0;
void gpu_ws_set_margin_override(int v) { ws_margin_override = v; }
void gpu_ws_set_cull_guard_pixels(int pixels) {
    if (pixels < 0) pixels = 0;
    if (pixels > 256) pixels = 256;
    ws_cull_guard_pixels = pixels;
}

#define WS_EXPLICIT_CULL_SITES_MAX 64
static uint32_t ws_explicit_bias_sites[WS_EXPLICIT_CULL_SITES_MAX];
static uint32_t ws_explicit_slti_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_bias_n = 0;
static int ws_explicit_slti_n = 0;
void gpu_ws_set_explicit_cull_sites(const uint32_t *bias, int nbias,
                                    const uint32_t *slti, int nslti) {
    if (nbias < 0) nbias = 0;
    if (nslti < 0) nslti = 0;
    if (nbias > WS_EXPLICIT_CULL_SITES_MAX) nbias = WS_EXPLICIT_CULL_SITES_MAX;
    if (nslti > WS_EXPLICIT_CULL_SITES_MAX) nslti = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_bias_n = nbias;
    ws_explicit_slti_n = nslti;
    for (int i = 0; i < nbias; i++) ws_explicit_bias_sites[i] = bias[i] & 0x1FFFFFFFu;
    for (int i = 0; i < nslti; i++) ws_explicit_slti_sites[i] = slti[i] & 0x1FFFFFFFu;
}
static int ws_explicit_site(const uint32_t *sites, int n, uint32_t pc) {
    uint32_t p = pc & 0x1FFFFFFFu;
    for (int i = 0; i < n; i++) if (sites[i] == p) return 1;
    return 0;
}
int psx_ws_is_cull_bias_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_bias_sites, ws_explicit_bias_n, pc);
}
int psx_ws_is_cull_slti_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_slti_sites, ws_explicit_slti_n, pc);
}

int psx_ws_x_margin(void) {
    if (ws_margin_override >= 0) return ws_margin_override;
    /* Native-wide: widen the world-space draw cull by the per-side reveal
     * (== the centering OFFSET in screen px) so the game SUBMITS the geometry
     * that previously fell outside the 4:3 cull window; the wide compositor then
     * rasterizes it into the revealed margins. Same recompiler emit sites as the
     * squash path ([widescreen.cull]); 0 at 4:3 so the cull stays byte-identical. */
    if (ws_native_wide_active()) return ws_nw_offset() + ws_cull_guard_pixels;
    if (!ws_active()) return 0;
    return (160 * (ws_xden - ws_xnum) + ws_xnum / 2) / ws_xnum;
}

/* ---- Capcom 2D background tile-loop widen ([widescreen.bg2d]) --------------
 * Mega Man X5/X6 use a pure-2D sprite engine that renders only a 4:3 (320px) field
 * of view — there is no overscan to "reveal." Its per-layer background renderer
 * (FUN_800270d0) draws `count` 16px tile columns × 16 rows from a start tile
 * column / start screen-x derived from the camera scroll. To produce a TRUE
 * wider FOV we widen that loop so it draws extra columns on BOTH sides of the
 * 320 view, filling the 16:9 reveal margins with real adjacent stage. The three
 * helpers below are hooked at the renderer's count / start-col / start-x
 * instructions by the recompiler (gen-time). They are IDENTITY unless native-
 * wide is engaged on a ~320 screen, so 4:3 (and the engine's own 512 hi-res
 * mode, which already draws 33 columns) stay byte-identical.
 *
 * LEFT = per-side reveal in whole 16px tile columns = ceil(nw_offset / 16). The
 * loop start moves LEFT columns earlier (tile col -LEFT in the 64-col ring,
 * screen-x -LEFT*16) and the count grows by 2*LEFT, so existing columns keep
 * their screen positions (stay aligned with objects/player) while new columns
 * are prepended left and appended right. The 64-column tile ring (mask 0x3f) is
 * far wider than the visible window, so the prepended columns fetch the correct
 * already-streamed adjacent tiles. */
static uint32_t g_bg2d_layer_base = 0x800971F8u;
static uint32_t g_bg2d_ring_base = 0x800A21B8u;
static uint32_t g_bg2d_map_size_addr = 0x800CD338u;
static uint32_t g_bg2d_layer_stride_addr = 0x8008EC10u;
static uint32_t g_bg2d_ring_cols = 64;
static uint32_t g_bg2d_layer_count = 3;
static uint32_t g_bg2d_layer_struct_stride = 0x54;
static uint32_t g_bg2d_packet_cap = 1000;
static int g_bg2d_native_cols = 21;

void gpu_ws_bg2d_configure(uint32_t layer_base, uint32_t ring_base,
                           uint32_t map_size_addr, uint32_t layer_stride_addr,
                           uint32_t ring_cols, uint32_t layer_count,
                           uint32_t layer_struct_stride, uint32_t packet_cap) {
    g_bg2d_layer_base = layer_base;
    g_bg2d_ring_base = ring_base;
    g_bg2d_map_size_addr = map_size_addr;
    g_bg2d_layer_stride_addr = layer_stride_addr;
    g_bg2d_ring_cols = ring_cols;
    g_bg2d_layer_count = layer_count;
    g_bg2d_layer_struct_stride = layer_struct_stride;
    g_bg2d_packet_cap = packet_cap;
}

static int ws_bg2d_left_cols(void) {
    if (!ws_native_wide_active()) return 0;
    /* Only the ~320 gameplay mode; the engine's 512 hi-res mode (title) draws
     * its own 33 columns and centres itself — never double-shift it. */
    if (ws_disp_w() > 384) return 0;
    int off = ws_nw_offset();           /* per-side reveal in screen px */
    if (off <= 0) return 0;
    return (off + 15) / 16;             /* ceil to whole tile columns */
}
/* HOST-SIDE reveal mode (elective, default ON): the guest-side widen below overruns
 * the engine's 1024-slot BG packet buffer / 999-tile cap in dense stages (void/stale
 * columns). When host-side mode is active we keep the GUEST renderer at its NATIVE 21
 * columns (these three helpers become IDENTITY → guest buffer/OT/cap byte-identical)
 * and draw the reveal margins ourselves into the wide surface (gpu_ws_mmx6_emit_reveal).
 * The stream hooks below stay active either way (they widen the tile RING, which the
 * host-side decoder reads). Toggle via gpu_ws_mmx6_set_hostside(); 0 = old guest-widen. */
static int g_mmx6_hostside = 0;   /* DEFAULT OFF: host-side reveal was the wrong approach
                                   * (garbled tpage + stale early ring read). The buffer-
                                   * relocation fix keeps the engine's correct guest-widen
                                   * render instead. Code kept inert pending removal. */
void gpu_ws_mmx6_set_hostside(int on) { g_mmx6_hostside = on ? 1 : 0; }
static int mmx6_hostside_active(void) {
    return g_mmx6_hostside && ws_native_wide_active() && ws_disp_w() <= 384
        && ws_nw_offset() > 0;
}
/* Column count: native (21) under host-side mode; else base + both-side reveal. */
static void mmx6_bg_refill_tick(void);   /* defined below (ring-freshness fix) */
int psx_ws_bg2d_cols(int base) {
    g_bg2d_native_cols = base;
    mmx6_bg_refill_tick();
    return mmx6_hostside_active() ? base : base + 2 * ws_bg2d_left_cols();
}
/* Start tile column: refill before the first column is consumed, then begin LEFT
 * earlier in the ring. The count hook retains the same tick for layouts that load
 * their loop bound before calculating the starting column. */
int psx_ws_bg2d_startcol(int col, unsigned mask) {
    mmx6_bg_refill_tick();
    return mmx6_hostside_active() ? col : ((col - ws_bg2d_left_cols()) & (int)mask);
}
/* Start screen-x: native under host-side mode; else LEFT*16 further left. */
int psx_ws_bg2d_startx(int x)        { return mmx6_hostside_active() ? x : (x - ws_bg2d_left_cols() * 16); }

/* Tile-RING STREAMER widen (same [widescreen.bg2d], FUN_800273e4). The renderer
 * above now draws LEFT extra columns each side, but the engine's 64-column tile
 * ring is only refreshed at its leading edge: the streamer re-streams ONE column
 * per side per frame at world-X scrollX-16 (left) and scrollX+16+width (right),
 * so the columns the widened renderer reaches are never populated (they show
 * empty=black or stale=old tiles, the "stale margins"). Push the streamed left/
 * right edge out by LEFT*16 so the ring stays valid across the widened window as
 * the camera scrolls (the incremental streamer fills each newly-leading column).
 * Identity (no extra streaming) at 4:3 / 512 hi-res, so the ring is byte-identical
 * there. The 64-col ring has ample slack (visible ~21 cols) for ±LEFT more. */
int psx_ws_bg2d_stream_left(int x)  { return x - ws_bg2d_left_cols() * 16; }
int psx_ws_bg2d_stream_right(int x) { return x + ws_bg2d_left_cols() * 16; }
int psx_ws_bg2d_undercap(int counter, int native_cap) {
    int cap = ws_bg2d_left_cols() > 0 ? (int)g_bg2d_packet_cap : native_cap;
    return counter < cap;
}

/* Compatibility for generated MMX6 sources predating the generic names. */
int psx_ws_mmx6_bg_cols(int base)       { return psx_ws_bg2d_cols(base); }
int psx_ws_mmx6_bg_startcol(int col)    { return psx_ws_bg2d_startcol(col, 0x3fu); }
int psx_ws_mmx6_bg_startx(int x)        { return psx_ws_bg2d_startx(x); }
int psx_ws_mmx6_bg_stream_left(int x)   { return psx_ws_bg2d_stream_left(x); }
int psx_ws_mmx6_bg_stream_right(int x)  { return psx_ws_bg2d_stream_right(x); }

/* Called at entry to MMX6's full tile-ring initializer (FUN_800269F4). The
 * independent layers invoke it only when stage/background data is dirty; gate
 * by frame because up to three layers initialize together. This is the exact
 * point where pixels retained from the old stage cease to be meaningful. */
void psx_ws_mmx6_bg_stage_init(void) {
    static uint32_t last_frame = 0xFFFFFFFFu;
    uint32_t frame = (uint32_t)s_frame_count;
    if (!ws_clear_reveal || ws_mode != 2 || frame == last_frame) return;
    last_frame = frame;
    g_mmx6_void_generation++;
    ws_clear_all_reveal_margins();
}

/* ===== MMX6 BG packet-buffer RELOCATION ([widescreen.bg2d] bufbase_site/cap_site) ===
 * The widened BG render (29 cols) overruns the engine's BG packet double-buffer (driver
 * base 0x800B91C0, stride 0x4000 = 1024 tile slots/buffer) and its per-frame 1000-tile cap
 * (FUN_800270d0) in dense stages -> the renderer returns early -> void/garbled columns.
 * Fix: when the widen is active, relocate the double-buffer into a larger, proven-free RAM
 * region (the 828KB heap->stack gap [0x800EA000,0x801B9000), measured 2026-06-21) and raise
 * the cap to match. The packet TEMPLATE (opcode 0x7D at word1) is uniform, so we seed the
 * relocated slots once. The OT links point at the relocated packets and the GPU DMA reads
 * them from guest RAM exactly as before. IDENTITY (no relocation, cap 1000) at 4:3 / 512 /
 * when the widen is off -> 4:3 and every other game byte-identical. */
#define MMX6_RELOC_BASE   0x80140000u   /* mid the proven-free heap->stack gap */
#define MMX6_RELOC_STRIDE 0x8000u       /* 2048 tile slots/buffer (was 0x4000=1024) */
#define MMX6_RELOC_CAP    1800          /* per-frame BG tile cap (was 1000; <= 2048) */
extern void psx_write_word(uint32_t addr, uint32_t val);

/* PARKED (default 0): relocation moves the BG packets to a larger free buffer and the
 * prims DO render from there (census-confirmed), but the engine's OT chain / per-slot
 * DR_TPAGE primitives are keyed to the original buffer layout, so relocated BG draws
 * BLACK (wrong/absent texpage). Needs the OT-chain + DR_TPAGE relocation REd before it
 * can ship. Toggle via mmx6_reveal {reloc:1} for continued debugging. When 0 the bufbase/
 * cap hooks are identity -> the committed guest-widen path (correct + fresh, dense-stage
 * overflow) is restored byte-for-byte. */
static int g_mmx6_reloc = 0;
void gpu_ws_mmx6_set_reloc(int on) { g_mmx6_reloc = on ? 1 : 0; }
int  gpu_ws_mmx6_reloc_get(void)   { return g_mmx6_reloc; }
static int mmx6_bg_reloc_active(void) {
    return g_mmx6_reloc && !g_mmx6_hostside && ws_bg2d_left_cols() > 0;
}

/* Hook at the driver's buffer-address addu (0x80026DC4): remap base 0x800B91C0 + bufidx*0x4000
 * to the relocated buffer (larger stride). Seeds the relocated slots' uniform SPRT template
 * (word1 = 0x7D000000) once per buffer. */
int psx_ws_mmx6_bg_bufbase(int addr) {
    if (!mmx6_bg_reloc_active()) return addr;
    uint32_t a = (uint32_t)addr;
    uint32_t bufidx = (a - 0x800B91C0u) >> 14;        /* 0 or 1 */
    if (bufidx > 1u) return addr;                     /* unexpected -> leave untouched */
    uint32_t newbase = MMX6_RELOC_BASE + bufidx * MMX6_RELOC_STRIDE;
    static int seeded[2] = { 0, 0 };
    if (!seeded[bufidx]) {
        /* Seed BOTH template fields the renderer does NOT fully rewrite:
         *   word0 = num_words(0x03, the libgpu OT header read by the GPU DMA) | next-link
         *           (low 24; the renderer rebuilds active links each frame, but the head
         *           packet's tail-link falls back to this default — terminate per buffer);
         *   word1 = SPRT16 opcode 0x7D (the renderer only RMWs its semi bit).
         * word2(xy)/word3(uv/clut) are written per draw, so they need no template. */
        uint32_t nslots = MMX6_RELOC_STRIDE / 0x10u;
        for (uint32_t s = 0; s < nslots; s++) {
            uint32_t next = (s + 1u < nslots)
                          ? ((newbase + (s + 1u) * 0x10u) & 0xFFFFFFu)
                          : 0xFFFFFFu;                 /* terminate the default chain */
            psx_write_word(newbase + s * 0x10u + 0u, 0x03000000u | next);
            psx_write_word(newbase + s * 0x10u + 4u, 0x7D000000u);
        }
        seeded[bufidx] = 1;
    }
    return (int)newbase;
}

/* Hook at the BG cap slti (0x80027278): counter < cap. Raises the cap to MMX6_RELOC_CAP when
 * relocated (the bigger buffer can hold them); plain 1000 otherwise (byte-identical). */
int psx_ws_mmx6_bg_undercap(int counter) {
    if (mmx6_bg_reloc_active()) return counter < MMX6_RELOC_CAP;
    return psx_ws_bg2d_undercap(counter, 1000);
}

/* ===== MMX6 host-side reveal columns ====================================== *
 * Keep the guest BG renderer native (21 cols) and draw the ±LEFT reveal-margin
 * columns ourselves, straight into the wide compositor surface, by replaying the
 * engine's own tile decode over guest RAM (FUN_800270d0). Guest packet buffer / OT /
 * 999-cap and the 4:3 image are untouched (no guest mutation; elective; 4:3 identical).
 *
 * The only field NOT in the tile packet is the texture PAGE (selected indirectly via
 * the tile's OT slot). We recover it empirically: as the guest draws its native BG
 * tiles we record clut -> {tpage, draw state} (clut<->tpage is 1:1 in this engine).
 * Reveal tiles resolve tpage by clut. Engine addresses (SLUS-01395):
 *   layer struct  0x800971F8 + layer*0x54  (scrollX +0xa, scrollY +0xe, parent +0x52)
 *   tile ring     0x800A21B8 + layer*0x1000 (u16[(col&0x3f) + (row&0x1f)*0x40])
 *   tile attr     [scratchpad 0x1F80000C] + (tile&0x3fff)*4   (>>0x18 == 0xff -> skip) */
extern uint32_t psx_read_word(uint32_t addr);
extern uint16_t psx_read_half(uint32_t addr);
extern uint8_t  psx_read_byte(uint32_t addr);

#define MMX6_CLUT_N    0x8000
#define MMX6_BG_BUF_LO 0x000B91C0u   /* BG packet buffer (masked), driver base 0x800B91C0 */
#define MMX6_BG_BUF_HI 0x000C11C0u   /* + 2 * 0x4000 */
typedef struct { uint16_t tpage; uint32_t color24; uint8_t semi_mode; uint8_t flags; } Mmx6ClutEnt;
static Mmx6ClutEnt *g_mmx6_clut = NULL;   /* flags: bit0 valid, bit1 raw_texture */

/* Record clut -> tpage/state for a native BG SPRT16 (called from the GP0 handler). */
void gpu_ws_mmx6_bg_record(uint32_t src_addr, uint16_t clut, uint16_t tpage,
                           uint32_t color24, int semi_mode, int raw) {
    uint32_t a = src_addr & 0x1FFFFFFFu;
    if (a < MMX6_BG_BUF_LO || a >= MMX6_BG_BUF_HI) return;   /* only background tiles */
    if (!g_mmx6_clut) {
        g_mmx6_clut = (Mmx6ClutEnt *)calloc(MMX6_CLUT_N, sizeof(Mmx6ClutEnt));
        if (!g_mmx6_clut) return;
    }
    Mmx6ClutEnt *e = &g_mmx6_clut[clut & (MMX6_CLUT_N - 1)];
    e->tpage = tpage; e->color24 = color24;
    e->semi_mode = (uint8_t)(semi_mode & 3);
    e->flags = (uint8_t)(1u | (raw ? 2u : 0u));
}

int gpu_ws_mmx6_reveal_is_active(void) { return mmx6_hostside_active(); }

/* Diagnostics (debug command mmx6_reveal). */
static long g_mmx6_emit_tiles = 0;   /* tiles drawn in the last reveal pass */
static long g_mmx6_emit_calls = 0;   /* reveal-pass invocations this run */
int  gpu_ws_mmx6_hostside_get(void) { return g_mmx6_hostside; }
long gpu_ws_mmx6_emit_tiles(void)   { return g_mmx6_emit_tiles; }
long gpu_ws_mmx6_emit_calls(void)   { return g_mmx6_emit_calls; }
int  gpu_ws_mmx6_clut_count(void) {
    if (!g_mmx6_clut) return 0;
    int n = 0;
    for (int i = 0; i < MMX6_CLUT_N; i++) if (g_mmx6_clut[i].flags & 1) n++;
    return n;
}

/* Draw the ±LEFT reveal columns for all 3 BG layers into the wide surface for the
 * vertical band being cleared (band_y / base_x from the GP0 fill). Pure guest reads. */
void gpu_ws_mmx6_emit_reveal(int base_x, int band_y) {
    if (!mmx6_hostside_active() || !g_mmx6_clut) return;
    int left = ws_bg2d_left_cols();
    if (left <= 0) return;
    uint32_t attr_base = psx_read_word(0x1F80000Cu);
    if (attr_base == 0) return;
    g_mmx6_emit_calls++;
    g_mmx6_emit_tiles = 0;
    sw_wide_set_target(base_x);                 /* g_wide_cur = this band's surface */
    for (uint32_t layer = 0; layer < g_bg2d_layer_count; layer++) {
        uint32_t ls = g_bg2d_layer_base + layer * g_bg2d_layer_struct_stride;
        int sx = (int16_t)psx_read_half(ls + 0xa);
        int sy = (int16_t)psx_read_half(ls + 0xe);
        int8_t parent = (int8_t)psx_read_byte(ls + 0x52);
        if (parent >= 0) {                      /* linked layer: add parent's scroll */
            uint32_t lp = g_bg2d_layer_base
                        + (uint32_t)(uint8_t)parent * g_bg2d_layer_struct_stride;
            sx += (int16_t)psx_read_half(lp + 0xa);
            sy += (int16_t)psx_read_half(lp + 0xe);
        }
        int sxr = sx; if (sxr < 0) sxr += 0xf;  int start_col = sxr >> 4;
        int sub_x = -(sx & 0xf);
        int syr = sy; if (syr < 0) syr += 0xf;  int start_row = syr >> 4;
        int sub_y = -(sy & 0xf);
        uint32_t ring = g_bg2d_ring_base + layer * (g_bg2d_ring_cols * 64u);
        for (int side = 0; side < 2; side++) {
            int ci0 = (side == 0) ? -left : g_bg2d_native_cols;
            for (int k = 0; k < left; k++) {
                int ci  = ci0 + k;                  /* column index relative to native start */
                int col = start_col + ci;
                int scr_x = sub_x + ci * 16;
                for (int row = 0; row < 16; row++) {
                    int trow = start_row + row;
                    uint16_t tile = psx_read_half(ring
                        + (uint32_t)((col & (int)(g_bg2d_ring_cols - 1u)) * 2)
                        + (uint32_t)((trow & 0x1f) * (int)(g_bg2d_ring_cols * 2u)));
                    if (tile == 0) continue;
                    uint32_t attr = psx_read_word(attr_base + (uint32_t)((tile & 0x3fff) * 4));
                    if ((attr >> 0x18) == 0xff) continue;
                    uint16_t clut = (uint16_t)(((((attr & 0xf000) >> 6) + 0x7980
                                     + ((attr >> 0x18) & 0x40) * 0x10)) | ((attr >> 8) & 0xf));
                    Mmx6ClutEnt *e = &g_mmx6_clut[clut & (MMX6_CLUT_N - 1)];
                    if (!(e->flags & 1)) continue;  /* tpage not learned yet (warmup) */
                    int u0 = (int)((attr >> 0xc)  & 0xf0);
                    int v0 = (int)((attr >> 0x10) & 0xf0);
                    uint16_t clut_x = (uint16_t)((clut & 0x3f) * 16);
                    uint16_t clut_y = (uint16_t)((clut >> 6) & 0x1ff);
                    int semi = (tile & 0x4000) ? 1 : 0;
                    int wy = band_y + sub_y + row * 16;
                    sw_wide_emit_tile(band_y, scr_x, wy, u0, v0, clut_x, clut_y,
                                      e->tpage, e->color24, semi, e->semi_mode,
                                      (e->flags & 2) ? 1 : 0);
                    g_mmx6_emit_tiles++;
                }
            }
        }
    }
}

/* ===== MMX6 BG tile-ring freshness fix ([widescreen.bg2d]) ================== *
 * The widened renderer (FUN_800270d0) draws LEFT extra tile columns on each side of
 * the 320 view, but the engine's streamer (FUN_800273e4 -> FUN_800274a0) only
 * refreshes ONE column per side per frame at the leading edge. On scene entry or
 * fast horizontal scroll the inner reveal columns therefore hold STALE tiles from a
 * previous scroll position (different scrollY) -> real-but-misplaced tiles spilling
 * up-and-left (the "staircase"). 4:3 is unaffected (it draws only the 21 columns the
 * engine keeps streamed). Fix: before the renderer draws, re-stream EVERY tile column
 * of the widened window for all 3 layers, so the render window and the ring-refresh
 * window are the same window. This is a faithful clone of FUN_800274a0's inner
 * metatile->ring fill, MINUS the scrollX wrap-bookkeeping writes (those drive the
 * engine's own incremental streamer; perturbing them would desync its leading edge).
 * Off-map columns fill tile 0 (the renderer skips them) = the engine's own void; at a
 * horizontal map-loop seam (engine early-return) we skip the column. Gated on
 * native-wide + ~320 mode, so 4:3 and the 512 hi-res title stay byte-identical. */
extern void psx_write_half(uint32_t addr, uint16_t val);

static void bg2d_clear_column(int layer, int ringcol, int worldY) {
    int rows = 0x12;
    if (worldY < 0) { worldY = 0; rows = 0x11; }
    int wrapY = worldY - (((worldY < 0) ? worldY + 0x1ff : worldY) >> 9) * 0x200;
    int ringrow = ((wrapY < 0) ? wrapY + 0xf : wrapY) >> 4;
    uint32_t ringbase = g_bg2d_ring_base
                      + (uint32_t)layer * (g_bg2d_ring_cols * 64u);
    for (int i = 0; i < rows; i++) {
        uint32_t cell = ringbase
                      + (uint32_t)((ringcol & (int)(g_bg2d_ring_cols - 1u)) * 2)
                      + (uint32_t)((ringrow & 0x1f) * (int)(g_bg2d_ring_cols * 2u));
        psx_write_half(cell, 0);
        ringrow = (ringrow + 1) & 0x1f;
    }
}

/* Faithful clone of FUN_800274a0's inner fill: stream one tile column at guest pixel
 * (worldX, worldY) of `layer` into the 64x32 ring. Compares mirror the engine's sltu
 * (UNSIGNED). When write!=0 the tiles are written to the ring; when write==0 they are
 * compared against the live ring (validation), counting *cmp_total / *cmp_bad. Returns
 * 1 if the column was streamed, 0 if skipped (map-loop seam early-return). */
static int bg2d_fill_column(int layer, int worldX, int worldY, int scrollX, int write,
                             int *cmp_total, int *cmp_bad) {
    /* The guest column streamer returns immediately for negative world X. Do
     * not wrap it into the far edge of the map, which produces repeated blocks
     * in the left reveal near a level boundary. */
    if (worldX < 0) return 0;
    uint32_t lbase = g_bg2d_layer_base + (uint32_t)layer * g_bg2d_layer_struct_stride;
    int rows = 0x12;
    if (worldY < 0) { worldY = 0; rows = 0x11; }
    int32_t metaCol = ((worldX < 0) ? worldX + 0xff : worldX) >> 8;     /* t1 */
    int32_t metaRow = ((worldY < 0) ? worldY + 0xff : worldY) >> 8;     /* t4 */
    int worldPeriod = (int)g_bg2d_ring_cols * 16;
    int worldShift = 0;
    for (int n = worldPeriod; n > 1; n >>= 1) worldShift++;
    int wrapX = worldX
              - (((worldX < 0) ? worldX + worldPeriod - 1 : worldX) >> worldShift)
                * worldPeriod;
    int colsh = ((wrapX < 0) ? wrapX + 0xf : wrapX) >> 4;               /* t7 = wrapX>>4 */
    int ringcol = colsh & (int)(g_bg2d_ring_cols - 1u);
    int tcolInMeta = colsh & 0xf;                                       /* s0 = t7 & 0xf */
    int wrapY = worldY - (((worldY < 0) ? worldY + 0x1ff : worldY) >> 9) * 0x200;
    int ringrow = ((wrapY < 0) ? wrapY + 0xf : wrapY) >> 4;             /* a2 */
    int trowInMeta = ringrow & 0xf;                                     /* t3 */

    int mapLeft  = psx_read_byte(lbase + 0x4d);
    int mapRight = psx_read_byte(lbase + 0x4e);
    /* C integer division toward zero makes worldX -255..-1 resolve to metatile
     * column 0 below. The native 4:3 streamer never requests those coordinates,
     * but the widened left window does at a stage starting on mapLeft. Treat
     * them as finite-map void before the engine's loop/wrap bookkeeping. */
    if (worldX < mapLeft * 0x100)
        return 2;
    if ((uint32_t)metaCol < (uint32_t)mapLeft) {
        metaCol = metaCol + 1 + (mapRight - mapLeft);
        if (scrollX <= (mapLeft - 1) * 0x100) {
            /* The native renderer never sees this column, so the engine leaves
             * its ring slot alone. Report finite-map void to the DMA-start
             * cleanup without mutating ring slots canonical draws can alias. */
            return 2;   /* finite-map void, not merely a normal streamed column */
        }
    }
    if ((uint32_t)mapRight < (uint32_t)metaCol) {
        metaCol = (metaCol - 1 - mapRight) + mapLeft;
        if ((mapRight + 1) * 0x100 <= scrollX) {
            return 2;
        }
    }
    int mapW = psx_read_byte(g_bg2d_map_size_addr);
    int mapH = psx_read_byte(g_bg2d_map_size_addr + 1u);
    uint32_t mapBase  = psx_read_word(0x1F800004u);
    uint32_t metaBase = psx_read_word(0x1F800008u);
    if (mapBase == 0 || metaBase == 0 || mapW <= 0 || mapH <= 0) return 0;
    int layerStride = (uint16_t)psx_read_half(g_bg2d_layer_stride_addr);

    int metaIdx = 0;
    if ((uint32_t)metaCol < (uint32_t)mapW && (uint32_t)metaRow < (uint32_t)mapH)
        metaIdx = psx_read_byte(mapBase + (uint32_t)(layer * layerStride + mapW * metaRow + metaCol));

    uint32_t ringbase = g_bg2d_ring_base
                      + (uint32_t)layer * (g_bg2d_ring_cols * 64u);
    for (int i = 0; i < rows; i++) {
        uint32_t cell = ringbase + (uint32_t)(ringcol * 2)
                      + (uint32_t)((ringrow & 0x1f) * (int)(g_bg2d_ring_cols * 2u));
        uint16_t tile = psx_read_half(metaBase + (uint32_t)metaIdx * 0x200u
                                      + (uint32_t)(trowInMeta * 0x20) + (uint32_t)(tcolInMeta * 2));
        if (write) {
            psx_write_half(cell, tile);
        } else if (cmp_total) {
            (*cmp_total)++;
            if (psx_read_half(cell) != tile && cmp_bad) (*cmp_bad)++;
        }
        ringrow = (ringrow + 1) & 0x1f;
        if (++trowInMeta == 0x10) {
            trowInMeta = 0;
            metaRow++;
            metaIdx = 0;
            if ((uint32_t)metaCol < (uint32_t)mapW && (uint32_t)metaRow < (uint32_t)mapH)
                metaIdx = psx_read_byte(mapBase + (uint32_t)(layer * layerStride + mapW * metaRow + metaCol));
        }
    }
    return 1;
}

static int  g_mmx6_freshfix   = 1;   /* re-stream the widened window each frame (the fix) */
static long g_mmx6_refill_cols = 0;  /* columns streamed in the last refill (diag) */
static uint32_t g_mmx6_refill_frame = 0xFFFFFFFFu;  /* frame of the last refill (once/frame) */
void gpu_ws_mmx6_set_freshfix(int on) { g_mmx6_freshfix = on ? 1 : 0; }
int  gpu_ws_mmx6_freshfix_get(void)   { return g_mmx6_freshfix; }
long gpu_ws_mmx6_refill_cols(void)    { return g_mmx6_refill_cols; }

/* Re-stream every tile column of the widened window for every BG layer. Independent
 * layers use their own scroll; parent-linked layers use the same combined scroll
 * that the renderer uses to index their ring. The guest's incremental streamer
 * skips linked layers because their native 4:3 window is prefilled, but the newly
 * revealed columns still need to be populated here. worldY = scrollY - 0x10 mirrors
 * the driver. ci spans the renderer's widened span [-LEFT, 20+LEFT].
 * No-op at 4:3 / 512 hi-res (left==0). Idempotent over the native columns the engine
 * already streamed correctly, so re-streaming them is harmless. */
void psx_ws_mmx6_bg_refill_all(void) {
    if (!g_mmx6_freshfix) return;
    if (!ws_native_wide_active() || ws_disp_w() > 384) return;
    int left = ws_bg2d_left_cols();
    if (left <= 0) return;
    g_mmx6_refill_cols = 0;
    for (uint32_t layer = 0; layer < g_bg2d_layer_count; layer++) {
        uint32_t lbase = g_bg2d_layer_base + layer * g_bg2d_layer_struct_stride;
        int sx = (int16_t)psx_read_half(lbase + 0xa);
        int sy = (int16_t)psx_read_half(lbase + 0xe);
        int8_t parent = (int8_t)psx_read_byte(lbase + 0x52);
        if (parent >= 0 && (uint32_t)(uint8_t)parent < g_bg2d_layer_count) {
            uint32_t pbase = g_bg2d_layer_base
                           + (uint32_t)(uint8_t)parent * g_bg2d_layer_struct_stride;
            sx += (int16_t)psx_read_half(pbase + 0xa);
            sy += (int16_t)psx_read_half(pbase + 0xe);
        }
        int sxr = sx;
        if (sxr < 0) sxr += 0xf;
        int start_col = sxr >> 4;
        for (int ci = -left; ci < g_bg2d_native_cols + left; ci++) {
            int world_x = sx + ci * 16;
            if (world_x < 0) {
                bg2d_clear_column((int)layer, start_col + ci, sy - 0x10);
                continue;
            }
            if (bg2d_fill_column((int)layer, world_x, sy - 0x10,
                                 sx, 1, NULL, NULL))
                g_mmx6_refill_cols++;
        }
    }
}

/* Trigger the once-per-frame widened-window refill. Both renderer setup hooks call
 * this so differing instruction order between games still refreshes the ring before
 * the first widened column is consumed. */
static void mmx6_bg_refill_tick(void) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f == g_mmx6_refill_frame) return;
    g_mmx6_refill_frame = f;
    psx_ws_mmx6_bg_refill_all();
}

/* Validation harness (debug mmx6_freshfix {validate:1}): for the NATIVE 21 columns —
 * which the engine streams correctly — run the clone in compare-only mode against the
 * live ring. Proves the clone is byte-exact before the refill (which overwrites the
 * ring) is trusted. Returns total cells compared; *bad = mismatches. */
int gpu_ws_mmx6_validate(int *bad_out) {
    int total = 0, bad = 0;
    for (uint32_t layer = 0; layer < g_bg2d_layer_count; layer++) {
        uint32_t lbase = g_bg2d_layer_base + layer * g_bg2d_layer_struct_stride;
        int sx = (int16_t)psx_read_half(lbase + 0xa);
        int sy = (int16_t)psx_read_half(lbase + 0xe);
        int8_t parent = (int8_t)psx_read_byte(lbase + 0x52);
        if (parent >= 0 && (uint32_t)(uint8_t)parent < g_bg2d_layer_count) {
            uint32_t pbase = g_bg2d_layer_base
                           + (uint32_t)(uint8_t)parent * g_bg2d_layer_struct_stride;
            sx += (int16_t)psx_read_half(pbase + 0xa);
            sy += (int16_t)psx_read_half(pbase + 0xe);
        }
        for (int ci = 0; ci < g_bg2d_native_cols; ci++)
            bg2d_fill_column((int)layer, sx + ci * 16, sy - 0x10,
                             sx, 0, &total, &bad);
    }
    if (bad_out) *bad_out = bad;
    return total;
}

/* Shared render-funnel screen-X cull widening ([widescreen.cull] auto_screen_x),
 * called identically by the gcc emit, the sljit JIT, and the interpreter so every
 * overlay execution path widens the same way. sx = the GTE screen-X as loaded by
 * the guest's lhu (low 16 bits significant); imm = the original bound (0x140 /
 * 0x141). Returns the sltiu verdict (1 = on-screen/keep). Sign-extends sx and
 * shifts by +margin so BOTH 16:9 margins pass; at 4:3 margin==0 so it reduces
 * bit-for-bit to the vanilla `(uint16)sx < imm`. */
int psx_ws_cull_sltiu(uint32_t sx, uint32_t imm) {
    int m = psx_ws_x_margin();
    return ((uint32_t)((int32_t)(int16_t)(uint16_t)sx + m)
            < (uint32_t)((int32_t)imm + 2 * m)) ? 1 : 0;
}

/* Signed right-edge widen for the min/max funnel idiom (`slti v, minSX, W`):
 * the paired LEFT edge is a separate bltz (psx_ws_cull_bltz below), so this
 * bound moves out by ONE margin only. Operand is an already sign-extended /
 * computed 32-bit screen X. Identity at margin 0 (4:3). */
int psx_ws_cull_slti(uint32_t sx, uint32_t imm) {
    return ((int32_t)sx < (int32_t)imm + psx_ws_x_margin()) ? 1 : 0;
}

/* Signed left-edge widen for the funnel's `bltz maxSX, reject`: reject only
 * when the prim ends left of the REVEALED edge (maxSX < -margin). Returns the
 * branch predicate. Identity at margin 0 (4:3). */
int psx_ws_cull_bltz(uint32_t v) {
    return ((int32_t)v < -psx_ws_x_margin()) ? 1 : 0;
}

/* ---- Cull signature configuration ([widescreen.cull] screen_w_imms /
 * screen_h_imms). The width/height immediates are per-game (Tomba: 0x140/0x141
 * + 0xE0/0xF1 on a 320 display; Ape Escape: 0x181 + 0xF1 on 368). Defaults
 * keep the original Tomba signature so existing configs are unchanged. The
 * sets are consulted by the shared detector on every backend (interp + sljit;
 * the recompiler reads the same config at gen time). */
static uint32_t ws_cull_w_imms[8] = { 0x140, 0x141 };
static int      ws_cull_w_n = 2;
static uint32_t ws_cull_h_imms[8] = { 0xE0, 0xF1 };
static int      ws_cull_h_n = 2;
void gpu_ws_set_cull_imms(const uint32_t *w, int nw, const uint32_t *h, int nh) {
    if (w && nw > 0) {
        if (nw > 8) nw = 8;
        for (int i = 0; i < nw; i++) ws_cull_w_imms[i] = w[i];
        ws_cull_w_n = nw;
    }
    if (h && nh > 0) {
        if (nh > 8) nh = 8;
        for (int i = 0; i < nh; i++) ws_cull_h_imms[i] = h[i];
        ws_cull_h_n = nh;
    }
}
int psx_ws_is_cull_w_imm(uint32_t imm) {
    return psx_ws_cull_imm_in(imm, ws_cull_w_imms, ws_cull_w_n);
}

/* ---- Runtime gates for the pattern-scanned widescreen hooks. The interp and
 * sljit derive widen sites by scanning live code; that derivation must honor
 * the SAME per-game [widescreen.cull] opt-ins the recompiler emit does. These
 * default OFF: a title that never opted in must never have its code
 * pattern-scanned and rewritten (an ungated backdrop false positive rewrites a
 * live GPR = wild-jump fatal — the exact class this gate closes). Set from
 * game.toml at startup (main.cpp). */
static int ws_auto_cull_on_cfg = 0;
static int ws_auto_backdrop_on_cfg = 0;
void gpu_ws_set_auto_hooks(int cull_on, int backdrop_on) {
    ws_auto_cull_on_cfg     = cull_on ? 1 : 0;
    ws_auto_backdrop_on_cfg = backdrop_on ? 1 : 0;
}
int psx_ws_auto_cull_on(void) { return ws_auto_cull_on_cfg; }

/* Detect the GTE screen-extent trivial-reject signature in a run of
 * instruction words: at least one width compare AND one height compare
 * (slti or sltiu, immediates from the configured sets). Lets the sljit JIT +
 * interpreter gate the cull-widening to real render funnels (a lone width
 * compare elsewhere must stay vanilla). Same shared scan the recompiler's
 * func_has_screen_extent_cull uses (ws_cull_detect.h). */
int psx_ws_func_has_screen_cull(const uint32_t *words, int n) {
    return psx_ws_cull_scan(words, n, ws_cull_w_imms, ws_cull_w_n,
                            ws_cull_h_imms, ws_cull_h_n);
}

/* Classify words[idx] as an X left-edge reject bltz (shared structural
 * detector, runtime imm sets). Caller qualifies the window first. */
int psx_ws_cull_bltz_at(const uint32_t *words, int n, int idx) {
    return psx_ws_cull_bltz_here(words, n, idx, ws_cull_w_imms, ws_cull_w_n);
}

/* Widescreen backdrop screen-X correction ([widescreen.backdrop] x_sites).
 * The parallax 2D backdrop layer (ocean/cloud/mountain/grass — overlay actor
 * handlers e.g. 0x801216BC) computes its screen-X in pure integer math
 * (screenX = (worldX - camX) >> parallax) and NEVER goes through the GTE, so
 * the GTE X-squash (gte_set_display_aspect) that gives 3D the wider 16:9 FOV
 * does not touch it. The recompiler emits this on each handler's final
 * screenX store so the backdrop is squashed by the SAME factor around the
 * screen centre: a far piece whose 4:3 screenX sat past the 320px edge (and
 * was GPU-clipped → the blue void / half-rectangles at the edges) is pulled
 * in to cover the revealed FOV. Identity at 4:3 / boot / FMV / full-2D (the
 * exact ws_active() predicate the GTE squash uses), so one build serves both.
 * x is the int16 screenX the handler was about to store. */
int psx_ws_backdrop_x(int x) {
    if (!ws_active()) return (int16_t)x;
    int32_t cx = ws_disp_w() / 2;                 /* screen centre (=160 @ 320) */
    return ws_scale_about((int16_t)x, cx);
}

/* Backdrop PRELOAD predicate + value substitution ([widescreen.cull]
 * auto_backdrop). The recompiler/sljit/interp detect each scrolling-backdrop
 * column-window generator (see ws_backdrop_detect.h) and route its window START
 * and END bounds through psx_ws_backdrop_value(). When native-wide is engaged we
 * WIDEN the camera-tracked window by the 16:9 reveal: the START (left) bound
 * moves left by `margin`, the END (right) bound moves right by `margin`, where
 * margin is the per-side reveal in COLUMNS. The window of window_cols columns
 * spans (at least) the full display width, so reveal_cols = window_cols *
 * reveal_px / display_px = window_cols * nw_offset / disp_w (+slack). This draws
 * ONLY the now-visible columns (no whole-row overdraw, no fixed cap), and the
 * generator's own low/high clamps still bound it at the level edges. Gated on
 * native-wide only (NOT squash: that path uses psx_ws_backdrop_x()). Returns
 * `orig` unchanged at 4:3 / squash / boot / FMV, so 4:3 stays byte-identical. */
int psx_ws_backdrop_preload(void) {
    return ws_auto_backdrop_on_cfg && ws_native_wide_active();
}

/* Live-tunable widen amount (set via the `ws_backdrop_margin` debug command):
 *   <0  => WHOLE-ROW preload: START->0, END->extent-1 (max generous)
 *    0  => identity: no widening, native-wide still on (A/B the effect)
 *   >0  => widen the camera-tracked window by N columns each side (bounded)
 * Default is a safe-but-generous bounded value; dialed live while debugging so
 * the widen strategy can be tuned without a rebuild. */
int g_ws_bd_margin = 0;   /* column preload proven irrelevant to the void; default off */
/* Set to 1 by the interpreter around its psx_ws_backdrop_value call so the value
 * function does NOT also ring-note: the interp records a richer entry (live
 * extent / camera / DL count). Native cache-DLL calls leave it 0, so the value
 * function notes them itself -- that is how the ring sees native execution. */
int g_ws_bd_from_interp = 0;

uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols) {
    if (!psx_ws_backdrop_preload()) return orig;   /* 4:3 -> byte-identical */
    if (window_cols < 0) window_cols = -window_cols;
    int      m   = g_ws_bd_margin;
    int      from_interp = g_ws_bd_from_interp;
    g_ws_bd_from_interp = 0;                        /* consume one-shot flag */
    uint32_t finalv;
    if (m < 0) {
        /* Whole-row: the generator's shared clamps (START<0 -> 0; END>=extent ->
         * extent-1) pin the loop to [0, extent-1]. Bounded because `extent` is a
         * byte and the DL count is a byte (row <= 255 cols). */
        enum { WS_BD_END_SENTINEL = 0x7FFF };
        finalv = is_end ? (uint32_t)WS_BD_END_SENTINEL : 0u;
    } else if (m == 0) {
        finalv = orig;                              /* effect off (still recorded) */
    } else {
        finalv = is_end ? (orig + (uint32_t)m) : (orig - (uint32_t)m);
    }
    /* Single chokepoint for every path (interp hook, native cache-DLL via the
     * overlay callback, sljit). The interp records its own richer entry, so skip
     * here when it set the flag; native-DLL calls (flag 0) are recorded here with
     * pc/extent/camera = 0 -- which is how the ring sees native execution. */
    if (!from_interp)
        psx_ws_backdrop_ring_note(0u, is_end ? 2 /*WS_BD_END*/ : 1 /*WS_BD_START*/,
                                  window_cols, orig, finalv, 0, 0, 0, 0u, 0u);
    return finalv;
}

/* ---- auto_backdrop diagnostic ring (always-on; see BACKDROP_PRELOAD.md) -----
 * Every backdrop-window rewrite (interp or native) is recorded here so the live
 * tile-row extent, the camera-tracked bounds, and exactly WHICH of a scene's
 * windows fire can be read back via the `ws_backdrop_ring` debug command --
 * query the ring for the window of interest, never pause/step. The recorder is
 * fed by the interpreter (which has the live CPU state: s7=extent, the DL count,
 * and the scratchpad camera-X) at the rewrite site. */
typedef struct {
    uint32_t frame, pc, orig, finalv, base, dl;
    int      kind, wcols, extent, camx, count;
} WsBdRingEnt;
#define WS_BD_RING_CAP 512
static WsBdRingEnt s_bd_ring[WS_BD_RING_CAP];
static uint32_t    s_bd_ring_seq = 0;

void psx_ws_backdrop_ring_note(uint32_t pc, int kind, int wcols, uint32_t orig,
                               uint32_t finalv, int extent, int camx, int count,
                               uint32_t base, uint32_t dl) {
    WsBdRingEnt *e = &s_bd_ring[s_bd_ring_seq & (WS_BD_RING_CAP - 1)];
    e->frame  = (uint32_t)s_frame_count; e->pc = pc; e->kind = kind; e->wcols = wcols;
    e->orig   = orig; e->finalv = finalv; e->extent = extent; e->camx = camx; e->count = count;
    e->base   = base; e->dl = dl;
    s_bd_ring_seq++;
}

int psx_ws_backdrop_ring_json(char *buf, int cap) {
    int n = (int)(s_bd_ring_seq < WS_BD_RING_CAP ? s_bd_ring_seq : WS_BD_RING_CAP);
    uint32_t start = s_bd_ring_seq - (uint32_t)n;
    int off = snprintf(buf, (size_t)cap, "\"seq\":%u,\"n\":%d,\"ents\":[", s_bd_ring_seq, n);
    for (int i = 0; i < n && off < cap - 200; i++) {
        WsBdRingEnt *e = &s_bd_ring[(start + (uint32_t)i) & (WS_BD_RING_CAP - 1)];
        off += snprintf(buf + off, (size_t)(cap - off),
            "%s{\"f\":%u,\"pc\":\"%08x\",\"k\":%d,\"wc\":%d,\"o\":%d,\"fin\":%d,\"ext\":%d,\"cam\":%d,\"cnt\":%d,\"base\":\"%08x\",\"dl\":\"%08x\"}",
            i ? "," : "", e->frame, e->pc, e->kind, e->wcols,
            (int)(int16_t)e->orig, (int)(int16_t)e->finalv, e->extent, e->camx, e->count,
            e->base, e->dl);
    }
    off += snprintf(buf + off, (size_t)(cap - off), "]");
    return off;
}

/* Backdrop store-site registry. The [widescreen.backdrop] x_sites are emitted
 * into native cache-DLL code by the recompiler, but overlay code very often
 * runs INTERPRETED (no DLL loaded), where the emit can't reach. So the runtime
 * also registers the same site PCs here and the dirty-RAM interpreter applies
 * psx_ws_backdrop_x() at those `sh` PCs — same transform, both paths. Tiny set;
 * a linear scan per matching SH is negligible. */
#define WS_BACKDROP_SITES_MAX 16
static uint32_t ws_backdrop_sites[WS_BACKDROP_SITES_MAX];
static int      ws_backdrop_site_n = 0;

void psx_ws_set_backdrop_sites(const uint32_t* pcs, int n) {
    ws_backdrop_site_n = 0;
    if (!pcs) return;
    for (int i = 0; i < n && ws_backdrop_site_n < WS_BACKDROP_SITES_MAX; i++)
        ws_backdrop_sites[ws_backdrop_site_n++] = pcs[i] & 0x1FFFFFFFu;
}

int psx_ws_is_backdrop_site(uint32_t pc) {
    if (!ws_backdrop_site_n) return 0;
    pc &= 0x1FFFFFFFu;
    for (int i = 0; i < ws_backdrop_site_n; i++)
        if (ws_backdrop_sites[i] == pc) return 1;
    return 0;
}

/* Snapshot live widescreen state for the TCP gpu_state diagnostic. Mirrors
 * the predicates above so a probe can see, mid-gameplay, whether the squash
 * and the cull-margin are actually engaged (8C). */
void gpu_ws_get_debug(GpuWsDebug* out) {
    if (!out) return;
    out->configured        = ws_configured();
    out->active            = ws_active();
    out->game_mode         = ws_game_mode();
    out->present_native_43 = gpu_ws_present_native_43();
    out->x_margin          = psx_ws_x_margin();
    out->xnum              = ws_xnum;
    out->xden              = ws_xden;
    out->mode              = ws_mode;
    out->nw_extra          = ws_nw_extra();
    out->cur_frame         = s_frame_count;
    out->last_tag_frame    = ws_last_tag_stamp;
    out->last_3d_frame     = ws_last_3d_stamp;
    out->gte_verts         = ws_gte_prev_verts;
    out->last_world3d_frame = ws_sust_world3d_stamp;
    out->ovh_prims         = ws_ovh_prev;
    out->last_ovh_frame    = ws_sust_ovh_stamp;
}

void gpu_ws_configure(int aspect_num, int aspect_den,
                      uint32_t sprite_anchor_addr, int hud_sprt_squash, int mode) {
    ws_cfg_num = aspect_num > 0 ? aspect_num : 4;
    ws_cfg_den = aspect_den > 0 ? aspect_den : 3;
    ws_mode    = mode;
    if (mode == 1) {
        /* Squash factor = (4*den)/(3*num) — the same factor the GTE applies. */
        int32_t n = 4 * ws_cfg_den, d = 3 * ws_cfg_num;
        int32_t a = n, b = d;
        while (b) { int32_t t = a % b; a = b; b = t; }
        ws_xnum = n / a;
        ws_xden = d / a;
    } else {
        /* Native-wide (2) and off (0): the GTE is NOT squashed. */
        ws_xnum = ws_xden = 1;
    }
    ws_anchor_addr = sprite_anchor_addr;
    ws_hud_sprt    = hud_sprt_squash;
}

/* Called from generated code at the entry of each [widescreen]
 * sprite_tag_funcs function: record prim pointer ($a0) -> projected anchor.
 * Gated on ws_configured (NOT ws_active): ws_active depends on game_mode which
 * THIS function sets, so gating it on ws_active would never let gameplay
 * start (the first character of a frame would be suppressed forever). */
void psx_ws_sprite_tag(CPUState* cpu) {
    if (!ws_engaged() || !ws_anchor_addr) return;
    uint32_t key = cpu->gpr[4] & 0x1FFFFCu;
    if (!key) return;
    uint32_t sxy = cpu->read_word(ws_anchor_addr);
    int32_t  ax  = (int32_t)(int16_t)(sxy & 0xFFFFu);
    uint32_t now = (uint32_t)s_frame_count;
    uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
    uint32_t victim = idx;
    for (int i = 0; i < WS_TAG_PROBES; i++) {
        uint32_t j = (idx + i) & (WS_TAG_BUCKETS - 1);
        WsTag *t = &ws_tags[j];
        if (t->key == key || t->key == 0) { victim = j; break; }
        if (now - t->stamp > 2) victim = j;  /* stale — reusable */
    }
    ws_tags[victim].key      = key;
    ws_tags[victim].stamp    = now;
    ws_tags[victim].anchor_x = ax;
    ws_last_tag_stamp = now;
}

/* Look up the executing GP0 command's prim in the tag table. The command's
 * first word lives at prim+4 (the PsyQ P_TAG header precedes it), but accept
 * a direct hit too in case a tag site passes the colour-word address. */
static int ws_tagged_anchor(int32_t *out_ax) {
    if (!ws_active() || gp0_cmd_source_addr == 0xFFFFFFFFu) return 0;
    uint32_t now = (uint32_t)s_frame_count;
    for (int variant = 0; variant < 2; variant++) {
        uint32_t key = (gp0_cmd_source_addr - (variant ? 0u : 4u)) & 0x1FFFFCu;
        uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
        for (int i = 0; i < WS_TAG_PROBES; i++) {
            WsTag *t = &ws_tags[(idx + i) & (WS_TAG_BUCKETS - 1)];
            if (t->key == key && now - t->stamp <= 2) {
                *out_ax = t->anchor_x;
                return 1;
            }
        }
    }
    return 0;
}

/* Native-wide-compatible "is the current GP0 prim sprite-tagged?" — same ws_tags
 * lookup as ws_tagged_anchor but gated on ws_engaged() (squash OR native-wide)
 * instead of ws_active() (squash only). Used by the GL native-wide 2D-backdrop
 * stretch to EXCLUDE foreground sprites: characters / Tomba / HUD are tagged by
 * psx_ws_sprite_tag (their prim ptr -> ws_tags), the 2D backdrop tiles are not.
 * Reads the live gp0_cmd_source_addr (the source addr of the prim being drawn). */
/* Flower-field backdrop data-structure address range — set by the dirty-RAM
 * interpreter when the backdrop generator (overlay 0x80116808) runs; its tile
 * packets live in [lo, hi]. The GL native-wide 2D-stretch gate matches the prim
 * being drawn (gp0_cmd_source_addr) against this to PRECISELY identify the
 * flower-field tiles — the 3D rock/foreground is untagged AND has narrow prims,
 * so tag/narrow heuristics alone mis-stretch and tear it. */
uint32_t g_ws_backdrop_lo = 0, g_ws_backdrop_hi = 0;
static int ws_nw_phase_backdrop = 0;
void gpu_ws_set_nw_phase_backdrop(int on) { ws_nw_phase_backdrop = on ? 1 : 0; }
/* diag: per-frame min/max of the prim source addrs the GL gate sees */
uint32_t g_bdg_src_lo = 0xFFFFFFFFu, g_bdg_src_hi = 0;
static uint32_t bdg_src_frame = 0xFFFFFFFFu;

/* ---- prim<->pixel correlation debug gate (ws_dbg_stretch command) ------------
 * To identify WHICH drawn prims constitute the native-wide 2D-backdrop void layer
 * (vs Tomba/HUD/3D), this lets a probe stretch an exact SELECTABLE prim set and
 * screenshot the result. When g_dbg_mode != 0 the GL stretch gate uses this
 * instead of the flower-struct range. Modes (buffer/frame-independent except 1):
 *   1 OT-range   : gp0_cmd_source_addr in [g_dbg_lo,g_dbg_hi]
 *   2 clut       : textured prim whose clut == g_dbg_clut
 *   3 tpage      : textured prim whose tpage == (g_dbg_clut & 0x1FF)
 *   4 tagged     : prim is sprite-tagged (psx_ws_prim_is_tagged)
 *   5 untag-tex  : textured AND not sprite-tagged
 *   6 all-tex    : every textured prim (sanity: should fill void + distort 3D)
 *   7 all        : every primitive (coarse correlation sanity check)
 *   8 all-flat   : every untextured primitive
 * Counters (per-frame, snapshotted by the present hook) report how many prims
 * matched and how many of those were sprite-tagged. */
int      g_dbg_mode = 0;
uint32_t g_dbg_lo = 0, g_dbg_hi = 0;
uint16_t g_dbg_clut = 0;
int      g_dbg_match_n = 0, g_dbg_match_tagged = 0;        /* snapshot (reported) */
static int s_dbg_match_n = 0, s_dbg_match_tagged = 0;      /* accumulate per frame */
int psx_ws_prim_is_tagged(void);   /* defined below */

static int dbg_gate_match(void) {
    uint32_t op   = (gp0_cmd_buf[0] >> 24) & 0xFFu;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint32_t a    = gp0_cmd_source_addr & 0x1FFFFFFFu;
    int textured  = ((op >= 0x20 && op <= 0x3F) && (op & 0x04)) || (op >= 0x64 && op <= 0x67);
    int tagged    = psx_ws_prim_is_tagged();
    int m = 0;
    switch (g_dbg_mode) {
        case 1: m = (g_dbg_hi > g_dbg_lo && a >= g_dbg_lo && a <= g_dbg_hi); break;
        case 2: m = (textured && clut == g_dbg_clut); break;
        case 3: m = (textured && ((uint16_t)(gp0_cmd_buf[4] >> 16) & 0x1FF) == (g_dbg_clut & 0x1FF)); break;
        case 4: m = tagged; break;
        case 5: m = (textured && !tagged); break;
        case 6: m = textured; break;
        case 7: m = 1; break;
        case 8: m = !textured; break;
        default: m = 0; break;
    }
    if (m) { s_dbg_match_n++; if (tagged) s_dbg_match_tagged++; }
    return m;
}

/* Called by the present hook each frame to publish the dbg match counters. */
void psx_ws_dbg_gate_frame_snapshot(void) {
    g_dbg_match_n = s_dbg_match_n; g_dbg_match_tagged = s_dbg_match_tagged;
    s_dbg_match_n = 0; s_dbg_match_tagged = 0;
}

/* Background-phase latch for the native-wide 2D-backdrop stretch. The far 2D
 * backdrop (a sprite-tagged tile grid: sky gradient + flower-ball field) draws
 * FIRST each frame, before the GTE 3D world. Once a shaded/gouraud prim (the 3D
 * world's signature -- the backdrop is sprites/flat) is drawn, the backdrop phase
 * is over: later TAGGED prims (HUD, characters) must NOT be stretched. Latched
 * per-frame at GP0 decode (ws_bg_phase_note, sees every prim in draw order). */
static uint32_t s_bg_phase_frame = 0xFFFFFFFFu;
static int      s_bg_phase_over  = 0;
void ws_bg_phase_note(uint32_t op) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s_bg_phase_frame) { s_bg_phase_frame = f; s_bg_phase_over = 0; }
    if (op >= 0x30u && op <= 0x3Fu) {
        s_bg_phase_over = 1;   /* shaded = 3D world (within-frame draw order) */
        ws_last_3d_stamp = f;  /* diagnostic only — see ws_2d_only_scene() for
                                  why shaded prims are NOT the scene classifier */
    }
}
static int ws_bg_phase_over(void) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s_bg_phase_frame) { s_bg_phase_frame = f; s_bg_phase_over = 0; }
    return s_bg_phase_over;
}

int psx_ws_prim_in_backdrop(void) {
    if (gp0_cmd_source_addr != 0xFFFFFFFFu) {
        uint32_t f = (uint32_t)s_frame_count;
        if (f != bdg_src_frame) { bdg_src_frame = f; g_bdg_src_lo = 0xFFFFFFFFu; g_bdg_src_hi = 0; }
        if (gp0_cmd_source_addr < g_bdg_src_lo) g_bdg_src_lo = gp0_cmd_source_addr;
        if (gp0_cmd_source_addr > g_bdg_src_hi) g_bdg_src_hi = gp0_cmd_source_addr;
    }
    if (g_dbg_mode != 0) return dbg_gate_match();   /* correlation override */
    if (ws_nw_phase_backdrop && !ws_bg_phase_over()) return 1;
    /* Real gate: stretch the 2D backdrop = sprite-tagged prims drawn in the
     * background phase (before the 3D world). Fills the native-wide void for both
     * the flower grid and the sky band; HUD/characters (tagged but post-3D) and the
     * GTE world (untagged) are excluded. */
    if (ws_bg_phase_over()) return 0;
    return psx_ws_prim_is_tagged();
}

int psx_ws_prim_is_tagged(void) {
    if (!ws_engaged() || gp0_cmd_source_addr == 0xFFFFFFFFu) return 0;
    uint32_t now = (uint32_t)s_frame_count;
    for (int variant = 0; variant < 2; variant++) {
        uint32_t key = (gp0_cmd_source_addr - (variant ? 0u : 4u)) & 0x1FFFFCu;
        uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
        for (int i = 0; i < WS_TAG_PROBES; i++) {
            WsTag *t = &ws_tags[(idx + i) & (WS_TAG_BUCKETS - 1)];
            if (t->key == key && now - t->stamp <= 2) return 1;
        }
    }
    return 0;
}

/* Squash x around pivot ax by ws_xnum/ws_xden, round to nearest. */
static int32_t ws_scale_about(int32_t x, int32_t ax) {
    int32_t d = x - ax;
    int32_t s = (d * ws_xnum + (d >= 0 ? ws_xden / 2 : -ws_xden / 2)) / ws_xden;
    return ax + s;
}

/* Squash a width; never below 1px so nothing vanishes. */
static int32_t ws_scale_len(int32_t w) {
    int32_t s = (w * ws_xnum + ws_xden / 2) / ws_xden;
    return s < 1 ? 1 : s;
}

/* Horizontal display width in drawing space (e.g. 320). */
static int32_t ws_disp_w(void) {
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    return di.width ? (int32_t)di.width : 320;
}

static int32_t ws_disp_h(void) {
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    return di.height ? (int32_t)di.height : 240;
}

/* Full-screen fades and environmental filters are authored as 320x240 TILEs.
 * In native-wide mode, grow only primitives that cover the complete native
 * display; ordinary world-space rectangles remain untouched. */
static void ws_expand_fullscreen_rect(int32_t *x, int32_t y, int *w, int h) {
    if (!ws_native_wide_active()) return;
    int W = (int)ws_disp_w(), H = (int)ws_disp_h();
    if (*x <= 0 && *x + *w >= W && y <= 0 && y + h >= H) {
        int off = ws_nw_offset();
        *x -= off;
        *w += 2 * off;
    }
}

/* In-game HUD pivot for an untagged screen-space SPRT spanning [x, x+w).
 * Only reached during gameplay (ws_active true). HUD elements are sparse and
 * corner-anchored, so pivot by thirds: outer-third elements anchor to their
 * screen edge (keeping the wide-screen corner position at native proportions),
 * the middle third to centre. A composite (counter box + digits) sits inside
 * one zone, so its pieces share a pivot and stay aligned. (Full-2D menu
 * screens never reach here — they get zero squash + 4:3 pillarbox instead.) */
static int32_t ws_hud_pivot(int32_t x, int32_t w) {
    int32_t W = ws_disp_w();
    int32_t cx = 2 * x + w;            /* 2*centre, avoids losing the half */
    if (3 * cx < 2 * W) return 0;
    if (3 * cx > 4 * W) return W;
    return W / 2;
}

/* Shared transform for fixed-size textured sprites (8x8 / 16x16 / 1x1 dot):
 * squash *x0 in place (around the tagged anchor, else the HUD pivot) and
 * return the squashed draw width, or 0 = no change. */
static int ws_sprt_fixed_transform(int32_t *x0, int w) {
    if (!ws_active()) return 0;
    int32_t ax;
    if (ws_tagged_anchor(&ax)) {
        *x0 = ws_scale_about(*x0, ax);
        return (int)ws_scale_len(w);
    }
    if (ws_hud_sprt) {
        *x0 = ws_scale_about(*x0, ws_hud_pivot(*x0, w));
        return (int)ws_scale_len(w);
    }
    return 0;
}

/* ---- Native-wide HUD corner re-anchoring ([widescreen] nw_hud_corners) ------
 * In native-wide the whole frame is composited into a wider surface centred by
 * ws_nw_offset() per side (the reveal). Screen-space 2D HUD (drawn with fixed
 * rect/sprite GP0 commands, never through the GTE — a 3D title's world is all
 * polygons) therefore lands inset from the true wide edges by exactly the
 * reveal. This pushes an outer-third HUD primitive the rest of the way to its
 * wide corner with an additive thirds shift: left third −offset, right third
 * +offset, middle unchanged (matches the squash-path ws_hud_pivot geometry, but
 * as a translate since native-wide does not squash). Composite pieces in one
 * zone share a shift and stay aligned. A configured command-source range lets
 * pure-2D games opt in only their HUD packet arena, leaving world primitives
 * untouched. Identity unless native-wide is engaged AND the game opts in, so
 * 4:3 and non-opted titles are byte-identical.
 * Returns the signed x delta to add to the prim's x before draw_offset. */
static int ws_nw_hud_corners = 0;
static int ws_nw_hud_tag_rects = 0;   /* rects shift even when tagged (A/B) */
static uint32_t ws_nw_left_hud_packet_lo = 0;
static uint32_t ws_nw_left_hud_packet_hi = 0;
void gpu_ws_set_nw_hud_corners(int on) { ws_nw_hud_corners = on ? 1 : 0; }
void gpu_ws_set_nw_hud_tag_rects(int on) { ws_nw_hud_tag_rects = on ? 1 : 0; }
void gpu_ws_set_nw_left_hud_packet_range(uint32_t lo, uint32_t hi) {
    ws_nw_left_hud_packet_lo = lo & 0x1FFFFFFFu;
    ws_nw_left_hud_packet_hi = hi & 0x1FFFFFFFu;
}
static int ws_nw_left_hud_packet(void) {
    if (!ws_native_wide_active() || gp0_cmd_source_addr == 0xFFFFFFFFu ||
        ws_nw_left_hud_packet_hi <= ws_nw_left_hud_packet_lo)
        return 0;
    uint32_t a = gp0_cmd_source_addr & 0x1FFFFFFFu;
    int matched = a >= ws_nw_left_hud_packet_lo && a < ws_nw_left_hud_packet_hi;
    return matched;
}
static int32_t ws_nw_hud_shift(int32_t x, int32_t w) {
    if (!ws_native_wide_active()) return 0;
    int32_t off = ws_nw_offset();
    if (off <= 0) return 0;
    if (!ws_nw_left_hud_packet() && !ws_nw_hud_corners) return 0;
    /* Sprite-tag titles (anchor configured): HUD ≡ UNTAGGED rect-family prims
     * — the same discriminator the squash path's hud_sprt_squash used. Tagged
     * prims are character billboards positioned from their GTE anchor; they
     * must never re-anchor. (Poly/line sites are excluded wholesale for tag
     * titles in ws_nw_hud_shift_vertices — their polys are the world.)
     * ws_nw_hud_tag_rects (TCP ws_hud_mode) lifts the exclusion for rects,
     * for live A/B: some HUD composites (Tomba's AP counter) render through
     * the tagged sprite funnel and stay inset without it. */
    if (ws_anchor_addr && !ws_nw_hud_tag_rects && psx_ws_prim_is_tagged()) return 0;
    int32_t W  = ws_disp_w();
    int32_t cx = 2 * x + w;            /* 2*centre, avoids losing the half */
    if (3 * cx < 2 * W) return -off;   /* left third  -> pull to left edge  */
    if (3 * cx > 4 * W) return  off;   /* right third -> push to right edge */
    return 0;                          /* middle third -> stay centred      */
}

/* Re-anchor every X coordinate of a HUD polygon as one rigid composite. The
 * command-source filter above is what makes this safe for pure-2D titles: only
 * the game's dedicated HUD packet arena reaches here, never world polygons. */
static void ws_nw_hud_shift_vertices(int32_t *vx, int count) {
    if (count <= 0) return;
    /* Sprite-tag titles: polygon/line prims are the GTE world and the tagged
     * character billboards, never HUD — only the rect-family sites (which
     * call ws_nw_hud_shift directly, with the untagged filter) re-anchor. */
    if (ws_anchor_addr) return;
    int32_t lo = vx[0], hi = vx[0];
    for (int i = 1; i < count; i++) {
        if (vx[i] < lo) lo = vx[i];
        if (vx[i] > hi) hi = vx[i];
    }
    int32_t d = ws_nw_hud_shift(lo, hi - lo);
    if (d) for (int i = 0; i < count; i++) vx[i] += d;
}

/* ---- Native-wide full-frame 2D backdrop stretch ([widescreen] nw_backdrop) ---
 * A screen-space background plane (sky gradient / backdrop image) is drawn as an
 * axis-aligned quad covering the whole 4:3 framebuffer [0,W]x[0,H]. It is NOT
 * GTE-projected, so native-wide leaves it at its 4:3 span, composited centred =
 * black bars in the revealed side margins (the pillarboxed sky). Detect exactly
 * that shape — a 4-vertex quad whose corners form a rectangle spanning ~the full
 * display width from ~the left edge — and stretch its X vertices about the
 * display centre by the wide ratio so it fills the wider frame; the texture/UV
 * (or gradient) simply stretches horizontally (invisible on a sky). GTE-drawn
 * world quads are perspective-distorted (not axis-aligned) and partial-width, so
 * they never match. Identity unless native-wide is engaged AND the game opts in.
 * Transforms vx[0..3] IN PLACE (pre-draw_offset); returns 1 if it applied. */
static int ws_nw_backdrop = 0;
void gpu_ws_set_nw_backdrop(int on) { ws_nw_backdrop = on ? 1 : 0; }
static int ws_nw_flat_backdrop = 0;
void gpu_ws_set_nw_flat_backdrop(int on) { ws_nw_flat_backdrop = on ? 1 : 0; }
int gpu_ws_nw_flat_backdrop_enabled(void) { return ws_nw_flat_backdrop; }
static int ws_nw_backdrop_stretch_quad(int32_t *vx, const int32_t *vy) {
    if (!ws_nw_backdrop || !ws_native_wide_active()) return 0;
    int32_t extra = ws_nw_extra();
    if (extra <= 0) return 0;
    int32_t W = ws_disp_w();
    const int32_t EDGE = 24;                 /* slack for "touches the frame edge" */
    int32_t minx = vx[0], maxx = vx[0], miny = vy[0], maxy = vy[0];
    for (int i = 1; i < 4; i++) {
        if (vx[i] < minx) minx = vx[i];
        if (vx[i] > maxx) maxx = vx[i];
        if (vy[i] < miny) miny = vy[i];
        if (vy[i] > maxy) maxy = vy[i];
    }
    /* Must span the full display width and a real vertical extent. */
    if (minx > EDGE || maxx < W - EDGE || (maxy - miny) < 64) return 0;
    /* Axis-aligned: every vertex X sits at either the min or the max edge, and
     * every Y at the top or bottom edge (a true screen-space rectangle). */
    for (int i = 0; i < 4; i++) {
        int xe = (vx[i] - minx <= EDGE) || (maxx - vx[i] <= EDGE);
        int ye = (vy[i] - miny <= EDGE) || (maxy - vy[i] <= EDGE);
        if (!xe || !ye) return 0;
    }
    /* Stretch X about the display centre by (W+extra)/W so [0,W] -> [-off, W+off],
     * which the wide compositor (+off) maps onto the full [0, W+extra] surface. */
    int32_t cx = W / 2;
    for (int i = 0; i < 4; i++) {
        int32_t d = vx[i] - cx;
        vx[i] = cx + (d * (W + extra) + (d >= 0 ? W / 2 : -W / 2)) / W;
    }
    return 1;
}

/* Polyline state */
static uint16_t polyline_color;       /* mono polyline: current color */
static int32_t  polyline_prev_x, polyline_prev_y;  /* previous vertex */
static uint16_t polyline_prev_c;      /* shaded polyline: previous color */
static int      polyline_semi_trans;  /* semi-transparency flag from command word */
static int      polyline_has_prev;    /* have we seen at least one vertex? */

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
/* Instrumentation: per-vblank range/count of GP0(E5) draw-offset-Y sets. If a
 * single frame sets offsets in BOTH the top (y<128) and bottom (y>=128) buffer
 * bands, the game is drawing different parts of the scene into different display
 * buffers in the same frame (candidate root of a character-in-one-buffer
 * strobe). Snapshotted each vblank into the *_last copies, exposed via debug. */
int32_t  g_doff_min_this = 0x7fffffff, g_doff_max_this = -0x7fffffff;
uint32_t g_doff_cnt_this = 0;
int32_t  g_doff_min_last = 0, g_doff_max_last = 0;
uint32_t g_doff_cnt_last = 0;

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

/* I_STAT — defined in memory.c; declared early so gpu_read_gpustat can
 * raise the VBLANK bit when a synthetic vblank fires from a tight
 * BIOS-shell GPUSTAT poll loop, without going through the
 * vblank-callback path (which would call SDL_Delay/Sleep). */
extern uint32_t i_stat;
/* Central IRQ-raise choke point (interrupts.c) — also records the device ring. */
extern void psx_irq_raise(uint32_t bit, uint32_t detail);

/* Display area start (GP1(05h)) */
static uint32_t display_area_x;
static uint32_t display_area_y;

/* ----- Native-wide compositor driving (see runtime/src/gpu_sw_renderer.c) ----
 * The renderer keeps a separate wide surface per framebuffer; we tell it which
 * back buffer each draw targets and present from the displayed buffer's surface.
 * A "framebuffer base" is a VRAM x-origin the display has scanned out from — we
 * learn that set from GP1(05h) so the mirror only engages for real display
 * buffers (not offscreen texture builds). Generic: no per-game constants. */
#define WS_FB_BASES 4
static uint32_t ws_fb_base[WS_FB_BASES];
static int      ws_fb_n = 0;
static void ws_note_display_base(uint32_t bx) {
    for (int i = 0; i < ws_fb_n; i++) if (ws_fb_base[i] == bx) return;
    if (ws_fb_n < WS_FB_BASES) { ws_fb_base[ws_fb_n++] = bx; return; }
    for (int i = 1; i < WS_FB_BASES; i++) ws_fb_base[i - 1] = ws_fb_base[i];
    ws_fb_base[WS_FB_BASES - 1] = bx;
}
static int ws_is_fb_base(uint32_t bx) {
    for (int i = 0; i < ws_fb_n; i++) if (ws_fb_base[i] == bx) return 1;
    return 0;
}
/* Point the renderer's wide mirror at the current back buffer (draw_area_left)
 * when native-wide is active and that buffer is a known display buffer; else
 * disable mirroring for this draw. Called when the draw env changes. */
static void ws_nw_sync_target(void) {
    if (!ws_native_wide_active()) { gr_wide_disable_target(); return; }
    gr_wide_configure((int)ws_disp_w() + ws_nw_extra(), ws_nw_offset());
    uint32_t base = draw_area_left;
    if (ws_is_fb_base(base)) gr_wide_set_target((int)base);
    else                     gr_wide_disable_target();
}

static void ws_clear_all_reveal_margins(void) {
    for (int i = 0; i < ws_fb_n; i++)
        gr_wide_clear_margins((int)ws_fb_base[i], 0, 512, 0, 3);
}

/* Stage-init already clears both synthetic margins once. Do not keep clearing
 * a guessed finite-map side here: MMX6's authored layers enter the reveal at
 * different times, so the side guess produced a moving black trim over valid
 * stage art. A stale reveal tile is safer than deleting submitted content. */
void gpu_ws_begin_linked_list(void) { }


/* Horizontal display range (GP1(06h)) */
static uint32_t h_display_x1;
static uint32_t h_display_x2;

/* Vertical display range (GP1(07h)) */
static uint32_t v_display_y1;
static uint32_t v_display_y2;

/* GPUREAD latch (GP1(10h) get-info result, or VRAM read data) */
static uint32_t gpuread_latch;

/* C0 (VRAM→CPU) capture slot — forward declaration for gpu_read_gpuread */
#define C0_HISTORY_CAP_FWD 32
static struct C0HistEntry {
    uint16_t x, y, w, h;
    uint32_t func_addr, sp_val, s1_val;
    uint32_t first_words[4];
    int read_count;
} c0_history_fwd[C0_HISTORY_CAP_FWD];
static int c0_history_count_fwd = 0;
static int c0_capture_slot_fwd = -1;

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
uint64_t g_pollhack_vblank_count = 0;  /* instrumentation: poll-fallback VBlank IRQ count */

/* ---- Initialization ---- */

static void gpu_reset_state(int clear_vram) {
    if (clear_vram) {
        memset(vram, 0, sizeof(vram));
    }
    gr_init(vram);

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

void gpu_init(void) {
    gpu_reset_state(1);
}

/* ---- GPUSTAT read (0x1F801814) ---- */

uint32_t gpu_read_gpustat(void) {
    /* Advance vblank when polled enough times from within a single function.
     * This handles BIOS VSYNC wait loops that poll LCF in tight loops.
     *
     * IMPORTANT: only update emulation state here (LCF + I_STAT). Do NOT
     * fire the vblank callback — that calls sdl_vblank_present, which
     * runs SDL_RenderPresent and SDL_Delay (Sleep). Entering Sleep from
     * an MMIO-read code path means recompiled MIPS code spends real wall
     * time inside Sleep — wrong context, hard to reason about, and
     * accumulates host stack frames inside the recompiled call tree.
     * The callback fires from the proper VBLANK trigger in
     * psx_check_interrupts (cycle-paced). */
    gpustat_poll_count++;
    if (gpustat_poll_count >= GPUSTAT_POLL_VBLANK_THRESHOLD) {
        gpustat_poll_count = 0;
        /* The recompiled BIOS + game carry per-block interrupt checks
         * (psx_check_interrupts_at) and per-block cycle charging, so tight
         * LCF/VSync poll loops DO advance guest cycles and let the ONE
         * cycle-paced VBlank authority (interrupts.c) fire on schedule. The old
         * "spin forever" premise that justified raising VBlank+LCF from a raw
         * GPUSTAT read-count is stale — and it injected ~38 fake VBlanks/s
         * (measured on Crash Bash), delivering ~96/s to the game instead of 60,
         * over-advancing the game's VSync frame counter and jittering animation
         * (character strobe). Firing is now OFF by default; PSX_POLLHACK_VBLANK=1
         * restores the legacy behavior for A/B. */
        static int poll_fire = -1;
        if (poll_fire < 0) {
            const char* e = getenv("PSX_POLLHACK_VBLANK");
            poll_fire = (e && e[0] == '1') ? 1 : 0;
        }
        if (poll_fire) {
            g_pollhack_vblank_count++;   /* count only when actually firing */
            lcf ^= 1;
            psx_irq_raise(0, 0); /* IRQ_VBLANK (GPUSTAT-poll fallback path) */
            /* DEQUEUE: VBlank fired via the GPUSTAT-poll fallback path (distinct
             * from the cycle-paced VBlank in psx_check_interrupts). */
            event_ring_record_aux(EV_DEQ, (uint8_t)SRC_VBLANK, 0xFFFFFFFFu);
        }
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

    /* Read two 16-bit pixels from VRAM and pack into one 32-bit word.
     * Routed through the renderer facade: under the GL backend the GPU-side
     * framebuffer is authoritative and must be synced down before the CPU
     * array is read (no-op cost on the software backend). */
    uint32_t value = 0;
    for (int i = 0; i < 2; i++) {
        uint16_t rx = (vram_read_x + vram_read_col) % 1024;
        uint16_t ry = (vram_read_y + vram_read_row) % 512;
        value |= (uint32_t)gr_vram_read((int)rx, (int)ry) << (i * 16);

        if (++vram_read_col == vram_read_w) {
            vram_read_col = 0;
            if (++vram_read_row == vram_read_h) {
                /* Transfer complete */
                vram_read_active = 0;
                break;
            }
        }
    }

    /* Capture first words for C0 debug */
    if (c0_capture_slot_fwd >= 0 && c0_capture_slot_fwd < C0_HISTORY_CAP_FWD) {
        int rc = c0_history_fwd[c0_capture_slot_fwd].read_count++;
        if (rc < 4)
            c0_history_fwd[c0_capture_slot_fwd].first_words[rc] = value;
        if (!vram_read_active)
            c0_capture_slot_fwd = -1;  /* transfer complete */
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

/* i_stat extern declared earlier in this file (above gpu_read_gpustat). */

void gpu_vblank_tick(void) {
    lcf ^= 1;
    /* snapshot per-frame draw-offset-Y range for the strobe instrumentation */
    if (g_doff_cnt_this) {
        g_doff_min_last = g_doff_min_this;
        g_doff_max_last = g_doff_max_this;
        g_doff_cnt_last = g_doff_cnt_this;
    }
    g_doff_min_this = 0x7fffffff; g_doff_max_this = -0x7fffffff; g_doff_cnt_this = 0;
    gpustat_poll_count = 0;
    psx_irq_raise(0, 0); /* IRQ_VBLANK (gpu_vblank_tick) */
    if (vblank_callback) vblank_callback();
}

const uint16_t* gpu_get_vram(void) {
    return vram;
}

static uint8_t gpu_vram_byte(uint32_t byte_x, uint32_t y) {
    uint16_t hw = vram[((y & 511u) * 1024u) + ((byte_x >> 1) & 1023u)];
    return (byte_x & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

/* ---- Present-time screen-colour LUT (verified-enhancement, opt-in) -------
 *
 * PRESENT-TIME ONLY. This sits on the 15-bit-scanout -> RGB888 conversion that
 * feeds the SDL/GL present path. It never touches VRAM and never runs on the
 * depth24 (FMV) scanout (see gpu_display_pixel_rgb). It defaults to SCREEN_RAW
 * (the original exact 5->3-replicated expansion below), so with the feature off
 * the conversion — and therefore every hashed/oracle-diffed frame — is
 * byte-identical to upstream. Opt in via PSX_SCREEN={crt,composite,trinitron};
 * any other value (or unset) keeps the raw path.
 *
 * Built lazily on first use because env is read once and the GPU has no
 * settings plumb yet. The raw fast-path below is preserved verbatim so the
 * default never even consults the LUT. */
static ColorLut* s_screen_lut = NULL;
static int       s_screen_lut_init = 0;
static int       s_screen_kind_cfg = SCREEN_RAW;  /* config/launcher-set; env overrides */

void gpu_set_screen_kind(int kind) {
    if (kind < SCREEN_RAW || kind > SCREEN_TRINITRON) kind = SCREEN_RAW;
    if (kind == s_screen_kind_cfg) return;
    s_screen_kind_cfg = kind;
    s_screen_lut_init = 0;  /* rebuild on next scanout */
}

static void screen_lut_ensure(void) {
    if (s_screen_lut_init) return;
    s_screen_lut_init = 1;
    if (s_screen_lut) { color_lut_destroy(s_screen_lut); s_screen_lut = NULL; }
    /* Precedence: PSX_SCREEN env (debug override) wins if set+valid; otherwise
     * the config/launcher value. Default (no env, raw config) = passthrough. */
    ScreenKind kind = (ScreenKind)s_screen_kind_cfg;
    const char* name = getenv("PSX_SCREEN");
    ScreenKind envk;
    if (name && screen_kind_from_name(name, &envk)) kind = envk;
    if (kind == SCREEN_RAW) {
        s_screen_lut = NULL;  /* raw fast-path; passthrough */
        return;
    }
    ColorSettings s;
    s.screen = kind;
    s.darken = -1.0;          /* per-screen default */
    s.target = DISPLAY_SRGB;
    s_screen_lut = color_lut_create(&s);
    if (s_screen_lut && color_lut_is_passthrough(s_screen_lut)) {
        color_lut_destroy(s_screen_lut);
        s_screen_lut = NULL;
    }
}

static void gpu_rgb555_to_rgb888(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
    screen_lut_ensure();
    if (s_screen_lut) {
        color_lut_map555(s_screen_lut, c, r, g, b);
        return;
    }
    /* Default raw path — byte-identical to upstream. */
    *r = (uint8_t)((c & 0x1Fu) << 3);
    *g = (uint8_t)(((c >> 5) & 0x1Fu) << 3);
    *b = (uint8_t)(((c >> 10) & 0x1Fu) << 3);
}

void gpu_display_pixel_rgb(const GpuDisplayInfo* di, uint32_t x, uint32_t y,
                           uint8_t* r, uint8_t* g, uint8_t* b) {
    if (di->depth24) {
        uint32_t byte_x = ((di->display_x & 1023u) * 2u) + x * 3u;
        uint32_t vy = (di->display_y + y) & 511u;
        *r = gpu_vram_byte(byte_x + 0u, vy);
        *g = gpu_vram_byte(byte_x + 1u, vy);
        *b = gpu_vram_byte(byte_x + 2u, vy);
        return;
    }

    uint32_t vx = (di->display_x + x) & 1023u;
    uint32_t vy = (di->display_y + y) & 511u;
    gpu_rgb555_to_rgb888(vram[vy * 1024u + vx], r, g, b);
}

uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* di, uint32_t x, uint32_t y) {
    uint8_t r, g, b;
    gpu_display_pixel_rgb(di, x, y, &r, &g, &b);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void gpu_get_display_info(GpuDisplayInfo* out) {
    out->display_x = display_area_x;
    out->display_y = display_area_y;
    out->depth24   = (int)(display_depth & 1u);
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
    }
    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
}

/* Execute mono quad (GP0 0x28-0x2B) — two triangles: (0,1,2) and (2,1,3) */
static void gp0_exec_mono_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t vx[4], vy[4];
    for (int i = 0; i < 4; i++)
        parse_vertex(gp0_cmd_buf[1 + i], &vx[i], &vy[i]);

    /* Full-screen filters are commonly encoded as an axis-aligned quad. Drawing
     * a semi-transparent quad as two independent triangles blends their shared
     * diagonal twice; render the equivalent rectangle once to avoid that seam.
     * The full-screen helper also grows the native 320-wide filter across the
     * sidecar surface in native-wide mode. Ordinary world quads are unchanged. */
    if (vx[0] == vx[2] && vx[1] == vx[3] &&
        vy[0] == vy[1] && vy[2] == vy[3] &&
        vx[1] > vx[0] && vy[2] > vy[0] &&
        vx[0] <= 0 && vx[1] >= ws_disp_w() &&
        vy[0] <= 0 && vy[2] >= ws_disp_h()) {
        int32_t x = vx[0];
        int32_t y = vy[0];
        int w = (int)(vx[1] - vx[0]);
        int h = (int)(vy[2] - vy[0]);
        ws_expand_fullscreen_rect(&x, y, &w, h);
        x += draw_offset_x;
        y += draw_offset_y;
        gr_set_semi_transparency(semi_trans, (int)semi_transparency);
        gr_draw_flat_rect(x, y, w, h, color);
        return;
    }
    ws_nw_backdrop_stretch_quad(vx, vy);   /* full-frame 2D backdrop stretch (no-op else) */
    ws_nw_hud_shift_vertices(vx, 4);
    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
    gr_draw_flat_triangle(vx[2], vy[2], vx[1], vy[1], vx[3], vy[3], color);
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
    }
    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_gouraud_triangle(vx[0], vy[0], c[0],
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
    }
    ws_nw_backdrop_stretch_quad(vx, vy);   /* full-frame 2D backdrop stretch (sky gradient; no-op else) */
    ws_nw_hud_shift_vertices(vx, 4);
    for (int i = 0; i < 4; i++) {
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
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_gouraud_triangle(vx[0], vy[0], c[0],
                             vx[1], vy[1], c[1],
                             vx[2], vy[2], c[2]);
    gr_draw_gouraud_triangle(vx[2], vy[2], c[2],
                             vx[1], vy[1], c[1],
                             vx[3], vy[3], c[3]);
}

/* Helper: build texpage word from GPU state for SW renderer.
 * Format: bits 0-3 = X base, bit 4 = Y base, bits 5-6 = semi-trans, bits 7-8 = color depth */
static uint16_t current_texpage(void) {
    return (uint16_t)(texpage_x | (texpage_y << 4) |
                      (semi_transparency << 5) | (texpage_colors << 7));
}

/* Hardware: the texpage attribute word carried inside every textured polygon
 * (GP0 0x24-0x3F with the texture bit) is copied into the GPU draw-mode state
 * (GPUSTAT bits 0-8) exactly like GP0(E1) bits 0-8 — the poly's own word, not
 * the last E1, decides its semi-transparency mode, and later rectangle/sprite
 * prims (which carry no texpage word) consume the state the poly left behind.
 * Beetle: SetTPage(CB[4 + ((cc>>4)&1)] >> 16) on every textured poly.
 * Bits 9-10 (dither / draw-to-display) exist only in E1; bit 11 (texture
 * disable) only latches when GP1(09h) allowed it, which polys can't grant. */
static void set_tpage_from_poly(uint16_t tpage_word) {
    texpage_x         = tpage_word & 0xF;
    texpage_y         = (tpage_word >> 4) & 1;
    semi_transparency = (tpage_word >> 5) & 3;
    texpage_colors    = (tpage_word >> 7) & 3;
}

/* Helper: set up SW renderer state before a textured draw.
 * semi_trans: whether the primitive has semi-transparency enabled (from opcode bit)
 * raw_texture: 1 if this is a raw-texture opcode (bit 0 of GP0 opcode set) */
static void setup_textured_draw(uint32_t color24, int semi_trans, int raw_texture) {
    uint32_t r = (color24 >> 0) & 0xFF;
    uint32_t g = (color24 >> 8) & 0xFF;
    uint32_t b = (color24 >> 16) & 0xFF;
    gr_set_color_modulation((int)r, (int)g, (int)b, raw_texture);
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
}

/* Execute textured triangle (GP0 0x24-0x27) */
static void gp0_exec_textured_tri(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
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
    /* Texpage from word 4 bits 16-31 */
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[4] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;
    set_tpage_from_poly(tpage_word);

    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    setup_textured_draw(color24, semi_trans, raw_texture);
    gr_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
}

/* Execute textured quad (GP0 0x2C-0x2F) */
static void gp0_exec_textured_quad(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
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
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[4] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;
    set_tpage_from_poly(tpage_word);

    /* Widescreen: tagged billboard quads carry CPU-computed pixel offsets the
     * GTE squash never saw — re-squash every X around the prim's anchor. */
    {
        int32_t ws_ax;
        if (ws_tagged_anchor(&ws_ax))
            for (int i = 0; i < 4; i++) vx[i] = ws_scale_about(vx[i], ws_ax);
    }
    ws_nw_backdrop_stretch_quad(vx, vy);   /* full-frame 2D backdrop image stretch (no-op else) */
    ws_nw_hud_shift_vertices(vx, 4);

    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    setup_textured_draw(color24, semi_trans, raw_texture);

    if (vy[0] == vy[1] && vy[2] == vy[3] &&
        vx[0] == vx[2] && vx[1] == vx[3] &&
        u[0] == u[2] && u[1] == u[3] &&
        v[0] == v[1] && v[2] == v[3]) {
        int x = vx[0] < vx[1] ? vx[0] : vx[1];
        int y = vy[0] < vy[2] ? vy[0] : vy[2];
        int w = vx[0] < vx[1] ? vx[1] - vx[0] : vx[0] - vx[1];
        int h = vy[0] < vy[2] ? vy[2] - vy[0] : vy[0] - vy[2];
        int left_u  = vx[0] < vx[1] ? u[0] : u[1];
        int right_u = vx[0] < vx[1] ? u[1] : u[0];
        int top_v   = vy[0] < vy[2] ? v[0] : v[2];
        int bot_v   = vy[0] < vy[2] ? v[2] : v[0];
        if (w > 0 && h > 0) {
            if (right_u - left_u == w && bot_v - top_v == h) {
                gr_draw_textured_rect(x, y, w, h, left_u, top_v,
                                      clut_x, clut_y, tpage);
                return;
            }
            gr_draw_textured_rect_scaled(x, y, w, h, left_u, top_v,
                                         right_u, bot_v,
                                         clut_x, clut_y, tpage);
            return;
        }
    }

    gr_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
    gr_draw_textured_triangle(vx[2], vy[2], u[2], v[2],
                              vx[1], vy[1], u[1], v[1],
                              vx[3], vy[3], u[3], v[3],
                              clut_x, clut_y, tpage);
}

/* Execute shaded textured triangle (GP0 0x34-0x37) */
static void gp0_exec_shaded_textured_tri(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[3], vy[3];
    int u[3], v[3];
    uint32_t c[3];
    /* Layout: C0, V0, TC0+clut, C1, V1, TC1+tpage, C2, V2, TC2 */
    c[0] = gp0_cmd_buf[0] & 0xFFFFFFu;
    c[1] = gp0_cmd_buf[3] & 0xFFFFFFu;
    c[2] = gp0_cmd_buf[6] & 0xFFFFFFu;
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[4], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[7], &vx[2], &vy[2]);
    u[0] = gp0_cmd_buf[2] & 0xFF;  v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[5] & 0xFF;  v[1] = (gp0_cmd_buf[5] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[8] & 0xFF;  v[2] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[5] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;
    set_tpage_from_poly(tpage_word);

    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_shaded_textured_triangle(vx[0], vy[0], u[0], v[0], c[0],
                                     vx[1], vy[1], u[1], v[1], c[1],
                                     vx[2], vy[2], u[2], v[2], c[2],
                                     clut_x, clut_y, tpage, raw_texture);
}

/* Execute shaded textured quad (GP0 0x3C-0x3F) */
static void gp0_exec_shaded_textured_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[4], vy[4];
    int u[4], v[4];
    uint32_t c[4];
    /* Layout: C0, V0, TC0+clut, C1, V1, TC1+tpage, C2, V2, TC2, C3, V3, TC3 */
    c[0] = gp0_cmd_buf[0] & 0xFFFFFFu;
    c[1] = gp0_cmd_buf[3] & 0xFFFFFFu;
    c[2] = gp0_cmd_buf[6] & 0xFFFFFFu;
    c[3] = gp0_cmd_buf[9] & 0xFFFFFFu;
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
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[5] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;
    set_tpage_from_poly(tpage_word);

    ws_nw_hud_shift_vertices(vx, 4);
    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }

    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_shaded_textured_triangle(vx[0], vy[0], u[0], v[0], c[0],
                                     vx[1], vy[1], u[1], v[1], c[1],
                                     vx[2], vy[2], u[2], v[2], c[2],
                                     clut_x, clut_y, tpage, raw_texture);
    gr_draw_shaded_textured_triangle(vx[2], vy[2], u[2], v[2], c[2],
                                     vx[1], vy[1], u[1], v[1], c[1],
                                     vx[3], vy[3], u[3], v[3], c[3],
                                     clut_x, clut_y, tpage, raw_texture);
}

/* Execute mono line (GP0 0x40-0x47) — Bresenham */
static void gp0_exec_mono_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    parse_vertex(gp0_cmd_buf[2], &x1, &y1);
    int32_t vx[2] = { x0, x1 };
    ws_nw_hud_shift_vertices(vx, 2);
    x0 = vx[0]; x1 = vx[1];
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_line(x0, y0, x1, y1, color);
}

/* Execute shaded line (GP0 0x50-0x57) */
static void gp0_exec_shaded_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t c0 = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    uint16_t c1 = rgb888_to_rgb555(gp0_cmd_buf[2] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    parse_vertex(gp0_cmd_buf[3], &x1, &y1);
    int32_t vx[2] = { x0, x1 };
    ws_nw_hud_shift_vertices(vx, 2);
    x0 = vx[0]; x1 = vx[1];
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_shaded_line(x0, y0, c0, x1, y1, c1);
}

/* Execute mono rectangle (GP0 0x60-0x63) */
static void gp0_exec_mono_rect(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int w = gp0_cmd_buf[2] & 0xFFFFu;
    int h = (gp0_cmd_buf[2] >> 16) & 0xFFFFu;
    if (w > 1023) w = 1023;
    if (h > 511)  h = 511;
    ws_expand_fullscreen_rect(&x0, y0, &w, h);
    x0 += ws_nw_hud_shift(x0, w);   /* native-wide HUD corner re-anchor (no-op else) */
    x0 += draw_offset_x; y0 += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_rect(x0, y0, w, h, color);
}

/* Execute textured rectangle (GP0 0x64-0x67) */
static void gp0_exec_textured_rect(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    int w = gp0_cmd_buf[3] & 0x3FF;
    int h = (gp0_cmd_buf[3] >> 16) & 0x1FF;
    if (w > 1023) w = 1023;
    if (h > 511)  h = 511;

    /* Widescreen: tagged sprite parts squash around their projected anchor;
     * untagged SPRTs are screen-space 2D (HUD/menus) and squash around the
     * display centre. Texels keep full coverage via the scaled-rect path. */
    int ws_w = 0;
    if (ws_active() && w > 0) {
        int32_t ws_ax;
        if (ws_tagged_anchor(&ws_ax)) {
            x0 = ws_scale_about(x0, ws_ax);
            ws_w = (int)ws_scale_len(w);
        } else if (ws_hud_sprt) {
            x0 = ws_scale_about(x0, ws_hud_pivot(x0, w));
            ws_w = (int)ws_scale_len(w);
        }
    }
    x0 += ws_nw_hud_shift(x0, w);   /* native-wide HUD corner re-anchor (no-op else) */

    x0 += draw_offset_x; y0 += draw_offset_y;
    setup_textured_draw(color24, semi_trans, raw_texture);
    if (ws_w && ws_w != w)
        gr_draw_textured_rect_scaled(x0, y0, ws_w, h, u0, v0, u0 + w, v0 + h,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, w, h, u0, v0, clut_x, clut_y, current_texpage());
}

/* Execute 1x1 dot (GP0 0x68-0x6B) */
static void gp0_exec_mono_dot(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x, y;
    parse_vertex(gp0_cmd_buf[1], &x, &y);
    x += ws_nw_hud_shift(x, 1);
    x += draw_offset_x; y += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_rect(x, y, 1, 1, color);
}

/* Execute 8x8 textured sprite (GP0 0x74-0x77) */
static void gp0_exec_textured_8x8(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int ws_w = ws_sprt_fixed_transform(&x0, 8);
    x0 += ws_nw_hud_shift(x0, 8);   /* native-wide HUD corner re-anchor (no-op else) */
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    setup_textured_draw(color24, semi_trans, raw_texture);
    if (ws_w && ws_w != 8)
        gr_draw_textured_rect_scaled(x0, y0, ws_w, 8, u0, v0, u0 + 8, v0 + 8,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, 8, 8, u0, v0, clut_x, clut_y, current_texpage());
}

/* Execute 8x8 sprite (GP0 0x70-0x73) */
static void gp0_exec_mono_8x8(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    x0 += ws_nw_hud_shift(x0, 8);
    x0 += draw_offset_x; y0 += draw_offset_y;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_rect(x0, y0, 8, 8, color);
}

/* Execute 16x16 textured sprite (GP0 0x7C-0x7F) */
static void gp0_exec_textured_16x16(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    /* Never suppress MMX6 BG packets at a guessed finite-map boundary. The
     * classifier cannot distinguish an authored layer entering the reveal from
     * a stale ring slot; suppressing here caused the stage-start black flicker. */
    int ws_w = ws_sprt_fixed_transform(&x0, 16);
    x0 += ws_nw_hud_shift(x0, 16);   /* native-wide HUD corner re-anchor (no-op else) */
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    /* MMX6 host-side reveal: learn clut -> tpage/state from native BG tiles. */
    if (gpu_ws_mmx6_reveal_is_active())
        gpu_ws_mmx6_bg_record(gp0_cmd_source_addr, clut, current_texpage(),
                              color24, (int)semi_transparency, raw_texture);

    setup_textured_draw(color24, semi_trans, raw_texture);
    if (ws_w && ws_w != 16)
        gr_draw_textured_rect_scaled(x0, y0, ws_w, 16, u0, v0, u0 + 16, v0 + 16,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, 16, 16, u0, v0, clut_x, clut_y, current_texpage());
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

    /* Fill ignores draw area, mask bits, and draw offset — writes directly to
     * VRAM. Routed through the renderer so it also fills the hi-res
     * supersampling mirror (no-op cost when supersampling is off). */
    gr_fill_rect((int)dst_x, (int)dst_y, (int)width, (int)height, color16);

    /* Native-wide: when the game clears a display buffer, clear the full width
     * of that buffer's wide surface over the same rows — refreshing the centred
     * content region and keeping the revealed margins clean. */
    if (ws_native_wide_active() && ws_is_fb_base(dst_x)) {
        gr_wide_clear((int)dst_x, (int)dst_y, (int)height, color16);
        /* MMX6 host-side reveal: draw the ±LEFT margin BG columns into the freshly
         * cleared band (backmost, so objects mirror over them). Uses dst_y as the band
         * + live scroll; no dependence on this frame's not-yet-applied E3/E4/E5. */
        gpu_ws_mmx6_emit_reveal((int)dst_x, (int)dst_y);
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
    gr_set_texture_window(texture_window_value);
}

static void gp0_exec_draw_area_tl(void) {
    /* 0xE3 — Set Drawing Area Top-Left
     * Bits 0-9: X
     * Bits 10-19: Y */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_area_left = param & 0x3FF;
    draw_area_top  = (param >> 10) & 0x3FF;
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    ws_nw_sync_target();  /* back buffer (draw_area_left) → wide mirror surface */
}

static void gp0_exec_draw_area_br(void) {
    /* 0xE4 — Set Drawing Area Bottom-Right
     * Bits 0-9: X
     * Bits 10-19: Y */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_area_right  = param & 0x3FF;
    draw_area_bottom = (param >> 10) & 0x3FF;
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    ws_nw_sync_target();  /* back buffer (draw_area_left) → wide mirror surface */
}

static void gp0_exec_draw_offset(void) {
    /* 0xE5 — Set Drawing Offset
     * Bits 0-10: X (signed 11-bit)
     * Bits 11-21: Y (signed 11-bit) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_offset_x = sign_extend(param & 0x7FFu, 11);
    draw_offset_y = sign_extend((param >> 11) & 0x7FFu, 11);
    if (draw_offset_y < g_doff_min_this) g_doff_min_this = draw_offset_y;
    if (draw_offset_y > g_doff_max_this) g_doff_max_this = draw_offset_y;
    g_doff_cnt_this++;
    /* Native-wide mirrors framebuffer draws into a separate wide surface (canonical
     * VRAM stays faithful); the renderer needs the live draw offset to translate
     * into surface-local coords, so keep gr_set_draw_offset current. */
    gr_set_draw_offset(draw_offset_x, draw_offset_y);
}

static void gp0_exec_mask_bit(void) {
    /* 0xE6 — Mask Bit Setting
     * Bit 0: set mask bit when drawing (force bit 15 of pixels)
     * Bit 1: check mask bit (don't draw to pixels with bit 15 set) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    set_mask_bit   = param & 1;
    check_mask_bit = (param >> 1) & 1;
    gr_set_mask_bits((int)set_mask_bit, (int)check_mask_bit);
}

/* A0 upload history for debug inspection */
#define A0_HISTORY_CAP 128
typedef struct {
    uint16_t x, y, w, h;
    uint32_t first_words[4];
    int word_count;
    uint32_t func_addr;
    uint32_t sp_val;       /* CPU $sp at time of A0 header */
    uint32_t ra_val;       /* CPU $ra register at time of A0 header */
    uint32_t s1_val;       /* CPU $s1 = LoadImage RECT ptr (x/y/w/h) in FUN_80069dfc */
    uint32_t s2_val;       /* CPU $s2 = TRUE source pixel ptr (`move s2,a1` at entry) */
    uint32_t a0_val;       /* CPU $a0 at capture (RECT arg; may be clobbered) */
    uint32_t a1_val;       /* CPU $a1 at capture (source arg; may be clobbered) */
    uint32_t frame_stamp;  /* s_frame_count at the upload — for load-vs-upload ordering */
    uint32_t stack[10];    /* first 10 words from sp (sp+32=saved $s1, sp+36=saved $ra) */
} A0HistEntry;
static A0HistEntry a0_history[A0_HISTORY_CAP];
static int a0_history_count = 0;
static int a0_capture_slot = -1;  /* currently capturing data words for this slot */

extern uint32_t psx_read_word(uint32_t addr);

int gpu_get_a0_history(int index, int *x, int *y, int *w, int *h,
                       uint32_t *fw0, uint32_t *fw1, int *wcount) {
    if (index < 0 || index >= a0_history_count) return 0;
    *x = a0_history[index].x; *y = a0_history[index].y;
    *w = a0_history[index].w; *h = a0_history[index].h;
    *fw0 = a0_history[index].first_words[0];
    *fw1 = a0_history[index].first_words[1];
    *wcount = a0_history[index].word_count;
    return 1;
}
int gpu_get_a0_count(void) { return a0_history_count; }
int gpu_get_a0_extra(int index, uint32_t *func, uint32_t *sp, uint32_t *ra,
                     uint32_t *s1, uint32_t *stack10) {
    if (index < 0 || index >= a0_history_count) return 0;
    *func = a0_history[index].func_addr;
    *sp = a0_history[index].sp_val;
    *ra = a0_history[index].ra_val;
    *s1 = a0_history[index].s1_val;
    memcpy(stack10, a0_history[index].stack, 10 * sizeof(uint32_t));
    return 1;
}
/* True source pointer + arg regs + frame for the LoadImage upload (s2 = source). */
int gpu_get_a0_src(int index, uint32_t *s2, uint32_t *a0, uint32_t *a1, uint32_t *frame) {
    if (index < 0 || index >= a0_history_count) return 0;
    *s2 = a0_history[index].s2_val;
    *a0 = a0_history[index].a0_val;
    *a1 = a0_history[index].a1_val;
    *frame = a0_history[index].frame_stamp;
    return 1;
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

    /* Record for debug */
    if (a0_history_count < A0_HISTORY_CAP) {
        int slot = a0_history_count++;
        a0_history[slot].x = vram_write_x;
        a0_history[slot].y = vram_write_y;
        a0_history[slot].w = vram_write_w;
        a0_history[slot].h = vram_write_h;
        a0_history[slot].word_count = 0;
        a0_history[slot].func_addr = g_debug_current_func_addr;
        memset(a0_history[slot].first_words, 0, sizeof(a0_history[slot].first_words));
        /* Capture CPU context for caller tracing.
         * func_1FC38524 (shell LoadImage) uses $s1 as source data pointer. */
        a0_history[slot].sp_val = 0;
        a0_history[slot].ra_val = 0;
        a0_history[slot].s1_val = 0;
        a0_history[slot].s2_val = 0;
        a0_history[slot].a0_val = 0;
        a0_history[slot].a1_val = 0;
        a0_history[slot].frame_stamp = (uint32_t)s_frame_count;
        memset(a0_history[slot].stack, 0, sizeof(a0_history[slot].stack));
        if (debug_cpu_ptr) {
            uint32_t sp = debug_cpu_ptr->gpr[29];
            a0_history[slot].sp_val = sp;
            a0_history[slot].ra_val = debug_cpu_ptr->gpr[31];
            a0_history[slot].s1_val = debug_cpu_ptr->gpr[17]; /* $s1 */
            a0_history[slot].s2_val = debug_cpu_ptr->gpr[18]; /* $s2 = source ptr */
            a0_history[slot].a0_val = debug_cpu_ptr->gpr[4];  /* $a0 */
            a0_history[slot].a1_val = debug_cpu_ptr->gpr[5];  /* $a1 */
            uint32_t sp_phys = sp & 0x1FFFFFu;
            for (int si = 0; si < 10 && sp_phys + (si + 1) * 4 <= 0x200000u; si++)
                a0_history[slot].stack[si] = psx_read_word(sp + si * 4);
        }
        a0_capture_slot = slot;
    }

    vram_write_col = 0;
    vram_write_row = 0;

    uint32_t num_pixels = (uint32_t)vram_write_w * (uint32_t)vram_write_h;
    vram_write_remaining = (num_pixels + 1) / 2;

    if (vram_write_remaining > 0)
        gp0_state = GP0_VRAM_WRITE;
}

/* C0 (VRAM→CPU) history — uses c0_history_fwd declared at top of file */
#define C0_HISTORY_CAP C0_HISTORY_CAP_FWD
#define c0_history c0_history_fwd
#define c0_history_count c0_history_count_fwd
#define c0_capture_slot c0_capture_slot_fwd

int gpu_get_c0_count(void) { return c0_history_count; }
int gpu_get_c0_history(int index, int *x, int *y, int *w, int *h,
                       uint32_t *func, uint32_t *sp, uint32_t *s1,
                       uint32_t *fw0, uint32_t *fw1, int *rcount) {
    if (index < 0 || index >= c0_history_count) return 0;
    *x = c0_history[index].x; *y = c0_history[index].y;
    *w = c0_history[index].w; *h = c0_history[index].h;
    *func = c0_history[index].func_addr;
    *sp = c0_history[index].sp_val;
    *s1 = c0_history[index].s1_val;
    *fw0 = c0_history[index].first_words[0];
    *fw1 = c0_history[index].first_words[1];
    *rcount = c0_history[index].read_count;
    return 1;
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

    /* Record for debug */
    if (c0_history_count < C0_HISTORY_CAP) {
        int slot = c0_history_count++;
        c0_history[slot].x = vram_read_x;
        c0_history[slot].y = vram_read_y;
        c0_history[slot].w = vram_read_w;
        c0_history[slot].h = vram_read_h;
        c0_history[slot].func_addr = g_debug_current_func_addr;
        c0_history[slot].sp_val = debug_cpu_ptr ? debug_cpu_ptr->gpr[29] : 0;
        c0_history[slot].s1_val = debug_cpu_ptr ? debug_cpu_ptr->gpr[17] : 0;
        memset(c0_history[slot].first_words, 0, sizeof(c0_history[slot].first_words));
        c0_history[slot].read_count = 0;
        c0_capture_slot = slot;
    }

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

/* ---- Per-frame GP0 command ring (always-on, queried via debug server) ---- */
/* We record every GP0 command (header + up to 6 payload words) with the
 * frame number it was issued in. Per CLAUDE.md ring-buffer rule: capture
 * is continuous and observers query a window of interest later, not arm-
 * then-record. ~34 MB at 1M entries. Polyline / long commands get the
 * first 6 payload words; that's enough for the header + first vertex pair
 * + first uv/color word for diagnosing per-primitive state. */

extern uint64_t s_frame_count;  /* defined in debug_server.c */
extern uint32_t g_debug_last_store_pc;  /* defined in debug_server.c */
extern uint32_t g_debug_current_func_addr;  /* defined in debug_server.c */
uint32_t debug_guest_ra(void);  /* accessor in debug_server.c (guest $ra) */
uint32_t debug_guest_sp(void);  /* accessor in debug_server.c (guest $sp) */
extern uint8_t *memory_get_ram_ptr(void); /* raw 2MB main-RAM base (no lockstep) */

/* Bounded guest-stack unwind for VRAM-copy builder attribution. Scans the live
 * stack for words that are valid return addresses (main-RAM code range, and the
 * instruction at ret-8 is a jal/jalr), innermost first. A consumer skips the
 * libgpu funnel band to name the game-level routine that issued the copy. Pure
 * reads against the raw RAM array — no lockstep/observer pollution. */
uint32_t g_gp0_last_copy_sp = 0;   /* diag: raw guest $sp at last op-0x80 copy */
static void gp0_capture_builder_chain(uint32_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = 0;
    uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return;
    const uint32_t RMASK = 0x001FFFFFu;             /* 2 MB, mirror-folded */
    uint32_t rawsp = debug_guest_sp();
    g_gp0_last_copy_sp = rawsp;
    if ((rawsp & 0x1FFFFFFFu) >= 0x00800000u) return; /* not in RAM/mirror */
    uint32_t sp = rawsp & RMASK & ~3u;              /* fold to 2MB, word-align */
    /* Two-pass: prefer words that are genuine return addresses (call at ret-8),
     * but if guest code validation yields none, fall back to any code-range
     * stack word so the chain is never silently empty. */
    int found = 0;
    for (int pass = 0; pass < 2 && found == 0; pass++) {
        for (uint32_t off = 0; off + 4 <= 0x400u && found < 6; off += 4) {
            uint32_t w; memcpy(&w, ram + ((sp + off) & RMASK), 4);
            if ((w & 3u) || w < 0x80010000u || w >= 0x80120000u) continue;
            if (pass == 0) {
                uint32_t ins; memcpy(&ins, ram + ((w - 8u) & RMASK), 4);
                uint32_t op = ins >> 26;
                int is_call = (op == 3u) || (op == 0u && (ins & 0x3Fu) == 9u);
                if (!is_call) continue;
            }
            out[found++] = w;
        }
    }
}

#define GP0_RING_CAP        (1u << 20)  /* 1 048 576 entries */
/* GpuGp0RingEntry + GPU_GP0_RING_MAX_WORDS are public types in gpu.h. */

static GpuGp0RingEntry *gp0_ring = NULL;  /* lazy-alloc on first record */
static uint64_t gp0_ring_seq    = 0;      /* total commands recorded */
static uint32_t gp0_ring_head   = 0;      /* next write slot */

static void gp0_ring_record(const uint32_t *words, int n) {
    if (!gp0_ring) {
        gp0_ring = (GpuGp0RingEntry *)calloc(GP0_RING_CAP, sizeof(*gp0_ring));
        if (!gp0_ring) return;  /* OOM — capture disabled, no other effect */
    }
    GpuGp0RingEntry *e = &gp0_ring[gp0_ring_head];
    e->frame   = (uint32_t)s_frame_count;
    e->seq     = (uint32_t)gp0_ring_seq;
    e->src_addr = gp0_cmd_source_addr;
    e->pc      = g_debug_last_store_pc;
    e->func    = g_debug_current_func_addr;
    e->ra      = debug_guest_ra();
    e->opcode  = (uint8_t)((words[0] >> 24) & 0xFF);
    e->n_words = (uint8_t)(n > 255 ? 255 : (n < 0 ? 1 : n));
    e->pad     = 0;
    int copy_n = e->n_words > GPU_GP0_RING_MAX_WORDS ? GPU_GP0_RING_MAX_WORDS : e->n_words;
    for (int i = 0; i < copy_n; i++) e->cmd[i] = words[i];
    for (int i = copy_n; i < GPU_GP0_RING_MAX_WORDS; i++) e->cmd[i] = 0;
    if (e->opcode == 0x80) { gp0_capture_builder_chain(e->bld); e->csp = g_gp0_last_copy_sp; }
    else { for (int i = 0; i < 6; i++) e->bld[i] = 0; e->csp = 0; }
    gp0_ring_head = (gp0_ring_head + 1) % GP0_RING_CAP;
    gp0_ring_seq++;
}

/* Public accessors for debug_server.c */
uint64_t gpu_gp0_ring_total(void)    { return gp0_ring_seq; }
uint32_t gpu_gp0_ring_capacity(void) { return GP0_RING_CAP; }
uint32_t gpu_gp0_ring_max_words(void){ return GPU_GP0_RING_MAX_WORDS; }

void gpu_set_gp0_source(uint32_t addr) {
    gp0_next_source_addr = addr;
}

/* Fill `out[0..max_out-1]` with entries from the requested frame; returns
 * count. Walks from oldest in-buffer to newest so iteration order matches
 * draw order within a frame. */
int gpu_gp0_ring_dump_frame(uint32_t frame, GpuGp0RingEntry *out, int max_out) {
    if (!gp0_ring || max_out <= 0) return 0;
    uint32_t avail = (gp0_ring_seq < GP0_RING_CAP)
                   ? (uint32_t)gp0_ring_seq : GP0_RING_CAP;
    uint32_t start = (gp0_ring_seq < GP0_RING_CAP) ? 0 : gp0_ring_head;
    int n_out = 0;
    for (uint32_t i = 0; i < avail && n_out < max_out; i++) {
        uint32_t idx = (start + i) % GP0_RING_CAP;
        if (gp0_ring[idx].frame == frame) {
            out[n_out++] = gp0_ring[idx];
        }
    }
    return n_out;
}

/* Report the frame range currently held in the ring (oldest..newest). */
void gpu_gp0_ring_frame_span(uint32_t *out_oldest, uint32_t *out_newest) {
    if (out_oldest) *out_oldest = 0;
    if (out_newest) *out_newest = 0;
    if (!gp0_ring || gp0_ring_seq == 0) return;
    uint32_t avail = (gp0_ring_seq < GP0_RING_CAP)
                   ? (uint32_t)gp0_ring_seq : GP0_RING_CAP;
    uint32_t start = (gp0_ring_seq < GP0_RING_CAP) ? 0 : gp0_ring_head;
    uint32_t newest_idx = (start + avail - 1) % GP0_RING_CAP;
    if (out_oldest) *out_oldest = gp0_ring[start].frame;
    if (out_newest) *out_newest = gp0_ring[newest_idx].frame;
}

/* ---- Draw census ring (ALWAYS-ON) -----------------------------------------
 * Every drawn primitive (GP0 opcode 0x20-0x7F) records: frame, the prim's DMA
 * source address, the live camera (scratchpad camX/camY), and the prim's first
 * vertex in DRAWING space (pre draw_offset). Purpose: see object spawn/despawn
 * and edge-cull in DATA, not screenshots. When a background object despawns,
 * its prim simply stops appearing in the census at a specific camX while its
 * last recorded screen-x shows how far on-screen it still was — i.e. the
 * effective (4:3-sized) despawn margin, which the 16:9 view exceeds. A prim
 * present in the census but absent on screen would instead indict the renderer
 * gate. Query via TCP `ws_census` → CSV file (large dumps mustn't ride TCP). */
#define WS_CENSUS_CAP (1u << 21)            /* 2,097,152 entries * 24B = 48 MiB */
typedef struct {
    uint32_t frame;
    uint32_t src_addr;
    int16_t  cam_x, cam_y;
    int16_t  x, y;        /* first vertex (screen / pre-draw_offset) */
    int16_t  xmin, xmax;  /* screen-X extent across the prim's position verts */
    int16_t  base_x;      /* back-buffer origin (draw_area_left) at draw time */
    uint8_t  opcode;
    uint8_t  tagged;      /* psx_ws_prim_is_tagged() at draw time */
} WsCensusEntry;
static WsCensusEntry *ws_census = NULL;
static uint64_t       ws_census_seq = 0;
static int            ws_census_on  = 1;     /* always-on; toggle via ws_census */
extern uint16_t       psx_read_half(uint32_t addr);

/* Screen-X extent (raw vertex coords = SX, pre-draw_offset) across a prim's
 * position vertices. Polygons (0x20-0x3F) decoded exactly via the packet stride
 * (pos word = 1 + i*(1 + gouraud + textured)); rects/sprites/lines fall back to
 * the first vertex. Lets the census show how far right each path reaches and
 * whether anything is drawn past the 4:3 edge (native-wide margin diagnosis). */
static void prim_sx_extent(uint8_t op, int32_t *xmin, int32_t *xmax) {
    int32_t lo = 0x7FFF, hi = -0x8000;
    if (op >= 0x20 && op <= 0x3F) {
        int nv     = (op & 0x08) ? 4 : 3;
        int stride = 1 + ((op & 0x10) ? 1 : 0) + ((op & 0x04) ? 1 : 0);
        for (int i = 0; i < nv; i++) {
            int32_t vx, vy;
            parse_vertex(gp0_cmd_buf[1 + i * stride], &vx, &vy);
            if (vx < lo) lo = vx;
            if (vx > hi) hi = vx;
        }
    } else {
        int32_t vx, vy;
        parse_vertex(gp0_cmd_buf[1], &vx, &vy);
        lo = hi = vx;
    }
    *xmin = lo; *xmax = hi;
}

/* Natural-overhang noter (see the ws_ovh_* block up top for the rationale).
 * Called for every draw prim; counts POLYGONS whose raw SX extent crosses
 * outside [0, disp_w] by more than the jitter margin, and stamps the frame
 * (with the 2-consecutive-frames sustained rule) when enough do. */
static void ws_note_overhang(uint8_t op) {
    if (op < 0x20 || op > 0x3F) return;      /* polygons only — the world's prims */
    /* Sprite-funnel (tagged) prims slide in from off-screen routinely —
     * title-card letter tiles are tagged poly quads entering from the edges
     * and stamped a sustained overhang inside the hut (observed at frame
     * 20245). The WORLD funnel (Cluster-A RTPT terrain) never tags, so
     * untagged overhang is the uncorrupted "revealable content" signal.
     * Untagged-only also leaves non-tag titles unchanged (is_tagged == 0). */
    if (psx_ws_prim_is_tagged()) return;
    int32_t xmn, xmx;
    prim_sx_extent(op, &xmn, &xmx);
    int32_t W = ws_disp_w();
    if (xmn >= -WS_OVERHANG_DEEP_PX && xmx <= W + WS_OVERHANG_DEEP_PX) return;
    uint32_t f = (uint32_t)s_frame_count;
    if (f != ws_ovh_frame) {
        ws_ovh_prev  = (f == ws_ovh_frame + 1u) ? ws_ovh_count : 0;
        ws_ovh_frame = f; ws_ovh_count = 0;
    }
    ws_ovh_count++;
    if (ws_ovh_count >= WS_OVERHANG_MIN_PRIMS && ws_last_ovh_stamp != f) {
        if (f == ws_last_ovh_stamp + 1u) ws_sust_ovh_stamp = f;
        ws_last_ovh_stamp = f;
    }
}

static void ws_census_record(uint8_t opcode, int32_t x, int32_t y) {
    if (!ws_census_on) return;
    if (!ws_census) {
        ws_census = (WsCensusEntry *)calloc(WS_CENSUS_CAP, sizeof(WsCensusEntry));
        if (!ws_census) { ws_census_on = 0; return; }
    }
    int32_t xmn, xmx;
    prim_sx_extent(opcode, &xmn, &xmx);
    WsCensusEntry *e = &ws_census[ws_census_seq & (WS_CENSUS_CAP - 1)];
    e->frame    = (uint32_t)s_frame_count;
    e->src_addr = gp0_cmd_source_addr;
    e->cam_x    = (int16_t)psx_read_half(0x1F800176);
    e->cam_y    = (int16_t)psx_read_half(0x1F800186);
    e->x        = (int16_t)x;
    e->y        = (int16_t)y;
    e->xmin     = (int16_t)xmn;
    e->xmax     = (int16_t)xmx;
    e->base_x   = (int16_t)draw_area_left;
    e->opcode   = opcode;
    e->tagged   = (uint8_t)psx_ws_prim_is_tagged();
    ws_census_seq++;
}

/* Dump census rows for frames [f0,f1] to a CSV file. Returns row count. */
int gpu_ws_census_dump(uint32_t f0, uint32_t f1, const char *path) {
    if (!ws_census) return 0;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "frame,src_addr,cam_x,cam_y,x,y,xmin,xmax,base_x,opcode,tagged\n");
    uint64_t total = ws_census_seq;
    uint64_t avail = total < WS_CENSUS_CAP ? total : WS_CENSUS_CAP;
    uint64_t start = total - avail;
    int n = 0;
    for (uint64_t s = start; s < total; s++) {
        WsCensusEntry *e = &ws_census[s & (WS_CENSUS_CAP - 1)];
        if (e->frame < f0 || e->frame > f1) continue;
        fprintf(fp, "%u,0x%08X,%d,%d,%d,%d,%d,%d,%d,0x%02X,%u\n",
                e->frame, e->src_addr, e->cam_x, e->cam_y, e->x, e->y,
                e->xmin, e->xmax, e->base_x, e->opcode, e->tagged);
        n++;
    }
    fclose(fp);
    return n;
}

void gpu_ws_census_set(int on) { ws_census_on = on ? 1 : 0; }
uint64_t gpu_ws_census_seq(void) { return ws_census_seq; }

/* Execute a fully-collected GP0 command */
static void gp0_execute_command(void) {
    uint8_t opcode = (gp0_cmd_buf[0] >> 24) & 0xFF;
    gp0_opcode_count[opcode]++;
    gp0_ring_record(gp0_cmd_buf, gp0_words_needed);
    extern void ws_bg_phase_note(uint32_t op);
    ws_bg_phase_note(opcode);   /* native-wide 2D-backdrop stretch: background-phase latch */

    /* Draw-census: capture every drawing primitive's first vertex + camera. */
    if (opcode >= 0x20 && opcode <= 0x7F) {
        int32_t cvx, cvy;
        parse_vertex(gp0_cmd_buf[1], &cvx, &cvy);
        ws_census_record(opcode, cvx, cvy);
        ws_note_overhang(opcode);   /* 2D-only-scene classifier world signal */
    }

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
            int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
            int32_t x0, y0;
            parse_vertex(gp0_cmd_buf[1], &x0, &y0);
            (void)ws_sprt_fixed_transform(&x0, 1);  /* position only; 1px stays 1px */
            x0 += draw_offset_x; y0 += draw_offset_y;
            int u0 = gp0_cmd_buf[2] & 0xFF;
            int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
            uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
            uint16_t clut_x = (clut & 0x3F) * 16;
            uint16_t clut_y = (clut >> 6) & 0x1FF;
            setup_textured_draw(color24, semi_trans, raw_texture);
            gr_draw_textured_rect(x0, y0, 1, 1, u0, v0, clut_x, clut_y, current_texpage());
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
            gr_set_semi_transparency(semi_trans, (int)semi_transparency);
            gr_draw_flat_rect(x0, y0, 16, 16, color);
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
            gr_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
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
             * implemented. Fatal halt so we know exactly what's needed next,
             * with all rings queryable post-mortem. */
            {
                static char reason[96];
                snprintf(reason, sizeof(reason),
                         "GPU GP0 unimplemented command 0x%02X (word 0x%08X)",
                         opcode, gp0_cmd_buf[0]);
                psx_fatal_halt(reason);
            }
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
    /* Through the facade so the GL backend syncs its FBO down first. */
    return gr_vram_read(x, y);
}

static void gpu_write_gp0_body(uint32_t val) {
    gp0_write_count++;

    /* State: consuming pixel data for CPU→VRAM transfer */
    if (gp0_state == GP0_VRAM_WRITE) {
        /* Capture first few data words for debug */
        if (a0_capture_slot >= 0 && a0_capture_slot < A0_HISTORY_CAP) {
            int wc = a0_history[a0_capture_slot].word_count++;
            if (wc < 4)
                a0_history[a0_capture_slot].first_words[wc] = val;
        }
        /* Each word contains two 16-bit pixels (low halfword first) */
        for (int i = 0; i < 2; i++) {
            uint16_t pixel = (uint16_t)(val >> (i * 16));
            uint16_t wx = (vram_write_x + vram_write_col) % 1024;
            uint16_t wy = (vram_write_y + vram_write_row) % 512;

            /* Respect mask bit settings. The check reads through the facade:
             * under the GL backend GPU-drawn mask bits live in the FBO, not
             * the CPU array (one sync per burst; free when check is off). */
            if (check_mask_bit && (gr_vram_read((int)wx, (int)wy) & 0x8000))
                goto next_pixel;

            if (set_mask_bit)
                pixel |= 0x8000;

            /* Route through the renderer so the hi-res supersampling mirror
             * is kept coherent (writes native VRAM identically when off). */
            gr_vram_write((int)wx, (int)wy, pixel);

        next_pixel:
            if (++vram_write_col == vram_write_w) {
                vram_write_col = 0;
                if (++vram_write_row == vram_write_h) {
                    /* Transfer complete */
                    gp0_state = GP0_IDLE;
                    vram_write_remaining = 0;
                    text_xlate_vram_upload(vram_write_x, vram_write_y,
                                           vram_write_w, vram_write_h);
                    return;
                }
            }
        }

        if (--vram_write_remaining == 0) {
            gp0_state = GP0_IDLE;
            text_xlate_vram_upload(vram_write_x, vram_write_y,
                                   vram_write_w, vram_write_h);
        }

        return;
    }

    /* State: mono polyline — each word is a vertex (or terminator) */
    if (gp0_state == GP0_POLYLINE_MONO) {
        if ((val & 0xF000F000u) == 0x50005000u) {
            /* Terminator: hardware ends a polyline ONLY when the masked word
             * matches 0x50005000 (the 0x55555555 terminator) — Beetle
             * gpu.cpp:1030, psx-spx. The old `(val & 0xF000F000) != 0` test
             * also fired on any NEGATIVE vertex coordinate (Y=0xFFxx) and, at
             * shaded color positions, on any color component >= 0x10 in the
             * G byte — ending the polyline early and re-parsing its remaining
             * words as new GP0 commands. That de-phased the whole command
             * stream: garbage prims all over the Tomba2 attract (texture
             * garble) and eventually a legit texcoord word 0xFE65FE58 parsed
             * in IDLE state -> "GP0 unknown command 0xFE" fatal (village). */
            gp0_state = GP0_IDLE;
            return;
        }
        int32_t x, y;
        parse_vertex(val, &x, &y);
        x += draw_offset_x; y += draw_offset_y;
        if (polyline_has_prev) {
            gr_draw_line(polyline_prev_x, polyline_prev_y, x, y, polyline_color);
        }
        polyline_prev_x = x; polyline_prev_y = y;
        polyline_has_prev = 1;
        return;
    }

    /* State: shaded polyline — alternating color, vertex words */
    if (gp0_state == GP0_POLYLINE_SHADED) {
        /* Even words (after cmd) are colors, odd words are vertices.
         * Sequence: [cmd+C0] [V0] [C1] [V1] [C2] [V2] ...
         * polyline_has_prev tracks: 0=need V0, 1=need C_next, 2=need V_next */
        if (!polyline_has_prev) {
            /* First vertex */
            int32_t x, y;
            parse_vertex(val, &x, &y);
            x += draw_offset_x; y += draw_offset_y;
            polyline_prev_x = x; polyline_prev_y = y;
            polyline_prev_c = polyline_color;
            polyline_has_prev = 1;
            return;
        }
        if (polyline_has_prev == 1) {
            /* Expecting color word (or terminator). Same hardware rule as the
             * mono case: ONLY the masked 0x50005000 pattern terminates. */
            if ((val & 0xF000F000u) == 0x50005000u) {
                gp0_state = GP0_IDLE;
                return;
            }
            polyline_color = rgb888_to_rgb555(val & 0xFFFFFFu);
            polyline_has_prev = 2;
            return;
        }
        /* polyline_has_prev == 2: vertex word */
        {
            int32_t x, y;
            parse_vertex(val, &x, &y);
            x += draw_offset_x; y += draw_offset_y;
            gr_draw_shaded_line(polyline_prev_x, polyline_prev_y, polyline_prev_c,
                                x, y, polyline_color);
            polyline_prev_x = x; polyline_prev_y = y;
            polyline_prev_c = polyline_color;
            polyline_has_prev = 1;
        }
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
        /* Unknown command — fatal halt with rings queryable post-mortem */
        static char reason[96];
        snprintf(reason, sizeof(reason),
                 "GPU GP0 unknown command 0x%02X (word 0x%08X)", opcode, val);
        psx_fatal_halt(reason);
    }

    if (word_count < 0) {
        /* Variable-length polyline command.
         * Mono  (0x48-0x4F): [cmd+color] [v0] [v1] ... [terminator]
         * Shaded(0x58-0x5F): [cmd+C0] [v0] [C1] [v1] ... [terminator]
         * Terminator: (word & 0xF000F000) == 0x50005000 (0x55555555). */
        int shaded = (opcode & 0x10) != 0;  /* 0x58+ = shaded, 0x48+ = mono */
        polyline_semi_trans = (val >> 25) & 1;
        polyline_color = rgb888_to_rgb555(val & 0xFFFFFFu);
        polyline_prev_c = polyline_color;
        polyline_has_prev = 0;
        gr_set_semi_transparency(polyline_semi_trans, (int)semi_transparency);
        gp0_state = shaded ? GP0_POLYLINE_SHADED : GP0_POLYLINE_MONO;
        gp0_draw_count++;
        /* Record polyline header (variable-length body not captured;
         * just enough so per-frame stream shows the polyline existed). */
        gp0_opcode_count[opcode]++;
        uint32_t hdr_only[1] = { val };
        gp0_ring_record(hdr_only, 1);
        return;
    }

    gp0_cmd_buf[0] = val;
    gp0_cmd_source_addr = gp0_next_source_addr;

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

/* Wall-time sampler bracket (phase_profile): tag GP0 command processing —
 * rasterization / batching / VRAM transfer work on the emu thread — as its
 * own phase so it is separable from the guest code that issued the write.
 * Covers both the MMIO store chokepoint and DMA channel-2 feeds. */
void gpu_write_gp0(uint32_t val) {
    extern int g_exec_phase;
    int prev_phase = g_exec_phase;
    g_exec_phase = 4;
    gpu_write_gp0_body(val);
    g_exec_phase = prev_phase;
}

/* ---- GP1 write (0x1F801814 write) ---- */

static void gp1_reset(void) {
    /* GP1(00h): Reset GPU — clears FIFO/control state and disables display.
     * VRAM contents survive a GPU reset on real hardware; only power-on init
     * clears our backing store. */
    gpu_reset_state(0);
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
    uint32_t next_disabled = val & 1;
    /* Native-wide reveal pixels have no guest-VRAM backing, so they cannot be
     * reconstructed from the canonical framebuffer after a scene change. Treat
     * the guest's ON->OFF transition as invalidation of those synthetic strips.
     * This is deliberately transition-scoped: MMX6 retains valid background
     * pixels across ordinary frames, so per-frame clearing pillarboxes gameplay. */
    if (ws_clear_reveal && next_disabled && !display_disabled && ws_mode == 2) {
        ws_clear_all_reveal_margins();
    }
    display_disabled = next_disabled;
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
    ws_note_display_base(display_area_x);  /* learn the display buffer set (native-wide) */
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
     * Mednafen-psx masks the subcommand to 4 bits (val & 0x0F) and
     * services cases 2..5, 7, 8. Tomba's ResetGraph() uses param 7 to
     * read the GPU version (must be 2) to pick its video-mode path —
     * the wrong value here lands the game on a no-draw branch. */
    uint32_t which = val & 0x0F;
    switch (which) {
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
        case 7: /* GPU version (real-hw + mednafen return 2) */
            gpuread_latch = 2;
            break;
        case 8: /* unknown info index, real hw / mednafen return 0 */
            gpuread_latch = 0;
            break;
        default:
            /* N=0,1,6,9..15: leave latch unchanged */
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
        default: {
            static char reason[96];
            snprintf(reason, sizeof(reason),
                     "GPU GP1 unknown command 0x%02X (word 0x%08X)", cmd, val);
            psx_fatal_halt(reason);
        }
    }
}

/* ---- boot snapshot: complete GPU register state (see boot_state.h) ---- */
#define GPU_SNAP_FIELDS(X) \
    X(texpage_x) X(texpage_y) X(semi_transparency) X(texpage_colors) \
    X(dither_enabled) X(draw_to_display) X(texture_disable) X(texture_window_value) \
    X(set_mask_bit) X(check_mask_bit) \
    X(interlace_field) X(reverse_flag) \
    X(draw_area_left) X(draw_area_top) X(draw_area_right) X(draw_area_bottom) \
    X(draw_offset_x) X(draw_offset_y) \
    X(hres1) X(hres2) X(vres) X(video_mode) X(display_depth) X(vertical_interlace) \
    X(display_disabled) X(irq1_flag) X(dma_direction) X(lcf) \
    X(display_area_x) X(display_area_y) \
    X(h_display_x1) X(h_display_x2) X(v_display_y1) X(v_display_y2) \
    X(gpuread_latch) \
    X(gp0_state) \
    X(gp0_cmd_buf) X(gp0_words_collected) X(gp0_words_needed) \
    X(gp0_next_source_addr) X(gp0_cmd_source_addr) \
    X(polyline_color) X(polyline_prev_x) X(polyline_prev_y) X(polyline_prev_c) \
    X(polyline_semi_trans) X(polyline_has_prev) \
    X(vram_write_x) X(vram_write_y) X(vram_write_w) X(vram_write_h) \
    X(vram_write_col) X(vram_write_row) X(vram_write_remaining) \
    X(vram_read_active) X(vram_read_x) X(vram_read_y) X(vram_read_w) X(vram_read_h) \
    X(vram_read_col) X(vram_read_row)
#define GPU_COSIM_SNAP_FIELDS(X) \
    X(texpage_x) X(texpage_y) X(semi_transparency) X(texpage_colors) \
    X(dither_enabled) X(draw_to_display) X(texture_disable) X(texture_window_value) \
    X(set_mask_bit) X(check_mask_bit) \
    X(interlace_field) X(reverse_flag) \
    X(draw_area_left) X(draw_area_top) X(draw_area_right) X(draw_area_bottom) \
    X(draw_offset_x) X(draw_offset_y) \
    X(hres1) X(hres2) X(vres) X(video_mode) X(display_depth) X(vertical_interlace) \
    X(display_disabled) X(irq1_flag) X(dma_direction) X(lcf) \
    X(display_area_x) X(display_area_y) \
    X(h_display_x1) X(h_display_x2) X(v_display_y1) X(v_display_y2) \
    X(gpuread_latch) \
    X(gp0_state) \
    X(gp0_cmd_buf) X(gp0_words_collected) X(gp0_words_needed) \
    X(polyline_color) X(polyline_prev_x) X(polyline_prev_y) X(polyline_prev_c) \
    X(polyline_semi_trans) X(polyline_has_prev) \
    X(vram_write_x) X(vram_write_y) X(vram_write_w) X(vram_write_h) \
    X(vram_write_col) X(vram_write_row) X(vram_write_remaining) \
    X(vram_read_active) X(vram_read_x) X(vram_read_y) X(vram_read_w) X(vram_read_h) \
    X(vram_read_col) X(vram_read_row)
uint32_t gpu_snapshot_bytes(void){ uint32_t n=0;
#define X(f) n += (uint32_t)sizeof(f);
    GPU_SNAP_FIELDS(X)
#undef X
    return n; }
void gpu_snapshot_write(uint8_t *p){
#define X(f) memcpy(p,&(f),sizeof(f)); p+=sizeof(f);
    GPU_SNAP_FIELDS(X)
#undef X
}
uint32_t gpu_cosim_snapshot_bytes(void){ uint32_t n=0;
#define X(f) n += (uint32_t)sizeof(f);
    GPU_COSIM_SNAP_FIELDS(X)
#undef X
    return n; }
void gpu_cosim_snapshot_write(uint8_t *p){
#define X(f) memcpy(p,&(f),sizeof(f)); p+=sizeof(f);
    GPU_COSIM_SNAP_FIELDS(X)
#undef X
}
void gpu_cosim_dump(char *out, int cap) {
    if (!out || cap <= 0) return;
    char *p = out;
    size_t rem = (size_t)cap;
#define APPEND(fmt, ...) do { \
        if (rem <= 1) break; \
        int w = snprintf(p, rem, fmt, __VA_ARGS__); \
        if (w < 0) { out[0] = 0; return; } \
        if ((size_t)w >= rem) { p += rem - 1; rem = 1; } \
        else { p += (size_t)w; rem -= (size_t)w; } \
    } while (0)
#define X(f) APPEND(" %s %llx", #f, (unsigned long long)(uint32_t)(f));
    X(texpage_x) X(texpage_y) X(semi_transparency) X(texpage_colors)
    X(dither_enabled) X(draw_to_display) X(texture_disable) X(texture_window_value)
    X(set_mask_bit) X(check_mask_bit)
    X(interlace_field) X(reverse_flag)
    X(draw_area_left) X(draw_area_top) X(draw_area_right) X(draw_area_bottom)
    X(draw_offset_x) X(draw_offset_y)
    X(hres1) X(hres2) X(vres) X(video_mode) X(display_depth) X(vertical_interlace)
    X(display_disabled) X(irq1_flag) X(dma_direction) X(lcf)
    X(display_area_x) X(display_area_y)
    X(h_display_x1) X(h_display_x2) X(v_display_y1) X(v_display_y2)
    X(gpuread_latch)
    X(gp0_state)
    for (int i = 0; i < 16; i++) {
        APPEND(" gp0_cmd_buf%d %08x", i, gp0_cmd_buf[i]);
    }
    X(gp0_words_collected) X(gp0_words_needed)
    X(gp0_next_source_addr) X(gp0_cmd_source_addr)
    X(polyline_color) X(polyline_prev_x) X(polyline_prev_y) X(polyline_prev_c)
    X(polyline_semi_trans) X(polyline_has_prev)
    X(vram_write_x) X(vram_write_y) X(vram_write_w) X(vram_write_h)
    X(vram_write_col) X(vram_write_row) X(vram_write_remaining)
    X(vram_read_active) X(vram_read_x) X(vram_read_y) X(vram_read_w) X(vram_read_h)
    X(vram_read_col) X(vram_read_row)
#undef X
    APPEND("%s", "\n");
#undef APPEND
}
int gpu_snapshot_read(const uint8_t *p, uint32_t len){ if(len!=gpu_snapshot_bytes()) return 0;
#define X(f) memcpy(&(f),p,sizeof(f)); p+=sizeof(f);
    GPU_SNAP_FIELDS(X)
#undef X
    return 1; }
uint16_t* gpu_get_vram_ptr(void){ return vram; }
uint32_t  gpu_get_vram_bytes(void){ return (uint32_t)sizeof(vram); }
