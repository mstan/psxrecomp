#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>

#include "crash_trace.h"
#include "frame_pacing.h"
#include "gpu.h"
#include "host_osd.h"
#include "host_time.h"
#include "latency_ring.h"
#include "psx_rewind.h"
#include "psx_savestate_menu.h"

static uint16_t *s_probe_vram;
const char *g_psx_fatal_reason;
int g_ws_bd_margin;
int g_ws_bd_from_interp;
uint32_t g_ws_backdrop_lo;
uint32_t g_ws_backdrop_hi;

void renderer_probe_set_vram(uint16_t *vram) { s_probe_vram = vram; }

const uint16_t *gpu_get_vram(void) { return s_probe_vram; }
uint16_t gpu_vram_peek(int x, int y)
{
    if (!s_probe_vram) return 0;
    x &= 1023;
    y &= 511;
    return s_probe_vram[y * 1024 + x];
}

void gpu_get_display_info(GpuDisplayInfo *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->display_x = 0;
    out->display_y = 0;
    out->width = 320;
    out->height = 240;
    out->screen_height = 240;
}

int gpu_display_is_depth24(void) { return 0; }
void gpu_display_pixel_rgb(const GpuDisplayInfo *di, uint32_t x, uint32_t y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    (void)di;
    uint16_t p = gpu_vram_peek((int)x, (int)y);
    if (r) *r = (uint8_t)((p & 31u) << 3);
    if (g) *g = (uint8_t)(((p >> 5) & 31u) << 3);
    if (b) *b = (uint8_t)(((p >> 10) & 31u) << 3);
}
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo *di, uint32_t x, uint32_t y)
{
    uint8_t r = 0, g = 0, b = 0;
    gpu_display_pixel_rgb(di, x, y, &r, &g, &b);
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
void gpu_depth24_present_row(const GpuDisplayInfo *di, uint32_t y, uint32_t *out, uint32_t count)
{
    for (uint32_t x = 0; x < count; x++) out[x] = gpu_display_pixel_argb(di, x, y);
}
uint32_t gpu_depth24_rgb_limit(uint32_t display_x, uint32_t crtc_w) { (void)display_x; return crtc_w; }
void gpu_depth24_upload_span_reset(void) {}
int gpu_depth24_present_hold_tick(void) { return 0; }
void gpu_depth24_on_savestate_loaded(void) {}

void host_osd_push(const char *msg, int duration_ms) { (void)msg; (void)duration_ms; }
void host_osd_set_status(const char *msg) { (void)msg; }
void host_osd_show_volume(int percent, int duration_ms) { (void)percent; (void)duration_ms; }
int host_volume_get(void) { return 100; }
void host_volume_set(int percent) { (void)percent; }
int host_volume_adjust(int delta) { (void)delta; return 100; }
int host_osd_needs_present(void) { return 0; }
int host_osd_image(const uint32_t **pixels, int *w, int *h) { if (pixels) *pixels = NULL; if (w) *w = 0; if (h) *h = 0; return 0; }
int host_osd_volume_image(const uint32_t **pixels, int *w, int *h) { if (pixels) *pixels = NULL; if (w) *w = 0; if (h) *h = 0; return 0; }
void host_osd_present_done(void) {}
void host_osd_draw_sdl(struct SDL_Renderer *renderer) { (void)renderer; }

void psx_savestate_menu_set_state(int open, int selected_slot) { (void)open; (void)selected_slot; }
void psx_savestate_menu_note_slots_changed(void) {}
int psx_savestate_menu_needs_present(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h) { if (pixels) *pixels = NULL; if (w) *w = 0; if (h) *h = 0; return 0; }

void psx_rewind_set_depth(uint32_t depth) { (void)depth; }
void psx_rewind_set_interval(uint32_t interval) { (void)interval; }
void psx_rewind_set_enabled(int enabled) { (void)enabled; }
void psx_rewind_configure(uint32_t bios_checksum, uint32_t entry_pc) { (void)bios_checksum; (void)entry_pc; }
void psx_rewind_shutdown(void) {}
int psx_rewind_enabled(void) { return 0; }
int psx_rewind_is_open(void) { return 0; }
int psx_rewind_needs_present(void) { return 0; }
void psx_rewind_note_frame(void) {}
void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc) { (void)cpu; (void)resume_pc; }
int psx_rewind_toggle(void) { return 0; }
int psx_rewind_cancel(void) { return 0; }
int psx_rewind_accept(void) { return 0; }
void psx_rewind_move(int delta) { (void)delta; }
void psx_rewind_nav_held(int left_down, int right_down, int accept_down, int cancel_down, uint32_t now_ms) { (void)left_down; (void)right_down; (void)accept_down; (void)cancel_down; (void)now_ms; }
void psx_rewind_present_tick(uint32_t now_ms) { (void)now_ms; }
int psx_rewind_overlay_image(const uint32_t **pixels, int *w, int *h) { if (pixels) *pixels = NULL; if (w) *w = 0; if (h) *h = 0; return 0; }
float psx_rewind_slide(void) { return 0.0f; }

uint64_t psx_host_mono_ms(void) { return (uint64_t)SDL_GetTicks(); }
void psx_host_sleep_micros(unsigned usec) { SDL_DelayNS((Uint64)usec * 1000u); }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
int psx_present_vsync_owns_cadence(void) { return 0; }

void latency_ring_frame_begin(void) {}
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void latency_ring_restamp_input(void) {}
void latency_ring_set_backend(const char *name) { (void)name; }
void latency_ring_set_present_mode(int mode) { (void)mode; }
int latency_ring_summary_json(char *buf, int buf_size, int window) { (void)window; return snprintf(buf, (size_t)buf_size, "{}"); }
int latency_ring_dump_json(char *buf, int buf_size, int max_frames) { (void)max_frames; return snprintf(buf, (size_t)buf_size, "[]"); }

void psx_fatal_halt(const char *reason)
{
    g_psx_fatal_reason = reason ? reason : "renderer_probe_fatal";
    fprintf(stderr, "psx_fatal_halt: %s\n", g_psx_fatal_reason);
    abort();
}

int psx_ws_prim_is_tagged(void) { return 0; }
int psx_ws_prim_in_backdrop(void) { return 0; }
int ws_native_wide_active(void) { return 0; }
int ws_nw_extra(void) { return 0; }
int ws_nw_present_width(void) { return 320; }
int gpu_ws_present_native_43(void) { return 0; }
int gpu_last_frame_vertical_split_screen(void) { return 0; }
void gpu_vertical_split_debug(int *active, int *left_age, int *right_age) { if (active) *active = 0; if (left_age) *left_age = 0; if (right_age) *right_age = 0; }
void gpu_ws_set_netplay_local_viewport(int enabled, int slot) { (void)enabled; (void)slot; }
int gpu_ws_netplay_local_viewport_base_x(void) { return 0; }
int gpu_ws_netplay_local_viewport_width(void) { return 320; }
int gpu_ws_nw_flat_backdrop_enabled(void) { return 0; }
int psx_ws_backdrop_preload(void) { return 0; }
uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols) { (void)is_end; (void)window_cols; return orig; }
int psx_ws_backdrop_x(int x) { return x; }
int psx_ws_x_margin(void) { return 0; }
int psx_ws_activation_margin(void) { return 0; }

void gpu_texture_correction_stats(uint64_t *attempts, uint64_t *armed, uint64_t *no_correction, uint64_t *no_source, uint64_t *no_depth)
{
    if (attempts) *attempts = 0;
    if (armed) *armed = 0;
    if (no_correction) *no_correction = 0;
    if (no_source) *no_source = 0;
    if (no_depth) *no_depth = 0;
}

#include "frame_interpolation.h"

uint64_t s_frame_count;
int g_ws_tex_edge_pct;

int psx_netplay_active(void) { return 0; }
void psx_ws_dbg_gate_frame_snapshot(void) {}
int present_shot_take(char *out, int n) { if (out && n > 0) out[0] = '\0'; return 0; }
void present_shot_done(int ok) { (void)ok; }

void frame_interpolation_schedule_reset(FrameInterpolationSchedule *schedule)
{
    if (schedule) memset(schedule, 0, sizeof(*schedule));
}

int frame_interpolation_schedule_begin(FrameInterpolationSchedule *schedule,
                                       uint64_t now,
                                       uint64_t frequency,
                                       double source_hz,
                                       double target_hz)
{
    (void)now;
    (void)frequency;
    (void)source_hz;
    (void)target_hz;
    frame_interpolation_schedule_reset(schedule);
    return 0;
}

int frame_interpolation_schedule_next(FrameInterpolationSchedule *schedule,
                                      uint64_t now,
                                      uint64_t *deadline,
                                      float *alpha)
{
    (void)schedule;
    (void)now;
    if (deadline) *deadline = 0;
    if (alpha) *alpha = 1.0f;
    return 0;
}

uint64_t frame_interpolation_schedule_end(const FrameInterpolationSchedule *schedule)
{
    (void)schedule;
    return 0;
}
