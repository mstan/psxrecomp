#ifndef PSX_SELFCHECK_H
#define PSX_SELFCHECK_H

/*
 * Solo rollback resim self-check (PSX_RB_SELFCHECK=1, offline only).
 *
 * Periodically during normal offline play: snapshot the machine at a safe
 * BB edge, keep playing N ticks while recording the per-boundary pad rows
 * the sampler applied to SIO plus the full netplay digest partitions, then
 * rewind to the snapshot and resim the same rows, comparing digests at every
 * boundary. Any live-vs-resim fork is exactly the class of determinism bug
 * that otherwise only surfaces as a doomed netplay episode in a two-peer
 * soak (SIO pace, GTE fold, present-edge PC, v0 restore, ...).
 *
 * Uses the same machinery as netplay rollback: boot_state raw buffer snaps,
 * netplay_state_digest partitions, RB restore recipe (cycle/IRQ resync, SPU
 * CD FIFO reset, NO cdrom_accelerate, light frontend hook). No recomp-net
 * dependency; fully offline, single process.
 *
 * Env:
 *   PSX_RB_SELFCHECK=1            enable
 *   PSX_RB_SELFCHECK_INTERVAL=N   boundaries between windows (default 600)
 *   PSX_RB_SELFCHECK_SPAN=N       window length in boundaries (default 32)
 *   PSX_RB_SELFCHECK_FAULT=1      corrupt one recorded digest per window to
 *                                 prove the comparator/report path
 *   PSX_RB_SELFCHECK_PRIME=N      throwaway resims before counted passes
 *                                 (default 1, max 4) — clears live-RECORD ambient
 *   PSX_RB_SELFCHECK_MASH=1       synthetic fighter-style button spam on live
 *                                 boundaries (IDLE/SAVE_WAIT/RECORD). Recorded
 *                                 rows replay during resim. Headless-safe.
 *   PSX_RB_SELFCHECK_MASH_SEED=N  xorshift seed (default 0xC0FFEE)
 *   PSX_RB_SELFCHECK_MASH_RATE=N  % chance to start a new press/release each
 *                                 boundary when not mid-hold (default 75)
 *
 * Verdict = warm resim#2 vs #3 (peer episode path). Cold #1 vs #2 is dig only.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Startup bind (after savestate_configure; needs HLE scheduler for the
 * rewind longjmp). Reads env; no-op unless PSX_RB_SELFCHECK=1. */
void psx_selfcheck_init(struct CPUState *cpu, uint32_t bios_checksum,
                        uint32_t entry_pc);

int  psx_selfcheck_enabled(void);

/* 1 while a window is open (record/replay/load). Offline GPU present stays
 * immediate — full BB-edge present-defer skews clk/tim between warm peers. */
int  psx_selfcheck_defer_present(void);

/* 1 while the resim replay is running: skip wall pacer / host audio pump /
 * host CD boost exactly like netplay resim. */
int  psx_selfcheck_resim_active(void);

/* 1 during replay: main.cpp must skip live pad sampling entirely (the
 * self-check republishes the recorded rows itself). */
int  psx_selfcheck_replay_input(void);

/* 1 during record or replay: main.cpp must skip the mid-frame low-latency
 * input re-sample so pads stay boundary-latched (netplay tick semantics). */
int  psx_selfcheck_input_locked(void);

/* Offline pad sampler notes what it applied to SIO this boundary (called
 * from apply_pad_slot_to_sio / apply_input_override_to_sio). */
void psx_selfcheck_note_pad(int slot, uint16_t buttons, uint8_t lx, uint8_t ly,
                            uint8_t rx, uint8_t ry, uint8_t analog);

/* 1 when mash owns this boundary's P1 pad word (active-low). Latched once
 * per offline vblank so mid-frame resampling sees a stable word. */
int psx_selfcheck_mash_override(uint16_t *out_buttons);

/* Once per offline vblank boundary, after pad sampling (main.cpp present
 * body). defer_window!=0 postpones opening a new window (e.g. multitap
 * arming still pending). */
void psx_selfcheck_finish_frame(int defer_window);

/* Safe BB-edge poll (beside savestate_poll in interrupts.c): performs the
 * deferred snapshot save. Loads only as a fallback when span-end flush did
 * not run (e.g. headless paths that skip present). Near-free when idle. */
void psx_selfcheck_poll(struct CPUState *cpu, uint32_t resume_pc);

/* After present-body C++ RAII: if a window span just ended, rewind+resume
 * immediately at that VBlank boundary (no post-span BB tail). Longjmps. */
void psx_selfcheck_flush_load(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SELFCHECK_H */
