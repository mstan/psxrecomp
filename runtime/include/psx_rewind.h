#ifndef PSX_REWIND_H
#define PSX_REWIND_H

/*
 * Local rewind ring + bottom filmstrip overlay (host only).
 *
 * Uses retcomm-rbengine snap_ring + boot_state blobs. Separate from netplay
 * g_snaps. Disabled while psx_netplay_active().
 *
 * Keyboard: F8 (config.ini [KeyMap] Rewind). Controller: View/Back or L3.
 * While open: D-pad / left-stick Left/Right, Cross load, Circle/Back close.
 *
 * OFF by default. The ring holds up to 200 whole-machine snapshots (2 MB RAM +
 * 1 MB VRAM + 512 KB SPU RAM each) and captures one every rewind_interval
 * frames, which is not a cost to hand a low-end host for a feature a session
 * may never open. Turn it on in settings.toml or the launcher's Display card.
 *
 * Env: PSX_REWIND=1 enables, PSX_REWIND=0 disables; either outranks the UI.
 *      PSX_REWIND_INTERVAL (1/4/8/12/15, default 15);
 *      PSX_REWIND_FMV_INTERVAL (frames while depth24/MDEC/XA, default 4);
 *      PSX_REWIND_DEPTH (50/100/150/200, default 50, max 200).
 * settings.toml [video] rewind / rewind_depth / rewind_interval (env still wins).
 */

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snap count kept in the local rewind ring. UI values: 25 / 50 / 75 / 100. */
void psx_rewind_set_depth(uint32_t depth);
void psx_rewind_set_interval(uint32_t interval);
/* Settings/launcher preference. Call before psx_rewind_configure(); to change
 * it mid-session, call this then configure() (on) or shutdown() (off). */
void psx_rewind_set_enabled(int enabled);

void psx_rewind_configure(uint32_t bios_checksum, uint32_t entry_pc);
void psx_rewind_shutdown(void);

int  psx_rewind_enabled(void);
int  psx_rewind_is_open(void);
/* 1 while open or slide animation still visible. */
int  psx_rewind_needs_present(void);

/* Guest vblank: bump frame counter / schedule capture when not paused. */
void psx_rewind_note_frame(void);

/* Safe BB-edge (beside savestate_poll): capture due snap / apply load. */
void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc);

int  psx_rewind_toggle(void);          /* open/close; 1 if state changed */
int  psx_rewind_cancel(void);          /* close without load */
int  psx_rewind_accept(void);          /* stage load of selected snap */
void psx_rewind_move(int delta);       /* -1 / +1 selection */

/* Edge-triggered nav with repeat. *_down is 1 while held. */
void psx_rewind_nav_held(int left_down, int right_down, int accept_down,
                         int cancel_down, uint32_t now_ms);

void psx_rewind_present_tick(uint32_t now_ms);

/* Bottom-panel ARGB8888 (little-endian). Valid until next rewind call. */
int  psx_rewind_overlay_image(const uint32_t **pixels, int *w, int *h);
/* 0..1 slide (0 = off-screen below, 1 = docked). */
float psx_rewind_slide(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_REWIND_H */
