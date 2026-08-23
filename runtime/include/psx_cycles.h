#ifndef PSXRECOMP_PSX_CYCLES_H
#define PSXRECOMP_PSX_CYCLES_H

#include <stddef.h>
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

/* Diagnostic replay clock: advance the CPU-visible guest clock without
 * servicing devices, then restore the authoritative live clock afterward.
 * Used only while g_ls_replay_active is set. */
int      psx_cycle_replay_begin(uint64_t start_cycle);
uint64_t psx_cycle_replay_end(void);

/* Transactional I-cache view for the overlay differential harness. The
 * interpreter keeps the authoritative post-call cache; native replay starts
 * from the saved entry tags, mutates a temporary view, then restores live. */
int      psx_icache_shadow_record_begin(void);
int      psx_icache_shadow_replay_begin(void);
void     psx_icache_shadow_replay_end(void);
void     psx_icache_shadow_abort(void);

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

/* Deferred under-deadline charges (MotK VLC load-charge batching).
 * psx_cyc_charge accumulates here; publish via psx_cyc_batch_flush /
 * psx_advance_cycles before IRQ checks, MMIO, or any cycle read that must
 * match the published counter. Guest totals at those barriers are unchanged. */
extern uint32_t g_psx_cyc_batch;
extern uint32_t g_psx_cyc_batch_limit;

/* GCC/Clang-generated functions can defer deadline probes within a basic
 * block. Interrupt/MMIO edges still publish the accumulated guest cycles. */
extern int g_psx_cyc_bb_defer;

/* Emitter-level VLC load-charge batching: when non-NULL, psx_cyc_charge
 * accumulates into *g_psx_cyc_local_acc instead of g_psx_cyc_batch. Publish
 * via psx_cyc_local_publish / psx_cyc_batch_flush before IRQ/MMIO barriers. */
extern uint32_t *g_psx_cyc_local_acc;

/* Advance guest time. Overlay DLLs forward this through their callback shim;
 * normal runtime/generated code keeps the common production path inlined. */
#if defined(PSX_OVERLAY_DLL_BUILD)
void psx_advance_cycles(uint32_t cycles);
#else
/* The common production path is inlined: bump the
 * counter and only service devices when the next event deadline is due.
 * Guest-visible timing is unchanged (service_to_now replays exact events).
 *
 * Watchdog / PC-sample throttles live in psx_devices_service_to_now (fired
 * on the HARD_CAP / event cadence, ≥ every 16K guest cycles) — not on every
 * per-instruction charge. MotK VLC issues millions of advances/s; two add+
 * branch pairs there were pure host tax. */
/* CPU overclock. The guest CPU runs as native code, so "faster CPU" cannot
 * mean "execute quicker" -- it means CHARGE LESS for the same work. Every
 * device schedules off psx_cycle_count and the CRTC fires VBlank on a fixed
 * cycle period, so scaling the per-instruction charge down by N lets the CPU
 * complete N times as much work per frame while timers, SPU, CDROM and the
 * refresh rate keep their real-world rates. Scaling the VBlank period instead
 * would slow everything EXCEPT the CPU, which is the opposite.
 *
 * Q16 fixed point with a carried remainder rather than a divide: this is the
 * hottest path in the runtime (millions of charges/sec), and integer division
 * of a 1-2 cycle instruction cost would floor to zero and lose all time.
 * 65536 == 1.0 == stock, and the accumulator makes the total charge exact
 * over time rather than drifting by the truncation each call. */
extern uint32_t g_psx_oc_scale_q16;
extern uint32_t g_psx_oc_accum;

/* percent: 100 = stock. hueponik's pal100full8 needs >900%. */
void     psx_set_cpu_overclock(uint32_t percent);
uint32_t psx_get_cpu_overclock(void);

static inline uint32_t psx_oc_apply(uint32_t cycles) {
    if (g_psx_oc_scale_q16 == 65536u) return cycles;   /* stock: exact */
    uint64_t t = (uint64_t)cycles * (uint64_t)g_psx_oc_scale_q16
               + (uint64_t)g_psx_oc_accum;
    g_psx_oc_accum = (uint32_t)(t & 0xFFFFu);
    return (uint32_t)(t >> 16);
}

static inline void psx_advance_cycles(uint32_t cycles) {
#if !defined(PSX_COSIM)
    if (g_psx_cyc_batch) {
        uint32_t b = g_psx_cyc_batch;
        g_psx_cyc_batch = 0;
        g_psx_cyc_batch_limit = 0;
        if (cycles <= UINT32_MAX - b) cycles += b;
        else {
            /* Extreme: publish b first, then continue with cycles. */
            psx_cycle_count += (uint64_t)psx_oc_apply(b);
            if (!psx_in_device_service &&
                (psx_next_service_cycle == 0u ||
                 psx_cycle_count >= psx_next_service_cycle)) {
                psx_devices_service_to_now();
            }
        }
    }
#endif
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
        /* RAW, not overclocked. Charges made while servicing a device are the
         * device's own time, not CPU instruction retirement. Scaling them too
         * compresses DMA and peripheral work by the same factor, which is not
         * an overclock but a speed hack on the whole machine -- and it broke
         * boot outright at 10x while 3x survived.
         *
         * DuckStation overclocks the CPU alone: device delays keep their
         * real-world duration. Same intent here. */
        psx_cycle_count += (uint64_t)cycles;
        return;
    }
    /* CPU instruction retirement: this is the only charge an overclock scales. */
    cycles = psx_oc_apply(cycles);
    if (cycles == 0u) return;
    psx_cycle_count += (uint64_t)cycles;
    if (psx_next_service_cycle == 0u ||
        psx_cycle_count >= psx_next_service_cycle) {
        psx_devices_service_to_now();
    }
#endif
}
#endif

/* Publish deferred charges (IRQ edge / MMIO / savestate). Overlay DLLs keep
 * their pending total in the callback shim rather than these host globals. */
#if defined(PSX_OVERLAY_DLL_BUILD)
void overlay_flush_cycles(void);
static inline void psx_cyc_local_publish(void) { }
static inline void psx_cyc_batch_flush(void) { overlay_flush_cycles(); }
#else
/* Publish function-local charges into the normal batch/advance path while
 * keeping the local pointer installed (nested charges resume locally). */
static inline void psx_cyc_local_publish(void) {
#if !defined(PSX_COSIM)
    uint32_t *acc = g_psx_cyc_local_acc;
    if (!acc) return;
    uint32_t v = *acc;
    if (!v) return;
    *acc = 0;
    g_psx_cyc_local_acc = NULL;
    psx_advance_cycles(v);
    g_psx_cyc_local_acc = acc;
#endif
}

static inline void psx_cyc_batch_flush(void) {
#if !defined(PSX_COSIM)
    psx_cyc_local_publish();
    uint32_t b = g_psx_cyc_batch;
    if (!b) return;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    psx_advance_cycles(b);
#endif
}
#endif

/* Read accessor for telemetry (includes deferred batch). */
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

/* Post-load probe cycle diagnostics (optional; main.cpp soft-load tooling). */
extern int      g_plp_cycle_diag;
extern uint64_t g_plp_adv_calls;
extern uint32_t g_plp_adv_max_chunk;
extern uint64_t g_plp_adv_sum;
extern uint64_t g_plp_svc_calls;

/* Save-state restore: re-anchor the deadline device model after psx_cycle_count
 * is overwritten from a snapshot. Pass the live CPU so GTE/muldiv completion
 * stamps and load-absorb give-back are rewound with the guest clock. */
void psx_cycles_resync_after_restore(struct CPUState *cpu);

/* Soft rematch / session_reboot: zero the guest clock and deadline bookkeeping.
 * Soft-exit longjmps out of vblank (inside psx_devices_service_to_now) leave
 * psx_in_device_service stuck at 1 and a huge leftover cycle count — without
 * this reset the next match never services devices or fires vblanks. */
void psx_cycles_reset_for_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_CYCLES_H */
