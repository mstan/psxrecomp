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
#include "pgxp.h"
#include "pgxp_vertex_cache.h"
#include "mod_memory.h"
#include "gpu_primitive_reject.h"
#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include "gpu_render.h"
#include "text_xlate.h"
#include "crash_trace.h"
#include "debug_server.h"
#include "cpu_state.h"
#include "event_ring.h"
#include "color_lut.h"
#include "mod_runtime.h"
#include "sio.h"
#include "ws_cull_detect.h"
#include "ws_aspect_cone_math.h"
#include "ws_ui_group.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint16_t psx_read_half(uint32_t addr);
extern uint8_t  psx_read_byte(uint32_t addr);
extern uint32_t psx_read_word(uint32_t addr);

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
static uint16_t gp0_ot_rank = 0xFFFFu;

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
static int      ws_auto_ui_squash;
static int      ws_auto_ui_dense;
static int      ws_active(void);
static uint64_t ws_auto_ui_candidate_count;
static uint64_t ws_auto_ui_transform_count;
#define WS_UI_PREPASS_MAX 2048u
typedef struct {
    WsUiGroupItem group;
    uint32_t src_addr;
    uint16_t ot_rank;
    /* Diagnostic only (ws_ui_groups): the key is a hash, so a dump of keys
     * alone says two prims differ without saying WHICH component differed.
     * Keep the raw inputs so an observer can attribute a split to the CLUT,
     * the texpage, the 24px Y band, or the poly-vs-rect family. */
    int32_t  y;
    int32_t  h;
    uint8_t  op;
} WsUiPrepassItem;
static WsUiPrepassItem ws_ui_prepass[WS_UI_PREPASS_MAX];
static uint32_t ws_ui_prepass_count;
static uint16_t ws_ui_prepass_rank = 0xFFFFu;

/* Why a UI-looking primitive did NOT reach the squash partition.
 *
 * Every rejection here leaves a primitive at its raw 4:3 X while the cluster
 * around it is squashed toward an anchor -- which is precisely how a HUD mark
 * ends up stranded in open screen at a wide aspect. The gates are silent
 * `return`s, so a stranded primitive is indistinguishable from one that was
 * never drawn at all. These counters let ws_ui_groups name the responsible gate
 * from a capture instead of it being guessed at. Diagnostic only; nothing in
 * the transform path reads them. */
static struct {
    uint32_t opcode;      /* not in the textured quad / rect families      */
    uint32_t not_axis;    /* textured quad, but not axis-aligned           */
    uint32_t degenerate;  /* zero or negative extent                       */
    uint32_t too_big;     /* full-screen or large-primitive reject         */
    uint32_t cap;         /* WS_UI_PREPASS_MAX reached                     */
    uint32_t rank;        /* admitted, then dropped by the max_rank filter */
} ws_ui_reject;

/* Geometry of the primitives the max_rank filter discarded. A count alone
 * cannot say whether those are stray world geometry (fine to drop) or HUD
 * marks belonging to a cluster that IS being squashed (not fine -- they are
 * left behind at their 4:3 X). Small fixed ring; the filter typically drops a
 * handful. */
#define WS_UI_RANKDROP_MAX 8
static struct { int32_t x, y, w, h; uint16_t rank; uint8_t op; }
    ws_ui_rankdrop[WS_UI_RANKDROP_MAX];
static uint32_t ws_ui_rankdrop_count;

/* Wide-aspect mode: 0 = off (4:3 identity), 1 = squash (legacy hack — compress
 * a wider FOV into the 320 frame, present stretched), 2 = native-wide (render
 * the wider FOV into an actually wider frame and present 1:1; the GTE is NOT
 * squashed — the frame is widened via draw_offset/draw_area and the display
 * read). ws_cfg_num/den hold the configured aspect that drives the wide extent
 * in native-wide mode. */
static int      ws_mode    = 0;
static int      ws_cfg_num = 4, ws_cfg_den = 3;
static void ws_nw_sync_target(void);

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
static int32_t ws_disp_x(void);
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
void gpu_ws_set_auto_ui_squash(int on) {
    ws_auto_ui_squash = on ? 1 : 0;
    ws_ui_prepass_count = 0;
    ws_ui_prepass_rank = 0xFFFFu;
    ws_auto_ui_dense = 0;
    ws_auto_ui_candidate_count = 0;
    ws_auto_ui_transform_count = 0;
}
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
static int ws_precise_nclip_cfg = 0;
#define WS_GAMEPLAY_STATE_VALUES_MAX 16
static uint32_t ws_gameplay_state_addr = 0;
static uint32_t ws_gameplay_state_values[WS_GAMEPLAY_STATE_VALUES_MAX];
static int ws_gameplay_state_value_count = 0;
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
void gpu_pgxp_rederive_enable(void);
void gpu_ws_set_precise_nclip(int on) {
    ws_precise_nclip_cfg = on ? 1 : 0;
    /* Precise NCLIP and clamp rescue consume PGXP dataflow shadows even when
     * both correction features are off — re-derive the engine arm. */
    gpu_pgxp_rederive_enable();
}
int gpu_ws_precise_nclip_enabled(void) { return ws_precise_nclip_cfg && ws_active(); }
void gpu_ws_set_gameplay_state_gate(uint32_t addr,
                                    const uint32_t *values, int nvalues) {
    if (nvalues < 0) nvalues = 0;
    if (nvalues > WS_GAMEPLAY_STATE_VALUES_MAX)
        nvalues = WS_GAMEPLAY_STATE_VALUES_MAX;
    ws_gameplay_state_addr = addr;
    ws_gameplay_state_value_count = nvalues;
    for (int i = 0; i < nvalues; i++)
        ws_gameplay_state_values[i] = values[i];
}

static int ws_gameplay_state_matches(void) {
    if (!ws_gameplay_state_addr || ws_gameplay_state_value_count == 0)
        return -1;
    uint32_t state = psx_read_word(ws_gameplay_state_addr);
    for (int i = 0; i < ws_gameplay_state_value_count; i++)
        if (state == ws_gameplay_state_values[i]) return 1;
    return 0;
}

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
    int state_match = ws_gameplay_state_matches();
    if (state_match >= 0) return state_match;
    if (ws_full_2d_mode()) return 1;
    if (ws_gte_game_mode_cfg &&
        (uint32_t)s_frame_count - ws_last_gte_stamp <= WS_GTE_GAME_MODE_HYSTERESIS) return 1;
    return (uint32_t)s_frame_count - ws_last_tag_stamp <= 2;
}

/* True when the current frame needs native 4:3 content correction, so ALL
 * squash must be suppressed and the content rendered pixel-native:
 *   - FMV video (24-bit, or streamed 15-bit colour MDEC), and
 *   - full-2D screens (menus/title/save — no character billboards this frame).
 * Per-prim center-squash mangles composite 2D UI (dialog boxes built from
 * tiled cap/middle pieces), so such screens get zero squash and are placed in
 * a centered 4:3 safe area. The presentation layer independently decides
 * whether that safe area lives inside a fixed configured outer canvas. The
 * FMV check is cached per frame; game_mode is a cheap live check. */
/* 2D-only gameplay scenes (sprite-tag titles). A room interior or a sky-only
 * fall is gameplay-classified (the character prims tag every frame) but has
 * no world-scale GTE projection — there is no 3D world and nothing beyond
 * the canonical frame to reveal, so wide presents can only stretch flat art
 * (the backdrop stretch fires wholesale because the background phase never
 * ends). Keep those scenes as native 4:3 content: the canonical buffer is
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

static uint32_t s_ws_fmv_frame_cache = 0xFFFFFFFFu;
static int      s_ws_fmv_cached = 0;

int gpu_ws_content_native_43(void) {
    if (!ws_engaged()) return 0;
    if (!ws_game_mode()) return 1;                 /* full-2D screen */
    if (ws_2d_only_scene()) return 1;              /* 2D-only gameplay scene */
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s_ws_fmv_frame_cache) {
        s_ws_fmv_frame_cache = f;
        GpuDisplayInfo di; gpu_get_display_info(&di);
        s_ws_fmv_cached = di.depth24 || mdec_recently_active(WS_FMV_HYSTERESIS);
    }
    return s_ws_fmv_cached;
}

/* Scene classification controls only content correction. The presentation
 * layer independently owns the configured outer canvas. */
static int ws_active(void) { return ws_configured() && !gpu_ws_content_native_43(); }

/* ----- Native-wide (mode 2) ------------------------------------------------
 * Render the wider FOV into an actually wider frame instead of squashing it.
 * Engaged on game frames only (FMV/menu frames retain 4:3-safe content). The
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
    return ws_mode == 2 && !gpu_ws_content_native_43();
}
/* Cull/spawn setup often runs while a scene is loading, before the first GTE
 * frame can classify it as gameplay.  Keep that pre-render setup aware of the
 * configured native-wide viewport; presentation itself remains gated by
 * ws_native_wide_active(). */
static int ws_native_wide_configured(void) {
    return ws_mode == 2 && ws_cfg_num * 3 > ws_cfg_den * 4;
}

static int ws_local_viewport_cfg = 0;
static int ws_local_viewport_slot = 0;
static int ws_local_viewport_draw_target(int *base_x);
static int ws_vertical_split_active(void);
void gpu_ws_set_netplay_local_viewport(int enabled, int slot) {
    ws_local_viewport_cfg = enabled ? 1 : 0;
    ws_local_viewport_slot = slot == 1 ? 1 : 0;
    ws_nw_sync_target();
}

static int ws_local_viewport_layout(int *base_x, int *source_w,
                                    int *wide_w, int *offset) {
    if (!ws_local_viewport_cfg || !ws_native_wide_configured() ||
        !ws_vertical_split_active())
        return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.depth24 || di.width < 2 || di.height == 0)
        return 0;
    if ((di.width & 1u) != 0)
        return 0;

    int src_w = (int)di.width / 2;
    int target_w = ((int)di.height * ws_cfg_num + ws_cfg_den / 2) / ws_cfg_den;
    if (target_w < src_w)
        target_w = src_w;
    int off = (target_w - src_w) / 2;
    int final_w = src_w + off * 2;

    if (base_x) *base_x = (int)di.display_x + (ws_local_viewport_slot ? src_w : 0);
    if (source_w) *source_w = src_w;
    if (wide_w) *wide_w = final_w;
    if (offset) *offset = off;
    return final_w > src_w && off > 0;
}

static int ws_nw_configured_offset(void) {
    if (!ws_native_wide_configured()) return 0;
    int local_offset = 0;
    if (ws_local_viewport_layout(NULL, NULL, NULL, &local_offset))
        return local_offset;
    int numr = 3 * ws_cfg_num - 4 * ws_cfg_den;
    int w = (int)ws_disp_w();
    return (w * numr + 4 * ws_cfg_den) / (8 * ws_cfg_den);
}
static int ws_nw_offset(void) {
    if (!ws_native_wide_active()) return 0;
    return ws_nw_configured_offset();
}
int ws_nw_extra(void) { return 2 * ws_nw_offset(); }

int ws_nw_present_width(void) {
    int wide_w = 0;
    if (ws_local_viewport_layout(NULL, NULL, &wide_w, NULL))
        return wide_w;
    return (int)ws_disp_w() + ws_nw_extra();
}

int gpu_ws_netplay_local_viewport_base_x(void) {
    int base = 0;
    return ws_local_viewport_layout(&base, NULL, NULL, NULL) ? base : 0;
}

int gpu_ws_netplay_local_viewport_width(void) {
    int wide_w = 0;
    return ws_local_viewport_layout(NULL, NULL, &wide_w, NULL) ? wide_w : 0;
}

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
static int ws_activation_guard_pixels = 0;
void gpu_ws_set_margin_override(int v) { ws_margin_override = v; }
void gpu_ws_set_cull_guard_pixels(int pixels) {
    if (pixels < 0) pixels = 0;
    if (pixels > 256) pixels = 256;
    ws_cull_guard_pixels = pixels;
}
void gpu_ws_set_activation_guard_pixels(int pixels) {
    if (pixels < 0) pixels = 0;
    if (pixels > 256) pixels = 256;
    ws_activation_guard_pixels = pixels;
}

#define WS_EXPLICIT_CULL_SITES_MAX 64
static uint32_t ws_explicit_bias_sites[WS_EXPLICIT_CULL_SITES_MAX];
static uint32_t ws_explicit_slti_sites[WS_EXPLICIT_CULL_SITES_MAX];
static uint32_t ws_explicit_range_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_bias_n = 0;
static int ws_explicit_slti_n = 0;
static int ws_explicit_range_n = 0;
void gpu_ws_set_explicit_cull_sites(const uint32_t *bias, int nbias,
                                    const uint32_t *slti, int nslti,
                                    const uint32_t *range, int nrange) {
    if (nbias < 0) nbias = 0;
    if (nslti < 0) nslti = 0;
    if (nrange < 0) nrange = 0;
    if (nbias > WS_EXPLICIT_CULL_SITES_MAX) nbias = WS_EXPLICIT_CULL_SITES_MAX;
    if (nslti > WS_EXPLICIT_CULL_SITES_MAX) nslti = WS_EXPLICIT_CULL_SITES_MAX;
    if (nrange > WS_EXPLICIT_CULL_SITES_MAX) nrange = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_bias_n = nbias;
    ws_explicit_slti_n = nslti;
    ws_explicit_range_n = nrange;
    for (int i = 0; i < nbias; i++) ws_explicit_bias_sites[i] = bias[i] & 0x1FFFFFFFu;
    for (int i = 0; i < nslti; i++) ws_explicit_slti_sites[i] = slti[i] & 0x1FFFFFFFu;
    for (int i = 0; i < nrange; i++) ws_explicit_range_sites[i] = range[i] & 0x1FFFFFFFu;
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
static uint32_t ws_explicit_slti_lower_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_slti_lower_n = 0;
void gpu_ws_set_slti_lower_cull_sites(const uint32_t *sites, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX)
        nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_slti_lower_n = nsites;
    for (int i = 0; i < nsites; i++)
        ws_explicit_slti_lower_sites[i] = sites[i] & 0x1FFFFFFFu;
}
int psx_ws_is_cull_slti_lower_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_slti_lower_sites,
                            ws_explicit_slti_lower_n, pc);
}
static uint32_t ws_explicit_negsub_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_negsub_n = 0;
void gpu_ws_set_negsub_cull_sites(const uint32_t *sites, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_negsub_n = nsites;
    for (int i = 0; i < nsites; i++)
        ws_explicit_negsub_sites[i] = sites[i] & 0x1FFFFFFFu;
}
int psx_ws_is_cull_negsub_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_negsub_sites, ws_explicit_negsub_n, pc);
}
static uint32_t ws_explicit_vxrange_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_vxrange_n = 0;
void gpu_ws_set_vxrange_cull_sites(const uint32_t *sites, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_vxrange_n = nsites;
    for (int i = 0; i < nsites; i++)
        ws_explicit_vxrange_sites[i] = sites[i] & 0x1FFFFFFFu;
}
int psx_ws_is_cull_vxrange_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_vxrange_sites, ws_explicit_vxrange_n, pc);
}
static uint32_t ws_explicit_depth_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_depth_n = 0;
void gpu_ws_set_depth_cull_sites(const uint32_t *sites, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_depth_n = nsites;
    for (int i = 0; i < nsites; i++)
        ws_explicit_depth_sites[i] = sites[i] & 0x1FFFFFFFu;
}
int psx_ws_is_cull_depth_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_depth_sites, ws_explicit_depth_n, pc);
}
int32_t psx_ws_depth_bound(int32_t imm) {
    if (psx_ws_x_margin() <= 0) return imm;
    int64_t numerator = (int64_t)imm * 3 * ws_cfg_num;
    int64_t denominator = 4 * ws_cfg_den;
    if (denominator <= 0) return imm;
    int64_t result = numerator >= 0
        ? (numerator + denominator / 2) / denominator
        : -((-numerator + denominator / 2) / denominator);
    return (int32_t)result;
}
int psx_ws_is_cull_range_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_range_sites, ws_explicit_range_n, pc);
}
static uint32_t ws_explicit_plane_nx_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_plane_nx_n = 0;
void gpu_ws_set_plane_nx_sites(const uint32_t *sites, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_plane_nx_n = nsites;
    for (int i = 0; i < nsites; i++)
        ws_explicit_plane_nx_sites[i] = sites[i] & 0x1FFFFFFFu;
}
int psx_ws_is_cull_plane_nx_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_plane_nx_sites, ws_explicit_plane_nx_n, pc);
}
/* Side frustum-plane normal-X scale ([widescreen.cull] plane_nx_sites). The
 * plane half-angle goes as atan(nz/nx), so widening the cone by the aspect
 * ratio means scaling nx by the INVERSE factor (4*den)/(3*num):
 * atan(nz/(nx*4*den/(3*num))) == atan((3*num)/(4*den)*tan(theta)). Identity
 * at 4:3 (margin 0). */
int32_t psx_ws_plane_nx(int32_t nx) {
    if (psx_ws_x_margin() <= 0) return nx;
    int64_t numerator = (int64_t)nx * 4 * ws_cfg_den;
    int64_t denominator = 3 * ws_cfg_num;
    if (denominator <= 0) return nx;
    int64_t result = numerator >= 0
        ? (numerator + denominator / 2) / denominator
        : -((-numerator + denominator / 2) / denominator);
    return (int32_t)result;
}

static uint32_t ws_explicit_xclip_load_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_xclip_load_n = 0;
void gpu_ws_set_xclip_load_sites(const uint32_t *sites, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_xclip_load_n = nsites;
    for (int i = 0; i < nsites; i++)
        ws_explicit_xclip_load_sites[i] = sites[i] & 0x1FFFFFFFu;
}
int psx_ws_is_cull_xclip_load_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_xclip_load_sites, ws_explicit_xclip_load_n, pc);
}
/* Per-primitive X-reject bound ([widescreen.cull] xclip_load_sites). While
 * the margins are revealed the reject is disabled (INT32_MAX passes every
 * ANDI-masked u16 screen X, including wrapped off-left coords at 655xx); the
 * wide-surface scissor clips the overflow. Vanilla loaded value at 4:3. */
uint32_t psx_ws_xclip_bound(uint32_t vanilla) {
    return psx_ws_x_margin() > 0 ? 0x7FFFFFFFu : vanilla;
}

typedef struct {
    uint32_t address;
    uint32_t expected;
    uint32_t result;
} WsCullKeepSite;
static WsCullKeepSite ws_cull_keep_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_cull_keep_n = 0;
void gpu_ws_set_cull_keep_sites(const uint32_t *addresses,
                                const uint32_t *expected,
                                const uint32_t *results, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_cull_keep_n = nsites;
    for (int i = 0; i < nsites; i++) {
        ws_cull_keep_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_cull_keep_sites[i].expected = expected[i];
        ws_cull_keep_sites[i].result = results[i] ? 1u : 0u;
    }
}
uint32_t psx_ws_cull_keep_result(uint32_t vanilla, uint32_t forced) {
    return psx_ws_x_margin() > 0 ? (forced ? 1u : 0u) : vanilla;
}
int psx_ws_cull_keep_site(uint32_t pc, uint32_t instr, uint32_t vanilla,
                          uint32_t *out) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    for (int i = 0; i < ws_cull_keep_n; i++) {
        const WsCullKeepSite *site = &ws_cull_keep_sites[i];
        if (site->address != phys || site->expected != instr) continue;
        if (out) *out = psx_ws_cull_keep_result(vanilla, site->result);
        return 1;
    }
    return 0;
}

/* [[widescreen.cull.widen]] site registry — the interpreter's half.
 *
 * The recompiler emits the widened helper directly into native code, but the
 * same PC can also execute under the dirty-RAM interpreter, or inside an
 * overlay shard built without this config. Without a runtime lookup those
 * paths would evaluate the VANILLA compare while the AOT image evaluates the
 * widened one, so the same site would cull differently depending on which
 * backend happened to run it. Registering the sites here keeps both paths on
 * the same verdict, exactly as the keep registry above does. */
typedef struct {
    uint32_t address;
    uint32_t expected;
    uint32_t mode;   /* WsCullWidenMode: 0 imm_upper, 1 imm_lower,
                      *                  2 bound_rt,  3 bound_rs */
} WsCullWidenSite;
static WsCullWidenSite ws_cull_widen_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_cull_widen_n = 0;

void gpu_ws_set_cull_widen_sites(const uint32_t *addresses,
                                 const uint32_t *expected,
                                 const uint32_t *modes, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_cull_widen_n = nsites;
    for (int i = 0; i < nsites; i++) {
        ws_cull_widen_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_cull_widen_sites[i].expected = expected[i];
        ws_cull_widen_sites[i].mode = modes[i];
    }
}

/* Returns 1 and writes the widened verdict when `pc`/`instr` is a registered
 * widen site. `rs`/`rt` are the live operand values; `imm` the sign-extendable
 * immediate for the SLTI forms. Identity at margin 0 in every mode. */
int psx_ws_cull_widen_site(uint32_t pc, uint32_t instr, uint32_t rs,
                           uint32_t rt, uint32_t imm, uint32_t *out) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    for (int i = 0; i < ws_cull_widen_n; i++) {
        const WsCullWidenSite *site = &ws_cull_widen_sites[i];
        if (site->address != phys || site->expected != instr) continue;
        if (!out) return 1;
        switch (site->mode) {
            case 0:  *out = (uint32_t)psx_ws_cull_slti(rs, imm);        break;
            case 1:  *out = (uint32_t)psx_ws_cull_slti_lower(rs, imm);  break;
            case 2:  *out = (uint32_t)psx_ws_cull_slt_widen(rs, rt, 1); break;
            default: *out = (uint32_t)psx_ws_cull_slt_widen(rs, rt, 0); break;
        }
        return 1;
    }
    return 0;
}

typedef struct {
    uint32_t address;
    uint32_t expected;
} WsAngleSite;
static WsAngleSite ws_angle_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_angle_n = 0;
static uint64_t ws_angle_calls = 0;
static uint64_t ws_angle_43_identity = 0;
static uint32_t ws_angle_max_vanilla = 0;
static uint32_t ws_angle_max_widened = 0;

void gpu_ws_set_angle_sites(const uint32_t *addresses,
                            const uint32_t *expected, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX)
        nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_angle_n = nsites;
    for (int i = 0; i < nsites; i++) {
        ws_angle_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_angle_sites[i].expected = expected[i];
    }
    ws_angle_calls = 0;
    ws_angle_43_identity = 0;
    ws_angle_max_vanilla = 0;
    ws_angle_max_widened = 0;
}

uint32_t psx_ws_angle_widen(uint32_t vanilla) {
    ws_angle_calls++;
    const int margin = psx_ws_x_margin();
    if (margin <= 0) {
        ws_angle_43_identity++;
        return vanilla;
    }
    const uint32_t widened =
        psx_ws_widen_angle_q12(vanilla, margin);
    if (vanilla > ws_angle_max_vanilla)
        ws_angle_max_vanilla = vanilla;
    if (widened > ws_angle_max_widened)
        ws_angle_max_widened = widened;
    return widened;
}

int psx_ws_angle_site(uint32_t pc, uint32_t instr, uint32_t *out) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    for (int i = 0; i < ws_angle_n; i++) {
        const WsAngleSite *site = &ws_angle_sites[i];
        if (site->address != phys || site->expected != instr) continue;
        if (out)
            *out = psx_ws_angle_widen(
                (uint32_t)(int32_t)(int16_t)(instr & 0xFFFFu));
        return 1;
    }
    return 0;
}

typedef struct {
    uint32_t address;
    uint32_t expected;
    uint32_t cosine_threshold;
    uint8_t object_reg;
    uint8_t x_reg;
    uint8_t z_reg;
    uint8_t y_reg;
    uint8_t queue_guard;
    uint64_t calls;
    uint64_t identity_43;
    uint64_t vanilla_keep;
    uint64_t visible_keep;
    uint64_t guard_keep;
    uint64_t hysteresis_keep;
    uint64_t outside_reject;
    uint64_t queue_reject;
} WsAspectConeSite;

typedef struct {
    uint32_t site;
    uint32_t object;
    uint32_t last_seen_frame;
    uint8_t active;
} WsAspectConeState;

#define WS_ASPECT_CONE_STATE_CAP 512
static WsAspectConeSite ws_aspect_cone_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_aspect_cone_n = 0;
static uint32_t ws_aspect_cone_forward_addr = 0;
static uint32_t ws_aspect_cone_object_type_offset = 0;
static uint32_t ws_aspect_cone_hysteresis_pixels = 0;
static uint32_t ws_aspect_cone_queue_reserve = 0;
static uint32_t ws_aspect_cone_queue_count_addrs[3];
static uint32_t ws_aspect_cone_queue_capacities[3];
static uint32_t ws_aspect_cone_queue_type_masks[3];
static WsAspectConeState ws_aspect_cone_state[WS_ASPECT_CONE_STATE_CAP];
static int ws_aspect_cone_wide_latched = 0;
static uint64_t ws_aspect_cone_calls = 0;
static uint64_t ws_aspect_cone_43_identity = 0;
static uint64_t ws_aspect_cone_vanilla_keep = 0;
static uint64_t ws_aspect_cone_visible_keep = 0;
static uint64_t ws_aspect_cone_guard_keep = 0;
static uint64_t ws_aspect_cone_hysteresis_keep = 0;
static uint64_t ws_aspect_cone_outside_reject = 0;
static uint64_t ws_aspect_cone_queue_reject = 0;
static uint32_t ws_aspect_cone_queue_highwater[3];

void gpu_ws_set_aspect_cone(const uint32_t *addresses,
                            const uint32_t *expected,
                            const uint32_t *cosine_thresholds,
                            const uint32_t *object_regs,
                            const uint32_t *x_regs,
                            const uint32_t *z_regs,
                            const uint32_t *y_regs,
                            const uint32_t *queue_guards,
                            int nsites,
                            uint32_t forward_addr,
                            uint32_t object_type_offset,
                            uint32_t hysteresis_pixels,
                            uint32_t queue_reserve,
                            const uint32_t queue_count_addrs[3],
                            const uint32_t queue_capacities[3],
                            const uint32_t queue_type_masks[3]) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX)
        nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_aspect_cone_n = nsites;
    memset(ws_aspect_cone_sites, 0, sizeof(ws_aspect_cone_sites));
    for (int i = 0; i < nsites; i++) {
        ws_aspect_cone_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_aspect_cone_sites[i].expected = expected[i];
        ws_aspect_cone_sites[i].cosine_threshold =
            cosine_thresholds ? cosine_thresholds[i] : 0;
        ws_aspect_cone_sites[i].object_reg =
            (uint8_t)(object_regs ? object_regs[i] & 31u : 0u);
        ws_aspect_cone_sites[i].x_reg =
            (uint8_t)(x_regs ? x_regs[i] & 31u : 0u);
        ws_aspect_cone_sites[i].z_reg =
            (uint8_t)(z_regs ? z_regs[i] & 31u : 0u);
        ws_aspect_cone_sites[i].y_reg =
            (uint8_t)(y_regs ? y_regs[i] & 31u : 0u);
        ws_aspect_cone_sites[i].queue_guard =
            (uint8_t)(!queue_guards || queue_guards[i] != 0);
    }
    ws_aspect_cone_forward_addr = forward_addr;
    ws_aspect_cone_object_type_offset = object_type_offset;
    ws_aspect_cone_hysteresis_pixels = hysteresis_pixels;
    ws_aspect_cone_queue_reserve = queue_reserve;
    for (int i = 0; i < 3; i++) {
        ws_aspect_cone_queue_count_addrs[i] =
            queue_count_addrs ? queue_count_addrs[i] : 0;
        ws_aspect_cone_queue_capacities[i] =
            queue_capacities ? queue_capacities[i] : 0;
        ws_aspect_cone_queue_type_masks[i] =
            queue_type_masks ? queue_type_masks[i] : 0;
    }
    memset(ws_aspect_cone_state, 0, sizeof(ws_aspect_cone_state));
    ws_aspect_cone_wide_latched = 0;
    ws_aspect_cone_calls = 0;
    ws_aspect_cone_43_identity = 0;
    ws_aspect_cone_vanilla_keep = 0;
    ws_aspect_cone_visible_keep = 0;
    ws_aspect_cone_guard_keep = 0;
    ws_aspect_cone_hysteresis_keep = 0;
    ws_aspect_cone_outside_reject = 0;
    ws_aspect_cone_queue_reject = 0;
    memset(ws_aspect_cone_queue_highwater, 0,
           sizeof(ws_aspect_cone_queue_highwater));
}

static WsAspectConeSite *ws_aspect_cone_find(uint32_t address,
                                             uint32_t expected) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    for (int i = 0; i < ws_aspect_cone_n; i++) {
        WsAspectConeSite *site = &ws_aspect_cone_sites[i];
        if (site->address == phys &&
            (expected == 0 || site->expected == expected))
            return site;
    }
    return NULL;
}

static WsAspectConeState *ws_aspect_cone_object_state(uint32_t site,
                                                       uint32_t object) {
    const uint32_t now = (uint32_t)s_frame_count;
    uint32_t slot = (((object >> 2) * 2654435761u) ^
                     ((site >> 2) * 2246822519u)) &
                    (WS_ASPECT_CONE_STATE_CAP - 1u);
    WsAspectConeState *oldest = NULL;
    for (uint32_t probe = 0; probe < 8; probe++) {
        WsAspectConeState *entry =
            &ws_aspect_cone_state[(slot + probe) &
                                  (WS_ASPECT_CONE_STATE_CAP - 1u)];
        if (entry->site == site && entry->object == object) {
            entry->last_seen_frame = now;
            return entry;
        }
        if (entry->object == 0 || now - entry->last_seen_frame > 120u) {
            entry->site = site;
            entry->object = object;
            entry->last_seen_frame = now;
            entry->active = 0;
            return entry;
        }
        if (!oldest ||
            entry->last_seen_frame < oldest->last_seen_frame)
            oldest = entry;
    }
    oldest->site = site;
    oldest->object = object;
    oldest->last_seen_frame = now;
    oldest->active = 0;
    return oldest;
}

static int ws_aspect_cone_queue_has_guard_room(uint32_t object) {
    if (object == 0) return 0;
    const uint8_t type =
        psx_read_byte(object + ws_aspect_cone_object_type_offset);
    if (type >= 32) return 1;
    const uint32_t bit = 1u << type;
    for (int i = 0; i < 3; i++) {
        if ((ws_aspect_cone_queue_type_masks[i] & bit) == 0) continue;
        const uint32_t cap = ws_aspect_cone_queue_capacities[i];
        const uint32_t reserve = ws_aspect_cone_queue_reserve;
        if (cap == 0 || ws_aspect_cone_queue_count_addrs[i] == 0)
            return 1;
        const uint32_t count =
            psx_read_half(ws_aspect_cone_queue_count_addrs[i]);
        return count < ((cap > reserve) ? cap - reserve : 0u);
    }
    return 1;
}

static void ws_aspect_cone_sample_queues(void) {
    /* The game clears each queue at frame start and appends as candidates are
     * accepted. Sample throughout the producer loop so the high-water mark
     * sees the final occupancy rather than only the first zero count. */
    for (int i = 0; i < 3; i++) {
        if (ws_aspect_cone_queue_count_addrs[i] == 0) continue;
        const uint32_t count =
            psx_read_half(ws_aspect_cone_queue_count_addrs[i]);
        if (count > ws_aspect_cone_queue_highwater[i])
            ws_aspect_cone_queue_highwater[i] = count;
    }
}

uint32_t psx_ws_aspect_cone_result(uint32_t site_address, uint32_t vanilla,
                                   uint32_t object, int32_t x, int32_t z,
                                   int32_t y) {
    WsAspectConeSite *site = ws_aspect_cone_find(site_address, 0);
    if (!site) return vanilla;

    ws_aspect_cone_calls++;
    site->calls++;
    const int total_margin = psx_ws_x_margin();
    if (total_margin <= 0) {
        ws_aspect_cone_43_identity++;
        site->identity_43++;
        if (ws_aspect_cone_wide_latched) {
            memset(ws_aspect_cone_state, 0, sizeof(ws_aspect_cone_state));
            ws_aspect_cone_wide_latched = 0;
        }
        return vanilla;
    }
    ws_aspect_cone_wide_latched = 1;
    ws_aspect_cone_sample_queues();

    WsAspectConeState *state =
        ws_aspect_cone_object_state(site->address, object);
    /* The guarded Tomba sites are signed reject predicates: zero falls
     * through to model participation, while one branches to rejection. */
    if (!vanilla) {
        state->active = 1;
        ws_aspect_cone_vanilla_keep++;
        site->vanilla_keep++;
        return 0;
    }

    const int visible_margin =
        (total_margin > ws_cull_guard_pixels)
            ? total_margin - ws_cull_guard_pixels : 0;
    const int32_t fx =
        (int16_t)psx_read_half(ws_aspect_cone_forward_addr + 0u);
    const int32_t fz =
        (int16_t)psx_read_half(ws_aspect_cone_forward_addr + 2u);
    const int32_t fy =
        (int16_t)psx_read_half(ws_aspect_cone_forward_addr + 4u);
    const uint32_t threshold = site->cosine_threshold;

    const int in_visible =
        psx_ws_aspect_cone_contains(x, z, y, fx, fz, fy, threshold,
                                    visible_margin);
    const int in_guard =
        !in_visible && psx_ws_aspect_cone_contains(
            x, z, y, fx, fz, fy, threshold, total_margin);
    const int in_hysteresis =
        !in_visible && !in_guard && state->active &&
        psx_ws_aspect_cone_contains(
            x, z, y, fx, fz, fy, threshold,
            total_margin + (int)ws_aspect_cone_hysteresis_pixels);

    if (!in_visible && !in_guard && !in_hysteresis) {
        state->active = 0;
        ws_aspect_cone_outside_reject++;
        site->outside_reject++;
        return 1;
    }
    if (!in_visible && site->queue_guard &&
        !ws_aspect_cone_queue_has_guard_room(object)) {
        state->active = 0;
        ws_aspect_cone_queue_reject++;
        site->queue_reject++;
        return 1;
    }
    state->active = 1;
    if (in_visible) {
        ws_aspect_cone_visible_keep++;
        site->visible_keep++;
    } else if (in_guard) {
        ws_aspect_cone_guard_keep++;
        site->guard_keep++;
    } else {
        ws_aspect_cone_hysteresis_keep++;
        site->hysteresis_keep++;
    }
    return 0;
}

int psx_ws_aspect_cone_site(CPUState *cpu, uint32_t pc, uint32_t instr,
                            uint32_t vanilla, uint32_t *out) {
    const WsAspectConeSite *site = ws_aspect_cone_find(pc, instr);
    if (!site) return 0;
    if (out) {
        *out = psx_ws_aspect_cone_result(
            pc, vanilla, cpu->gpr[site->object_reg],
            (int32_t)(int16_t)cpu->gpr[site->x_reg],
            (int32_t)(int16_t)cpu->gpr[site->z_reg],
            (int32_t)(int16_t)cpu->gpr[site->y_reg]);
    }
    return 1;
}

int psx_ws_x_margin(void) {
    if (ws_margin_override >= 0) return ws_margin_override;
    /* Native-wide: widen the world-space draw cull by the per-side reveal
     * (== the centering OFFSET in screen px) so the game SUBMITS the geometry
     * that previously fell outside the 4:3 cull window; the wide compositor then
     * rasterizes it into the revealed margins. Same recompiler emit sites as the
     * squash path ([widescreen.cull]); 0 at 4:3 so the cull stays byte-identical. */
    /* Unlike rendering/presentation, do not wait for game-mode detection here.
     * Tomba 2 builds its terrain-cell and actor spawn lists during scene load;
     * returning zero until the first 3D frame permanently bakes a 4:3 frustum
     * into those lists. */
    if (ws_native_wide_configured())
        return ws_nw_configured_offset() + ws_cull_guard_pixels;
    if (!ws_active()) return 0;
    return (160 * (ws_xden - ws_xnum) + ws_xnum / 2) / ws_xnum
           + ws_cull_guard_pixels;
}

int psx_ws_activation_margin(void) {
    const int margin = psx_ws_x_margin();
    return margin > 0 ? margin + ws_activation_guard_pixels : 0;
}

int32_t psx_ws_player_x_bound(int32_t vanilla)
{
    const int margin = psx_ws_x_margin();
    if (margin <= 0) return vanilla;
    return (int32_t)(((int64_t)vanilla * (320 + 2 * margin)) / 320);
}

#define WS_SIGNED_BOUND_MAX 64
static uint32_t ws_signed_bound_addr[WS_SIGNED_BOUND_MAX];
static uint32_t ws_signed_bound_expected[WS_SIGNED_BOUND_MAX];
static int ws_signed_bound_count = 0;
void gpu_ws_set_signed_x_bound_sites(const uint32_t *addresses,
                                     const uint32_t *expected, int count) {
    if (count < 0) count = 0;
    if (count > WS_SIGNED_BOUND_MAX) count = WS_SIGNED_BOUND_MAX;
    ws_signed_bound_count = count;
    for (int i = 0; i < count; i++) {
        ws_signed_bound_addr[i] = addresses[i] & 0x1FFFFFFFu;
        ws_signed_bound_expected[i] = expected[i];
    }
}
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t instr) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    for (int i = 0; i < ws_signed_bound_count; i++)
        if (ws_signed_bound_addr[i] == phys && ws_signed_bound_expected[i] == instr)
            return 1;
    return 0;
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
static int g_bg2d_parent_links = 1;

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

void gpu_ws_bg2d_set_parent_links(int on) {
    g_bg2d_parent_links = on ? 1 : 0;
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
/* Column count: base + both-side reveal. */
static void mmx6_bg_refill_tick(void);   /* defined below (ring-freshness fix) */
int psx_ws_bg2d_cols(int base) {
    g_bg2d_native_cols = base;
    mmx6_bg_refill_tick();
    return base + 2 * ws_bg2d_left_cols();
}
/* Start tile column: refill before the first column is consumed, then begin LEFT
 * earlier in the ring. The count hook retains the same tick for layouts that load
 * their loop bound before calculating the starting column. */
int psx_ws_bg2d_startcol(int col, unsigned mask) {
    mmx6_bg_refill_tick();
    return (col - ws_bg2d_left_cols()) & (int)mask;
}
/* Start screen-x: LEFT*16 further left. */
int psx_ws_bg2d_startx(int x)        { return x - ws_bg2d_left_cols() * 16; }

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

/* NOT MMX6-only, and NOT dead -- see the survey note on the declarations in
 * runtime/include/gpu.h. Live generated code in CrashBash, MegaManX5 and Tsumu
 * calls these, and seven repos call psx_ws_mmx6_bg_undercap below. The mmx6 in
 * the name is historical; the recompiler emits these names for any title with
 * a [widescreen.bg2d] block. Renaming means touching code_generator.cpp, which
 * rolls the overlay cache tag for every game. */
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

/* Retired packet-buffer relocation hooks. These symbols remain part of the
 * generated-code ABI; their bodies preserve the committed guest-widen path. */
int psx_ws_mmx6_bg_bufbase(int addr) {
    return addr;
}

int psx_ws_mmx6_bg_undercap(int counter) {
    return psx_ws_bg2d_undercap(counter, 1000);
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
    uint32_t mapBase  = psx_read_word(0x1F800004u);
    uint32_t metaBase = psx_read_word(0x1F800008u);
    if (mapBase == 0 || metaBase == 0 || mapW <= 0) return 0;
    int layerStride = (uint16_t)psx_read_half(g_bg2d_layer_stride_addr);

    /*
     * The Capcom streamer has no map-height field or vertical bounds check.
     * After normalizing negative worldY to zero it indexes rows as
     *   layer*stride + mapW*metaRow + metaCol
     * directly. Treating map_size_addr+1 as a guessed height rejects every
     * MMX4 column because that adjacent byte is zero.
     */
    int metaIdx = psx_read_byte(
        mapBase + (uint32_t)(layer * layerStride + mapW * metaRow + metaCol));

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
            metaIdx = psx_read_byte(
                mapBase +
                (uint32_t)(layer * layerStride + mapW * metaRow + metaCol));
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
        if (g_bg2d_parent_links &&
            parent >= 0 && (uint32_t)(uint8_t)parent < g_bg2d_layer_count) {
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
        if (g_bg2d_parent_links &&
            parent >= 0 && (uint32_t)(uint8_t)parent < g_bg2d_layer_count) {
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
 * called identically by the gcc emit and the interpreter so every
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

/* Signed fixed lower-bound widen (`slti v, x, -W`): move the reject edge left
 * by one live reveal margin. The encoded immediate must be sign-extended. */
int psx_ws_cull_slti_lower(uint32_t sx, uint32_t imm) {
    int32_t bound = (int32_t)(int16_t)(uint16_t)imm;
    return ((int32_t)sx < bound - psx_ws_x_margin()) ? 1 : 0;
}

/* Register-bound widen for `SLT rd, rs, rt` ([[widescreen.cull.widen]]).
 *
 * The keep-site helper above PINS a verdict, which is correct only for a
 * separately proven binary decision. At a clip-code packer it is actively
 * wrong: pinning the classifier tells the clipper nothing crosses the screen
 * edge, so polygons that do are never subdivided, are submitted with
 * coordinates outside the GPU's legal primitive range, and the hardware drops
 * the whole primitive. Widening moves the BOUND instead, so the clipper still
 * classifies correctly and simply clips at the revealed edge.
 *
 * `bound_is_rt` selects which operand carries the bound, which is the only
 * thing that differs between the two idioms:
 *   bound_is_rt : coord in rs, bound in rt -> rs <  rt + m   (upper edge)
 *   otherwise   : bound in rs, coord in rt -> rs + m <  rt   (lower edge)
 * Both reduce to the vanilla `(int32_t)rs < (int32_t)rt` at margin 0, so 4:3
 * stays bit-for-bit identical. */
int psx_ws_cull_slt_widen(uint32_t rs, uint32_t rt, int bound_is_rt) {
    const int32_t m = psx_ws_x_margin();
    const int32_t a = (int32_t)rs, b = (int32_t)rt;
    return bound_is_rt ? ((a < b + m) ? 1 : 0)
                       : ((a + m < b) ? 1 : 0);
}

/* Signed left-edge widen for the funnel's `bltz maxSX, reject`: reject only
 * when the prim ends left of the REVEALED edge (maxSX < -margin). Returns the
 * branch predicate. Identity at margin 0 (4:3). */
int psx_ws_cull_bltz(uint32_t v) {
    return ((int32_t)v < -psx_ws_x_margin()) ? 1 : 0;
}
int psx_ws_cull_vxrange(uint32_t x, uint32_t imm) {
    int32_t margin = psx_ws_x_margin();
    uint32_t bound = (uint32_t)(int32_t)(int16_t)(uint16_t)imm;
    return (((x + (uint32_t)margin) & 0xFFFFu) <
            (bound + 2u * (uint32_t)margin)) ? 1 : 0;
}

/* ---- Cull signature configuration ([widescreen.cull] screen_w_imms /
 * screen_h_imms). The width/height immediates are per-game (Tomba: 0x140/0x141
 * + 0xE0/0xF1 on a 320 display; Ape Escape: 0x181 + 0xF1 on 368). Defaults
 * keep the original Tomba signature so existing configs are unchanged. The
 * sets are consulted by the shared detector on every backend (interp;
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

/* ---- Runtime gates for the pattern-scanned widescreen hooks. The interp
 * derives widen sites by scanning live code; that derivation must honor
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
 * (slti or sltiu, immediates from the configured sets). Lets the
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
 * In native-wide mode there is nothing to squash (the 4:3 frame is presented
 * with side reveal instead), so the same sites are STRETCHED about the screen
 * centre by (disp_w + nw_extra) / disp_w: a 4:3-authored backdrop then covers
 * the widened frame instead of leaving unpainted margins (Xenogears battle
 * mountain panels — pre-calculated POLY_FT4 screen coords stored by main-EXE
 * `sh` sites, wtrace-evidenced).
 * x is the int16 screenX the handler was about to store. */
int psx_ws_backdrop_x(int x) {
    if (ws_native_wide_active()) {
        int32_t X = ws_disp_x();
        int32_t W = (int32_t)ws_disp_w();
        int32_t extra = ws_nw_extra();
        int32_t cx = W / 2;
        int32_t d = (int16_t)x - X - cx;
        return (int)(X + cx + (d * (W + extra) + (d >= 0 ? W / 2 : -W / 2)) / W);
    }
    if (!ws_active()) return (int16_t)x;
    int32_t cx = ws_disp_x() + ws_disp_w() / 2;   /* screen centre (=160 @ 320) */
    return ws_scale_about((int16_t)x, cx);
}

/* Backdrop PRELOAD predicate + value substitution ([widescreen.cull]
 * auto_backdrop). The recompiler/interp detect each scrolling-backdrop
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
     * overlay callback). The interp records its own richer entry, so skip
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

/* ws_ui_groups — dump the auto_ui_squash partition for the LAST prepass.
 *
 * auto_ui_squash squashes each spatial run about its own anchor, so a HUD
 * element that lands in two runs gets two anchors and comes apart as the frame
 * widens. Nothing exposed which run a primitive ended up in, which made that
 * failure mode guesswork: the key is a hash of CLUT/texpage/Y-band/family, so
 * comparing keys tells you two prims differ without telling you why, and the
 * anchor takes only three values so anchor equality cannot prove co-grouping.
 *
 * This reports both the raw key inputs and the union-find root, which together
 * answer "did these two merge, and if not, which component split them". */
int psx_ws_ui_groups_json(char *buf, int cap) {
    int off = snprintf(buf, (size_t)cap,
        "\"active\":%d,\"squash\":%d,\"dense\":%d,\"rank\":%d,"
        "\"disp_x\":%d,\"disp_w\":%d,\"join_gap\":%d,"
        "\"rejected\":{\"opcode\":%u,\"not_axis\":%u,\"degenerate\":%u,"
        "\"too_big\":%u,\"cap\":%u,\"rank\":%u},"
        "\"n\":%u,",
        ws_active(), ws_auto_ui_squash, ws_auto_ui_dense,
        ws_ui_prepass_rank != 0xFFFFu ? (int)ws_ui_prepass_rank : -1,
        ws_disp_x(), ws_disp_w(), WS_UI_GROUP_JOIN_GAP,
        ws_ui_reject.opcode, ws_ui_reject.not_axis, ws_ui_reject.degenerate,
        ws_ui_reject.too_big, ws_ui_reject.cap, ws_ui_reject.rank,
        ws_ui_prepass_count);
    off += snprintf(buf + off, (size_t)(cap - off), "\"rank_dropped\":[");
    for (uint32_t i = 0; i < ws_ui_rankdrop_count && off < cap - 120; i++) {
        off += snprintf(buf + off, (size_t)(cap - off),
            "%s{\"op\":\"%02x\",\"rank\":%u,\"x\":%d,\"w\":%d,\"y\":%d,\"h\":%d}",
            i ? "," : "", ws_ui_rankdrop[i].op, ws_ui_rankdrop[i].rank,
            ws_ui_rankdrop[i].x, ws_ui_rankdrop[i].w,
            ws_ui_rankdrop[i].y, ws_ui_rankdrop[i].h);
    }
    off += snprintf(buf + off, (size_t)(cap - off), "],\"items\":[");
    for (uint32_t i = 0; i < ws_ui_prepass_count && off < cap - 220; i++) {
        const WsUiPrepassItem *it = &ws_ui_prepass[i];
        /* Recover the key components so a split is attributable. Mirrors
         * ws_auto_ui_group_key_words; kept in step with it by construction. */
        int32_t centre_y = it->y + it->h / 2;
        unsigned band = (unsigned)((centre_y < 0 ? 0 : centre_y / 24) & 0x1F);
        unsigned family = it->op < 0x60u ? 1u : 2u;
        off += snprintf(buf + off, (size_t)(cap - off),
            "%s{\"i\":%u,\"op\":\"%02x\",\"key\":\"%08x\",\"root\":%u,"
            "\"x\":%d,\"w\":%d,\"y\":%d,\"h\":%d,"
            "\"band\":%u,\"family\":%u,\"anchor\":%d,\"src\":\"%08x\"}",
            i ? "," : "", i, it->op, it->group.key, it->group.root,
            it->group.x, it->group.width, it->y, it->h,
            band, family, it->group.anchor, it->src_addr);
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
    out->present_native_43 = gpu_ws_content_native_43();
    out->x_margin          = psx_ws_x_margin();
    out->activation_margin = psx_ws_activation_margin();
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
    out->auto_ui_squash    = ws_auto_ui_squash;
    out->auto_ui_dense     = ws_auto_ui_dense;
    out->auto_ui_ot_rank   =
        ws_ui_prepass_rank != 0xFFFFu ? ws_ui_prepass_rank : UINT32_MAX;
    out->auto_ui_candidates = ws_auto_ui_candidate_count;
    out->auto_ui_transforms = ws_auto_ui_transform_count;
    out->aspect_cone_calls = ws_aspect_cone_calls;
    out->aspect_cone_43_identity = ws_aspect_cone_43_identity;
    out->aspect_cone_vanilla_keep = ws_aspect_cone_vanilla_keep;
    out->aspect_cone_visible_keep = ws_aspect_cone_visible_keep;
    out->aspect_cone_guard_keep = ws_aspect_cone_guard_keep;
    out->aspect_cone_hysteresis_keep =
        ws_aspect_cone_hysteresis_keep;
    out->aspect_cone_outside_reject =
        ws_aspect_cone_outside_reject;
    out->aspect_cone_queue_reject = ws_aspect_cone_queue_reject;
    for (int i = 0; i < 3; i++)
        out->aspect_cone_queue_highwater[i] =
            ws_aspect_cone_queue_highwater[i];
    out->angle_calls = ws_angle_calls;
    out->angle_43_identity = ws_angle_43_identity;
    out->angle_max_vanilla = ws_angle_max_vanilla;
    out->angle_max_widened = ws_angle_max_widened;
}

int gpu_ws_get_aspect_cone_site_debug(
    uint32_t address, GpuWsAspectConeSiteDebug* out) {
    WsAspectConeSite *site = ws_aspect_cone_find(address, 0);
    if (!site || !out) return 0;
    out->address = site->address | 0x80000000u;
    out->calls = site->calls;
    out->identity_43 = site->identity_43;
    out->vanilla_keep = site->vanilla_keep;
    out->visible_keep = site->visible_keep;
    out->guard_keep = site->guard_keep;
    out->hysteresis_keep = site->hysteresis_keep;
    out->outside_reject = site->outside_reject;
    out->queue_reject = site->queue_reject;
    return 1;
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
    /* Adaptive view can change mode/extent without the guest reissuing E3/E4
     * immediately. Keep the active mirror target and its scissor in lockstep
     * with the new live aspect instead of waiting for another draw-env packet. */
    ws_nw_sync_target();
}

/* Called from generated code at the entry of each [widescreen]
 * sprite_tag_funcs function: record prim pointer ($a0) -> projected anchor.
 * Gated on ws_configured (NOT ws_active): ws_active depends on game_mode which
 * THIS function sets, so gating it on ws_active would never let gameplay
 * start (the first character of a frame would be suppressed forever). */
void psx_ws_sprite_tag(CPUState* cpu) {
    if (!ws_engaged() || !ws_anchor_addr) return;
    uint32_t key = psx_ram_map_read(cpu->gpr[4] & 0x1FFFFFFFu) & ~3u;
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
        uint32_t key = psx_ram_map_read((gp0_cmd_source_addr - (variant ? 0u : 4u)) & 0x1FFFFFFFu) & ~3u;
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
static int ws_nw_textured_edges = 0;
int g_ws_tex_edge_pct = 0;
void gpu_ws_set_nw_textured_edges(int on, int scale_pct) {
    ws_nw_textured_edges = on ? 1 : 0;
    g_ws_tex_edge_pct = scale_pct;
}
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
    if (ws_nw_textured_edges) {
        uint32_t op = (gp0_cmd_buf[0] >> 24) & 0xFFu;
        if (op >= 0x20u && op <= 0x3Fu && (op & 0x04u))
            return 2; /* GL gate: expand only vertices beyond canonical edges */
    }
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
        uint32_t key = psx_ram_map_read((gp0_cmd_source_addr - (variant ? 0u : 4u)) & 0x1FFFFFFFu) & ~3u;
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

/* Horizontal display viewport in drawing space (e.g. 320). For a selected
 * netplay-local split viewport this is that player's authored half, not the
 * whole composed display. */
static int32_t ws_disp_x(void) {
    int base = 0;
    if (ws_local_viewport_layout(&base, NULL, NULL, NULL))
        return (int32_t)base;
    return 0;
}

static int32_t ws_disp_w(void) {
    int src_w = 0;
    if (ws_local_viewport_layout(NULL, &src_w, NULL, NULL))
        return (int32_t)src_w;
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
    int X = (int)ws_disp_x(), W = (int)ws_disp_w(), H = (int)ws_disp_h();
    if (*x <= X && *x + *w >= X + W && y <= 0 && y + h >= H) {
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
    int32_t X = ws_disp_x();
    int32_t W = ws_disp_w();
    int32_t cx = 2 * (x - X) + w;      /* 2*centre, avoids losing the half */
    if (3 * cx < 2 * W) return X;
    if (3 * cx > 4 * W) return X + W;
    return X + W / 2;
}

/* Automatic UI correction is deliberately tied to draw provenance, not to
 * primitive size or "does this look 2D?" guesses. Ape's ordering table submits
 * HUD/font/icon packets in the final layer, after every depth-sorted world
 * bucket. A read-only prepass finds that final rank and complete spatial groups
 * before the list streams through GP0. This excludes CPU-built characters (the
 * source of the old squashed-Spike regression) even when their packets are
 * axis-aligned, and gives animated glyphs a shared anchor on their first frame. */
static int ws_auto_ui_anchor(int32_t *out_anchor) {
    if (!ws_auto_ui_squash || !ws_active() ||
        gp0_cmd_source_addr == 0xFFFFFFFFu)
        return 0;
    uint32_t src = psx_ram_map_read(gp0_cmd_source_addr & 0x1FFFFFFFu) & ~3u;
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
        if (ws_ui_prepass[i].src_addr != src) continue;
        if (out_anchor) *out_anchor = ws_ui_prepass[i].group.anchor;
        ws_auto_ui_candidate_count++;
        return 1;
    }
    return 0;
}

static uint32_t ws_auto_ui_group_key_words(const uint32_t *words,
                                           uint32_t op,
                                           int32_t y, int32_t h) {
    uint32_t clut = words[2] >> 16;
    uint32_t tpage = 0;
    if (op >= 0x20u && op <= 0x3Fu && (op & 0x04u)) {
        int tp_index = (op & 0x10u) ? 5 : 4;
        tpage = (words[tp_index] >> 16) & 0x1FFu;
    }
    int32_t centre_y = y + h / 2;
    uint32_t band = (uint32_t)(centre_y < 0 ? 0 : centre_y / 24) & 0x1Fu;
    uint32_t family = op < 0x60u ? 1u : 2u;
    uint32_t key = (clut * 2654435761u) ^ (tpage << 11) ^
                   (band << 3) ^ family;
    return key ? key : 1u;
}

static int ws_axis_aligned_quad(const int32_t vx[4], const int32_t vy[4]) {
    int32_t min_x = vx[0], max_x = vx[0], min_y = vy[0], max_y = vy[0];
    for (int i = 1; i < 4; i++) {
        if (vx[i] < min_x) min_x = vx[i];
        if (vx[i] > max_x) max_x = vx[i];
        if (vy[i] < min_y) min_y = vy[i];
        if (vy[i] > max_y) max_y = vy[i];
    }
    unsigned corners = 0;
    for (int i = 0; i < 4; i++) {
        if      (vx[i] == min_x && vy[i] == min_y) corners |= 1u;
        else if (vx[i] == max_x && vy[i] == min_y) corners |= 2u;
        else if (vx[i] == min_x && vy[i] == max_y) corners |= 4u;
        else if (vx[i] == max_x && vy[i] == max_y) corners |= 8u;
        else return 0;
    }
    return corners == 15u;
}

static int ws_auto_ui_transform_quad(int32_t vx[4], const int32_t vy[4]) {
    if (psx_ws_prim_is_tagged() || !ws_axis_aligned_quad(vx, vy))
        return 0;

    int32_t min_x = vx[0], max_x = vx[0], min_y = vy[0], max_y = vy[0];
    for (int i = 1; i < 4; i++) {
        if (vx[i] < min_x) min_x = vx[i];
        if (vx[i] > max_x) max_x = vx[i];
        if (vy[i] < min_y) min_y = vy[i];
        if (vy[i] > max_y) max_y = vy[i];
    }
    int32_t width = max_x - min_x, height = max_y - min_y;
    int32_t X = ws_disp_x(), W = ws_disp_w(), H = ws_disp_h();
    if ((min_x <= X && max_x >= X + W && min_y <= 0 && max_y >= H) ||
        (width > W / 2 && height > H / 4))
        return 0;

    int32_t anchor;
    if (!ws_auto_ui_anchor(&anchor)) return 0;
    for (int i = 0; i < 4; i++) vx[i] = ws_scale_about(vx[i], anchor);
    ws_auto_ui_transform_count++;
    return 1;
}

static int ws_auto_ui_transform_rect(int32_t *x, int32_t y, int *w, int h) {
    if (!x || !w || *w <= 0 || psx_ws_prim_is_tagged())
        return 0;
    int32_t X = ws_disp_x(), W = ws_disp_w(), H = ws_disp_h();
    if ((*x <= X && *x + *w >= X + W && y <= 0 && y + h >= H) ||
        (*w > W / 2 && h > H / 4))
        return 0;
    int32_t anchor;
    if (!ws_auto_ui_anchor(&anchor)) return 0;
    *x = ws_scale_about(*x, anchor);
    *w = (int)ws_scale_len(*w);
    ws_auto_ui_transform_count++;
    return 1;
}

/* Shared transform for fixed-size textured sprites (8x8 / 16x16 / 1x1 dot):
 * squash *x0 in place (around the tagged anchor, else the HUD pivot) and
 * return the squashed draw width, or 0 = no change. */
static int ws_sprt_fixed_transform(int32_t *x0, int32_t y0, int w) {
    if (!ws_active()) return 0;
    int32_t ax;
    if (ws_tagged_anchor(&ax)) {
        *x0 = ws_scale_about(*x0, ax);
        return (int)ws_scale_len(w);
    }
    int auto_w = w;
    if (ws_auto_ui_transform_rect(x0, y0, &auto_w, w))
        return auto_w;
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
    int32_t X  = ws_disp_x();
    int32_t W  = ws_disp_w();
    int32_t cx = 2 * (x - X) + w;      /* 2*centre, avoids losing the half */
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
    int32_t X = ws_disp_x();
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
    if (minx > X + EDGE || maxx < X + W - EDGE || (maxy - miny) < 64) return 0;
    /* Axis-aligned: every vertex X sits at either the min or the max edge, and
     * every Y at the top or bottom edge (a true screen-space rectangle). */
    for (int i = 0; i < 4; i++) {
        int xe = (vx[i] - minx <= EDGE) || (maxx - vx[i] <= EDGE);
        int ye = (vy[i] - miny <= EDGE) || (maxy - vy[i] <= EDGE);
        if (!xe || !ye) return 0;
    }
    /* Stretch X about the display centre by (W+extra)/W so [0,W] -> [-off, W+off],
     * which the wide compositor (+off) maps onto the full [0, W+extra] surface. */
    int32_t cx = X + W / 2;
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
/* Stage one complete GP0(A0) transfer so renderer backends receive one bulk
 * rectangle instead of hundreds of thousands of single-pixel callbacks. The
 * CPU-visible transfer remains ordered because GP0 accepts no next command
 * until this payload is complete. Maximum PS1 transfer = full VRAM (1 MiB). */
static uint16_t vram_write_pixels[1024 * 512];

/* Depth24 CPU→VRAM upload span (halfwords, exclusive end). See
 * gpu_depth24_rgb_limit — declared early so gpu_reset_state can clear it. */
static uint32_t s_d24_upload_x1 = 0;
static int      s_d24_present_hold = 0; /* vblanks to skip Swap after GP1(07h) */
static uint32_t s_d24_prev_disp_h = 0;  /* last GP1(07h) band height */
static void depth24_note_upload(uint32_t x, uint32_t w);

static void gp0_commit_cpu_to_vram(void) {
    for (uint32_t row = 0; row < vram_write_h; row++)
        for (uint32_t col = 0; col < vram_write_w; col++)
            vram_write_pixels[row * vram_write_w + col] =
                vram[((vram_write_y + row) & 511u) * 1024u +
                     ((vram_write_x + col) & 1023u)];
    gr_vram_transfer_in(vram_write_x, vram_write_y,
                        vram_write_w, vram_write_h, vram_write_pixels);
    depth24_note_upload(vram_write_x, vram_write_w);
    gp0_state = GP0_IDLE;
    vram_write_remaining = 0;
    text_xlate_vram_upload(vram_write_x, vram_write_y,
                           vram_write_w, vram_write_h);
}

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

static int draw_area_intersects_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return 0;
    int r = x + w - 1;
    int b = y + h - 1;
    return !((int)draw_area_right < x ||
             (int)draw_area_left > r ||
             (int)draw_area_bottom < y ||
             (int)draw_area_top > b);
}

static int ws_local_viewport_draw_target(int *base_x) {
    int base = 0, src_w = 0;
    if (!ws_local_viewport_layout(&base, &src_w, NULL, NULL))
        return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (draw_area_intersects_rect(base, (int)di.display_y,
                                  src_w, (int)di.height)) {
        if (base_x) *base_x = base;
        return 1;
    }
    return 0;
}

static int ws_display_viewport_draw_target(int *base_x) {
    if (!ws_native_wide_active())
        return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.depth24 || di.width == 0 || di.height == 0)
        return 0;
    if (!draw_area_intersects_rect((int)di.display_x, (int)di.display_y,
                                   (int)di.width, (int)di.height))
        return 0;
    if (base_x) *base_x = (int)di.display_x;
    return 1;
}

typedef struct GpuVerticalSplitTrace {
    uint8_t left_seen;
    uint8_t right_seen;
    uint16_t display_w;
    uint16_t display_h;
} GpuVerticalSplitTrace;

static GpuVerticalSplitTrace split_trace_this;
static GpuVerticalSplitTrace split_trace_last;
static uint8_t split_recent_left_age = 255;
static uint8_t split_recent_right_age = 255;
static uint16_t split_recent_display_w = 0;
static uint16_t split_recent_display_h = 0;

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
/* Host presentation defers its authoritative CRTC banner until the guest has
 * actually issued GP1(08h). This is host-observation state, not guest state. */
static uint32_t display_mode_programmed;
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

static void split_trace_reset(GpuVerticalSplitTrace *trace) {
    memset(trace, 0, sizeof(*trace));
}

static void split_trace_note_draw_area(void) {
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.depth24 || di.width < 256 || di.height < 128)
        return;
    if ((di.width & 1u) != 0)
        return;
    if (di.display_x + di.width > 1024u || di.display_y + di.height > 512u)
        return;
    if (draw_area_right <= draw_area_left || draw_area_bottom <= draw_area_top)
        return;

    const int disp_l = (int)di.display_x;
    const int disp_r = (int)(di.display_x + di.width - 1u);
    const int mid = disp_l + (int)(di.width / 2u);
    const int tol = (int)di.width / 32 > 8 ? (int)di.width / 32 : 8;

    const int area_l = (int)draw_area_left;
    const int area_r = (int)draw_area_right;

    if (split_trace_this.display_w != 0 &&
        (split_trace_this.display_w != (uint16_t)di.width ||
         split_trace_this.display_h != (uint16_t)di.height)) {
        split_trace_reset(&split_trace_this);
    }
    split_trace_this.display_w = (uint16_t)di.width;
    split_trace_this.display_h = (uint16_t)di.height;

    if (area_l <= disp_l + tol &&
        area_r >= mid - 1 - tol && area_r <= mid - 1 + tol) {
        split_trace_this.left_seen = 1;
    }
    if (area_l >= mid - tol && area_l <= mid + tol &&
        area_r >= disp_r - tol) {
        split_trace_this.right_seen = 1;
    }
}

static int ws_vertical_split_active(void) {
    return split_recent_left_age <= 8 && split_recent_right_age <= 8;
}

int gpu_last_frame_vertical_split_screen(void) {
    return ws_vertical_split_active();
}

void gpu_vertical_split_debug(int *active, int *left_age, int *right_age) {
    if (active) *active = gpu_last_frame_vertical_split_screen();
    if (left_age) *left_age = split_recent_left_age;
    if (right_age) *right_age = split_recent_right_age;
}

/* Point the renderer's wide mirror at the current back buffer (draw_area_left)
 * when native-wide is active and that buffer is a known display buffer; else
 * disable mirroring for this draw. Called when the draw env changes. */
static void ws_nw_sync_target(void) {
    if (!ws_native_wide_active()) { gr_wide_disable_target(); return; }
    gr_wide_configure(ws_nw_present_width(), ws_nw_offset());
    int local_base = 0;
    if (ws_local_viewport_draw_target(&local_base)) {
        gr_wide_set_target(local_base);
        return;
    }
    if (ws_local_viewport_cfg) { gr_wide_disable_target(); return; }
    int display_base = 0;
    if (ws_display_viewport_draw_target(&display_base)) {
        gr_wide_set_target(display_base);
        return;
    }
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
void gpu_ws_begin_linked_list(void) {
    gp0_ot_rank = 0xFFFFu;
}

void gpu_set_gp0_linked_list_node(uint32_t addr, uint32_t word_count) {
    (void)addr;
    if (word_count == 0) {
        gp0_ot_rank = gp0_ot_rank == 0xFFFFu ? 0u
                                             : (uint16_t)(gp0_ot_rank + 1u);
    }
}

void gpu_ws_end_linked_list(void) {
    gp0_ot_rank = 0xFFFFu;
}


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
        gpu_vram_dirty_mark_all();
    }
    gr_init(vram);

    /* Reset GP0 state machine (+ leftover cmd/xfer crumbs that survive in
     * gpu_snapshot and fork av digests on rematch). */
    gp0_state = GP0_IDLE;
    gp0_words_collected = 0;
    gp0_words_needed = 0;
    memset(gp0_cmd_buf, 0, sizeof(gp0_cmd_buf));
    gp0_next_source_addr = 0xFFFFFFFFu;
    gp0_cmd_source_addr = 0xFFFFFFFFu;
    polyline_color = 0;
    polyline_prev_x = polyline_prev_y = 0;
    polyline_prev_c = 0;
    polyline_semi_trans = 0;
    polyline_has_prev = 0;
    vram_write_x = vram_write_y = 0;
    vram_write_w = vram_write_h = 0;
    vram_write_col = vram_write_row = 0;
    vram_write_remaining = 0;
    vram_read_active = 0;
    vram_read_x = vram_read_y = 0;
    vram_read_w = vram_read_h = 0;
    vram_read_col = vram_read_row = 0;

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
    split_trace_reset(&split_trace_this);
    split_trace_reset(&split_trace_last);
    split_recent_left_age = 255;
    split_recent_right_age = 255;
    split_recent_display_w = 0;
    split_recent_display_h = 0;
    draw_offset_x = 0;
    draw_offset_y = 0;
    texture_window_value = 0;

    hres2 = 0;
    hres1 = 0;
    vres = 0;
    video_mode = 0;
    if (clear_vram)
        display_mode_programmed = 0;
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
    s_ws_fmv_frame_cache = 0xFFFFFFFFu;
    s_ws_fmv_cached = 0;
    s_d24_upload_x1 = 0;
    s_d24_present_hold = 0;
    s_d24_prev_disp_h = 0;
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

/* Netplay: present/finish_frame deferred from mid-psx_cyc_step to the next
 * psx_check_interrupts BB edge. MotK menu wait-loop (0x8006CDA0) was digesting
 * peers at different instr points (post-lw v0 vs post-slt v0=1) → cpu+ram fork
 * on idle sealed resim. Guest VBlank raise / LCF stay immediate. */
static int s_present_pending;
static int s_flushing_present;

void gpu_vblank_clear_deferred_present(void) {
    s_present_pending = 0;
    /* longjmp from flush_resume abandons the flush_present stack frame —
     * must drop the reentrancy guard or every later flush no-ops forever. */
    s_flushing_present = 0;
}

void gpu_vblank_arm_deferred_present(void) {
    /* Coalesce: at most one deferred present. Stacking (≥2) drained in one
     * flush as double finish_frame at the same guest cycle (MotK soak:
     * fin@N and fin@N+1 share dig/cyc → episode skew + clk/tim ±9). */
    if (s_present_pending < 1)
        s_present_pending = 1;
}

int gpu_vblank_present_pending(void) {
    return s_present_pending > 0;
}

void gpu_vblank_release_present_flush_guard(void) {
    s_flushing_present = 0;
}

void gpu_vblank_flush_present(void) {
    if (s_flushing_present || s_present_pending <= 0)
        return;
    /* Belt-and-suspenders with interrupts.c: never finish_frame inside the
     * exception handler (IEc-clear BB edges used to drain present_pending). */
    {
        extern int psx_get_in_exception(void);
        if (psx_get_in_exception())
            return;
    }
    /* Hold finish_frame while native memcard SIO is in flight. MotK needs
     * BB-edge commit for menu-wait determinism, but draining mid card
     * busy-wait wedges save/load on Ape Escape (empty starfield) and the
     * same class of titles. Keep s_present_pending; retry after card idle
     * (sio_hold_present_for_card has a stale escape). */
    {
        extern int psx_netplay_active(void);
        if (psx_netplay_active() && sio_hold_present_for_card())
            return;
    }
    /* MotK menu wait (0x8006CD54↔0x8006CDA0) and post-FMV overlay wait
     * (0x800768C8↔0x80076880): present ONLY at an explicit B edge. Never
     * present on an A edge (sticky B must not allow that). Non-wait edges
     * (FMV / cutover) must present even if sticky still names the wait loop. */
    {
        extern int psx_netplay_active(void);
        extern uint32_t psx_compiled_irq_resume_pc(void);
        extern uint32_t psx_last_irq_check_pc(void);
        extern uint32_t psx_netplay_rb_sticky_bb_pc(void);
        if (psx_netplay_active()) {
            const uint32_t wait_a = 0x8006CD54u;
            const uint32_t wait_b = 0x8006CDA0u;
            const uint32_t wait2_a = 0x800768C8u;
            const uint32_t wait2_b = 0x80076880u;
            uint32_t pc = psx_compiled_irq_resume_pc();
            uint32_t last = psx_last_irq_check_pc();
            uint32_t sticky = psx_netplay_rb_sticky_bb_pc();
            uint32_t edge = pc ? pc : last;
            if (edge == wait_a || edge == wait2_a)
                return;
            if (edge == wait_b || edge == wait2_b || edge != 0u) {
                /* B edge, or non-wait (FMV/cutover) — present; ignore sticky */
            } else if (sticky == wait_a || sticky == wait2_a) {
                return; /* latch cleared, sticky A — defer */
            }
            /* sticky B or unrelated/0 — present */
        }
    }
    s_flushing_present = 1;
    while (s_present_pending > 0) {
        s_present_pending--;
        if (vblank_callback)
            vblank_callback();
    }
    s_flushing_present = 0;
}

void gpu_vblank_tick(void) {
    lcf ^= 1;
    /* GPUSTAT.13 (interlace FIELD): on real hardware this alternates per
     * field while GP1(08h) vertical interlace is on, in antiphase with the
     * even/odd-lines bit 31 during active display. Modeled here at vblank
     * granularity: field = !LCF while interlaced, pinned to the legacy 0 in
     * progressive (GP1(08h) clears it) so titles see identical GPUSTAT.
     * First consumer
     * is OpenBIOS's shell waitVSync, which polls for the alternating
     * (bit31,bit13) = (1,0)/(0,1) pattern on real hardware — under
     * PCSX-Redux it never runs this path (pcsx_present() short-circuits to
     * the vblank-IRQ wait, and Redux holds bit13 constant at 1), so this
     * poll first became reachable in this runtime. */
    if (vertical_interlace)
        interlace_field = (lcf ^ 1) & 1;
    /* snapshot per-frame draw-offset-Y range for the strobe instrumentation */
    if (g_doff_cnt_this) {
        g_doff_min_last = g_doff_min_this;
        g_doff_max_last = g_doff_max_this;
        g_doff_cnt_last = g_doff_cnt_this;
    }
    g_doff_min_this = 0x7fffffff; g_doff_max_this = -0x7fffffff; g_doff_cnt_this = 0;
    split_trace_last = split_trace_this;
    if (split_trace_this.display_w != 0 &&
        (split_recent_display_w != split_trace_this.display_w ||
         split_recent_display_h != split_trace_this.display_h)) {
        split_recent_left_age = 255;
        split_recent_right_age = 255;
        split_recent_display_w = split_trace_this.display_w;
        split_recent_display_h = split_trace_this.display_h;
    }
    if (split_trace_this.left_seen) {
        split_recent_left_age = 0;
    } else if (split_recent_left_age < 255) {
        split_recent_left_age++;
    }
    if (split_trace_this.right_seen) {
        split_recent_right_age = 0;
    } else if (split_recent_right_age < 255) {
        split_recent_right_age++;
    }
    split_trace_reset(&split_trace_this);
    gpustat_poll_count = 0;
    /* Trusted package-selected plugins run on guest VBlank, independent of
     * host presentation, pacing, turbo, or skipped frames. */
    mod_runtime_on_vblank();
    /* Ape LOAD: RAM-only libcard waiter + idle-skip can starve sio_tick /
     * interrupt-check pumps; VBlank always runs. */
    {
        extern void sio_ape_card_unstick_pump(void);
        sio_ape_card_unstick_pump();
    }
    psx_irq_raise(0, 0); /* IRQ_VBLANK (gpu_vblank_tick) */
    if (!vblank_callback)
        return;
    {
        extern int psx_netplay_active(void);
        /* Offline selfcheck keeps immediate present: BB-edge defer +
         * post-IRQ flush reintroduces clk/tim/csv phase skew between warm
         * resim peers (selfcheck soak: many FAILs with d_cyc≠0). Netplay
         * defers — both peers share the same present contract from boot. */
        if (psx_netplay_active()) {
            /* Coalesce to one deferred present (see arm_deferred_present). */
            if (s_present_pending < 1)
                s_present_pending = 1;
        } else {
            vblank_callback();
        }
    }
}

const uint16_t* gpu_get_vram(void) {
    return vram;
}

static uint8_t gpu_vram_byte(uint32_t byte_x, uint32_t y) {
    uint16_t hw = vram[((y & 511u) * 1024u) + ((byte_x >> 1) & 1023u)];
    return (byte_x & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

/* Depth24: note/query/reset the CPU→VRAM upload span tracked above. Used to
 * hide trailing RGB columns when a movie blit doesn't fill the full CRTC
 * width — MotK's Star Wars crawl leaves ~8px of stale VRAM on the right.
 * Only FB-class A0s (w >= 256 halfwords) grow the span; texture uploads must
 * not collapse it. During present-hold, ignore updates entirely. */
static void depth24_note_upload(uint32_t x, uint32_t w) {
    if (!(display_depth & 1u) || w < 256u) return;
    if (s_d24_present_hold > 0) return;
    uint32_t x1 = x + w;
    if (x1 > 1024u) x1 = 1024u;
    if (x1 > s_d24_upload_x1) s_d24_upload_x1 = x1;
}

uint32_t gpu_depth24_rgb_limit(uint32_t display_x, uint32_t crtc_w) {
    if (!(display_depth & 1u) || crtc_w == 0u)
        return crtc_w;
    /* No uploads yet → treat as uncovered (present blanks until first blit). */
    if (s_d24_upload_x1 == 0u)
        return 0u;
    uint32_t dx = display_x & 1023u;
    if (s_d24_upload_x1 <= dx) return 0u;
    uint32_t hw = s_d24_upload_x1 - dx;
    uint32_t rgb = (hw * 2u) / 3u;
    if (rgb == 0u) return 0u;
    if (rgb >= crtc_w) return crtc_w;
    return rgb;
}

void gpu_depth24_upload_span_reset(void) {
    s_d24_upload_x1 = 0;
}

int gpu_depth24_present_hold_tick(void) {
    if (s_d24_present_hold <= 0) return 0;
    s_d24_present_hold--;
    return 1;
}

void gpu_depth24_on_savestate_loaded(void) {
    /* Hold skips Swap — after restore we want the restored VRAM visible now.
     * Upload span / prev_h were restored from the GPU snap. */
    s_d24_present_hold = 0;
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
        /* No horizontal wrap for 24-bit DAC scanout: bytes past the 2048-byte
         * VRAM row are blank on hardware / accurate emulators, not sheared
         * from X=0. Wrapping here produced a flickering right-edge strip. */
        if (byte_x + 2u >= 2048u) {
            *r = *g = *b = 0;
            return;
        }
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

/* Batch depth24 (FMV) scanline → ARGB. Same semantics as calling
 * gpu_display_pixel_argb(di, x, y) for x in [0, count) (byte-identical
 * output, including the black-fill past the 2048-byte VRAM row), but hoists
 * the per-row invariants (vy, base_byte_x, the "how many pixels are past the
 * row edge" test) out of the per-pixel path instead of recomputing them
 * `count` times and paying three gpu_vram_byte() calls per pixel. The
 * per-pixel path was the dominant present-side cost of FMV frames (3x the
 * VRAM touches of the 16-bit path, done as a function-call chain instead of
 * a straight-line loop). Still scalar C — no host-endianness assumptions,
 * same byte-order shifts as gpu_vram_byte. */
void gpu_depth24_present_row(const GpuDisplayInfo* di, uint32_t y, uint32_t* out,
                             uint32_t count) {
    uint32_t vy = (di->display_y + y) & 511u;
    uint32_t base_byte_x = (di->display_x & 1023u) * 2u;
    const uint16_t* row = vram + (size_t)vy * 1024u;
    uint32_t valid = 0u;
    uint32_t x;

    if (base_byte_x <= 2045u)
        valid = (2045u - base_byte_x) / 3u + 1u;
    if (valid > count)
        valid = count;

    for (x = 0; x < valid; x++) {
        uint32_t byte_x = base_byte_x + x * 3u;
        uint32_t bx1 = byte_x + 1u;
        uint32_t bx2 = byte_x + 2u;
        uint32_t hw0 = row[byte_x >> 1];
        uint32_t hw1 = row[bx1 >> 1];
        uint32_t hw2 = row[bx2 >> 1];
        uint8_t r = (byte_x & 1u) ? (uint8_t)(hw0 >> 8) : (uint8_t)hw0;
        uint8_t g = (bx1 & 1u) ? (uint8_t)(hw1 >> 8) : (uint8_t)hw1;
        uint8_t b = (bx2 & 1u) ? (uint8_t)(hw2 >> 8) : (uint8_t)hw2;
        out[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    for (; x < count; x++)
        out[x] = 0xFF000000u;
}

int gpu_display_is_depth24(void) {
    return (int)(display_depth & 1u);
}

int gpu_display_is_pal(void) {
    return (int)(video_mode & 1u);
}

int gpu_display_mode_is_programmed(void) {
    return display_mode_programmed ? 1 : 0;
}

/* Opt-in Nx CRTC (mod plugins). 1 = stock; 2 = half-period VBlank. */
static uint32_t s_crtc_refresh_multiplier = 1u;
/* 0 = derive from GP1 video_mode; else absolute period before Nx divide. */
static uint32_t s_crtc_period_override = 0u;

void gpu_set_crtc_refresh_multiplier(uint32_t multiplier) {
    if (multiplier < 1u)
        multiplier = 1u;
    if (multiplier > 8u)
        multiplier = 8u;
    s_crtc_refresh_multiplier = multiplier;
}

uint32_t gpu_get_crtc_refresh_multiplier(void) {
    return s_crtc_refresh_multiplier ? s_crtc_refresh_multiplier : 1u;
}

void gpu_set_crtc_vblank_period_override(uint32_t cycles) {
    s_crtc_period_override = cycles;
}

uint32_t gpu_get_crtc_vblank_period_override(void) {
    return s_crtc_period_override;
}

uint32_t gpu_vblank_period_cycles(void) {
    const uint32_t base = s_crtc_period_override
        ? s_crtc_period_override
        : (video_mode ? PSX_VBLANK_CYCLES_PAL : PSX_VBLANK_CYCLES_NTSC);
    const uint32_t mult = gpu_get_crtc_refresh_multiplier();
    return base / mult;
}

void gpu_get_display_info(GpuDisplayInfo* out) {
    out->display_x = display_area_x;
    out->display_y = display_area_y;
    out->depth24   = (int)(display_depth & 1u);
    out->disabled  = (int)display_disabled;

    /* Dot-clock divider from GP1(08h) hres (psx-spx / DuckStation). */
    uint32_t cycles;
    uint32_t mode_w;
    if (hres2) {
        cycles = 7;
        mode_w = 368;
    } else {
        static const uint32_t cyc_lut[4] = { 10, 8, 5, 4 };
        static const uint32_t hres_lut[4] = { 256, 320, 512, 640 };
        cycles = cyc_lut[hres1 & 3];
        mode_w = hres_lut[hres1 & 3];
    }

    /* Visible pixel width from GP1(06h): (((X2-X1)/cycles)+2) & ~3 (psx-spx).
     * Fall back to the mode width when the range is unset/degenerate. */
    uint32_t dots = (h_display_x2 > h_display_x1) ? (h_display_x2 - h_display_x1) : 0;
    uint32_t w = mode_w;
    if (dots > 0u && cycles > 0u) {
        w = ((dots / cycles) + 2u) & ~3u;
        if (w == 0u) w = 4u;
    }

    /* DuckStation GetFullDisplayResolution: clamp Y1/Y2 to the broadcast
     * active region before taking the difference. Unclamped Y2 past the
     * active end (common overscan programming) includes a flickering junk
     * line at the bottom of present that DuckStation crops away. */
    const int ymin = video_mode ? 20 : 16;  /* PAL : NTSC */
    const int ymax = video_mode ? 308 : 256;
    int y1 = (int)v_display_y1;
    int y2 = (int)v_display_y2;
    if (y1 < ymin) y1 = ymin;
    if (y1 > ymax) y1 = ymax;
    if (y2 < ymin) y2 = ymin;
    if (y2 > ymax) y2 = ymax;
    uint32_t h = (y2 > y1) ? (uint32_t)(y2 - y1) : 240u;
    if (vres) h *= 2; /* 480i */

    /* 24-bit scanout uses the same CRTC pixel width as 15-bit (DuckStation /
     * Beetle: coordinates stay 16-bit-based; W RGB occupies W*3/2 halfwords).
     * MotK FMV: GP1(06h) yields 512; the logo is centered in that RGB line.
     * A blanket (W*2)/3 (512→341) left-shifts the frame and clips the right
     * of the video — do not reintroduce it. Right-edge junk is a separate
     * present/filter issue, not a reason to shrink CRTC width. */

    /* Clamp to sane maximums */
    if (w > 640) w = 640;
    if (h > 512) h = 512;

    out->width  = w;
    out->height = h;
}

/* Debug accessors for GP1 display-range / mode (TCP gpu_state). */
void gpu_get_crtc_debug(uint32_t *x1, uint32_t *x2, uint32_t *y1, uint32_t *y2,
                        uint32_t *hres1_out, uint32_t *hres2_out) {
    if (x1) *x1 = h_display_x1;
    if (x2) *x2 = h_display_x2;
    if (y1) *y1 = v_display_y1;
    if (y2) *y2 = v_display_y2;
    if (hres1_out) *hres1_out = hres1;
    if (hres2_out) *hres2_out = hres2;
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

extern int gte_geometry_correction_enabled(void);
static int s_texture_correction_enabled = 0;
extern int gte_precision_load_word(uint32_t addr, uint32_t packed,
                                   int32_t *x16, int32_t *y16, uint16_t *z);

/* The PGXP dataflow engine feeds the two correction features AND the
 * widescreen precision consumers (precise NCLIP, clamp rescue). Arm it while
 * any of them is on. Every setter re-derives through here so init order and
 * runtime toggles (debug server) can never leave the engine stale. */
void gpu_pgxp_rederive_enable(void) {
    pgxp_set_enabled(s_texture_correction_enabled ||
                     gte_geometry_correction_enabled() ||
                     ws_precise_nclip_cfg);
    /* ALU/MULDIV hooks feed only the correction consumers' dataflow chains
     * (and tier-2 cpu_mode, gated inside pgxp.cpp); an NCLIP-only arm skips
     * their per-instruction bodies (LOAD/STORE/COP2 carry the SXY chain). */
    pgxp_set_full_hooks(s_texture_correction_enabled ||
                        gte_geometry_correction_enabled());
}

void gpu_texture_correction_set(int enabled) {
    s_texture_correction_enabled = enabled ? 1 : 0;
    gpu_pgxp_rederive_enable();
}

int gpu_texture_correction_enabled(void) {
    return s_texture_correction_enabled;
}

uint32_t gpu_texture_correction_hits(void) {
    return sw_perspective_triangle_count();
}

/* PGXP correction is decided once for the complete GP0 source primitive. A
 * quad therefore resolves all four packet vertices before either of its two
 * renderer triangles is submitted. The renderer-facing helpers below may only
 * arm from this immutable decision; they never resolve or repair one vertex in
 * isolation. */
#define PGXP_PRIMITIVE_MAX_VERTICES 4
typedef struct {
    int count;
    int indices[PGXP_PRIMITIVE_MAX_VERTICES];

    int position_requested;
    int position_armed;
    int32_t raw_x[PGXP_PRIMITIVE_MAX_VERTICES];
    int32_t raw_y[PGXP_PRIMITIVE_MAX_VERTICES];
    int32_t precise_x[PGXP_PRIMITIVE_MAX_VERTICES];
    int32_t precise_y[PGXP_PRIMITIVE_MAX_VERTICES];
    uint8_t cls[PGXP_PRIMITIVE_MAX_VERTICES];

    int depth_requested;
    int depth_armed;
    int depth_reject_reason; /* 0 = none, 1 = no source, 2 = incomplete Z */
    int depth_miss_slot;
    uint8_t depth_miss_why;
    uint16_t z[PGXP_PRIMITIVE_MAX_VERTICES];
    float q[PGXP_PRIMITIVE_MAX_VERTICES];
} PgxpPrimitiveDecision;

static struct {
    uint64_t primitives_armed;
    uint64_t primitives_rejected_incomplete_position;
    uint64_t primitives_rejected_incomplete_depth;
    uint64_t primitives_mixed_position;
    uint64_t vertices_promoted_from_native;
    uint64_t vertices_demoted_to_native;
    uint64_t vertex_cache_overflow;
} s_pgxp_primitive_stats;

static PgxpVertexCache s_pgxp_vertex_cache;

void gpu_pgxp_primitive_stats(uint64_t *primitives_armed,
                              uint64_t *rejected_incomplete_position,
                              uint64_t *rejected_incomplete_depth,
                              uint64_t *mixed_position,
                              uint64_t *promoted_from_native,
                              uint64_t *demoted_to_native,
                              uint64_t *cache_overflow) {
    if (primitives_armed)
        *primitives_armed = s_pgxp_primitive_stats.primitives_armed;
    if (rejected_incomplete_position)
        *rejected_incomplete_position =
            s_pgxp_primitive_stats.primitives_rejected_incomplete_position;
    if (rejected_incomplete_depth)
        *rejected_incomplete_depth =
            s_pgxp_primitive_stats.primitives_rejected_incomplete_depth;
    if (mixed_position)
        *mixed_position = s_pgxp_primitive_stats.primitives_mixed_position;
    if (promoted_from_native)
        *promoted_from_native =
            s_pgxp_primitive_stats.vertices_promoted_from_native;
    if (demoted_to_native)
        *demoted_to_native =
            s_pgxp_primitive_stats.vertices_demoted_to_native;
    if (cache_overflow)
        *cache_overflow = s_pgxp_primitive_stats.vertex_cache_overflow;
}

static void prepare_pgxp_primitive(PgxpPrimitiveDecision *decision,
                                   const int *indices, int count,
                                   int textured, const int32_t *vx,
                                   const int32_t *vy) {
    memset(decision, 0, sizeof(*decision));
    decision->count = count;
    for (int i = 0; i < count; i++)
        decision->indices[i] = indices[i];

    decision->position_requested = gte_geometry_correction_enabled();
    if (decision->position_requested) {
        for (int i = 0; i < count; i++) {
            const int index = indices[i];
            const uint32_t word = gp0_cmd_buf[index];
            const uint32_t addr = (gp0_cmd_source_addr == 0xFFFFFFFFu)
                                      ? 0xFFFFFFFFu
                                      : gp0_cmd_source_addr +
                                            (uint32_t)index * 4u;
            uint16_t sz;
            parse_vertex(word, &decision->raw_x[i], &decision->raw_y[i]);
            const int src = pgxp_get_precise_vertex(
                addr, word, decision->raw_x[i], decision->raw_y[i],
                &decision->precise_x[i], &decision->precise_y[i], &sz);
            decision->cls[i] =
                (src == PGXP_SRC_NATIVE && addr == 0xFFFFFFFFu)
                    ? 3u
                    : (uint8_t)src;
        }
        int canonical_precise_count = 0;
        for (int i = 0; i < count; i++) {
            const int candidate_precise =
                decision->cls[i] != PGXP_SRC_NATIVE && decision->cls[i] != 3u;
            int32_t candidate_x16 =
                (int32_t)((int64_t)vx[i] * 65536);
            int32_t candidate_y16 =
                (int32_t)((int64_t)vy[i] * 65536);
            if (candidate_precise) {
                candidate_x16 = (int32_t)(
                    (int64_t)decision->precise_x[i] +
                    (int64_t)(vx[i] - decision->raw_x[i]) * 65536);
                candidate_y16 = (int32_t)(
                    (int64_t)decision->precise_y[i] +
                    (int64_t)(vy[i] - decision->raw_y[i]) * 65536);
            }
            int canonical_precise = 0;
            const int cache_result = pgxp_vertex_cache_resolve(
                &s_pgxp_vertex_cache, s_frame_count,
                vx[i], vy[i], candidate_precise,
                candidate_x16, candidate_y16, &decision->precise_x[i],
                &decision->precise_y[i], &canonical_precise);
            if (cache_result == PGXP_VERTEX_CACHE_OVERFLOW)
                s_pgxp_primitive_stats.vertex_cache_overflow++;
            if (canonical_precise) canonical_precise_count++;
            if (!candidate_precise && canonical_precise)
                s_pgxp_primitive_stats.vertices_promoted_from_native++;
            if (candidate_precise && !canonical_precise)
                s_pgxp_primitive_stats.vertices_demoted_to_native++;
        }
        decision->position_armed = canonical_precise_count != 0;
        if (canonical_precise_count != count)
            s_pgxp_primitive_stats
                .primitives_rejected_incomplete_position++;
        if (canonical_precise_count != 0 &&
            canonical_precise_count != count)
            s_pgxp_primitive_stats.primitives_mixed_position++;
    }

    decision->depth_requested = textured && s_texture_correction_enabled;
    if (decision->depth_requested) {
        if (gp0_cmd_source_addr == 0xFFFFFFFFu) {
            decision->depth_reject_reason = 1;
            s_pgxp_primitive_stats.primitives_rejected_incomplete_depth++;
        } else {
            int depth_count = 0;
            for (int i = 0; i < count; i++) {
                const int index = indices[i];
                const uint32_t addr = psx_ram_map_read(
                    (gp0_cmd_source_addr + (uint32_t)index * 4u) &
                    0x1FFFFFFFu) & ~3u;
                const int have_depth = gte_precision_load_word(
                    addr, gp0_cmd_buf[index], NULL, NULL, &decision->z[i]) &&
                    decision->z[i] != 0;
                if (have_depth) {
                    depth_count++;
                } else if (decision->depth_reject_reason == 0) {
                    decision->depth_reject_reason = 2;
                    decision->depth_miss_slot = i;
                    decision->depth_miss_why = (uint8_t)
                        pgxp_debug_shadow_class(addr, gp0_cmd_buf[index]);
                }
            }
            decision->depth_armed = (depth_count == count);
            if (decision->depth_armed) {
                float qmax = 0.0f;
                for (int i = 0; i < count; i++) {
                    decision->q[i] = 1.0f / (float)decision->z[i];
                    if (decision->q[i] > qmax) qmax = decision->q[i];
                }
                for (int i = 0; i < count; i++)
                    decision->q[i] /= qmax;
            } else {
                s_pgxp_primitive_stats
                    .primitives_rejected_incomplete_depth++;
            }
        }
    }

    if (decision->position_armed || decision->depth_armed)
        s_pgxp_primitive_stats.primitives_armed++;
}

/* PGXP resolution census (G1 coverage gap). Double-buffered by frame parity
 * so the debug thread can read the completed frame while this one builds.
 * Only triangles from imperfect source decisions get entries; totals give the
 * denominator. */
#define PGXP_CENSUS_CAP 8192
static PgxpCensusEnt s_pgxp_census[2][PGXP_CENSUS_CAP];
static uint32_t s_pgxp_census_n[2], s_pgxp_census_frame[2];
static uint32_t s_pgxp_census_tris[2], s_pgxp_census_clean[2];

int gpu_pgxp_census_dump(uint32_t *frame_out, uint32_t *tris, uint32_t *clean,
                         PgxpCensusEnt *out, int max_out) {
    /* newest COMPLETE frame: never the one still building, else the fresher */
    uint32_t cur = (uint32_t)s_frame_count;
    int slot;
    if (s_pgxp_census_frame[0] == cur)      slot = 1;
    else if (s_pgxp_census_frame[1] == cur) slot = 0;
    else slot = (s_pgxp_census_frame[0] > s_pgxp_census_frame[1]) ? 0 : 1;
    if (frame_out) *frame_out = s_pgxp_census_frame[slot];
    if (tris)      *tris      = s_pgxp_census_tris[slot];
    if (clean)     *clean     = s_pgxp_census_clean[slot];
    int n = (int)s_pgxp_census_n[slot];
    if (n > max_out) n = max_out;
    for (int i = 0; i < n; i++) out[i] = s_pgxp_census[slot][i];
    return n;
}

static void prepare_precise_triangle(const PgxpPrimitiveDecision *decision,
                                     int v0, int v1, int v2,
                                     const int32_t vx[3],
                                     const int32_t vy[3]) {
    const int slots[3] = { v0, v1, v2 };
    gr_set_perspective_triangle(0, 0.0f, 0.0f, 0.0f);
    if (!decision->position_requested) {
        /* Precise NCLIP is a GTE-side sign audit. It must never smuggle the
         * unclamped +/-4096 projection into a GPU primitive: saturation-rail
         * replacement is outside PGXP's same-cell contract and is a direct
         * route to full-screen polygon spikes. */
        gr_set_precise_triangle(0, 0,0, 0,0, 0,0);
        return;
    }
    {
        uint32_t f = (uint32_t)s_frame_count;
        int slot = (int)(f & 1);
        if (s_pgxp_census_frame[slot] != f) {
            s_pgxp_census_frame[slot] = f;
            s_pgxp_census_n[slot] = 0;
            s_pgxp_census_tris[slot] = 0;
            s_pgxp_census_clean[slot] = 0;
        }
        s_pgxp_census_tris[slot]++;
        if (decision->position_armed &&
            decision->cls[v0] == 2 && decision->cls[v1] == 2 &&
            decision->cls[v2] == 2) {
            s_pgxp_census_clean[slot]++;
        } else if (s_pgxp_census_n[slot] < PGXP_CENSUS_CAP) {
            PgxpCensusEnt *e = &s_pgxp_census[slot][s_pgxp_census_n[slot]++];
            int32_t x0 = vx[0], x1 = vx[0], y0 = vy[0], y1 = vy[0];
            for (int i = 1; i < 3; i++) {
                if (vx[i] < x0) x0 = vx[i];
                if (vx[i] > x1) x1 = vx[i];
                if (vy[i] < y0) y0 = vy[i];
                if (vy[i] > y1) y1 = vy[i];
            }
            e->x0 = (int16_t)x0; e->y0 = (int16_t)y0;
            e->x1 = (int16_t)x1; e->y1 = (int16_t)y1;
            e->cls[0] = decision->cls[v0];
            e->cls[1] = decision->cls[v1];
            e->cls[2] = decision->cls[v2];
            e->_pad = 0;
            e->src = gp0_cmd_source_addr;
            e->frame = f;
        }
    }
    if (!decision->position_armed) {
        gr_set_precise_triangle(0, 0,0, 0,0, 0,0);
        return;
    }
    int32_t fx[3], fy[3];
    for (int i = 0; i < 3; i++) {
        const int vertex = slots[i];
        fx[i] = decision->precise_x[vertex];
        fy[i] = decision->precise_y[vertex];
    }
    gr_set_precise_triangle(1, fx[0],fy[0], fx[1],fy[1], fx[2],fy[2]);
}

/* Arming rate for perspective-correct UVs, per condition.
 *
 * perspective_triangles alone cannot answer "how much texture warp is left",
 * because it has no denominator: comparing it to gp0_draw mixes in untextured
 * mono and gouraud primitives that are correctly never armed, and comparing it
 * to a vertex-lookup count divided by three is the same error. `attempts`
 * counts exactly the textured triangles that reach this predicate, so
 * armed/attempts IS the perspective coverage, and the three reject counters
 * say which condition is spending it. Diagnostic only. */
static struct {
    uint64_t attempts;      /* textured triangles submitted                 */
    uint64_t armed;         /* got perspective-correct UVs                  */
    uint64_t no_correction; /* texture correction off                       */
    uint64_t no_source;     /* CPU-built primitive, no packet address       */
    uint64_t no_depth;      /* a vertex had no recorded Z, or Z == 0        */
} s_texcorr;

void gpu_texture_correction_stats(uint64_t *attempts, uint64_t *armed,
                                  uint64_t *no_correction,
                                  uint64_t *no_source, uint64_t *no_depth) {
    if (attempts)      *attempts      = s_texcorr.attempts;
    if (armed)         *armed         = s_texcorr.armed;
    if (no_correction) *no_correction = s_texcorr.no_correction;
    if (no_source)     *no_source     = s_texcorr.no_source;
    if (no_depth)      *no_depth      = s_texcorr.no_depth;
}

/* Enable perspective UVs only when every position word came from an exact
 * SWC2 projection store at that same DMA packet address. This preserves the
 * association through ordering-table reordering and rejects CPU-built UI. */
static PgxpCensusEnt s_pgxp_texcensus[2][PGXP_CENSUS_CAP];
static uint32_t s_pgxp_texcensus_n[2], s_pgxp_texcensus_frame[2];

/* Record a texture-UV correction miss with the prim's rough screen bbox
 * (raw packet coords + draw offset; close enough to localize the surface). */
static void pgxp_texcensus_note(const int *indices, uint8_t reason, uint8_t vtx,
                                uint8_t why) {
    uint32_t f = (uint32_t)s_frame_count;
    int slot = (int)(f & 1);
    if (s_pgxp_texcensus_frame[slot] != f) {
        s_pgxp_texcensus_frame[slot] = f;
        s_pgxp_texcensus_n[slot] = 0;
    }
    if (s_pgxp_texcensus_n[slot] >= PGXP_CENSUS_CAP) return;
    PgxpCensusEnt *e = &s_pgxp_texcensus[slot][s_pgxp_texcensus_n[slot]++];
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < 3; i++) {
        int32_t rx, ry;
        parse_vertex(gp0_cmd_buf[indices[i]], &rx, &ry);
        rx += draw_offset_x; ry += draw_offset_y;
        if (i == 0) { x0 = x1 = rx; y0 = y1 = ry; }
        else {
            if (rx < x0) x0 = rx;
            if (rx > x1) x1 = rx;
            if (ry < y0) y0 = ry;
            if (ry > y1) y1 = ry;
        }
    }
    e->x0 = (int16_t)x0; e->y0 = (int16_t)y0;
    e->x1 = (int16_t)x1; e->y1 = (int16_t)y1;
    e->cls[0] = reason; e->cls[1] = vtx; e->cls[2] = why; e->_pad = 0;
    e->src = gp0_cmd_source_addr;
    e->frame = f;
}

int gpu_pgxp_texcensus_dump(uint32_t *frame_out, PgxpCensusEnt *out, int max_out) {
    uint32_t cur = (uint32_t)s_frame_count;
    int slot; /* prefer the newest COMPLETE frame, not merely opposite parity */
    if (s_pgxp_texcensus_frame[0] == cur)      slot = 1;
    else if (s_pgxp_texcensus_frame[1] == cur) slot = 0;
    else slot = (s_pgxp_texcensus_frame[0] > s_pgxp_texcensus_frame[1]) ? 0 : 1;
    if (frame_out) *frame_out = s_pgxp_texcensus_frame[slot];
    int n = (int)s_pgxp_texcensus_n[slot];
    if (n > max_out) n = max_out;
    for (int i = 0; i < n; i++) out[i] = s_pgxp_texcensus[slot][i];
    return n;
}

static void prepare_texture_triangle(const PgxpPrimitiveDecision *decision,
                                     int v0, int v1, int v2) {
    const int slots[3] = { v0, v1, v2 };
    int indices[3];
    for (int i = 0; i < 3; i++)
        indices[i] = decision->indices[slots[i]];

    gr_set_perspective_triangle(0, 0.0f, 0.0f, 0.0f);
    gr_set_depth_triangle(0, 0.0f, 0.0f, 0.0f);
    s_texcorr.attempts++;
    if (!decision->depth_requested) {
        s_texcorr.no_correction++;
        return;
    }
    if (decision->depth_reject_reason == 1) {
        s_texcorr.no_source++;
        pgxp_texcensus_note(indices, 0u, 0u, 0u);
        return;
    }
    if (!decision->depth_armed) {
        s_texcorr.no_depth++;
        pgxp_texcensus_note(
            indices, 1u,
            (uint8_t)decision->indices[decision->depth_miss_slot],
            decision->depth_miss_why);
        return;
    }
    s_texcorr.armed++;
    gr_set_perspective_triangle(1, decision->q[v0], decision->q[v1],
                                decision->q[v2]);
    /* q was normalized once across the source primitive. Absolute GTE screen Z
     * remains separate so cross-primitive depth ordering is meaningful. */
    gr_set_depth_triangle(1, (float)decision->z[v0],
                          (float)decision->z[v1],
                          (float)decision->z[v2]);
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
    gpu_vram_dirty_mark_row(vy);
}

/* Inclusive draw-area reject (same predicate as raster_pixel / hardware clip).
 *
 * MotK inter-movie / title / char-select OT drains often set GP0(E3/E4) to
 * (0,0)-(0,0) then submit thousands of 1x1 dots and shaded quads. Real GPU
 * clips those for free; our GL path was building two triangles per clipped
 * prim (gpu_share ~0.9, host FPS ~5–10). Skip the host rasterizer when the
 * post-offset bbox cannot touch the draw area. Side effects that must still
 * run (texpage latch, oversize reject) happen before these checks.
 *
 * Native-wide is intentionally different in X: the mirror renderer translates
 * canonical framebuffer coordinates by the live reveal offset and scissors to
 * the FULL wide surface, not GP0(E3/E4)'s 4:3 X range. Commit 31015ce originally
 * compared every primitive only with the guest draw area here, rejecting the
 * margin geometry before the mirror renderer could see it. Match the mirror's
 * exact X extent while its framebuffer target is active; Y remains the guest
 * draw area because native-wide does not extend vertically. At 4:3, in FMV/
 * menus, in squash mode, and for offscreen texture targets, margin is zero and
 * this remains the original fast reject byte-for-byte.
 *
 * OpokXeno independently identified the same host-side regression on Xenogears
 * and contributed the original generalized fix in psxrecomp PR #73:
 * https://github.com/mstan/psxrecomp/pull/73
 * Keep that credit with this guarded framebuffer-target variant. */
static inline int32_t draw_area_wide_x_margin(void) {
    int local_base = 0;
    if (!ws_native_wide_active()) return 0;
    if (ws_local_viewport_draw_target(&local_base)) return (int32_t)ws_nw_offset();
    if (ws_display_viewport_draw_target(NULL)) return (int32_t)ws_nw_offset();
    if (!ws_is_fb_base(draw_area_left)) return 0;
    return (int32_t)ws_nw_offset();
}

static inline void draw_area_host_x_bounds(int32_t *left, int32_t *right) {
    int32_t margin = draw_area_wide_x_margin();
    *left  = (int32_t)draw_area_left;
    *right = (int32_t)draw_area_right;
    if (margin > 0) {
        /* Use the union of the guest draw area and the widescreen mirror.
         * Wider staging areas may share the framebuffer X origin; clamping them
         * to the mirror width would drop valid canonical VRAM writes. */
        int32_t wide_left  = (int32_t)draw_area_left - margin;
        int32_t wide_right = (int32_t)draw_area_left + (int32_t)ws_disp_w() + margin - 1;
        if (wide_left  < *left)  *left  = wide_left;
        if (wide_right > *right) *right = wide_right;
    }
}

static inline int draw_area_out_point(int32_t x, int32_t y) {
    int32_t left, right;
    draw_area_host_x_bounds(&left, &right);
    return x < left || x > right
        || y < (int32_t)draw_area_top  || y > (int32_t)draw_area_bottom;
}

static inline int draw_area_out_bbox(const int32_t *vx, const int32_t *vy, int n) {
    int32_t minx = vx[0], maxx = vx[0], miny = vy[0], maxy = vy[0];
    for (int i = 1; i < n; i++) {
        if (vx[i] < minx) minx = vx[i];
        if (vx[i] > maxx) maxx = vx[i];
        if (vy[i] < miny) miny = vy[i];
        if (vy[i] > maxy) maxy = vy[i];
    }
    int32_t left, right;
    draw_area_host_x_bounds(&left, &right);
    return maxx < left || minx > right
        || maxy < (int32_t)draw_area_top  || miny > (int32_t)draw_area_bottom;
}

static inline int draw_area_out_rect(int32_t x, int32_t y, int w, int h) {
    if (w <= 0 || h <= 0) return 1;
    int32_t left, right;
    draw_area_host_x_bounds(&left, &right);
    return (x + w - 1) < left
        || x > right
        || (y + h - 1) < (int32_t)draw_area_top
        || y > (int32_t)draw_area_bottom;
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
    if (psx_gpu_triangle_oversize(vx, vy, 0, 1, 2)) return;
    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_bbox(vx, vy, 3)) return;
    const int pgxp_indices[3] = { 1, 2, 3 };
    PgxpPrimitiveDecision pgxp;
    prepare_pgxp_primitive(&pgxp, pgxp_indices, 3, 0, vx, vy);
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    prepare_precise_triangle(&pgxp, 0, 1, 2,
                             vx, vy);
    gr_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
}

/* Execute mono quad (GP0 0x28-0x2B) — two triangles: (0,1,2) and (2,1,3) */
static void gp0_exec_mono_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t vx[4], vy[4];
    for (int i = 0; i < 4; i++)
        parse_vertex(gp0_cmd_buf[1 + i], &vx[i], &vy[i]);
    int rej_a = psx_gpu_triangle_oversize(vx, vy, 0, 1, 2);
    int rej_b = psx_gpu_triangle_oversize(vx, vy, 2, 1, 3);
    if (rej_a && rej_b) return;
    /* Full-screen filters are commonly encoded as an axis-aligned quad. Drawing
     * a semi-transparent quad as two independent triangles blends their shared
     * diagonal twice; render the equivalent rectangle once to avoid that seam.
     * The full-screen helper also grows the native 320-wide filter across the
     * sidecar surface in native-wide mode. Ordinary world quads are unchanged. */
    if (vx[0] == vx[2] && vx[1] == vx[3] &&
        vy[0] == vy[1] && vy[2] == vy[3] &&
        vx[1] > vx[0] && vy[2] > vy[0] &&
        vx[0] <= 0 && vx[1] >= ws_disp_w() &&
        vy[0] <= 0 && vy[2] >= ws_disp_h() &&
        !gte_geometry_correction_enabled()) {
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
    if (draw_area_out_bbox(vx, vy, 4)) return;
    const int pgxp_indices[4] = { 1, 2, 3, 4 };
    PgxpPrimitiveDecision pgxp;
    prepare_pgxp_primitive(&pgxp, pgxp_indices, 4, 0, vx, vy);
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    /* Semi axis-aligned mono quads (UI boxes/borders): one rect, not two tris.
     * Thin semi borders (e.g. CTR name-entry OT-1144 teal 3×H) otherwise double-
     * blend their shared diagonal — nearly the whole strip — and overpaint 3D. */
    if (semi_trans && !pgxp.position_armed && ws_axis_aligned_quad(vx, vy)) {
        int32_t min_x = vx[0], max_x = vx[0], min_y = vy[0], max_y = vy[0];
        for (int i = 1; i < 4; i++) {
            if (vx[i] < min_x) min_x = vx[i];
            if (vx[i] > max_x) max_x = vx[i];
            if (vy[i] < min_y) min_y = vy[i];
            if (vy[i] > max_y) max_y = vy[i];
        }
        int w = (int)(max_x - min_x);
        int h = (int)(max_y - min_y);
        if (w > 0 && h > 0) {
            gr_draw_flat_rect(min_x, min_y, w, h, color);
            return;
        }
    }
    if (!rej_a) {
        int32_t tx[3] = { vx[0], vx[1], vx[2] };
        int32_t ty[3] = { vy[0], vy[1], vy[2] };
        prepare_precise_triangle(&pgxp, 0, 1, 2, tx, ty);
        gr_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
    }
    if (!rej_b) {
        int32_t tx[3] = { vx[2], vx[1], vx[3] };
        int32_t ty[3] = { vy[2], vy[1], vy[3] };
        prepare_precise_triangle(&pgxp, 2, 1, 3, tx, ty);
        gr_draw_flat_triangle(vx[2], vy[2], vx[1], vy[1], vx[3], vy[3], color);
    }
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
    if (psx_gpu_triangle_oversize(vx, vy, 0, 1, 2)) return;
    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_bbox(vx, vy, 3)) return;
    const int pgxp_indices[3] = { 1, 3, 5 };
    PgxpPrimitiveDecision pgxp;
    prepare_pgxp_primitive(&pgxp, pgxp_indices, 3, 0, vx, vy);
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    prepare_precise_triangle(&pgxp, 0, 1, 2,
                             vx, vy);
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
    int rej_a = psx_gpu_triangle_oversize(vx, vy, 0, 1, 2);
    int rej_b = psx_gpu_triangle_oversize(vx, vy, 2, 1, 3);
    if (rej_a && rej_b) return;
    ws_nw_backdrop_stretch_quad(vx, vy);   /* full-frame 2D backdrop stretch (sky gradient; no-op else) */
    ws_nw_hud_shift_vertices(vx, 4);
    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_bbox(vx, vy, 4)) return;
    const int pgxp_indices[4] = { 1, 3, 5, 7 };
    PgxpPrimitiveDecision pgxp;
    prepare_pgxp_primitive(&pgxp, pgxp_indices, 4, 0, vx, vy);
    /* Capture vertex data when armed. */
    if (sq_cap_armed && sq_cap_count < SQ_CAP_MAX) {
        GpuSqCapEntry* e = &sq_cap_buf[sq_cap_count++];
        for (int i = 0; i < 4; i++) {
            e->vx[i] = vx[i]; e->vy[i] = vy[i];
            e->color[i] = gp0_cmd_buf[i * 2] & 0xFFFFFFu;
        }
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    if (!rej_a) {
        int32_t tx[3] = { vx[0], vx[1], vx[2] };
        int32_t ty[3] = { vy[0], vy[1], vy[2] };
        prepare_precise_triangle(&pgxp, 0, 1, 2, tx, ty);
        gr_draw_gouraud_triangle(vx[0], vy[0], c[0],
                                 vx[1], vy[1], c[1],
                                 vx[2], vy[2], c[2]);
    }
    if (!rej_b) {
        int32_t tx[3] = { vx[2], vx[1], vx[3] };
        int32_t ty[3] = { vy[2], vy[1], vy[3] };
        prepare_precise_triangle(&pgxp, 2, 1, 3, tx, ty);
        gr_draw_gouraud_triangle(vx[2], vy[2], c[2],
                                 vx[1], vy[1], c[1],
                                 vx[3], vy[3], c[3]);
    }
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
    set_tpage_from_poly(tpage_word);   /* latches even for size-rejected polys */
    if (psx_gpu_triangle_oversize(vx, vy, 0, 1, 2)) return;

    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_bbox(vx, vy, 3)) return;

    const int pgxp_indices[3] = { 1, 3, 5 };
    PgxpPrimitiveDecision pgxp;
    prepare_pgxp_primitive(&pgxp, pgxp_indices, 3, 1, vx, vy);
    setup_textured_draw(color24, semi_trans, raw_texture);
    prepare_precise_triangle(&pgxp, 0, 1, 2,
                             vx, vy);
    prepare_texture_triangle(&pgxp, 0, 1, 2);
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
    set_tpage_from_poly(tpage_word);   /* latches even for size-rejected polys */
    int rej_a = psx_gpu_triangle_oversize(vx, vy, 0, 1, 2);
    int rej_b = psx_gpu_triangle_oversize(vx, vy, 2, 1, 3);
    if (rej_a && rej_b) return;
    /* Widescreen: tagged billboard quads carry CPU-computed pixel offsets the
     * GTE squash never saw — re-squash every X around the prim's anchor. */
    {
        int32_t ws_ax;
        if (ws_tagged_anchor(&ws_ax))
            for (int i = 0; i < 4; i++) vx[i] = ws_scale_about(vx[i], ws_ax);
    }
    ws_auto_ui_transform_quad(vx, vy);
    ws_nw_backdrop_stretch_quad(vx, vy);   /* full-frame 2D backdrop image stretch (no-op else) */
    ws_nw_hud_shift_vertices(vx, 4);

    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_bbox(vx, vy, 4)) return;
    const int pgxp_indices[4] = { 1, 3, 5, 7 };
    PgxpPrimitiveDecision pgxp;
    prepare_pgxp_primitive(&pgxp, pgxp_indices, 4, 1, vx, vy);

    setup_textured_draw(color24, semi_trans, raw_texture);

    if (vy[0] == vy[1] && vy[2] == vy[3] &&
        vx[0] == vx[2] && vx[1] == vx[3] &&
        u[0] == u[2] && u[1] == u[3] &&
        v[0] == v[1] && v[2] == v[3] &&
        !pgxp.position_armed && !pgxp.depth_armed) {
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

    if (!rej_a) {
        int32_t tx[3] = { vx[0], vx[1], vx[2] };
        int32_t ty[3] = { vy[0], vy[1], vy[2] };
        prepare_precise_triangle(&pgxp, 0, 1, 2, tx, ty);
        prepare_texture_triangle(&pgxp, 0, 1, 2);
        gr_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                                  vx[1], vy[1], u[1], v[1],
                                  vx[2], vy[2], u[2], v[2],
                                  clut_x, clut_y, tpage);
    }
    if (!rej_b) {
        int32_t tx[3] = { vx[2], vx[1], vx[3] };
        int32_t ty[3] = { vy[2], vy[1], vy[3] };
        prepare_precise_triangle(&pgxp, 2, 1, 3, tx, ty);
        prepare_texture_triangle(&pgxp, 2, 1, 3);
        gr_draw_textured_triangle(vx[2], vy[2], u[2], v[2],
                                  vx[1], vy[1], u[1], v[1],
                                  vx[3], vy[3], u[3], v[3],
                                  clut_x, clut_y, tpage);
    }
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
    set_tpage_from_poly(tpage_word);   /* latches even for size-rejected polys */
    if (psx_gpu_triangle_oversize(vx, vy, 0, 1, 2)) return;

    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_bbox(vx, vy, 3)) return;

    const int pgxp_indices[3] = { 1, 4, 7 };
    PgxpPrimitiveDecision pgxp;
    prepare_pgxp_primitive(&pgxp, pgxp_indices, 3, 1, vx, vy);
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    prepare_precise_triangle(&pgxp, 0, 1, 2,
                             vx, vy);
    prepare_texture_triangle(&pgxp, 0, 1, 2);
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
    set_tpage_from_poly(tpage_word);   /* latches even for size-rejected polys */
    int rej_a = psx_gpu_triangle_oversize(vx, vy, 0, 1, 2);
    int rej_b = psx_gpu_triangle_oversize(vx, vy, 2, 1, 3);
    if (rej_a && rej_b) return;
    ws_auto_ui_transform_quad(vx, vy);
    ws_nw_hud_shift_vertices(vx, 4);
    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_bbox(vx, vy, 4)) return;
    const int pgxp_indices[4] = { 1, 4, 7, 10 };
    PgxpPrimitiveDecision pgxp;
    prepare_pgxp_primitive(&pgxp, pgxp_indices, 4, 1, vx, vy);

    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    if (!rej_a) {
        int32_t tx[3] = { vx[0], vx[1], vx[2] };
        int32_t ty[3] = { vy[0], vy[1], vy[2] };
        prepare_precise_triangle(&pgxp, 0, 1, 2, tx, ty);
        prepare_texture_triangle(&pgxp, 0, 1, 2);
        gr_draw_shaded_textured_triangle(vx[0], vy[0], u[0], v[0], c[0],
                                         vx[1], vy[1], u[1], v[1], c[1],
                                         vx[2], vy[2], u[2], v[2], c[2],
                                         clut_x, clut_y, tpage, raw_texture);
    }
    if (!rej_b) {
        int32_t tx[3] = { vx[2], vx[1], vx[3] };
        int32_t ty[3] = { vy[2], vy[1], vy[3] };
        prepare_precise_triangle(&pgxp, 2, 1, 3, tx, ty);
        prepare_texture_triangle(&pgxp, 2, 1, 3);
        gr_draw_shaded_textured_triangle(vx[2], vy[2], u[2], v[2], c[2],
                                         vx[1], vy[1], u[1], v[1], c[1],
                                         vx[3], vy[3], u[3], v[3], c[3],
                                         clut_x, clut_y, tpage, raw_texture);
    }
}

/* Execute mono line (GP0 0x40-0x47) — Bresenham */
static void gp0_exec_mono_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    parse_vertex(gp0_cmd_buf[2], &x1, &y1);
    if (psx_gpu_line_oversize(x0, y0, x1, y1)) return;
    int32_t vx[2] = { x0, x1 };
    ws_nw_hud_shift_vertices(vx, 2);
    x0 = vx[0]; x1 = vx[1];
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    {
        int32_t lx[2] = { x0, x1 }, ly[2] = { y0, y1 };
        if (draw_area_out_bbox(lx, ly, 2)) return;
    }
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
    if (psx_gpu_line_oversize(x0, y0, x1, y1)) return;
    int32_t vx[2] = { x0, x1 };
    ws_nw_hud_shift_vertices(vx, 2);
    x0 = vx[0]; x1 = vx[1];
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    {
        int32_t lx[2] = { x0, x1 }, ly[2] = { y0, y1 };
        if (draw_area_out_bbox(lx, ly, 2)) return;
    }
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
    /* Same auto_ui squash the textured rect path gets. Without it a flat
     * -coloured HUD mark keeps its 4:3 X while the textured primitives of the
     * same widget move toward their anchor, so at a wide aspect it is left
     * behind in open screen. Untouched when the prepass did not admit this
     * primitive, and rects never carry GTE output. */
    if (ws_active() && w > 0) {
        int corrected_w = w;
        if (ws_auto_ui_transform_rect(&x0, y0, &corrected_w, h))
            w = corrected_w;
    }
    x0 += ws_nw_hud_shift(x0, w);   /* native-wide HUD corner re-anchor (no-op else) */
    x0 += draw_offset_x; y0 += draw_offset_y;
    if (draw_area_out_rect(x0, y0, w, h)) return;
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
        } else {
            int corrected_w = w;
            if (ws_auto_ui_transform_rect(&x0, y0, &corrected_w, h))
                ws_w = corrected_w;
            else if (ws_hud_sprt) {
                x0 = ws_scale_about(x0, ws_hud_pivot(x0, w));
                ws_w = (int)ws_scale_len(w);
            }
        }
    }
    x0 += ws_nw_hud_shift(x0, w);   /* native-wide HUD corner re-anchor (no-op else) */

    x0 += draw_offset_x; y0 += draw_offset_y;
    {
        int dw = (ws_w && ws_w != w) ? ws_w : w;
        if (draw_area_out_rect(x0, y0, dw, h)) return;
    }
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
    ws_sprt_fixed_transform(&x, y, 1);   /* auto_ui squash; no-op when unadmitted */
    x += ws_nw_hud_shift(x, 1);
    x += draw_offset_x; y += draw_offset_y;
    if (draw_area_out_point(x, y)) return;
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
    int ws_w = ws_sprt_fixed_transform(&x0, y0, 8);
    x0 += ws_nw_hud_shift(x0, 8);   /* native-wide HUD corner re-anchor (no-op else) */
    x0 += draw_offset_x; y0 += draw_offset_y;
    {
        int dw = (ws_w && ws_w != 8) ? ws_w : 8;
        if (draw_area_out_rect(x0, y0, dw, 8)) return;
    }
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
    int ws_w = ws_sprt_fixed_transform(&x0, y0, 8);
    x0 += ws_nw_hud_shift(x0, 8);
    x0 += draw_offset_x; y0 += draw_offset_y;
    {
        int dw = (ws_w && ws_w != 8) ? ws_w : 8;
        if (draw_area_out_rect(x0, y0, dw, 8)) return;
        gr_set_semi_transparency(semi_trans, (int)semi_transparency);
        gr_draw_flat_rect(x0, y0, dw, 8, color);
    }
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
    int ws_w = ws_sprt_fixed_transform(&x0, y0, 16);
    x0 += ws_nw_hud_shift(x0, 16);   /* native-wide HUD corner re-anchor (no-op else) */
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    {
        int dw = (ws_w && ws_w != 16) ? ws_w : 16;
        if (draw_area_out_rect(x0, y0, dw, 16)) return;
    }
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
    int local_base = 0;
    if (ws_native_wide_active() &&
        (ws_is_fb_base(dst_x) ||
         (ws_local_viewport_draw_target(&local_base) &&
          dst_x == (uint32_t)local_base))) {
        gr_wide_clear((int)dst_x, (int)dst_y, (int)height, color16);
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
    split_trace_note_draw_area();
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
    split_trace_note_draw_area();
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
            uint32_t sp_phys = psx_ram_map_read(sp & 0x1FFFFFFFu);
            for (int si = 0; si < 10 && sp_phys + (si + 1) * 4 <= g_psx_ram_size; si++)
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

static void ws_ui_prepass_add(const uint32_t *words, uint32_t source_addr,
                              uint16_t rank) {
    if (rank == 0xFFFFu) return;
    if (ws_ui_prepass_count >= WS_UI_PREPASS_MAX) { ws_ui_reject.cap++; return; }
    uint32_t op = words[0] >> 24;
    int32_t min_x, max_x, min_y, max_y;

    if (op >= 0x20u && op <= 0x3Fu && (op & 0x04u) &&
        (op & 0x08u)) {
        int indices[4];
        if (op & 0x10u) {
            indices[0] = 1; indices[1] = 4;
            indices[2] = 7; indices[3] = 10;
        } else {
            indices[0] = 1; indices[1] = 3;
            indices[2] = 5; indices[3] = 7;
        }
        int32_t vx[4], vy[4];
        for (int i = 0; i < 4; i++)
            parse_vertex(words[indices[i]], &vx[i], &vy[i]);
        if (!ws_axis_aligned_quad(vx, vy)) { ws_ui_reject.not_axis++; return; }
        min_x = max_x = vx[0]; min_y = max_y = vy[0];
        for (int i = 1; i < 4; i++) {
            if (vx[i] < min_x) min_x = vx[i];
            if (vx[i] > max_x) max_x = vx[i];
            if (vy[i] < min_y) min_y = vy[i];
            if (vy[i] > max_y) max_y = vy[i];
        }
    } else if (op >= 0x60u && op <= 0x7Fu) {
        /* GP0 rectangle / sprite. Bits 4-3 select the size (00 variable,
         * 01 1x1, 10 8x8, 11 16x16) and bit 2 whether it is textured.
         *
         * Only the TEXTURED families used to be admitted here, which silently
         * dropped every flat-coloured HUD mark. Those then kept their raw 4:3
         * X while the textured primitives beside them were squashed toward an
         * anchor -- so at a wide aspect they were left stranded in open screen,
         * to the left of the cluster they belong to. A GP0 rectangle is always
         * screen-space and never carries GTE output, so admitting the whole
         * range cannot reach world geometry. */
        const unsigned size_sel = (op >> 3) & 3u;
        const int textured = (op >> 2) & 1;
        parse_vertex(words[1], &min_x, &min_y);
        int32_t width, height;
        if (size_sel == 0u) {
            if (textured) {
                width  = (int32_t)(words[3] & 0x3FFu);
                height = (int32_t)((words[3] >> 16) & 0x1FFu);
            } else {
                /* Mirrors gp0_exec_mono_rect: the full 16-bit field, then
                 * clamped to the hardware maximum. */
                width  = (int32_t)(words[2] & 0xFFFFu);
                height = (int32_t)((words[2] >> 16) & 0xFFFFu);
                if (width > 1023) width = 1023;
                if (height > 511)  height = 511;
            }
        } else {
            width = height = size_sel == 1u ? 1 : (size_sel == 2u ? 8 : 16);
        }
        if (width <= 0 || height <= 0) { ws_ui_reject.degenerate++; return; }
        max_x = min_x + width;
        max_y = min_y + height;
    } else {
        ws_ui_reject.opcode++;
        return;
    }

    int32_t width = max_x - min_x, height = max_y - min_y;
    int32_t X = ws_disp_x(), W = ws_disp_w(), H = ws_disp_h();
    if ((min_x <= X && max_x >= X + W && min_y <= 0 && max_y >= H) ||
        (width > W / 2 && height > H / 4)) {
        ws_ui_reject.too_big++;
        return;
    }

    WsUiPrepassItem *item = &ws_ui_prepass[ws_ui_prepass_count++];
    item->group.key =
        ws_auto_ui_group_key_words(words, op, min_y, height);
    item->group.x = min_x - X;
    item->group.width = width;
    item->group.y = min_y;
    item->group.height = height;
    item->group.anchor = 0;
    item->group.root = ws_ui_prepass_count - 1u;
    item->src_addr = psx_ram_map_read(source_addr & 0x1FFFFFFFu) & ~3u;
    item->ot_rank = rank;
    item->y  = min_y;
    item->h  = height;
    item->op = (uint8_t)op;
}

void gpu_ws_prepass_linked_list(uint32_t start_addr) {
    ws_ui_prepass_count = 0;
    ws_ui_prepass_rank = 0xFFFFu;
    ws_auto_ui_dense = 0;
    ws_ui_reject.opcode = ws_ui_reject.not_axis = ws_ui_reject.degenerate =
        ws_ui_reject.too_big = ws_ui_reject.cap = ws_ui_reject.rank = 0;
    ws_ui_rankdrop_count = 0;
    if (!ws_auto_ui_squash || !ws_active()) return;

    uint32_t addr = psx_mod_gpu_dma_resolve_address(start_addr);
    uint32_t safety = 0;
    uint16_t rank = 0xFFFFu;
    const uint32_t max_nodes = 0x40000u;

    for (;;) {
        if (safety++ > max_nodes) {
            ws_ui_prepass_count = 0;
            return;
        }
        uint32_t header = psx_read_word(addr);
        uint32_t num_words = (header >> 24) & 0xFFu;
        if (num_words == 0) {
            rank = rank == 0xFFFFu ? 0u : (uint16_t)(rank + 1u);
        } else if (rank != 0xFFFFu) {
            uint32_t word_addr =
                psx_mod_gpu_dma_resolve_address(addr + 4u);
            uint32_t offset = 0;
            while (offset < num_words) {
                uint32_t first = psx_read_word(
                    psx_mod_gpu_dma_resolve_address(
                        word_addr + offset * 4u));
                uint8_t op = (uint8_t)(first >> 24);
                int count = gp0_command_word_count(op);
                if (count <= 0 || offset + (uint32_t)count > num_words)
                    break;
                /* CPU->VRAM data follows its 3-word header and is not a command
                 * stream. Such transfers are not UI draws; stop this node. */
                if (op >= 0xA0u && op <= 0xBFu) break;
                uint32_t words[12] = {0};
                for (int i = 0; i < count && i < 12; i++) {
                    words[i] = psx_read_word(
                        psx_mod_gpu_dma_resolve_address(
                            word_addr + (offset + (uint32_t)i) * 4u));
                }
                ws_ui_prepass_add(words,
                    psx_mod_gpu_dma_resolve_address(
                        word_addr + offset * 4u), rank);
                offset += (uint32_t)count;
            }
        }

        uint32_t next = header & 0xFFFFFFu;
        if (next == 0xFFFFFFu) break;
        addr = psx_mod_gpu_dma_resolve_address(next);
    }
    if (ws_ui_prepass_count == 0) {
        ws_ui_prepass_count = 0;
        return;
    }

    /* Empty ordering-table buckets can trail the actual frontmost layer.
     * Selecting the last empty bucket made the memory-card glyph layer (rank
     * 4095 followed by an empty rank 4096) disappear from the correction
     * pass. Pick the highest rank that contains an eligible UI primitive. */
    uint16_t max_rank = ws_ui_prepass[0].ot_rank;
    for (uint32_t i = 1; i < ws_ui_prepass_count; i++) {
        if (ws_ui_prepass[i].ot_rank > max_rank)
            max_rank = ws_ui_prepass[i].ot_rank;
    }
    ws_ui_prepass_rank = max_rank;

    uint32_t out = 0;
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
        if (ws_ui_prepass[i].ot_rank == max_rank) {
            ws_ui_prepass[out++] = ws_ui_prepass[i];
        } else if (ws_ui_rankdrop_count < WS_UI_RANKDROP_MAX) {
            const WsUiPrepassItem *it = &ws_ui_prepass[i];
            ws_ui_rankdrop[ws_ui_rankdrop_count].x    = it->group.x;
            ws_ui_rankdrop[ws_ui_rankdrop_count].w    = it->group.width;
            ws_ui_rankdrop[ws_ui_rankdrop_count].y    = it->y;
            ws_ui_rankdrop[ws_ui_rankdrop_count].h    = it->h;
            ws_ui_rankdrop[ws_ui_rankdrop_count].rank = it->ot_rank;
            ws_ui_rankdrop[ws_ui_rankdrop_count].op   = it->op;
            ws_ui_rankdrop_count++;
        }
    }
    ws_ui_reject.rank = ws_ui_prepass_count - out;
    ws_ui_prepass_count = out;
    /*
     * A high final-layer primitive count is a good "dense 2D menu" signal for
     * titles without an explicit gameplay detector, where grouping everything
     * around the centre avoids tearing text grids apart. For GTE-gated 3D
     * titles, though, reaching this path already means a gameplay frame is
     * being stretched. WipEout 3's race HUD is dense enough to trip the old
     * threshold, which pinned every HUD group to the 4:3 centre. Keep edge
     * groups edge-anchored in those frames so the HUD adapts to the wide view.
     */
    ws_auto_ui_dense = ws_ui_prepass_count >= 32u && !ws_gte_game_mode_cfg;
    if (ws_ui_prepass_count == 0) return;

    const int32_t group_origin = ws_disp_x();
    WsUiGroupItem groups[WS_UI_PREPASS_MAX];
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++)
        groups[i] = ws_ui_prepass[i].group;
    ws_ui_group_assign(groups, ws_ui_prepass_count, ws_disp_w(),
                       ws_auto_ui_dense);
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
        ws_ui_prepass[i].group.anchor = group_origin + groups[i].anchor;
        /* Copy the union-find root back too. Without this ws_ui_groups reports
         * the stale insert-time index, which reads as "nothing merged" even
         * when runs formed -- the exact question the command exists to answer. */
        ws_ui_prepass[i].group.root = groups[i].root;
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
    /* live DRAM fold — registered unique high pages via psx_ram_map_read */
    #define RMASK_APPLY(a) (psx_ram_map_read((uint32_t)(a) & 0x1FFFFFFFu))
    uint32_t rawsp = debug_guest_sp();
    g_gp0_last_copy_sp = rawsp;
    if ((rawsp & 0x1FFFFFFFu) >= 0x00800000u) return; /* not in RAM/mirror */
    uint32_t sp = RMASK_APPLY(rawsp) & ~3u;              /* fold to 2MB, word-align */
    /* Two-pass: prefer words that are genuine return addresses (call at ret-8),
     * but if guest code validation yields none, fall back to any code-range
     * stack word so the chain is never silently empty. */
    int found = 0;
    for (int pass = 0; pass < 2 && found == 0; pass++) {
        for (uint32_t off = 0; off + 4 <= 0x400u && found < 6; off += 4) {
            uint32_t w; memcpy(&w, ram + (RMASK_APPLY(sp + off)), 4);
            if ((w & 3u) || w < 0x80010000u || w >= 0x80120000u) continue;
            if (pass == 0) {
                uint32_t ins; memcpy(&ins, ram + (RMASK_APPLY(w - 8u)), 4);
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
    e->ot_rank = gp0_ot_rank;
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
            (void)ws_sprt_fixed_transform(&x0, y0, 1);  /* position only; 1px stays 1px */
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
            int ws_w = ws_sprt_fixed_transform(&x0, y0, 16);
            x0 += ws_nw_hud_shift(x0, 16);
            x0 += draw_offset_x; y0 += draw_offset_y;
            gr_set_semi_transparency(semi_trans, (int)semi_transparency);
            gr_draw_flat_rect(x0, y0, (ws_w && ws_w != 16) ? ws_w : 16, 16,
                              color);
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
            if (check_mask_bit && (gr_vram_read((int)wx, (int)wy) & 0x8000)) {
                goto next_pixel;
            }

            if (set_mask_bit)
                pixel |= 0x8000;

            /* CPU VRAM remains immediately authoritative, preserving mid-DMA
             * reads/savestates. The renderer mirror is committed in bulk when
             * the GP0 payload completes. */
            vram[(uint32_t)wy * 1024u + wx] = pixel;
            gpu_vram_dirty_mark_row((uint32_t)wy);

        next_pixel:
            if (++vram_write_col == vram_write_w) {
                vram_write_col = 0;
                if (++vram_write_row == vram_write_h) {
                    /* Transfer complete */
                    gp0_commit_cpu_to_vram();
                    return;
                }
            }
        }

        if (--vram_write_remaining == 0) {
            gp0_commit_cpu_to_vram();
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
        if (polyline_has_prev &&
            !psx_gpu_line_oversize(polyline_prev_x, polyline_prev_y, x, y)) {
            gr_draw_line(polyline_prev_x, polyline_prev_y, x, y, polyline_color);
        }
        polyline_prev_x = x; polyline_prev_y = y;
        polyline_has_prev = 1;
        return;
    }

    /* State: shaded polyline — alternating color, vertex words */
    if (gp0_state == GP0_POLYLINE_SHADED) {
        /* The terminator can arrive in either the color or vertex position.
         * Check it before interpreting the alternating shaded-polyline stream;
         * otherwise a vertex-position terminator is consumed as coordinates and
         * de-phases all following GP0 commands. */
        if ((val & 0xF000F000u) == 0x50005000u) {
            gp0_state = GP0_IDLE;
            return;
        }
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
            /* Expecting color word. */
            polyline_color = rgb888_to_rgb555(val & 0xFFFFFFu);
            polyline_has_prev = 2;
            return;
        }
        /* polyline_has_prev == 2: vertex word */
        {
            int32_t x, y;
            parse_vertex(val, &x, &y);
            x += draw_offset_x; y += draw_offset_y;
            if (!psx_gpu_line_oversize(polyline_prev_x, polyline_prev_y, x, y))
                gr_draw_shaded_line(polyline_prev_x, polyline_prev_y,
                                    polyline_prev_c, x, y, polyline_color);
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

/* Internal rendered-frame counter: the game finishing a frame = it flips the
 * display origin to the other buffer (GP1(05h) with a CHANGED address). This
 * is the "internal FPS" every mainstream emulator reports, distinct from the
 * vblank rate — under load the two diverge and both matter. */
static uint64_t s_display_flips = 0;
uint64_t psx_gpu_display_flip_count(void) { return s_display_flips; }
static void gp1_display_area_start(uint32_t val) {
    /* GP1(05h): Start of display area in VRAM
     * bits 0-9: X (in halfwords, 0-1023)
     * bits 10-18: Y (0-511) */
    uint32_t nx = val & 0x3FF;
    uint32_t ny = (val >> 10) & 0x1FF;
    if (nx != display_area_x || ny != display_area_y) s_display_flips++;
    display_area_x = nx;
    display_area_y = ny;
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
    uint32_t y1 = val & 0x3FF;
    uint32_t y2 = (val >> 10) & 0x3FF;
    uint32_t h = (y2 > y1) ? (y2 - y1) : 0u;
    /* MotK intro→crawl retargets the band while staying in depth24. Stale
     * trailing RGB from the prior movie would flash for a frame or two —
     * reset the upload span and hold present (skip Swap) for 3 vblanks. */
    if ((display_depth & 1u) && s_d24_prev_disp_h != 0u && h != s_d24_prev_disp_h) {
        s_d24_upload_x1 = 0;
        s_d24_present_hold = 3;
    }
    v_display_y1 = y1;
    v_display_y2 = y2;
    if (h != 0u) s_d24_prev_disp_h = h;
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
    uint32_t new_depth = (val >> 4) & 1;
    hres1 = val & 3;
    vres = (val >> 2) & 1;
    video_mode = (val >> 3) & 1;
    display_mode_programmed = 1;
    if (new_depth != display_depth)
        s_d24_upload_x1 = 0; /* rising/falling: drop stale coverage */
    display_depth = new_depth;
    vertical_interlace = (val >> 5) & 1;
    /* GPUSTAT.13 holds the legacy constant 0 in progressive (see the vblank
     * field flip); clear it on the switch so a title that toggles interlace
     * on and back off doesn't leave the field bit latched at 1. */
    if (!vertical_interlace)
        interlace_field = 0;
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
#include "pst_wire.h"

/* GPU snap fields are scalars / u32 arrays — emit as LE u32/i32 (no struct pad). */
static int gpu_snap_emit(PstW *w) {
#define WU(f) do { if (!pst_w_u32(w, (uint32_t)(f))) return 0; } while (0)
#define WI(f) do { if (!pst_w_i32(w, (int32_t)(f))) return 0; } while (0)
#define WH(f) do { if (!pst_w_u16(w, (uint16_t)(f))) return 0; } while (0)
    WU(texpage_x); WU(texpage_y); WU(semi_transparency); WU(texpage_colors);
    WU(dither_enabled); WU(draw_to_display); WU(texture_disable); WU(texture_window_value);
    WU(set_mask_bit); WU(check_mask_bit);
    WU(interlace_field); WU(reverse_flag);
    WU(draw_area_left); WU(draw_area_top); WU(draw_area_right); WU(draw_area_bottom);
    WI(draw_offset_x); WI(draw_offset_y);
    WU(hres1); WU(hres2); WU(vres); WU(video_mode); WU(display_depth); WU(vertical_interlace);
    WU(display_disabled); WU(irq1_flag); WU(dma_direction); WU(lcf);
    WU(display_area_x); WU(display_area_y);
    WU(h_display_x1); WU(h_display_x2); WU(v_display_y1); WU(v_display_y2);
    WU(gpuread_latch);
    WU((uint32_t)gp0_state);
    for (int i = 0; i < 16; i++) WU(gp0_cmd_buf[i]);
    WI(gp0_words_collected); WI(gp0_words_needed);
    WU(gp0_next_source_addr); WU(gp0_cmd_source_addr);
    WH(polyline_color); WI(polyline_prev_x); WI(polyline_prev_y); WH(polyline_prev_c);
    WI(polyline_semi_trans); WI(polyline_has_prev);
    WH(vram_write_x); WH(vram_write_y); WH(vram_write_w); WH(vram_write_h);
    WH(vram_write_col); WH(vram_write_row); WU(vram_write_remaining);
    WI(vram_read_active); WH(vram_read_x); WH(vram_read_y); WH(vram_read_w); WH(vram_read_h);
    WH(vram_read_col); WH(vram_read_row);
    /* Depth24 present helpers (MotK FMV) — must resume with upload span. */
    WU(s_d24_upload_x1); WI(s_d24_present_hold); WU(s_d24_prev_disp_h);
#undef WU
#undef WI
#undef WH
    return 1;
}
static int gpu_snap_parse(PstR *r) {
    uint32_t u; int32_t i; uint16_t h;
#define RU(f) do { if (!pst_r_u32(r, &u)) return 0; (f) = u; } while (0)
#define RI(f) do { if (!pst_r_i32(r, &i)) return 0; (f) = i; } while (0)
#define RH(f) do { if (!pst_r_u16(r, &h)) return 0; (f) = h; } while (0)
    RU(texpage_x); RU(texpage_y); RU(semi_transparency); RU(texpage_colors);
    RU(dither_enabled); RU(draw_to_display); RU(texture_disable); RU(texture_window_value);
    RU(set_mask_bit); RU(check_mask_bit);
    RU(interlace_field); RU(reverse_flag);
    RU(draw_area_left); RU(draw_area_top); RU(draw_area_right); RU(draw_area_bottom);
    RI(draw_offset_x); RI(draw_offset_y);
    RU(hres1); RU(hres2); RU(vres); RU(video_mode); RU(display_depth); RU(vertical_interlace);
    RU(display_disabled); RU(irq1_flag); RU(dma_direction); RU(lcf);
    RU(display_area_x); RU(display_area_y);
    RU(h_display_x1); RU(h_display_x2); RU(v_display_y1); RU(v_display_y2);
    RU(gpuread_latch);
    if (!pst_r_u32(r, &u)) return 0;
    gp0_state = (Gp0State)u;
    for (int k = 0; k < 16; k++) RU(gp0_cmd_buf[k]);
    RI(gp0_words_collected); RI(gp0_words_needed);
    RU(gp0_next_source_addr); RU(gp0_cmd_source_addr);
    RH(polyline_color); RI(polyline_prev_x); RI(polyline_prev_y); RH(polyline_prev_c);
    RI(polyline_semi_trans); RI(polyline_has_prev);
    RH(vram_write_x); RH(vram_write_y); RH(vram_write_w); RH(vram_write_h);
    RH(vram_write_col); RH(vram_write_row); RU(vram_write_remaining);
    RI(vram_read_active); RH(vram_read_x); RH(vram_read_y); RH(vram_read_w); RH(vram_read_h);
    RH(vram_read_col); RH(vram_read_row);
    RU(s_d24_upload_x1); RI(s_d24_present_hold); RU(s_d24_prev_disp_h);
#undef RU
#undef RI
#undef RH
    /* A restored GPU snapshot already contains an authoritative GP1(08h)
     * display mode even though the host-only observation flag is not wired. */
    display_mode_programmed = 1;
    return 1;
}

uint32_t gpu_snapshot_bytes(void) {
    PstW w; pst_w_init(&w, NULL, 0);
    (void)gpu_snap_emit(&w);
    return (uint32_t)w.written;
}
void gpu_snapshot_write(uint8_t *p) {
    PstW w; uint32_t n = gpu_snapshot_bytes();
    pst_w_init(&w, p, n);
    (void)gpu_snap_emit(&w);
}
uint32_t gpu_cosim_snapshot_bytes(void) { return gpu_snapshot_bytes(); }
void gpu_cosim_snapshot_write(uint8_t *p) { gpu_snapshot_write(p); }
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
int gpu_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    if (len != gpu_snapshot_bytes()) return 0;
    pst_r_init(&r, p, len);
    if (!gpu_snap_parse(&r)) return 0;
    /* Sync renderer clip/scissor to restored GP0(E3/E4); vars alone leave GL
     * on a stale draw area after savestate load. */
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    ws_nw_sync_target();
    return 1;
}
uint16_t* gpu_get_vram_ptr(void){ return vram; }
uint32_t  gpu_get_vram_bytes(void){ return (uint32_t)sizeof(vram); }
