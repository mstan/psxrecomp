/*
 * interrupts.c — v4 interrupt delivery for recompiled BIOS.
 *
 * Pure hardware simulation. No BIOS state, no HLE, no interpreter.
 *
 * Since recompiled code runs as native C (no per-instruction stepping),
 * interrupt delivery happens at dispatch loop boundaries. The dispatch
 * loop calls psx_check_interrupts() after each function returns.
 *
 * Vblank is fired on a dispatch-count schedule to approximate 60 Hz.
 *
 * ReturnFromException handling:
 *   On real hardware, ReturnFromException (B0:0x17 or SYSCALL(3))
 *   restores the full register context from the TCB and jumps to the
 *   saved EPC.  This effectively "longjmps" out of the exception
 *   handler, bypassing any remaining chain-walk code.
 *
 *   In our recompiled model the exception handler runs as a nested
 *   psx_dispatch call.  A normal function return would unwind only one
 *   frame, leaving the chain walker running with corrupted registers.
 *   We use setjmp/longjmp to model the real hardware behaviour:
 *   psx_check_interrupts sets a jump point before dispatching the
 *   handler, and psx_exception_longjmp() (called by the runtime's
 *   ReturnFromException implementation) longjmps back, unwinding the
 *   entire handler call tree in one step.
 */

#include "interrupts.h"
#include "sio.h"
#include "timers.h"
#include "gpu.h"
#include "cdrom.h"
#include "cpu_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

/* COP0 register indices */
#define COP0_SR    12
#define COP0_CAUSE 13
#define COP0_EPC   14

/* I_STAT and I_MASK are owned by memory.c */
extern uint32_t i_stat;
extern uint32_t i_mask;

/* Dispatch counter for vblank scheduling. */
#define VBLANK_INTERVAL 50000
static uint32_t dispatch_count;
static uint64_t total_checks;

/* Reentrancy guard: prevent interrupt handler from triggering interrupts. */
static int in_exception;

/* After the exception handler returns, suppress the next interrupt delivery
 * to give the interrupted code at least one block of execution — matching
 * real hardware where at least one instruction runs after RFE before the
 * pending interrupt can re-fire.  Without this, unhandled interrupts cause
 * a livelock: the handler runs, doesn't clear I_STAT, returns, and the
 * very next psx_check_interrupts re-enters immediately. */
static int post_exception_cooldown;

/* setjmp target for ReturnFromException during handler dispatch. */
static jmp_buf exception_jmpbuf;

int psx_get_in_exception(void) { return in_exception; }

void interrupts_init(void) {
    dispatch_count = 0;
    in_exception = 0;
    total_checks = 0;
    post_exception_cooldown = 0;
}

/*
 * Called by the runtime's ReturnFromException implementation (traps.c)
 * when the recompiled BIOS handler or a chain callback invokes
 * B0:0x17 or SYSCALL(3) during exception handling.
 *
 * At this point the caller has already:
 *   - Restored all GPRs from the TCB save area
 *   - Done RFE on the saved SR
 *   - Set cpu->pc = saved EPC
 *
 * We longjmp back to psx_check_interrupts, which will clear
 * in_exception and return, effectively letting the interrupted
 * code resume at the saved EPC through normal dispatch.
 */
void psx_exception_longjmp(void) {
    longjmp(exception_jmpbuf, 1);
}

void psx_restore_state_escape(void) {
    if (in_exception)
        longjmp(exception_jmpbuf, 2);
    /* Not in exception context — return normally, let caller's `return;` handle it. */
}

void psx_check_interrupts(CPUState* cpu) {
    total_checks++;

    /* SIO delayed IRQ delivery removed from here.
     * sio_tick() is now called only from SIO register accesses
     * (sio_read/sio_write) and I_STAT reads (memory.c).  The BIOS
     * pad detection sequence clears I_STAT bit 7 then polls I_STAT
     * waiting for it to re-appear.  If we tick here, the IRQ fires
     * during the delay loop BEFORE the clear, and the BIOS never
     * sees it. */

    /* VBlank / timer tick — only when NOT inside the exception handler.
     *
     * On real hardware, VBlank is a hardware signal tied to the CRT
     * scanline counter, not to instruction count.  The handler runs
     * for a few hundred cycles at most.  In our model, the handler
     * runs thousands of block leaders (each calling psx_check_interrupts),
     * so dispatch_count can exceed VBLANK_INTERVAL during a single
     * handler invocation, causing VBlanks to stack up and starve the
     * main code.  Gating the tick on !in_exception prevents this. */
    if (!in_exception) {
        dispatch_count++;
        if (dispatch_count >= VBLANK_INTERVAL) {
            dispatch_count = 0;
            i_stat |= (1 << IRQ_VBLANK);
            gpu_vblank_tick();  /* Toggle LCF (GPUSTAT bit 31) */
            timers_tick(33868); /* ~1 NTSC frame worth of cycles */
            cdrom_tick();      /* Process pending CDROM responses */
        }
    }

    /* Check if any interrupts are pending. */
    if ((i_stat & i_mask) == 0) return;
    if (in_exception) return;

    /* Post-exception cooldown: let at least one block execute after RFE. */
    if (post_exception_cooldown > 0) {
        post_exception_cooldown--;
        return;
    }

    /* Check COP0 SR: IEc (bit 0) must be set, and IM2 (bit 10) must be set. */
    uint32_t sr = cpu->cop0[COP0_SR];
    if (!(sr & 0x01)) return;    /* Interrupts globally disabled */
    if (!(sr & (1 << 10))) return; /* Hardware interrupt bit not enabled */

    in_exception = 1;
    uint32_t pre_handler_istat = i_stat;  /* snapshot for cooldown decision */

    /* Set COP0 Cause: ExcCode=0 (interrupt), IP2 pending. */
    cpu->cop0[COP0_CAUSE] = (cpu->cop0[COP0_CAUSE] & ~0x7C) | (0 << 2);
    cpu->cop0[COP0_CAUSE] |= (1 << 10);

    /* Push SR exception stack: shift bits [5:0] left by 2. */
    cpu->cop0[COP0_SR] = (sr & ~0x3F) | ((sr & 0x0F) << 2);

    /* EPC: set to a sentinel value. The recompiled exception handler reads
     * memory at [EPC] to check for COP2 branch delay. We use a dedicated
     * address in the kernel scratch area.  Address 0x80000048 is chosen
     * because it's between the exception vectors (0x80-0xBF) and the
     * kernel data pointer area (0x100+), and not used by the BIOS. */
    uint32_t sentinel = 0x80000048u;
    cpu->write_word(sentinel, 0x00000000u); /* NOP */
    cpu->cop0[COP0_EPC] = sentinel;

    /* Save the interrupted code's full register state.
     *
     * On real hardware, the exception handler saves all GPRs to the
     * TCB save area at entry, and ReturnFromException restores them
     * before jumping back to EPC.  The interrupted code always gets
     * its exact pre-exception register values back.
     *
     * In our model, the recompiled handler runs as a C function and
     * its `return;` goes back here — not to EPC.  The handler's
     * normal exit path (0xBFC10944) restores registers from the
     * kernel jmpbuf (intended for longjmp to WaitEvent's caller),
     * which corrupts the interrupted code's registers.
     *
     * We save all GPRs/HI/LO before the handler and restore them
     * after, so the interrupted code resumes with its original state
     * — matching real hardware behaviour. */
    uint32_t saved_gpr[32];
    uint32_t saved_hi, saved_lo;
    for (int i = 0; i < 32; i++) saved_gpr[i] = cpu->gpr[i];
    saved_hi = cpu->hi;
    saved_lo = cpu->lo;

    /* Dispatch the BIOS exception handler.
     * BEV (SR bit 22) selects between 0x80000080 and 0xBFC00180.
     *
     * setjmp is placed here so ReturnFromException (longjmp code 1)
     * and RestoreState (longjmp code 2) can escape the handler call
     * tree.
     *
     * The loop handles the PSX VSync mechanism (SaveState/RestoreState):
     *   - Code 0: normal entry — dispatch the handler.
     *   - Code 2: RestoreState redirect — re-dispatch to cpu->pc
     *     (e.g. VSync callback loop at 0xBFC421D8), still in exception
     *     context.  The redirected code eventually calls ReturnFromException.
     *   - Code 1: ReturnFromException — exit the loop entirely. */
    uint32_t target_pc;
    if (sr & 0x00400000u) {
        target_pc = 0xBFC00180u;
    } else {
        uint32_t w0 = cpu->read_word(0x80000080u);
        uint32_t w1 = cpu->read_word(0x80000084u);
        uint32_t hi_val = (w0 & 0xFFFF) << 16;
        int16_t lo_val = (int16_t)(w1 & 0xFFFF);
        target_pc = hi_val + (uint32_t)(int32_t)lo_val;
    }

    for (;;) {
        int jmp_val = setjmp(exception_jmpbuf);
        if (jmp_val == 2) {
            /* RestoreState redirect: re-dispatch to cpu->pc.
             * GPRs were already set by RestoreState — do NOT restore.
             * Stay in exception context so ReturnFromException works. */
            target_pc = cpu->pc;
            continue;
        }
        if (jmp_val == 0) {
            /* Normal entry (or after RestoreState redirect): dispatch. */
            psx_dispatch(cpu, target_pc);
        }
        /* jmp_val 0 (normal return) or 1 (ReturnFromException): done. */
        break;
    }

    /* Restore the interrupted code's registers.
     *
     * The handler has done its work (acknowledged i_stat, delivered
     * events, etc.) via MMIO writes and RAM writes — those side
     * effects are in memory.  We restore GPRs so the interrupted
     * code continues with its pre-exception state.
     *
     * For SR: if the handler did ReturnFromException (RFE already
     * applied, IEc restored), we keep that.  If the handler exited
     * normally (jmpbuf path, no RFE), IEc is still 0 from the
     * exception push — we do RFE to pop the SR stack. */
    for (int i = 0; i < 32; i++) cpu->gpr[i] = saved_gpr[i];
    cpu->hi = saved_hi;
    cpu->lo = saved_lo;

    if (!(cpu->cop0[COP0_SR] & 0x01)) {
        uint32_t sr2 = cpu->cop0[COP0_SR];
        cpu->cop0[COP0_SR] = (sr2 & 0xFFFFFFC0u) | ((sr2 >> 2) & 0x0Fu);
    }

    in_exception = 0;

    /* Adaptive cooldown: if the handler acknowledged the interrupt (cleared
     * some I_STAT bits), the interrupt won't immediately re-fire and we need
     * no cooldown.  If I_STAT is unchanged (no handler claimed the interrupt),
     * give the main code a generous window to make progress — e.g. to let
     * the shell finish installing handlers.  On real hardware, the CPU
     * executes at least one instruction between exceptions; in our model
     * each "block" is many instructions, but the handler also consumes
     * hundreds of sub-dispatches per invocation. */
    if ((i_stat & i_mask) != 0 && i_stat == pre_handler_istat) {
        post_exception_cooldown = 500;  /* unclaimed: give main code time */
    } else {
        post_exception_cooldown = 1;    /* claimed: minimal cooldown */
    }
}
