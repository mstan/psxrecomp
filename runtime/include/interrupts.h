#ifndef PSXRECOMP_INTERRUPTS_H
#define PSXRECOMP_INTERRUPTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* IRQ bit positions in I_STAT/I_MASK */
#define IRQ_VBLANK  0
#define IRQ_GPU     1
#define IRQ_CDROM   2
#define IRQ_DMA     3
#define IRQ_TIMER0  4
#define IRQ_TIMER1  5
#define IRQ_TIMER2  6
#define IRQ_SIO0    7
#define IRQ_SIO1    8
#define IRQ_SPU     9
#define IRQ_PIO     10

void interrupts_init(void);

/* Save-state restore: re-anchor host-only IRQ pacing after psx_cycle_count is
 * overwritten from a snapshot. Clears absolute-cycle cooldowns / deferred
 * switches that would otherwise strand delivery until the restored clock
 * catches up to the pre-load host timeline (warm-reload picture freeze). */
void interrupts_resync_after_restore(void);

/* Netplay diag: last VBLANK irqctx exit/restore (+ v0) for peer diff. */
void interrupts_log_last_vblank_irqctx(const char *tag);

/* Central IRQ-raise choke point: sets the I_STAT bit AND records the raise edge
 * into the always-on device-event ring (device_trace) with the guest cycle.
 * Every hardware source (VBLANK/GPU/CDROM/DMA/timers/SIO/SPU) funnels its raise
 * through here so the device-timing audit sees every event from one place.
 * `detail` is source-specific (DMA channel, timer index) or 0. */
void psx_irq_raise(uint32_t bit, uint32_t detail);

/* Called from the dispatch loop after each function returns.
 * Services due scheduled edges, checks (i_stat & i_mask), and if pending + COP0
 * allows, dispatches the exception handler. */
void psx_check_interrupts(struct CPUState* cpu);

/* Cheap block-edge predicate for generated/native code. Device events are
 * raised by psx_advance_cycles(); this reports whether the comparatively
 * expensive delivery/scheduler path can have an architectural effect now. */
int psx_interrupt_delivery_needed(const struct CPUState* cpu);
void psx_interrupt_delivery_diag(uint64_t *need_defer, uint64_t *need_irq,
                                 uint64_t *skip_none, uint64_t *skip_sr,
                                 uint64_t *skip_cooldown, uint64_t *skip_nested);

/* Hot-path counters for psx_check_interrupts (monotonic). Any out_* may be NULL.
 * entry     = every call
 * fast_sr   = early return: pending I_STAT but IEc/IM2 clear
 * fast_none = early return: nothing pending
 * mid       = total_checks (past fast paths; toward / into deliver eval)
 * eval      = reached irq_deliver_eval label
 * irq_deliv = g_irq_deliver_count (any HW IRQ exception taken) */
void psx_interrupt_check_path_diag(uint64_t *entry, uint64_t *fast_sr,
                                   uint64_t *fast_none, uint64_t *mid,
                                   uint64_t *eval, uint64_t *irq_deliv);
/* Interrupt check with the compiled guest PC to resume if a game-installed
 * handler later RFEs to the sentinel outside the synchronous host window. */
void psx_check_interrupts_at(struct CPUState* cpu, uint32_t resume_pc);
int psx_interrupts_checked_at_current_cycle(uint32_t resume_pc);
/* Dispatch-entry check for re-enterable compiled PCs. De-dupes an immediately
 * preceding check at the same guest PC/cycle so generated transfers can keep
 * their explicit checks while dirty/interp-to-static entries get a boundary. */
void psx_check_interrupts_dispatch_entry(struct CPUState* cpu, uint32_t resume_pc);

/* Accumulate emitted PSX cycles toward the next VBlank trigger.
 * Called from psx_advance_cycles() so the VBlank rate is gated on
 * guest cycles (correct PSX timing) rather than block-dispatch
 * count (which was 5-6x too fast and squeezed game-time to ~60% of
 * real). Period follows GP1(08h) video mode: 564480 cycles NTSC
 * (33.8688 MHz / 60) or 677376 PAL (33.8688 MHz / 50). */
void interrupts_advance_cycles(uint32_t cycles);
void interrupts_service_scheduled_events(void);
uint32_t interrupts_cycles_to_vblank(void);
/* VBlank phase within the current frame (0 .. VBLANK_CYCLES-1). Persisted in
 * BS_SEC_IRQ (and selfcheck's out-of-band latch) so resim keeps the snap's
 * phase. Legacy 8-byte IRQ sections still rebase to 0 on load. */
uint32_t interrupts_get_cycles_since_vblank(void);
void     interrupts_set_cycles_since_vblank(uint32_t v);

/* Cycle-budgeted precise event slicing: minimum guest-CPU-cycle distance to the
 * next DELIVERABLE hardware interrupt (source raises its I_STAT bit AND that bit
 * is unmasked in i_mask). UINT32_MAX if none scheduled. Single source of truth
 * for the two-tier block executor's fast/slice decision. See PRECISE_IRQ_SLICE.md. */
uint32_t cycles_to_next_event(void);

/* Query whether we are currently inside an exception handler dispatch. */
int psx_get_in_exception(void);

/* Most recent block-leader IRQ-check guest PC / compiled resume latch.
 * Used by the post-savestate freeze probe (vblank-time "where was the game"). */
uint32_t psx_last_irq_check_pc(void);
uint32_t psx_compiled_irq_resume_pc(void);
/* Soft-return / netplay BYE: drop sticky resume latches so rematch dig0 snaps
 * cannot inherit a prior-match game PC (see pick_snap_resume_pc). */
void psx_irq_clear_resume_latches(void);
/* Arm compiled IRQ resume before top-level flush_resume / savestate dispatch
 * so a latched I_STAT cannot take LEGACY_SENTINEL with take_pc=0. */
void psx_irq_arm_compiled_resume_pc(uint32_t pc);
uint64_t psx_last_irq_check_cycle(void);
uint64_t psx_interrupt_total_checks(void);
uint32_t psx_interrupt_fast_maintenance(void);

/* Snapshot internal counters for the freeze_check diagnostic.  Any out_*
 * pointer may be NULL.  All counters are monotonically non-decreasing
 * (dispatch_count resets each VBlank). */
void psx_get_freeze_diag(uint64_t *out_total_checks,
                         uint32_t *out_dispatch_count,
                         int *out_in_exception,
                         int *out_post_exc_cooldown,
                         uint64_t *out_exc_entries,
                         uint64_t *out_exc_reentry_blocks);

/* longjmp back to psx_check_interrupts, unwinding the exception handler.
 * Called by ReturnFromException (B0:0x17 or SYSCALL(3)) when inside
 * the exception handler to model real hardware's RFE+JR $k0 unwind. */
void psx_exception_longjmp(void);

/* longjmp back to psx_check_interrupts for RestoreState redirection.
 *
 * On real hardware, RestoreState (A0:0x14) restores all GPRs from a
 * save buffer and does `jr $ra`, which jumps to the restored $ra —
 * abandoning the current call stack.  In our model, a plain `return;`
 * would go back to the C caller (the chain walker), not to $ra.
 *
 * This function longjmps with code 2 (vs. 1 for ReturnFromException),
 * so psx_check_interrupts can re-dispatch to cpu->pc while still in
 * exception context.  The redirected code (e.g., VSync callback loop)
 * will eventually call ReturnFromException (longjmp code 1) to exit. */
void psx_restore_state_escape(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_INTERRUPTS_H */
