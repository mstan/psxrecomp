#ifndef PSX_HOST_OSD_H
#define PSX_HOST_OSD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-only OSD (toasts + volume bar). Not part of guest VRAM / savestates.
 *
 * Visual overlays (toasts, volume bar, draw helpers) are compiled active only
 * when RECOMP_LAUNCHER is defined (recomp-ui linked via PSX_RECOMP_UI=ON).
 * host_volume_get/set/adjust still work without the launcher; adjust is silent
 * when OSD is gated off.
 */

/* Top-left text toast. duration_ms <= 0 uses 2000. */
void host_osd_push(const char *msg, int duration_ms);

/* Persistent top-left status text. Passing NULL/empty clears it. */
void host_osd_set_status(const char *msg);

/* Right-side vertical volume bar (percent clamped 0..100). */
void host_osd_show_volume(int percent, int duration_ms);

/* Host master volume 0..100 (applied by the audio pump). */
int  host_volume_get(void);
void host_volume_set(int percent);          /* silent clamp */
int  host_volume_adjust(int delta);         /* clamp, show bar, return new % */

/* 1 while a toast/bar is visible, or one more frame is needed to clear it. */
int host_osd_needs_present(void);

/* ARGB8888 images for the active overlays. Valid until the next host_osd_*
 * call. Returns 1 if there are pixels to draw. After compositing both (or
 * deciding neither is active), call host_osd_present_done(). */
int host_osd_image(const uint32_t **pixels, int *w, int *h);          /* text */
int host_osd_volume_image(const uint32_t **pixels, int *w, int *h);   /* bar */
void host_osd_present_done(void);

/* Software / SDL_Renderer path: draw overlays over the current backbuffer. */
struct SDL_Renderer;
void host_osd_draw_sdl(struct SDL_Renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif /* PSX_HOST_OSD_H */
