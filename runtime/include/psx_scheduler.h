/* psx_scheduler.h — deterministic TCB cooperative-thread scheduler (carve-out).
 *
 * Replaces the non-deterministic host-fiber bridge (traps.c
 * psx_change_thread_fiber / psx_thread_fiber_entry, which maps each guest
 * thread to a Win32/ucontext fiber and overloads cpu->pc=0 as a host-control
 * signal) with a structured, deterministic scheduler that operates on the REAL
 * guest TCB structures.
 *
 * Rationale (see HLE_SCHEDULER_CARVEOUT_PLAN.md + CLAUDE.md §0 AMENDMENT): the
 * PS1 BIOS scheduler is pure-TCB — a thread's entire context (GPRs + guest SP +
 * EPC) lives in its TCB; a "thread switch" is just changing which TCB
 * dword_108->entry points to (ExceptionHandler 0xC80 / ReturnFromException
 * 0xF40, docs/psx_bios_disasm.txt). There is NO stack switch on real hardware,
 * so host fibers were never semantically required — they exist only because a
 * recompiled infinite-loop function never returns to its native caller. Because
 * guest control flow here is dispatch-based (tail-call trampoline + every-
 * block-leader re-entry), a thread switch can instead be a deterministic
 * unwind-to-scheduler + re-dispatch-the-target's-EPC.
 *
 * INVARIANT this design lives or dies on: guest thread context lives ONLY in
 * CPUState / RAM / the TCB reg array — NEVER in a host fiber stack, generated
 * C locals, a pc=0 sentinel, or a global resume PC. A yield/switch/RFE may
 * unwind the host stack ONLY after all guest-visible state is committed and the
 * resume PC is dispatchable, and only at a block-leader boundary.
 *
 * ── SCAFFOLDING (plan steps 1-2) ──────────────────────────────────────────
 * This header + the matching definitions in traps.c are INERT: the host-fiber
 * bridge (psx_change_thread_fiber) is still authoritative. Later steps wire the
 * outer scheduler loop and structured escapes in to replace it.
 */
#ifndef PSX_SCHEDULER_H
#define PSX_SCHEDULER_H

#include <stdint.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Structured scheduler-escape reasons — replaces the overloaded cpu->pc=0
 * host-control sentinel. The outer scheduler loop reads the reason after each
 * psx_dispatch returns to decide what to run next. */
typedef enum {
    PSX_RUN_CONTINUE = 0,    /* dispatch returned normally; keep running the current thread */
    PSX_RUN_YIELD_TO_TCB,    /* ChangeThread / cross-thread RFE: switch to g_sched_escape.target_tcb */
    PSX_RUN_RESUME_CURRENT,  /* same-thread RFE: re-dispatch the current thread at resume_pc */
    PSX_RUN_GUEST_EXIT,      /* a guest thread legitimately ended */
    PSX_RUN_RETURN_TO_LOBBY, /* netplay soft-exit: tear down match, keep lobby WS */
    PSX_RUN_FATAL,           /* invariant violation — fail closed, never silently continue */
} psx_run_reason_t;

typedef struct {
    psx_run_reason_t reason;
    uint32_t target_tcb;     /* for PSX_RUN_YIELD_TO_TCB */
    uint32_t resume_pc;      /* the dispatchable resume PC the escape commits */
} psx_sched_escape_t;

/* The single outer-scheduler escape target + its escape descriptor. A
 * yield/switch/RFE commits all guest-visible state to CPUState/RAM/TCB, fills
 * g_sched_escape, and longjmp(g_scheduler_jmpbuf, 1)s here. (Wired in a later
 * step; defined inert in traps.c for now.) */
extern jmp_buf            g_scheduler_jmpbuf;
extern psx_sched_escape_t g_sched_escape;

/* Deferred cooperative thread switch (Ape Escape memcard fix #2). Thin exports
 * over the scheduler's private TCB save/restore + PCB[0] accessors, so the
 * interrupt path can re-save a deferred thread cleanly and re-point the kernel
 * current-thread pointer at the outermost dispatch boundary. See traps.c. */
struct CPUState;
void     psx_sched_save_context(struct CPUState* cpu, uint32_t tcb, uint32_t resume_pc);
void     psx_sched_set_current_tcb(struct CPUState* cpu, uint32_t tcb);
uint32_t psx_sched_current_tcb(struct CPUState* cpu);

/* Fail-closed guard: is `pc` a valid, dispatchable guest resume point? A TCB
 * EPC / resume PC that is NOT dispatchable (0, the exception sentinel, or not a
 * known function/re-enterable block-leader entry) is an invariant violation —
 * the scheduler must fail loudly rather than dispatch into nothing (the old
 * pc=0 silent fiber-recreate pathology). */
int psx_is_dispatchable(uint32_t pc);

/* The outer scheduler trampoline. Replaces the bare top-level psx_dispatch()
 * call in main.cpp: dispatch the current thread until it yields via a
 * structured escape (longjmp to g_scheduler_jmpbuf), then run whatever the
 * escape selected. Returns ONLY on an abnormal top-level exit (the outermost
 * dispatch saw cpu->pc==0 with no escape pending) — main.cpp's existing
 * exit-diagnostic dump then runs. Forward-declared for main.cpp (C++). */
struct CPUState;
void psx_scheduler_run(struct CPUState* cpu);

/*
 * Soft-exit netplay back to the lobby UI. Sets PSX_RUN_RETURN_TO_LOBBY and
 * longjmps out of the scheduler when inside psx_scheduler_run; otherwise just
 * latches the flag for the caller to observe. Safe from the netplay barrier.
 */
void psx_request_return_to_lobby(void);
int  psx_return_to_lobby_requested(void);
void psx_clear_return_to_lobby(void);

/* Save-state restore: unwind to psx_scheduler_run and re-dispatch resume_pc
 * (the restored CPUState's PC). Call on the scheduler fiber at a block-leader
 * boundary, in_exception == 0. Never returns (longjmp). */
void psx_scheduler_resume_at(uint32_t resume_pc);

/* HLE-tier standing subsystem replacement (CLAUDE.md §0 amendments
 * 2026-06-29 + 2026-07-02). 1 = deterministic TCB scheduler (default, both
 * BIOS backends — the LLE host-fiber bridge it replaces is non-deterministic);
 * 0 = legacy host-fiber bridge. Default from [runtime] hle_scheduler via
 * psx_hle_scheduler_set_default (call before any dispatch); env
 * PSX_HLE_SCHEDULER wins over config. Latched at first query; the mode cannot
 * change mid-run. */
void psx_hle_scheduler_set_default(int on);
int  psx_hle_scheduler_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SCHEDULER_H */
