/* gpu.h — PS1 GPU hardware simulation (Phase 3).
 *
 * Implements GPUSTAT, GP0, GP1, and 1024x512 16-bit VRAM.
 * No rendering to screen — just correct hardware state transitions.
 */

#ifndef PSXRECOMP_GPU_H
#define PSXRECOMP_GPU_H

#include <stdint.h>

typedef struct CPUState CPUState;

#ifdef __cplusplus
extern "C" {
#endif

void     gpu_init(void);
uint32_t gpu_read_gpustat(void);   /* 0x1F801814 read */
uint32_t gpu_read_gpuread(void);   /* 0x1F801810 read */
void     gpu_write_gp0(uint32_t val);  /* 0x1F801810 write */
void     gpu_write_gp1(uint32_t val);  /* 0x1F801814 write */
void     gpu_set_gp0_source(uint32_t addr); /* diagnostic source for next GP0 word */
/* Linked-list provenance for the next GP0 words. `word_count == 0` identifies
 * an ordering-table link node; packet nodes retain the most recently visited
 * OT rank. This lets diagnostics distinguish front-layer UI packets from
 * depth-sorted world packets without guessing from primitive shape. */
void     gpu_set_gp0_linked_list_node(uint32_t addr, uint32_t word_count);
void     gpu_vblank_tick(void);        /* Toggle LCF, called at each simulated vblank */

/* Display presentation accessors (Phase 3). */
const uint16_t* gpu_get_vram(void);    /* Pointer to 1024x512 16-bit VRAM */

typedef struct {
    uint32_t display_x, display_y;     /* VRAM start of display area (GP1(05h)) */
    uint32_t width, height;            /* Derived from display mode + ranges */
    int      depth24;                  /* GP1(08h) display depth flag: RGB888 scanout */
    int      disabled;                 /* GP1(03h) display disable flag */
} GpuDisplayInfo;

void gpu_get_display_info(GpuDisplayInfo* out);

/* Perspective-correct UV arming rate, per condition. armed/attempts is the
 * real perspective coverage: attempts counts only textured triangles, so it is
 * the denominator that gp0_draw (which includes untextured primitives that are
 * correctly never armed) cannot provide. Diagnostic; any pointer may be NULL. */
void gpu_texture_correction_stats(uint64_t *attempts, uint64_t *armed,
                                  uint64_t *no_correction,
                                  uint64_t *no_source, uint64_t *no_depth);
/* Frame-canonical PGXP decisions. primitives_armed counts a source primitive
 * once when canonical precise positions and/or coherent per-vertex depth were
 * armed. Mixed/promoted/demoted/overflow are observed events, not sentinels. */
void gpu_pgxp_primitive_stats(uint64_t *primitives_armed,
                              uint64_t *rejected_incomplete_position,
                              uint64_t *rejected_incomplete_depth,
                              uint64_t *mixed_position,
                              uint64_t *promoted_from_native,
                              uint64_t *demoted_to_native,
                              uint64_t *cache_overflow);
/* GP1(08h) bit4 — 24-bit display. Renderers skip FBO upload queues while set:
 * packed RGB888 lives in the CPU mirror; treating A0 rects as 1555 FBO uploads
 * both wastes bandwidth and force-flushes when UP_RECTS_MAX is hit (MotK FMV). */
int  gpu_display_is_depth24(void);
/* GP1(08h) bit 3: 0 = NTSC, 1 = PAL. */
int  gpu_display_is_pal(void);
/* Nonzero after GP1(08h) has selected a display mode (or one was restored). */
int  gpu_display_mode_is_programmed(void);
/* Guest CPU cycles per CRTC frame (33.8688 MHz / 60 NTSC, / 50 PAL). */
#define PSX_VBLANK_CYCLES_NTSC 564480u
#define PSX_VBLANK_CYCLES_PAL  677376u
uint32_t gpu_vblank_period_cycles(void);
/*
 * Force guest cycles per VBlank regardless of GP1(08h) PAL/NTSC. 0 clears.
 * Used so NTSC display mode can keep a 50 Hz IRQ (25 fps with stock skip)
 * for audio-safe ports of PAL titles. Still divided by the Nx multiplier.
 */
void gpu_set_crtc_vblank_period_override(uint32_t cycles);
uint32_t gpu_get_crtc_vblank_period_override(void);
/*
 * Experimental enhancement: GooseStation-style Nx video timing. Multiplier 2
 * halves the guest VBlank period (NTSC 120 Hz / PAL 100 Hz CRTC). Default 1.
 * Host pacing follows the resulting live period unless a mod explicitly owns
 * the wall target through psx_mod_set_native_vblank_rate().
 */
void gpu_set_crtc_refresh_multiplier(uint32_t multiplier);
uint32_t gpu_get_crtc_refresh_multiplier(void);
void gpu_display_pixel_rgb(const GpuDisplayInfo* di, uint32_t x, uint32_t y,
                           uint8_t* r, uint8_t* g, uint8_t* b);
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* di, uint32_t x, uint32_t y);
/* Batch equivalent of calling gpu_display_pixel_argb(di, x, y) for x in
 * [0, count) on a depth24 (FMV) scanline — same output, far less per-pixel
 * overhead. `out` must hold at least `count` uint32_t ARGB entries. */
void gpu_depth24_present_row(const GpuDisplayInfo* di, uint32_t y, uint32_t* out,
                             uint32_t count);
/* Depth24: RGB columns covered by CPU→VRAM uploads since the last reset.
 * Returns 0 when no uploads yet, crtc_w when coverage is full/unknown-beyond,
 * else the covered RGB width. Present black-fills [limit, crtc_w) without
 * shrinking the CRTC-derived draw width (avoids a flickering black pillar). */
uint32_t gpu_depth24_rgb_limit(uint32_t display_x, uint32_t crtc_w);
void     gpu_depth24_upload_span_reset(void);
/* MotK intro cuts retarget GP1(07h). While hold > 0, present should skip
 * Swap (keep prior frame) so stale trailing VRAM never flashes. One tick
 * per vblank; returns non-zero while the hold is still active after tick. */
int      gpu_depth24_present_hold_tick(void);
/* After savestate restore: clear ephemeral hold so the restored depth24
 * frame can present immediately (span/prev_h come from the GPU snap). */
void     gpu_depth24_on_savestate_loaded(void);
/* GP1(06h)/GP1(07h)/GP1(08h) fields for debug (gpu_state). */
void gpu_get_crtc_debug(uint32_t *x1, uint32_t *x2, uint32_t *y1, uint32_t *y2,
                        uint32_t *hres1_out, uint32_t *hres2_out);
uint64_t gpu_get_gp0_count(void);  /* Total GP0 writes since init */
void gpu_get_gp0_stats(uint64_t* nop, uint64_t* fill, uint64_t* draw, uint64_t* env, uint64_t* copy);

typedef struct {
    uint32_t left, top, right, bottom;
    int32_t offset_x, offset_y;
} GpuDrawArea;
void gpu_get_draw_area(GpuDrawArea* out);
/* Non-zero when the frame that just reached vblank used two side-by-side draw
 * areas. Presentation-only helpers use this to expand a local split-screen
 * viewport without changing the underlying framebuffer or netplay hashes. */
int  gpu_last_frame_vertical_split_screen(void);
void gpu_vertical_split_debug(int *active, int *left_age, int *right_age);
uint16_t gpu_vram_peek(int x, int y);

/* Shaded quad vertex capture (Phase 4.5 debug). */
typedef struct {
    int32_t vx[4], vy[4];
    uint32_t color[4];
} GpuSqCapEntry;
void gpu_arm_shaded_quad_capture(void);
int  gpu_get_shaded_quad_capture(const GpuSqCapEntry** out);

/* Per-frame GP0 command ring (always-on; query via debug server).
 * Each entry records the GP0 command header + up to 6 payload words
 * (longer commands like 0x3C shaded textured quad are truncated to 6).
 * Stamped with the s_frame_count value at issue time so a debug
 * client can pull all commands for any frame in the ring window. */
#define GPU_GP0_RING_MAX_WORDS 12
typedef struct {
    uint32_t frame;
    uint32_t seq;
    uint32_t src_addr;      /* RAM/MMIO source address of command header, if known */
    uint32_t pc;            /* g_debug_last_store_pc when command completes */
    uint32_t func;          /* g_debug_current_func_addr at issue (executing guest fn) */
    uint32_t ra;            /* guest $ra at issue: direct caller of the leaf GP0 helper */
    uint8_t  opcode;
    uint8_t  n_words;       /* total command length; >MAX means truncated */
    uint16_t ot_rank;       /* linked-list OT rank, 0xFFFF outside/unknown */
    uint32_t cmd[GPU_GP0_RING_MAX_WORDS];
    /* Builder attribution (populated only for VRAM->VRAM copies, op 0x80):
     * bounded guest-stack unwind of validated return addresses, innermost
     * first. Lets a caller skip the libgpu funnel band and name the game-level
     * routine that assembled the copy. Zero for non-copy commands. */
    uint32_t bld[6];
    uint32_t csp;           /* raw guest $sp at copy time (diagnostic) */
} GpuGp0RingEntry;

uint64_t gpu_gp0_ring_total(void);
uint32_t gpu_gp0_ring_capacity(void);
uint32_t gpu_gp0_ring_max_words(void);
int      gpu_gp0_ring_dump_frame(uint32_t frame, GpuGp0RingEntry *out, int max_out);

/* PGXP per-prim resolution census: one entry per submitted triangle whose
 * source primitive was not cleanly all-dataflow. cls[] describes the emitted
 * triangle's vertices; all three may be 2 when an unlisted fourth quad vertex
 * rejected the whole source primitive. 0 = shadow miss (addr known, no valid
 * entry), 1 = geometry-cache fallback, 2 = dataflow, 3 = no packet address
 * (CPU-built submission path). Draw-space bbox, so it overlays the display
 * half directly. */
typedef struct {
    int16_t  x0, y0, x1, y1;
    uint8_t  cls[3];
    uint8_t  _pad;
    uint32_t src;
    uint32_t frame;
} PgxpCensusEnt;
/* Copies the census of the most recent COMPLETE frame; returns entry count.
 * tris/clean report that frame's totals (all triangles vs all-dataflow). */
int gpu_pgxp_census_dump(uint32_t *frame_out, uint32_t *tris, uint32_t *clean,
                         PgxpCensusEnt *out, int max_out);
/* Same shape for texture-UV correction misses: cls[0] carries the reason
 * (0 = no_source, 1 = no_depth), cls[1] the offending vertex index. */
int gpu_pgxp_texcensus_dump(uint32_t *frame_out, PgxpCensusEnt *out, int max_out);

void     gpu_gp0_ring_frame_span(uint32_t *out_oldest, uint32_t *out_newest);

/* Vblank presentation callback — called from gpu_vblank_tick().
 * Under netplay the callback is deferred to gpu_vblank_flush_present() at
 * BB / IRQ-check edges so finish_frame digests are not sampled mid-block.
 * Flush also holds while sio_hold_present_for_card() so native save/load
 * busy-waits are not interrupted by MotK-style BB-edge commit. */
typedef void (*gpu_vblank_cb)(void);
void gpu_set_vblank_callback(gpu_vblank_cb cb);
void gpu_vblank_flush_present(void);
void gpu_vblank_clear_deferred_present(void);
/* Queue one deferred present (netplay RB resume when I_STAT already has
 * VBlank — snap resync cleared the pending callback). */
void gpu_vblank_arm_deferred_present(void);
/* 1 while a netplay deferred present is armed (MotK CD54 VBlank hold). */
int  gpu_vblank_present_pending(void);
/* Clear flush_present reentrancy guard before longjmp (flush_resume). */
void gpu_vblank_release_present_flush_guard(void);

/* Present-time screen-colour model (see color_lut.h ScreenKind: 0=raw,
 * 1=crt, 2=composite, 3=trinitron). Config/launcher-driven; the PSX_SCREEN
 * env var, if set, overrides this. Default 0 (raw) is byte-identical. */
void gpu_set_screen_kind(int kind);

/* Widescreen proportion correction (active only when aspect != 4:3 and the
 * game's [widescreen] block opts in — see config_loader.h). Tagged
 * character/billboard prims are re-squashed around their projected anchor;
 * untagged SPRT prims (screen-space HUD/menus) around the display centre.
 * psx_ws_sprite_tag is the per-prim callback the recompiler emits at the
 * entry of each [widescreen] sprite_tag_funcs function. */
/* mode: 0 = off (4:3 identity), 1 = squash (legacy hack), 2 = native-wide
 * (render the wider FOV into a wider frame, present 1:1; GTE not squashed). */
void gpu_ws_configure(int aspect_num, int aspect_den,
                      uint32_t sprite_anchor_addr, int hud_sprt_squash, int mode);
/* [widescreen] full_2d: opt a pure-2D sprite game into the widescreen present
 * path (treat every in-game frame as gameplay, since it never tags 3D prims). */
void gpu_ws_set_full_2d(int on);
void gpu_ws_set_auto_ui_squash(int on);
/* [widescreen.bg2d] Capcom 2D background tile-loop widen — hooked at the renderer's
 * column-count / start-tile-col / start-screen-x instructions. Identity at 4:3
 * and in the engine's 512 hi-res mode. */
void gpu_ws_bg2d_configure(uint32_t layer_base, uint32_t ring_base,
                           uint32_t map_size_addr, uint32_t layer_stride_addr,
                           uint32_t ring_cols, uint32_t layer_count,
                           uint32_t layer_struct_stride, uint32_t packet_cap);
/* Some Capcom variants (MMX5/MMX6) can compose a layer's scroll with a parent
 * selected by byte +0x52. MMX4's streamer always uses each layer's own scroll. */
void gpu_ws_bg2d_set_parent_links(int on);
int psx_ws_bg2d_cols(int base);
int psx_ws_bg2d_startcol(int col, unsigned mask);
int psx_ws_bg2d_startx(int x);
int psx_ws_bg2d_stream_left(int x);
int psx_ws_bg2d_stream_right(int x);
int psx_ws_bg2d_undercap(int counter, int native_cap);
/* MISNAMED, NOT MMX6-SPECIFIC, AND NOT SAFE TO DELETE (surveyed 2026-07-27).
 *
 * These read as "MMX6 compat shims" and the comment here used to say exactly
 * that. They are not. They are the widescreen-2D entry points that the
 * recompiler emits for ANY title with a [widescreen.bg2d] block, and live
 * generated code in several non-MMX6 games links them today:
 *
 *   psx_ws_mmx6_bg_cols/startcol/startx/stream_left/stream_right
 *       -> CrashBashRecomp, MegaManX5Recomp, TsumuRecomp (+ -release)
 *   psx_ws_mmx6_bg_undercap (below, in gpu.c)
 *       -> ApeEscapeRecomp, CrashBashRecomp, MegaManX4Recomp,
 *          MegaManX5Recomp, TsumuRecomp, Vigilante8PSXRecomp, THPS2
 *
 * Each one forwards to its psx_ws_bg2d_* twin above, which is the real
 * implementation. Only the NAME is historical: this began as MMX6 widescreen
 * work and was generalised without renaming the emitted symbols.
 *
 * Deleting or renaming them breaks widescreen in every repo listed above, and
 * a rename cannot be runtime-only: the names are emitted by
 * recompiler/src/code_generator.cpp, which is in codegen_hash_sources.cmake,
 * so changing it rolls the overlay cache tag and forces a regen + reshard of
 * every title. That is the reason this is still called mmx6 in 2026.
 *
 * Verify before touching, don't trust this comment: grep the generated trees
 * of the game repos for psx_ws_mmx6_bg_undercap and see who turns up.
 */
int psx_ws_mmx6_bg_cols(int base);
int psx_ws_mmx6_bg_startcol(int col);
int psx_ws_mmx6_bg_startx(int x);
int psx_ws_mmx6_bg_stream_left(int x);
int psx_ws_mmx6_bg_stream_right(int x);
struct CPUState;
void psx_ws_sprite_tag(struct CPUState* cpu);

/* Native-wide (mode 2) on a game frame. ws_nw_extra() is the total width the
 * frame grows by, in display pixels (the present path widens the display read
 * by this; 0 when native-wide is inactive). */
int  ws_native_wide_active(void);
int  ws_nw_extra(void);
int  ws_nw_present_width(void);
void gpu_ws_set_netplay_local_viewport(int enabled, int slot);
int  gpu_ws_netplay_local_viewport_base_x(void);
int  gpu_ws_netplay_local_viewport_width(void);
/* True when scene classification requires native 4:3 content (FMV video or a
 * full-2D menu/title screen). This suppresses GTE/UI widening and selects the
 * centered content-safe transform; it does not select the outer viewport. */
int  gpu_ws_content_native_43(void);
/* Per-side X cull-margin (screen/world units) emitted into the game's draw-
 * cull immediates by the recompiler ([widescreen.cull]); 0 unless stretching. */
int  psx_ws_x_margin(void);
void gpu_ws_set_cull_guard_pixels(int pixels);
/* Bias/range activation-window margin. This may include an additional
 * resident-object lead while render/terrain paths retain psx_ws_x_margin(). */
int  psx_ws_activation_margin(void);
void gpu_ws_set_activation_guard_pixels(int pixels);
void gpu_ws_set_explicit_cull_sites(const uint32_t *bias, int nbias,
                                    const uint32_t *slti, int nslti,
                                    const uint32_t *range, int nrange);
void gpu_ws_set_slti_lower_cull_sites(const uint32_t *sites, int nsites);
void gpu_ws_set_negsub_cull_sites(const uint32_t *sites, int nsites);
void gpu_ws_set_vxrange_cull_sites(const uint32_t *sites, int nsites);
void gpu_ws_set_depth_cull_sites(const uint32_t *sites, int nsites);
void gpu_ws_set_plane_nx_sites(const uint32_t *sites, int nsites);
void gpu_ws_set_xclip_load_sites(const uint32_t *sites, int nsites);
void gpu_ws_set_cull_keep_sites(const uint32_t *addresses,
                                const uint32_t *expected,
                                const uint32_t *results, int nsites);
/* [[widescreen.cull.widen]] sites, so the dirty-RAM interpreter and any overlay
 * shard reach the same widened verdict the AOT image does. */
void gpu_ws_set_cull_widen_sites(const uint32_t *addresses,
                                 const uint32_t *expected,
                                 const uint32_t *modes, int nsites);
void gpu_ws_set_angle_sites(const uint32_t *addresses,
                            const uint32_t *expected, int nsites);
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
                            const uint32_t queue_type_masks[3]);
int  psx_ws_is_cull_bias_site(uint32_t pc);
int  psx_ws_is_cull_slti_site(uint32_t pc);
int  psx_ws_is_cull_slti_lower_site(uint32_t pc);
int  psx_ws_is_cull_negsub_site(uint32_t pc);
int  psx_ws_is_cull_vxrange_site(uint32_t pc);
int  psx_ws_is_cull_depth_site(uint32_t pc);
int32_t psx_ws_depth_bound(int32_t imm);
int  psx_ws_is_cull_range_site(uint32_t pc);
int  psx_ws_is_cull_plane_nx_site(uint32_t pc);
int32_t  psx_ws_plane_nx(int32_t nx);
int  psx_ws_is_cull_xclip_load_site(uint32_t pc);
uint32_t psx_ws_xclip_bound(uint32_t vanilla);
uint32_t psx_ws_cull_keep_result(uint32_t vanilla, uint32_t forced);
int psx_ws_cull_keep_site(uint32_t pc, uint32_t instr, uint32_t vanilla,
                          uint32_t *out);
int psx_ws_cull_widen_site(uint32_t pc, uint32_t instr, uint32_t rs,
                           uint32_t rt, uint32_t imm, uint32_t *out);
uint32_t psx_ws_angle_widen(uint32_t vanilla);
int psx_ws_angle_site(uint32_t pc, uint32_t instr, uint32_t *out);
uint32_t psx_ws_aspect_cone_result(uint32_t site, uint32_t vanilla,
                                   uint32_t object, int32_t x, int32_t z,
                                   int32_t y);
int psx_ws_aspect_cone_site(CPUState *cpu, uint32_t pc, uint32_t instr,
                            uint32_t vanilla, uint32_t *out);
/* Scale a signed Q16 horizontal gameplay limit into the active native-wide
 * game field. Identity at 4:3 / menus / FMV. */
int32_t psx_ws_player_x_bound(int32_t vanilla);
void gpu_ws_set_signed_x_bound_sites(const uint32_t *addresses,
                                     const uint32_t *expected, int count);
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t instr);

/* Shared render-funnel screen-X cull widening ([widescreen.cull] auto_screen_x):
 * the gcc emit and the interpreter both route a flagged
 * `sltiu rt, sx, 0x140/0x141` through this one helper so every overlay execution
 * path widens identically. Returns the sltiu verdict (1 = keep). 0 at 4:3. */
int  psx_ws_cull_sltiu(uint32_t sx, uint32_t imm);
/* Signed-funnel variants (min/max + center±halfwidth idioms — see
 * ws_cull_detect.h): right-edge widen for `slti v, minSX, W` and left-edge
 * widen for the paired `bltz maxSX` reject. Identity at 4:3. */
int  psx_ws_cull_slti(uint32_t sx, uint32_t imm);
int  psx_ws_cull_slti_lower(uint32_t sx, uint32_t imm);
/* Register-bound widen for SLT ([[widescreen.cull.widen]]). bound_is_rt picks
 * which operand carries the bound: 1 -> rs < rt + m, 0 -> rs + m < rt. Identity
 * at margin 0. Use instead of pinning a clip classifier, which stops the
 * clipper subdividing and gets whole primitives dropped by the GPU. */
int  psx_ws_cull_slt_widen(uint32_t rs, uint32_t rt, int bound_is_rt);
int  psx_ws_cull_bltz(uint32_t v);
int  psx_ws_cull_vxrange(uint32_t x, uint32_t imm);
/* True if a run of instruction words carries the screen-extent reject signature
 * (a width compare AND a height compare from the configured immediate sets).
 * Used by the interp to gate the widening to real render funnels. */
int  psx_ws_func_has_screen_cull(const uint32_t *words, int n);
/* Classify words[idx] as an X left-edge reject bltz (runtime imm sets). */
int  psx_ws_cull_bltz_at(const uint32_t *words, int n, int idx);
/* Per-game cull signature immediates ([widescreen.cull] screen_w_imms /
 * screen_h_imms); defaults 0x140/0x141 + 0xE0/0xF1. */
void gpu_ws_set_cull_imms(const uint32_t *w, int nw, const uint32_t *h, int nh);
int  psx_ws_is_cull_w_imm(uint32_t imm);
/* Per-game opt-in gates for the pattern-scanned interp widen hooks
 * (auto_screen_x cull + auto_backdrop preload). Default OFF: a title that
 * never opted in must never have its live code pattern-scanned and rewritten. */
void gpu_ws_set_auto_hooks(int cull_on, int backdrop_on);
int  psx_ws_auto_cull_on(void);
/* Perspective-correct UV interpolation for textured world polygons
 * ([video] perspective_texturing). Also turns on the SWC2 RAM-provenance
 * tracking it needs, so a polygon only qualifies when every position word in
 * its DMA packet was written by a GTE projection store. Default off. */
void gpu_texture_correction_set(int enabled);
int gpu_texture_correction_enabled(void);
/* Triangles drawn with perspective-correct UVs since startup. */
uint32_t gpu_texture_correction_hits(void);
/* GTE-activity gameplay detector ([widescreen] gte_game_mode) for 3D titles
 * with no sprite-tag helper: gte.cpp notes every RTPS/RTPT projection; a frame
 * that projects enough vertices is stamped as gameplay. */
void gpu_ws_set_gte_game_mode(int on);
void gpu_ws_set_precise_nclip(int on);
void psx_ws_note_gte_project(int nverts);
/* Optional authoritative gameplay-state gate. When configured, it replaces
 * heuristic gameplay classification for native-wide presentation. */
void gpu_ws_set_gameplay_state_gate(uint32_t addr,
                                    const uint32_t *values, int nvalues);
/* Native-wide HUD corner re-anchoring ([widescreen] nw_hud_corners): push
 * outer-third screen-space HUD primitives out to the true wide-frame corners
 * (they otherwise sit inset by the reveal). Runtime-only. Off by default. */
void gpu_ws_set_nw_hud_corners(int on);
/* Targeted alternative for sprite-heavy 2D games: corner-anchor only primitives
 * whose ordering-table packet lives in the configured half-open RAM range. */
void gpu_ws_set_nw_left_hud_packet_range(uint32_t lo, uint32_t hi);
void gpu_ws_begin_linked_list(void);
void gpu_ws_end_linked_list(void);
void gpu_ws_prepass_linked_list(uint32_t start_addr);
/* Native-wide full-frame 2D backdrop stretch ([widescreen] nw_backdrop):
 * stretch a screen-space quad that covers the whole 4:3 framebuffer (sky
 * gradient / backdrop image) to fill the wide frame, so it no longer
 * pillarboxes at the reveal margins. Runtime-only. Off by default. */
void gpu_ws_set_nw_backdrop(int on);
/* Native-wide flat-polygon backdrop stretch ([widescreen] nw_flat_backdrop):
 * stretch untextured primitives in the wide mirror without changing the
 * canonical 4:3 framebuffer. Intended for flat-colour sky/water backdrops. */
void gpu_ws_set_nw_flat_backdrop(int on);
int  gpu_ws_nw_flat_backdrop_enabled(void);
/* Stretch the title-opted textured pre-3D backdrop phase in the wide mirror. */
void gpu_ws_set_nw_phase_backdrop(int on);

/* Backdrop screen-X correction ([widescreen.backdrop] x_sites). The parallax
 * 2D backdrop layer computes screen-X without the GTE, so it misses the
 * widescreen squash; the recompiler emits this on each backdrop handler's
 * final screenX store to squash it around the screen centre (identity at 4:3).
 * Pulls far backdrop pieces in from past the 320px edge to cover the 16:9 FOV. */
int  psx_ws_backdrop_x(int x);

/* Backdrop PRELOAD ([widescreen.cull] auto_backdrop). psx_ws_backdrop_preload()
 * is nonzero only while native-wide widescreen is engaged (0 at 4:3 / boot /
 * FMV / full-2D), so the rewrite is byte-identical when off. psx_ws_backdrop_value()
 * is the one value substitution shared by the gcc emit and the
 * interpreter for a detected window bound: it returns `orig` unless preload is
 * engaged, in which case it forces the bound to preload the WHOLE finite tile
 * row (START -> 0, END -> large sentinel pinned by the generator's high clamp to
 * extent-1). Every column is submitted, so the widened 16:9 viewport never gaps;
 * the generator's clamps + the byte-sized row extent keep it bounded. */
int      psx_ws_backdrop_preload(void);
uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols);

/* auto_backdrop diagnostic ring (always-on). The interpreter records every
 * window rewrite (with the live extent / DL count / camera-X); `ws_backdrop_ring`
 * dumps it. psx_ws_backdrop_ring_json() formats the recent entries into `buf`
 * (returns bytes written, excluding the caller's JSON envelope). */
void psx_ws_backdrop_ring_note(uint32_t pc, int kind, int wcols, uint32_t orig,
                               uint32_t finalv, int extent, int camx, int count,
                               uint32_t base, uint32_t dl);
int  psx_ws_backdrop_ring_json(char *buf, int cap);

/* auto_ui_squash partition dump (`ws_ui_groups`). Reports, for the last UI
 * prepass, each primitive's raw key inputs (CLUT/texpage band/family via op,
 * y, h) alongside its union-find root and final anchor — enough to tell whether
 * two HUD primitives shared a run, and which key component split them if not. */
int  psx_ws_ui_groups_json(char *buf, int cap);

/* Live-tunable backdrop widen amount (ws_backdrop_margin command): <0 whole-row,
 * 0 off, >0 N-column widen. g_ws_bd_from_interp is the interp's one-shot
 * "I will record the rich entry myself" flag (see gpu.c / dirty_ram_interp.c). */
extern int g_ws_bd_margin;
extern int g_ws_bd_from_interp;

/* Native-wide: nonzero if the GP0 prim currently being drawn is sprite-tagged
 * (a character / Tomba / HUD element). The GL 2D-backdrop stretch uses this to
 * EXCLUDE foreground sprites (only the untagged 2D backdrop is stretched). */
int psx_ws_prim_is_tagged(void);

/* Flower-field backdrop data-structure address range + a predicate matching the
 * prim being drawn against it (gp0_cmd_source_addr ∈ [lo,hi]). The dirty-RAM
 * interp sets lo/hi when the backdrop generator runs; the GL stretch gate uses
 * psx_ws_prim_in_backdrop() to identify the flower-field tiles precisely. */
extern uint32_t g_ws_backdrop_lo, g_ws_backdrop_hi;
int psx_ws_prim_in_backdrop(void);

/* Backdrop store-site registry: the runtime registers the [widescreen.backdrop]
 * x_sites here (from game.toml) so the dirty-RAM interpreter applies the same
 * psx_ws_backdrop_x() squash at those `sh` PCs that the recompiler emits into
 * native cache DLLs — overlay code frequently runs interpreted, where the emit
 * can't reach. PCs are masked to the physical (KUSEG) range. */
void psx_ws_set_backdrop_sites(const uint32_t* pcs, int n);
int  psx_ws_is_backdrop_site(uint32_t pc);

/* Live widescreen state for diagnostics (TCP gpu_state). All pointers
 * optional. last_tag_frame/cur_frame let the caller see game_mode freshness. */
typedef struct {
    int      configured;        /* ws_xnum != ws_xden */
    int      active;            /* squash currently applied this frame */
    int      game_mode;         /* tagged char/billboard prim within 2 frames */
    int      present_native_43; /* frame presents pillarboxed 4:3 (FMV/full-2D) */
    int      x_margin;          /* psx_ws_x_margin() right now */
    int      activation_margin; /* psx_ws_activation_margin() right now */
    int      xnum, xden;        /* squash factor */
    int      mode;              /* 0 = off, 1 = squash, 2 = native-wide */
    int      nw_extra;          /* native-wide frame growth (display px), 0 if off */
    uint64_t cur_frame;
    uint32_t last_tag_frame;    /* frame of newest tagged prim */
    uint32_t last_3d_frame;     /* frame of newest shaded prim (diagnostic) */
    uint32_t gte_verts;         /* RTPS/RTPT verts in the last completed frame */
    uint32_t last_world3d_frame;/* newest SUSTAINED world-scale projection frame */
    uint32_t ovh_prims;         /* overhanging polys in the last completed frame */
    uint32_t last_ovh_frame;    /* newest SUSTAINED polygon-overhang frame (the
                                   2D-only-scene classifier's world signal) */
    int      auto_ui_squash;     /* final-OT grouped UI correction configured */
    int      auto_ui_dense;      /* current list classified as a dense menu */
    uint32_t auto_ui_ot_rank;    /* highest populated UI rank in current list */
    uint64_t auto_ui_candidates;
    uint64_t auto_ui_transforms;
    uint64_t aspect_cone_calls;
    uint64_t aspect_cone_43_identity;
    uint64_t aspect_cone_vanilla_keep;
    uint64_t aspect_cone_visible_keep;
    uint64_t aspect_cone_guard_keep;
    uint64_t aspect_cone_hysteresis_keep;
    uint64_t aspect_cone_outside_reject;
    uint64_t aspect_cone_queue_reject;
    uint32_t aspect_cone_queue_highwater[3];
    uint64_t angle_calls;
    uint64_t angle_43_identity;
    uint32_t angle_max_vanilla;
    uint32_t angle_max_widened;
} GpuWsDebug;
void gpu_ws_get_debug(GpuWsDebug* out);

typedef struct {
    uint32_t address;
    uint64_t calls;
    uint64_t identity_43;
    uint64_t vanilla_keep;
    uint64_t visible_keep;
    uint64_t guard_keep;
    uint64_t hysteresis_keep;
    uint64_t outside_reject;
    uint64_t queue_reject;
} GpuWsAspectConeSiteDebug;
/* Exact-site telemetry for separating actor-list and per-child participation
 * predicates. Returns zero when address is not configured. */
int gpu_ws_get_aspect_cone_site_debug(
    uint32_t address, GpuWsAspectConeSiteDebug* out);

/* Diagnostic: force psx_ws_x_margin() to return v (>=0) regardless of state,
 * or -1 to restore the normal computed margin. For live cull-margin sweeps. */
void gpu_ws_set_margin_override(int v);

/* Native-wide HUD corner re-anchoring: allow TAGGED rect-family prims to
 * shift too (live A/B via TCP ws_hud_mode; some HUD composites render
 * through the tagged sprite funnel). */
void gpu_ws_set_nw_hud_tag_rects(int on);

/* Always-on draw-census ring: every drawn primitive records frame / source
 * addr / camera / first-vertex screen pos, so object spawn/despawn and edge
 * culls are observable in data. Dump frames [f0,f1] to a CSV file. */
int      gpu_ws_census_dump(uint32_t f0, uint32_t f1, const char *path);
void     gpu_ws_census_set(int on);
uint64_t gpu_ws_census_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_GPU_H */
