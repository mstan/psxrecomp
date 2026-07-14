#include "gpu_gl_renderer.h"
#include "gpu_render.h"

#include <string.h>

/* These enhancement/debug controls historically lived in the GL translation
 * unit even though the software backend also consumes them. Keep their shared
 * defaults when GL itself is unavailable. */
int g_ws_bd_stretch_on = 1;
int g_ws_bd_stretch_pct = 0;
int g_ws_bd_phase_thresh = 24;
int g_ws_bd_phase_mode = 1;
int g_bdg_applied = 0, g_bdg_prims = 0, g_bdg_clearx = -999999;
int g_bdg_cur = 0, g_bdg_base = 0, g_bdg_w = 0, g_bdg_off = 0;

int gl_renderer_init_context(struct SDL_Window *win) { (void)win; return 0; }
void gl_renderer_set_swap_interval(int interval) { (void)interval; }
void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz) {
    (void)enabled; (void)host_hz; (void)target_hz;
}
void gl_renderer_set_interpolation_suspended(int suspended) { (void)suspended; }
void gl_renderer_interpolation_diag(int *enabled, int *suspended,
                                    int *history_frames,
                                    double *host_hz, double *target_hz,
                                    uint64_t *swaps) {
    if (enabled) *enabled = 0;
    if (suspended) *suspended = 0;
    if (history_frames) *history_frames = 0;
    if (host_hz) *host_hz = 0.0;
    if (target_hz) *target_hz = 0.0;
    if (swaps) *swaps = 0;
}
void gl_renderer_runtime_diag(uint64_t out[6]) {
    if (out) memset(out, 0, 6 * sizeof(uint64_t));
}
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h,
                         int linear, int force_4_3) {
    (void)pixels; (void)src_w; (void)src_h; (void)linear; (void)force_4_3;
}
void gl_renderer_present_blank(void) {}
void gl_renderer_sync_cpu(void) {}
void gl_renderer_present_vram(int x, int y, int w, int h, int linear,
                              int force_4_3) {
    (void)x; (void)y; (void)w; (void)h; (void)linear; (void)force_4_3;
}
int gl_renderer_present_wide_fbo(int x, int y, int h, int linear) {
    (void)x; (void)y; (void)h; (void)linear; return 0;
}
void gl_renderer_set_display_aspect(int num, int den) { (void)num; (void)den; }
void gl_renderer_shutdown(void) {}
int gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out) {
    (void)x; (void)y; (void)w; (void)h; (void)out; return 0;
}
void gl_renderer_diag(int *dirty, int pending[5], int pack[5]) {
    if (dirty) *dirty = 0;
    if (pending) memset(pending, 0, 5 * sizeof(int));
    if (pack) memset(pack, 0, 5 * sizeof(int));
}
uint64_t gl_renderer_coh_total(void) { return 0; }
int gl_renderer_coh_get(uint64_t seq, GlCohEvent *out) {
    (void)seq; (void)out; return 0;
}
uint64_t gl_renderer_pres_total(void) { return 0; }
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out) {
    (void)seq; (void)out; return 0;
}
int gl_renderer_perf_aggregate(int wide_filter, double out[18]) {
    (void)wide_filter; if (out) memset(out, 0, 18 * sizeof(double)); return 0;
}
void gl_renderer_set_ws_ablate(int mode) { (void)mode; }
int gl_renderer_get_ws_ablate(void) { return 0; }
uint64_t gl_renderer_perf_prim_split(double *fraction) {
    if (fraction) *fraction = 0.0; return 0;
}
void gl_renderer_batch_diag(uint64_t out[8]) {
    if (out) memset(out, 0, 8 * sizeof(uint64_t));
}
void gl_renderer_set_wide_fast(int on) { (void)on; }
int gl_renderer_get_wide_fast(void) { return 0; }
int gl_renderer_vram_diff(uint32_t *count, int bbox[4], int samples[8][2],
                          uint16_t samples_px[8][2]) {
    (void)count; (void)bbox; (void)samples; (void)samples_px; return 0;
}
const GpuRenderBackend *gl_backend_get(void) { return NULL; }
