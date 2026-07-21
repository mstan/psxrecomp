#ifndef PSXRECOMP_PSX_CYCLES_H
#define PSXRECOMP_PSX_CYCLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSX guest CPU cycle clock — the single source of truth for guest-
 * visible time. Peripherals derive all schedules from this counter. */
extern uint64_t psx_cycle_count;

/* Deadline-device model bookkeeping (written by psx_cycles.c). Hot path
 * reads these from the inlined psx_advance_cycles below. */
extern uint64_t psx_next_service_cycle; /* absolute; 0 = dirty / recompute */
extern int      psx_in_device_service;  /* re-entrancy guard */
extern int      g_event_step_conservative;

/* Event-deadline device model: catch every device up to the charged
 * guest-cycle position and force a deadline recompute. memory.c calls
 * this at the top of every device-MMIO read/write. */
void psx_devices_mmio_sync(void);
void psx_devices_service_to_now(void);

/* Rare/slow advance path (COSIM, conservative 1-cycle stepping, lockstep). */
void psx_advance_cycles_slow(uint32_t cycles);

/* Sparse throttle fires (watchdog / PC sample) — not on the per-charge path. */
extern uint32_t psx_watchdog_throttle;
extern uint32_t psx_pc_sample_throttle;
void psx_cycles_watchdog_fire(void);
void psx_cycles_pc_sample_fire(void);

/* Lockstep replay flag (defined in dirty_ram_interp.c). */
extern int g_ls_replay_active;

/* Advance guest time. The common production path is inlined: bump the
 * counter and only service devices when the next event deadline is due.
 * Guest-visible timing is unchanged (service_to_now replays exact events).
 *
 * Watchdog / PC-sample throttles live in psx_devices_service_to_now (fired
 * on the HARD_CAP / event cadence, ≥ every 16K guest cycles) — not on every
 * per-instruction charge. MotK VLC issues millions of advances/s; two add+
 * branch pairs there were pure host tax. */
static inline void psx_advance_cycles(uint32_t cycles) {
    if (cycles == 0u) return;
#if defined(PSX_COSIM)
    psx_advance_cycles_slow(cycles);
    return;
#else
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(g_ls_replay_active | g_event_step_conservative, 0)) {
#else
    if (g_ls_replay_active || g_event_step_conservative) {
#endif
        psx_advance_cycles_slow(cycles);
        return;
    }
    if (psx_in_device_service) {
        psx_cycle_count += (uint64_t)cycles;
        return;
    }
    psx_cycle_count += (uint64_t)cycles;
    if (psx_next_service_cycle == 0u ||
        psx_cycle_count >= psx_next_service_cycle) {
        psx_devices_service_to_now();
    }
#endif
}

/* Read accessor for telemetry. */
uint64_t psx_get_cycle_count(void);

/* Idle-loop cycle skip (see psx_cycles.c "Idle-loop cycle skip"). */
struct CPUState;
void psx_idle_note_check(struct CPUState *cpu, uint32_t check_pc);
int  psx_idle_skip_is_enabled(void);
extern int      g_idle_skip_enabled;
extern uint64_t g_idle_skip_count;
extern uint64_t g_idle_skip_cycles;
extern uint32_t g_idle_skip_last_pc;
extern uint32_t g_idle_skip_last_quantum;

/* Save-state restore: re-anchor the deadline device model after psx_cycle_count
 * is overwritten from a snapshot. */
void psx_cycles_resync_after_restore(void);

/* Soft rematch / session_reboot: zero the guest clock and deadline bookkeeping.
 * Soft-exit longjmps out of vblank (inside psx_devices_service_to_now) leave
 * psx_in_device_service stuck at 1 and a huge leftover cycle count — without
 * this reset the next match never services devices or fires vblanks. */
void psx_cycles_reset_for_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_CYCLES_H */
