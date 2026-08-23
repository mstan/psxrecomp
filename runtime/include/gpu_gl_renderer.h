#ifndef PSX_GPU_GL_RENDERER_H
#define PSX_GPU_GL_RENDERER_H

/* GL backend context + present entry points, called from main.cpp's window
 * setup and present path when [video] renderer = "opengl".  The backend's
 * rasterization vtable is obtained separately via gl_backend_get()
 * (gpu_render.h).  SDL_Window is forward-declared (SDL typedefs it from this
 * same struct tag) so this header needs no SDL include. */

#include <stdint.h>

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Create the GL context on a window made with SDL_WINDOW_OPENGL.
 * Returns 1 on success, 0 to fall back to the SDL_Renderer present path. */
int  gl_renderer_init_context(struct SDL_Window *win);

/* Set the GL swap interval / vsync mode (1=vsync, 0=immediate, -1=adaptive).
 * Safe before or after context creation; applies live when a context exists. */
void gl_renderer_set_swap_interval(int interval);

/* Presentation-only frame interpolation. High-refresh sub-presents blend the
 * two most recent stable display images; guest simulation timing is unchanged. */
void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz,
                                   int blend_mode);
void gl_renderer_set_interpolation_suspended(int suspended);
/* Async present: the interpolation worker thread owns quad+OSD+SwapWindow
 * and re-presents the newest captured frame at host cadence (no blending);
 * the sim thread pays only flush + capture copy + fence. Netplay uses this
 * to take the present path off the lockstep critical path. */
void gl_renderer_set_present_async(int enabled, double host_hz);
void gl_renderer_interpolation_diag(int *enabled, int *suspended,
                                    int *history_frames,
                                    double *host_hz, double *target_hz,
                                    uint64_t *swaps);
/* Cumulative CPU-upload diagnostics: calls, rects, pixels, conversion ticks,
 * texture-upload ticks, FBO-draw ticks. Active only with PSX_RUNTIME_PERF_DIAG. */
void gl_renderer_runtime_diag(uint64_t out[6]);

/* Present an ARGB8888 image (BGRA byte order) as a letterboxed quad + swap.
 * Used for 24-bit (FMV) frames and the PSX_GL_FORCE_CPU_PRESENT diagnostic.
 * force_4_3 = pillarbox at native 4:3 even on a wide display aspect (FMVs
 * are authored 4:3 and get no GTE squash to compensate the stretch).
 * content_w: if 0 < content_w < src_w, only columns [0, content_w) are shown
 * (left-aligned in the letterbox; the rest stays cleared black). Used to hide
 * a trailing depth24 margin without changing CRTC width / stretching. */
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear,
                         int force_4_3, int content_w);

/* Pillarbox edge fill: on a 4:3-pinned present, extend the frame's own left and
 * right edge columns across the black margins instead of leaving them black.
 *
 * The 4:3 content stays exactly where it is, at its own scale, undistorted --
 * only the margins change. It exists for wide displays showing 2D screens
 * (menus, title, loading) that a 3D-gated widescreen layer deliberately keeps
 * at 4:3: on 32:9 those margins are most of the screen, and a flat-gradient
 * backdrop reads as a small floating island. It is a look, not a correctness
 * fix, and it is wrong where the edge pixels are busy, so it is opt-in and the
 * crop path (content_w) is excluded. */

/* Bezel art shown in the letterbox/pillarbox margins. Takes RGBA8 pixels; the
 * caller owns them and may free them on return. Passing NULL clears it.
 * Returns 0 only if a texture could not be created. */
int  gl_renderer_set_bezel(const void *rgba, int w, int h);
int  gl_renderer_has_bezel(void);
void gl_renderer_set_pillarbox_edge_fill(int enabled);
int  gl_renderer_pillarbox_edge_fill(void);

/* PGXP depth ([video] pgxp_depth): depth-test polygons using the per-vertex W
 * PGXP recovered, instead of relying solely on the ordering table.
 *
 * The PS1 sorts per PRIMITIVE, by one averaged depth quantised into buckets, so
 * interpenetrating or near-coplanar polygons resolve by submission order — an
 * answer the hardware cannot get right. Only primitives whose W provenance is
 * proven participate; everything else (2D, UI, anything the provenance test
 * rejects) neither tests nor writes, since it has no meaningful depth.
 *
 * Off is bit-identical to a build without the feature: the vertex program's z
 * term is 0, the test stays disabled, and the buffer is never cleared. */
void gl_renderer_set_pgxp_depth(int enabled);
int  gl_renderer_pgxp_depth(void);

/* Clear to black + swap (display-disabled frame). */
void gl_renderer_present_blank(void);

/* §33: re-present the last Live frame captured before Swap (or from a VRAM
 * snapshot when interpolation owned the last present). Used during rollback
 * resim so the window keeps a wall-clock present cadence without reading
 * mid-resim VRAM. Returns 1 if a Swap happened, 0 if no hold is available. */
/* VRAM banks — one GPU-side VRAM per emulated console. Dual-console switching
 * activates the peer's bank instead of reading 1 MiB back through
 * gr_vram_transfer_out and re-uploading it on every machine switch. */
int gl_renderer_vram_bank_create(int slot);
int gl_renderer_vram_bank_activate(int slot);
int gl_renderer_vram_bank_live(void);
int gl_renderer_vram_bank_seed(int dst, int src);

int gl_renderer_present_hold_last(void);

/* Sync the authoritative FBO down to CPU VRAM if the GPU side is ahead (else
 * a no-op). Screenshots and the debug server call this before reading CPU
 * VRAM. Do NOT use before 24-bit (FMV) scanout — a full readback can clobber
 * packed RGB888 MDEC bytes in the CPU mirror (use flush_cpu_uploads instead). */
void gl_renderer_sync_cpu(void);

/* Land pending CPU→VRAM uploads into the FBO without reading the FBO back.
 * Safe before 24-bit (FMV) CPU scanout of the mirror. */
void gl_renderer_flush_cpu_uploads(void);

/* Mark the whole display dirty, drop present-path latches, reset frame-
 * interpolation history, and force the next several SwapWindow calls even if
 * VRAM tiles match the last present. Call after savestate restore so a
 * reloaded identical frame still reaches the window (double/triple buffer). */
void gl_renderer_invalidate_present(void);

/* After savestate restore: push the CPU VRAM mirror into the GL FBO. Needed
 * when the load happened while GP1 depth24 was on — the normal path skips
 * framebuffer-sized uploads, which also skipped the full-VRAM boot_state
 * blit and left post-FMV menus without texture pages. */
void gl_renderer_restage_vram_after_savestate(void);

/* Netplay dual-raster: every GP0 also writes software VRAM @ 1× (snaps /
 * digests / GPUREAD authority) while the OpenGL hr FBO keeps settings-scale
 * SSAA for present-only. Never enables glReadPixels; CPU stays current. */
void gl_renderer_set_cpu_auth_dual(int on);

/* FMV present reconstruction, settings.toml [video] fmv_filter. Takes the
 * config enum VIDEO_FMV_FILTER_* (0 nearest, 1 bilinear, 2 sharp, 3 bicubic).
 * Only consulted while video antialiasing is on; AA off is always nearest.
 * PSX_FMV_FILTER overrides this for one run. */
void gl_renderer_set_fmv_filter(int cfg_value);
int  gl_renderer_cpu_auth_dual(void);

/* Post-savestate freeze probe: skip/swap/dirty-mark counters (GL present path).
 * take() returns deltas since the previous take/reset. Safe no-ops when GL is
 * inactive. rect_dirty tests the current present-tile dirty bits. */
void gl_renderer_present_probe_reset(void);
void gl_renderer_present_probe_take(uint64_t *skip_delta, uint64_t *swap_delta,
                                    uint64_t *dirty_mark_delta,
                                    int *force_remaining);
int  gl_renderer_present_rect_dirty(int disp_x, int disp_y, int w, int h);

/* THE present path for 15-bit frames: blit the display region straight from
 * the authoritative VRAM FBO into a letterboxed rect (no readback).
 * Deterministic — used for every 15-bit frame. linear = filter on scale.
 * force_4_3 pins to native 4:3 (15-bit MDEC FMV frames on a wide aspect). */
void gl_renderer_present_vram(int disp_x, int disp_y, int w, int h, int linear,
                              int force_4_3);

/* GPU-direct native-wide present: blit the displayed buffer's wide FBO (key =
 * disp_x) straight to the window, no CPU readback. Returns 0 if no wide surface
 * exists for disp_x (caller falls back to the CPU readout path). */
int gl_renderer_present_wide_fbo(int disp_x, int disp_y, int disp_h, int linear);

/* Display aspect for the present letterbox (default 4:3). A wide aspect
 * stretches the 4:3 frame; pair with gte_set_display_aspect (cpu_state.h)
 * for the widescreen field-of-view hack. */
void gl_renderer_set_display_aspect(int num, int den);

/* Select full native-wide mirror rendering instead of the centre-splice fast
 * path. Textured edge expansion needs the complete mirror surface. */
void gl_renderer_set_wide_fast(int on);

void gl_renderer_shutdown(void);

/* Diagnostics (debug server): read GPU-side VRAM without touching the CPU
 * array; report coherency flags + dirty rects. fbo_peek returns 0 when the
 * GL pipeline is inactive. */
int  gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out);
void gl_renderer_diag(int *gpu_dirty, int pending[5], int pack[5]);

/* Always-on coherency event ring (debug server "gl_coh_ring"): every upload
 * flush, fill, copy, draw bbox, pack, full readback, present, and probe
 * perturbation, with rect + frame. An op that flushes internally records its
 * own event AFTER the FLUSH it caused (the event after a FLUSH = trigger). */
enum {
    GL_COH_FLUSH    = 1,   /* CPU->FBO upload flush (pending box)     */
    GL_COH_FILL     = 2,   /* GP0(02) fill rect                       */
    GL_COH_COPY_SRC = 3,   /* GP0(80) copy, source rect               */
    GL_COH_COPY     = 4,   /* GP0(80) copy, dest rect                 */
    GL_COH_DRAW     = 5,   /* drawn prim bbox (clipped to draw area)  */
    GL_COH_PACK     = 6,   /* hr FBO -> raw mirror pack (dirty box)   */
    GL_COH_ENSURE   = 7,   /* full FBO -> CPU VRAM readback           */
    GL_COH_PRESENT  = 8,   /* 15-bit present blit (display rect)      */
    GL_COH_UPLOAD   = 9,   /* bulk CPU->VRAM transfer_in dest rect    */
    GL_COH_PEEK     = 10,  /* gl_fbo_peek probe (perturbs: flushes)   */
    GL_COH_DIFF     = 11,  /* gl_vram_diff probe (perturbs: flushes)  */
};

typedef struct {
    uint32_t frame;
    uint8_t  kind;
    int16_t  x0, y0, x1, y1;   /* native VRAM coords, inclusive */
} GlCohEvent;

uint64_t gl_renderer_coh_total(void);
/* Fetch event by absolute sequence number; 0 if evicted or out of range. */
int gl_renderer_coh_get(uint64_t seq, GlCohEvent *out);

/* Always-on present ring (debug server "gl_present_ring"): EVERY SwapWindow —
 * including blank (display-disabled) and CPU-quad presents, which the coherency
 * ring does not record — with the path taken, source display rect, letterbox
 * dest rect, a glGetError sample, wall-clock ms, and a backbuffer pixel sampled
 * at the letterbox centre right before the swap (the ground truth for "did this
 * swap present black"). */
enum {
    GL_PRES_VRAM  = 0,   /* 15-bit FBO blit present (gl_renderer_present_vram) */
    GL_PRES_WIDE  = 1,   /* native-wide FBO blit present                       */
    GL_PRES_CPU   = 2,   /* CPU-readout quad present (24-bit FMV / forced)     */
    GL_PRES_BLANK = 3,   /* display-disabled black present                     */
    GL_PRES_INTERP = 4,  /* host-refresh interpolation sub-present              */
};

typedef struct {
    uint32_t frame;        /* s_frame_count at swap                        */
    uint32_t t_ms;         /* SDL_GetTicks() at swap                       */
    uint8_t  path;         /* GL_PRES_*                                    */
    uint8_t  px_r, px_g, px_b; /* backbuffer sample at letterbox centre    */
    uint16_t glerr;        /* glGetError() drained just before the swap    */
    int16_t  dx, dy, w, h; /* source display rect (native px; 0 for blank) */
    int16_t  lx, ly, lw, lh; /* letterbox dest rect (window px)            */
    uint8_t  src_r, src_g, src_b, src_valid; /* blit SOURCE (hr FBO) sample
                                              * at the display-rect centre  */
} GlPresEvent;

uint64_t gl_renderer_pres_total(void);
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out);

/* frame_perf: aggregate the per-frame GPU/CPU phase-timing ring (debug server
 * "frame_perf"). wide_filter: -1 = all frames, 0 = 4:3 present, 1 = native-wide.
 * Fills out[13]: [0]=count, [1]=total_ms avg, [2]=total_ms max, [3]=emu_cpu_ms avg
 * (frame minus the present call), [4]=present_wall_ms avg, [5]=scene_gpu_ms avg,
 * [6]=scene_gpu_ms max, [7]=present_gpu_ms avg, [8]=present_gpu_ms max,
 * [9]=scene primitives/frame avg (pre double-draw), [10]=mirror_gpu_ms avg (of
 * scene_gpu, the native-wide mirror passes; GL_TIMESTAMP pairs), [11]=mirror_gpu_ms
 * max, [12]=mirror passes/frame avg, [13]=CPU wall in flush_tex_batch avg,
 * [14]=CPU wall in glb_wide_* avg, [15]=batches/frame avg, [16]=wide target
 * sets/frame avg, [17]=wide FBO creations/frame avg. GPU phases are true
 * GL_TIME_ELAPSED times (CPU-overhead independent). Returns the count. */
int gl_renderer_perf_aggregate(int wide_filter, double out[18]);

/* Native-wide mirror ablation (perf attribution, debug cmd gl_ws_ablate):
 * 0 = normal, 1 = skip the whole mirror pass (incl. wide_clear), 2 = full mirror
 * state churn without the draw calls, 3 = mirror draws stay on the hr FBO (no
 * per-pass FBO rebind; diagnostic only — corrupts both surfaces' content). */
void gl_renderer_set_ws_ablate(int mode);
int  gl_renderer_get_ws_ablate(void);

/* Cumulative textured fraction of scene primitives since boot (flat vs textured
 * batching decision). Sets *out_tex_frac; returns total prim count. */
uint64_t gl_renderer_perf_prim_split(double *out_tex_frac);
/* Cumulative textured-batch diagnostics: total, then flushes caused by
 * isolation, blend-mode, mask, filter, backdrop-gate, texture-window, capacity. */
void gl_renderer_batch_diag(uint64_t out[8]);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_GL_RENDERER_H */
