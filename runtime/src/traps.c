/* traps.c — Phase 2 trap handlers.
 *
 * All traps abort with a diagnostic message.
 * Phase 2 expects MMIO abort before any trap fires.
 */

#include "cpu_state.h"
#include "psx_runtime.h"   /* fix B: psx_exc_escape_reason_t + g_exc_escape_reason */
#include "debug_server.h"
#include "crash_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "psx_fiber.h"   /* cross-platform fibers (Win32 fibers / POSIX ucontext) */
#include "psx_scheduler.h" /* deterministic TCB scheduler carve-out (scaffolding) */
#include "parity_trace.h"  /* general two-process control-flow parity ring */

/* RAM reader adapter for the parity trace (cpu->read_word takes only addr). */
static uint32_t traps_parity_rw(void* ctx, uint32_t addr) {
    return ((CPUState*)ctx)->read_word(addr);
}

/* Forward declarations from interrupts.c */
int psx_get_in_exception(void);
void psx_exception_longjmp(void);

/* ── Deterministic TCB scheduler — SCAFFOLDING (plan steps 1-2, INERT) ───────
 * These definitions back psx_scheduler.h. They are NOT yet wired into the live
 * thread-switch path: psx_change_thread_fiber (the host-fiber bridge) below is
 * still authoritative. Later steps add the outer scheduler loop, route
 * ChangeThread / ReturnFromException through structured escapes (longjmp to
 * g_scheduler_jmpbuf with a psx_run_reason_t), and remove the cpu->pc=0
 * host-control signalling + per-frame fiber recreation. Exported (external
 * linkage) so the inert symbols don't trip unused-symbol checks. */
jmp_buf            g_scheduler_jmpbuf;
psx_sched_escape_t g_sched_escape;

/* Single-level "switch back to the yielder" safety net. The old fiber bridge
 * remembered each thread's return_fiber so a target whose dispatch RETURNS
 * (rather than yielding back) resumed its yielder instead of killing the run.
 * In the redispatch model we keep the same one-level notion: the yielder's TCB
 * latched at the last YIELD_TO_TCB, restored if the target's top-level dispatch
 * returns. Zero = no pending return target (top-level pc==0 is a real exit). */
static uint32_t g_sched_return_tcb = 0;

/* sched_escape_ring (plan step 8) — always-on record of every structured
 * scheduler escape, queryable via the `sched_escape_ring` debug command. This
 * is the deterministic replacement for the fiber thread_event kinds 3-12. */
typedef struct SchedEscapeEntry {
    uint32_t seq;
    uint32_t frame;
    uint32_t reason;       /* psx_run_reason_t */
    uint32_t current_tcb;
    uint32_t target_tcb;
    uint32_t resume_pc;
    uint32_t pc;
    uint32_t ra;
    uint32_t sp;
} SchedEscapeEntry;
#define SCHED_ESCAPE_RING_CAP 256u
SchedEscapeEntry g_sched_escape_ring[SCHED_ESCAPE_RING_CAP];
uint64_t g_sched_escape_seq = 0;

static void sched_escape_ring_log(CPUState* cpu, uint32_t reason,
                                  uint32_t current_tcb, uint32_t target_tcb,
                                  uint32_t resume_pc)
{
    extern uint64_t s_frame_count;
    SchedEscapeEntry* e =
        &g_sched_escape_ring[g_sched_escape_seq & (SCHED_ESCAPE_RING_CAP - 1u)];
    e->seq         = (uint32_t)g_sched_escape_seq;
    e->frame       = (uint32_t)s_frame_count;
    e->reason      = reason;
    e->current_tcb = current_tcb;
    e->target_tcb  = target_tcb;
    e->resume_pc   = resume_pc;
    e->pc          = cpu->pc;
    e->ra          = cpu->gpr[31];
    e->sp          = cpu->gpr[29];
    g_sched_escape_seq++;
}

int psx_is_dispatchable(uint32_t pc)
{
    /* Fail closed on the known-bad resume PCs that the old pc=0 sentinel +
     * sentinel-EPC pathologies produced. A nonzero, non-sentinel PC is treated
     * as potentially dispatchable for now; a later step tightens this to "is a
     * registered function entry / re-enterable block leader" via the dispatch
     * tables (psx_game_is_function_entry + the BIOS dispatch table). */
    if (pc == 0u) return 0;
    if (pc == PSX_EXC_SENTINEL_PC) return 0;
    return 1;
}

/* Dispatch call contract state (see cpu_state.h for the model). */
int      g_psx_call_bail      = 0;
uint64_t g_psx_bail_first     = 0;
uint64_t g_psx_bail_resolved  = 0;
uint64_t g_psx_bail_flattened = 0;
uint64_t g_psx_bail_anomaly   = 0;

/* ---- Deduped bail-source ledger (Tomba 2 wild-return / splash-spin diag) ----
 * psx_call_contract (cpu_state.h) calls psx_bail_record() at each FIRST-detected
 * contract violation with the call site's expected return address (site_ra — the
 * caller's continuation, i.e. the GAME wait-site that called into the kernel) and
 * the guest's actual wild target ($ra). We aggregate by (site_ra, wild_pc) and
 * surface the dominant key + unique-key count in the freeze heartbeat, so the
 * wild-return SOURCE is readable even when the bail storm saturates the main
 * thread and the window goes "Not Responding". unique_keys==1 ⇒ one bad call site
 * (a real contract bug); many ⇒ normal trampolines (just the cost of the spin). */
extern uint64_t s_frame_count;
#define BAIL_LEDGER_SLOTS 1024u
typedef struct {
    uint32_t site_ra;     /* expected return = caller continuation (game wait-site) */
    uint32_t wild_pc;     /* guest's actual $ra = wild-return destination          */
    uint32_t site_sp;
    uint32_t guest_sp;
    uint64_t count;
    uint32_t first_frame;
    uint8_t  used;
} BailLedgerSlot;
static BailLedgerSlot s_bail_ledger[BAIL_LEDGER_SLOTS];
static uint32_t s_bail_unique = 0;
/* First-ever wild return, latched (never overwritten) — the trigger instance. */
uint32_t g_bail_first_site_ra = 0, g_bail_first_wild_pc = 0;
uint32_t g_bail_first_site_sp = 0, g_bail_first_guest_sp = 0, g_bail_first_frame = 0;
/* Exception context at the first bail — confirms the "async interrupt delivered
 * mid-dirty-call" mechanism: in_exc=1 means the trigger bail happened while an
 * exception was being handled (= interrupt landed mid-overlay-call). */
uint32_t g_bail_first_in_exc = 0;
static int s_bail_first_latched = 0;

void psx_bail_record(uint32_t site_ra, uint32_t site_sp,
                     uint32_t wild_pc, uint32_t guest_sp) {
    if (!s_bail_first_latched) {
        s_bail_first_latched   = 1;
        g_bail_first_site_ra   = site_ra;  g_bail_first_wild_pc  = wild_pc;
        g_bail_first_site_sp   = site_sp;  g_bail_first_guest_sp = guest_sp;
        g_bail_first_frame     = (uint32_t)s_frame_count;
        g_bail_first_in_exc    = (uint32_t)psx_get_in_exception();
    }
    uint32_t h = (site_ra * 2654435761u) ^ (wild_pc * 40503u);
    uint32_t start = h & (BAIL_LEDGER_SLOTS - 1u);
    for (uint32_t i = 0; i < 16u; i++) {
        BailLedgerSlot *s = &s_bail_ledger[(start + i) & (BAIL_LEDGER_SLOTS - 1u)];
        if (s->used && s->site_ra == site_ra && s->wild_pc == wild_pc) {
            s->count++;
            return;
        }
        if (!s->used) {
            s->used = 1; s->site_ra = site_ra; s->wild_pc = wild_pc;
            s->site_sp = site_sp; s->guest_sp = guest_sp;
            s->count = 1; s->first_frame = (uint32_t)s_frame_count;
            s_bail_unique++;
            return;
        }
    }
    /* Probe window exhausted (>16 collisions): fold into the hashed slot so the
     * total stays honest even if the precise key is lost. */
    s_bail_ledger[start].count++;
}

/* Read the highest-count ledger entry (the dominant wild-return source). */
void psx_bail_ledger_top(uint32_t *site_ra, uint32_t *wild_pc, uint32_t *site_sp,
                         uint32_t *guest_sp, uint64_t *count, uint32_t *unique) {
    uint64_t best = 0; const BailLedgerSlot *bs = NULL;
    for (uint32_t i = 0; i < BAIL_LEDGER_SLOTS; i++) {
        if (s_bail_ledger[i].used && s_bail_ledger[i].count > best) {
            best = s_bail_ledger[i].count; bs = &s_bail_ledger[i];
        }
    }
    if (site_ra)  *site_ra  = bs ? bs->site_ra  : 0;
    if (wild_pc)  *wild_pc  = bs ? bs->wild_pc  : 0;
    if (site_sp)  *site_sp  = bs ? bs->site_sp  : 0;
    if (guest_sp) *guest_sp = bs ? bs->guest_sp : 0;
    if (count)    *count    = best;
    if (unique)   *unique   = s_bail_unique;
}

/* ---- $ra->1 corruption tripwire (Tomba 2 freeze confirm-first probe) ----
 * Latch the EXACT first moment guest $ra transitions to 0x00000001 (the degenerate
 * spin state $ra=$sp=1), tagged with the SITE that did it. Pins whether the clobber
 * is an overlay instruction (interp), the exception GPR-restore (exc-exit), or the
 * bail-flatten. Surfaced in the heartbeat. Site: 0=INTERP 1=EXC_EXIT 2=other. */
uint32_t g_ra_tw_latched = 0;
uint32_t g_ra_tw_site = 0;       /* which site clobbered $ra->1 */
uint32_t g_ra_tw_pc = 0;         /* guest PC at the clobber */
uint32_t g_ra_tw_prev_ra = 0;    /* $ra value immediately before */
uint32_t g_ra_tw_frame = 0;
uint32_t g_ra_tw_in_exc = 0;
uint32_t g_ra_tw_sp = 0;

void psx_ra_tripwire(CPUState *cpu, uint32_t prev_ra, uint32_t pc, uint32_t site) {
    if (g_ra_tw_latched) return;
    if (cpu->gpr[31] == 1u && prev_ra != 1u) {
        g_ra_tw_latched = 1;
        g_ra_tw_site    = site;
        g_ra_tw_pc      = pc;
        g_ra_tw_prev_ra = prev_ra;
        g_ra_tw_frame   = (uint32_t)s_frame_count;
        g_ra_tw_in_exc  = (uint32_t)psx_get_in_exception();
        g_ra_tw_sp      = cpu->gpr[29];
    }
}

static void trap_crash(const char* msg) {
    FILE* cf = fopen("psx_crash.txt", "w");
    if (cf) { fprintf(cf, "%s\n", msg); fclose(cf); }
    /* Full ring dump, then halt-and-serve (debug builds) / exit(1)
     * (release). Never returns — call sites' trailing exit(1) are
     * unreachable belt-and-braces. */
    psx_fatal_halt(msg);
}

static int psx_is_valid_tcb(CPUState* cpu, uint32_t tcb)
{
    uint32_t base = cpu->read_word(0x00000110u);
    uint32_t size = cpu->read_word(0x00000114u);
    if (base == 0 || size == 0) return 0;
    uint32_t offset = tcb - base;
    return offset < size && (offset % 0xC0u) == 0;
}

/* ChatGPT-conferred 2026-06-29 "slice-exit provenance": records WHY cpu->pc became 0
 * when a thread fiber's psx_dispatch returns (fiber_dispatch_exit). Distinguishes a
 * legitimate GUEST return (the guest's own jr/function-return left pc=0 — normal
 * per-frame recreate, fiber theory dead) from a HOST artifact (fiber-switch resume,
 * dispatch miss, etc.). Reset to GUEST_RETURN before each psx_dispatch slice; the
 * artifact sites override it. Logged as the value of the kind-13/26 fiber_dispatch_exit
 * thread event (was cpu->pc, which is always 0 there and uninformative). */
enum {
    PSX_PC0_GUEST_RETURN = 0,   /* default: guest jr/function-return set pc=0 */
    PSX_PC0_CHANGE_SELF  = 1,   /* psx_change_thread_fiber current==target */
    PSX_PC0_FIBER_SWITCH = 2,   /* psx_change_thread_fiber after psx_fiber_switch resume */
    PSX_PC0_CRIT_SECTION = 3,   /* EnterCriticalSection/ExitCriticalSection */
    PSX_PC0_DISPATCH_MISS = 4,  /* psx_unknown_dispatch couldn't resolve a target */
};
uint32_t g_pc0_reason = PSX_PC0_GUEST_RETURN;

static uint32_t psx_current_tcb_ptr(CPUState* cpu)
{
    uint32_t tcbh = cpu->read_word(0x00000108u);
    return tcbh ? cpu->read_word(tcbh) : 0;
}

static void psx_set_current_tcb(CPUState* cpu, uint32_t tcb)
{
    uint32_t tcbh = cpu->read_word(0x00000108u);
    if (tcbh) cpu->write_word(tcbh, tcb);
}

/* Save/restore PC ring — diagnoses thread-resume drift.
 *
 * Each thread switch logs the (op, tcb, pc, sp, ra) tuple at save and
 * restore. Read back via debug_server "thread_ctx_ring" to see whether
 * a thread's saved resume_pc matches the one we later restore. If a
 * save/restore pair for the same TCB shows different resume_pc with no
 * intervening progress, the recompiled code never advanced and we have
 * a thread-state corruption signal. */
typedef struct ThreadCtxRingEntry {
    uint32_t seq;
    uint32_t frame;
    uint8_t  op;       /* 0=save 1=restore */
    uint8_t  pad0[3];
    uint32_t tcb;
    uint32_t resume_pc;/* save: passed-in resume_pc; restore: read from TCB+128 */
    uint32_t gpr_29;   /* sp */
    uint32_t gpr_31;   /* ra */
    uint32_t cop0_sr;
    uint32_t cop0_epc;
} ThreadCtxRingEntry;
#define THREAD_CTX_RING_CAP 256u
ThreadCtxRingEntry g_thread_ctx_ring[THREAD_CTX_RING_CAP];
uint64_t g_thread_ctx_ring_seq = 0;

extern uint64_t s_frame_count;

static void thread_ctx_ring_log(CPUState* cpu, uint32_t tcb,
                                uint32_t resume_pc, uint8_t op)
{
    ThreadCtxRingEntry* e = &g_thread_ctx_ring[g_thread_ctx_ring_seq & (THREAD_CTX_RING_CAP - 1u)];
    e->seq        = (uint32_t)g_thread_ctx_ring_seq;
    e->frame      = (uint32_t)s_frame_count;
    e->op         = op;
    e->tcb        = tcb;
    e->resume_pc  = resume_pc;
    e->gpr_29     = cpu->gpr[29];
    e->gpr_31     = cpu->gpr[31];
    e->cop0_sr    = cpu->cop0[12];
    e->cop0_epc   = cpu->cop0[14];
    g_thread_ctx_ring_seq++;
}

/* Fail-closed: the host exception sentinel must NEVER be persisted into a TCB as a
 * thread's resume PC, nor read back out of one. Fix B made the interrupted resume PC
 * the REAL guest PC; if a sentinel ever lands in a TCB EPC slot again it means the
 * legacy boundary-fallback (interrupts.c) leaked into a SAVED (cross-thread) context,
 * which would silently mis-resume that thread through stale host escape state. Halt
 * loudly instead of resuming wrong. Never returns on a hit. */
static void psx_assert_no_sentinel_pc(const char* where, uint32_t tcb, uint32_t pc)
{
    if (pc == PSX_EXC_SENTINEL_PC) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "fatal: exception sentinel 0x%08X persisted in TCB 0x%08X resume PC (%s)",
                 (unsigned)pc, (unsigned)tcb, where);
        trap_crash(buf);
    }
}

static void psx_save_context_to_tcb(CPUState* cpu, uint32_t tcb, uint32_t resume_pc)
{
    psx_assert_no_sentinel_pc("save_context_to_tcb", tcb, resume_pc);
    uint32_t save = tcb + 8u;
    uint32_t sr = cpu->cop0[12];
    for (int i = 1; i < 32; i++) {
        if (i == 26) continue; /* k0 is the BIOS restore jump register. */
        cpu->write_word(save + (uint32_t)i * 4u, cpu->gpr[i]);
    }
    cpu->write_word(save + 128u, resume_pc);
    cpu->write_word(save + 132u, cpu->hi);
    cpu->write_word(save + 136u, cpu->lo);
    cpu->write_word(save + 140u, (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2));
    cpu->write_word(save + 144u, cpu->cop0[13]);
    thread_ctx_ring_log(cpu, tcb, resume_pc, 0);
    debug_server_log_thread_event(1, cpu, tcb, tcb, resume_pc);
}

static uint32_t psx_restore_context_from_tcb(CPUState* cpu, uint32_t tcb)
{
    uint32_t save = tcb + 8u;
    for (int i = 1; i < 32; i++) {
        if (i == 26) continue;
        cpu->gpr[i] = cpu->read_word(save + (uint32_t)i * 4u);
    }
    cpu->hi = cpu->read_word(save + 132u);
    cpu->lo = cpu->read_word(save + 136u);
    {
        uint32_t saved_sr = cpu->read_word(save + 140u);
        cpu->cop0[12] = (saved_sr & 0xFFFFFFC0u) | ((saved_sr >> 2) & 0x0Fu);
    }
    cpu->cop0[13] = cpu->read_word(save + 144u);
    cpu->gpr[26] = cpu->read_word(save + 128u);
    psx_assert_no_sentinel_pc("restore_context_from_tcb", tcb, cpu->gpr[26]);
    thread_ctx_ring_log(cpu, tcb, cpu->gpr[26], 1);
    debug_server_log_thread_event(2, cpu, psx_current_tcb_ptr(cpu), tcb, cpu->gpr[26]);
    return cpu->gpr[26];
}

/* Deferred cooperative thread switch (Ape Escape memcard fix #2).
 *
 * A genuine in-exception ChangeThread (interrupts.c kind-30) must be honored at
 * the OUTERMOST dispatch boundary, never mid-nested-host-dispatch: only at the
 * outermost boundary is the interrupted thread's guest state fully materialized
 * in CPUState (cpu->pc == the true block PC, GPRs live) rather than split across
 * a stale resume-PC latch + the host C call stack. Switching mid-nest saves a
 * poisoned TCB context (latched PC ahead of the live registers) → the resumed
 * thread dispatches a jumptable with a smeared index → misaligned DISPATCH FATAL.
 *
 * These thin exports let the interrupt path (interrupts.c) re-save the deferred
 * thread cleanly and re-point PCB[0] at the outermost boundary, using the same
 * TCB layout the scheduler save/restore already owns. */
void psx_sched_save_context(CPUState* cpu, uint32_t tcb, uint32_t resume_pc)
{
    psx_save_context_to_tcb(cpu, tcb, resume_pc);
}

void psx_sched_set_current_tcb(CPUState* cpu, uint32_t tcb)
{
    psx_set_current_tcb(cpu, tcb);
}

uint32_t psx_sched_current_tcb(CPUState* cpu)
{
    return psx_current_tcb_ptr(cpu);
}

typedef struct HostThreadFiber {
    uint32_t tcb;
    psx_fiber_t fiber;
    psx_fiber_t return_fiber;
    uint32_t return_tcb;
    CPUState* cpu;
    int used;
    int owned;
    int closed;
} HostThreadFiber;

static HostThreadFiber s_host_threads[32];
static psx_fiber_t s_main_fiber;

static uint32_t psx_tcb_state(CPUState* cpu, uint32_t tcb)
{
    return psx_is_valid_tcb(cpu, tcb) ? cpu->read_word(tcb) : 0;
}

static psx_fiber_t psx_current_host_fiber(void)
{
    if (!s_main_fiber) {
        s_main_fiber = psx_fiber_convert_thread();
        if (!s_main_fiber) {
            trap_crash("psx_fiber_convert_thread failed");
            exit(1);
        }
    }
    return psx_fiber_current();
}

static HostThreadFiber* psx_find_host_thread(uint32_t tcb)
{
    for (size_t i = 0; i < sizeof(s_host_threads) / sizeof(s_host_threads[0]); i++) {
        if (s_host_threads[i].used && s_host_threads[i].tcb == tcb) {
            return &s_host_threads[i];
        }
    }
    return NULL;
}

static HostThreadFiber* psx_alloc_host_thread(void)
{
    for (size_t i = 0; i < sizeof(s_host_threads) / sizeof(s_host_threads[0]); i++) {
        if (!s_host_threads[i].used) {
            memset(&s_host_threads[i], 0, sizeof(s_host_threads[i]));
            s_host_threads[i].used = 1;
            return &s_host_threads[i];
        }
    }
    trap_crash("BIOS thread fiber table full");
    exit(1);
}

static HostThreadFiber* psx_bind_current_host_thread(CPUState* cpu, uint32_t tcb)
{
    psx_fiber_t fiber = psx_current_host_fiber();
    HostThreadFiber* slot = psx_find_host_thread(tcb);
    if (!slot) {
        slot = psx_alloc_host_thread();
    }
    slot->tcb = tcb;
    slot->fiber = fiber;
    slot->cpu = cpu;
    slot->used = 1;
    slot->owned = (fiber != s_main_fiber);
    slot->closed = 0;
    return slot;
}

static void psx_thread_fiber_entry(void* param)
{
    HostThreadFiber* slot = (HostThreadFiber*)param;
    CPUState* cpu = slot->cpu;

    debug_server_log_thread_event(10, cpu, psx_current_tcb_ptr(cpu), slot->tcb, 0);
    psx_set_current_tcb(cpu, slot->tcb);
    uint32_t target_pc = psx_restore_context_from_tcb(cpu, slot->tcb);
    psx_assert_no_sentinel_pc("thread_fiber_entry dispatch", slot->tcb, target_pc);
    g_pc0_reason = PSX_PC0_GUEST_RETURN; /* default; artifact sites override during dispatch */
    if (target_pc != 0) {
        psx_dispatch(cpu, target_pc);
    }

    /* Tomba2 loader-thread diagnosis (Patch 1, trace-only): record the dispatch
     * EXIT state for this thread's fiber. kind 13 = psx_dispatch returned with
     * in_exception==0; kind 26 = with in_exception==1. target_pc field carries the
     * FINAL cpu->pc the dispatch returned with (0 = sentinel/clean-exit path; any
     * non-zero = a surfaced transfer). This is the decisive "exit reason" (vs the
     * garbage ra): if the worker thread's dispatch returns with cpu->pc==0 / a
     * surfaced transfer rather than a verified thread-close, closing the fiber here
     * is wrong. */
    /* value = g_pc0_reason (slice-exit provenance) instead of cpu->pc (always 0 here). */
    debug_server_log_thread_event(psx_get_in_exception() ? 26u : 13u, cpu,
                                  psx_current_tcb_ptr(cpu), slot->tcb, g_pc0_reason);
    debug_server_log_thread_event(11, cpu, psx_current_tcb_ptr(cpu), slot->tcb, target_pc);
    slot->closed = 1;
    /* Guest BIOS code owns the TCB state. A host fiber can finish because a
     * thread returned through the BIOS scheduler path, but changing the BIOS
     * TCB here races with games that keep that thread runnable. */

    if (slot->return_fiber) {
        if (psx_is_valid_tcb(cpu, slot->return_tcb)) {
            psx_set_current_tcb(cpu, slot->return_tcb);
            (void)psx_restore_context_from_tcb(cpu, slot->return_tcb);
        }
        debug_server_log_thread_event(12, cpu, slot->tcb, slot->return_tcb, 0);
        psx_fiber_switch(slot->return_fiber);
    }

    trap_crash("BIOS thread fiber returned with no scheduler target");
    exit(1);
}

static HostThreadFiber* psx_get_or_create_host_thread(CPUState* cpu, uint32_t tcb)
{
    HostThreadFiber* slot = psx_find_host_thread(tcb);
    if (psx_tcb_state(cpu, tcb) != 0x4000u) {
        return NULL;
    }

    if (slot && slot->fiber && !slot->closed) {
        return slot;
    }

    if (!slot) {
        slot = psx_alloc_host_thread();
    } else if (slot->fiber && slot->owned && slot->fiber != psx_fiber_current()) {
        psx_fiber_destroy(slot->fiber);
        slot->fiber = NULL;
    }

    slot->tcb = tcb;
    slot->cpu = cpu;
    slot->return_fiber = NULL;
    slot->return_tcb = 0;
    slot->owned = 1;
    slot->closed = 0;
    /* Guest-thread fiber stack. Default 1 MB; PSX_FIBER_STACK_KB overrides it so
     * the long-run-freeze repro can be run at 0.5x/2x stack (RECURSION_BUG.md §17
     * orthogonal test): if the overflow frame scales ~linearly with stack size,
     * the freeze is gradual host-stack accumulation, not a frame-50k trigger. */
    static long s_fiber_kb = -1;
    if (s_fiber_kb < 0) {
        const char *e = getenv("PSX_FIBER_STACK_KB");
        s_fiber_kb = (e && *e) ? atol(e) : 1024;
        if (s_fiber_kb < 256) s_fiber_kb = 256;
    }
    slot->fiber = psx_fiber_create((size_t)s_fiber_kb * 1024u, psx_thread_fiber_entry, slot);
    if (!slot->fiber) {
        trap_crash("psx_fiber_create failed for BIOS thread");
        exit(1);
    }
    return slot;
}

static int psx_change_thread_fiber(CPUState* cpu, uint32_t target_tcb)
{
    uint32_t current_tcb = psx_current_tcb_ptr(cpu);
    /* data-shard purity: a green-thread switch inside a capture window would
     * record OTHER threads' work into the shard — poison the capture. */
    { extern void ds_note_thread_switch(void); ds_note_thread_switch(); }
    debug_server_log_thread_event(3, cpu, current_tcb, target_tcb, 0);
    if (!psx_is_valid_tcb(cpu, current_tcb) || !psx_is_valid_tcb(cpu, target_tcb)) {
        debug_server_log_thread_event(4, cpu, current_tcb, target_tcb, 0);
        return 0;
    }
    if (current_tcb == target_tcb) {
        debug_server_log_thread_event(5, cpu, current_tcb, target_tcb, cpu->gpr[31]);
        g_pc0_reason = PSX_PC0_CHANGE_SELF;
        cpu->pc = 0;
        return 1;
    }

    /* Never let a speculative native shadow pass switch host/guest threads.
     * The authoritative interpreter pass would already have escaped if this
     * switch were real; reaching it only in native code proves divergence. */
    {
        extern int overlay_loader_shadow_native_thread_switch_bail(void);
        if (overlay_loader_shadow_native_thread_switch_bail()) return -1;
    }

    HostThreadFiber* current = psx_bind_current_host_thread(cpu, current_tcb);
    int saved_current_context = 0;
    if (psx_tcb_state(cpu, current_tcb) == 0x4000u) {
        psx_save_context_to_tcb(cpu, current_tcb, cpu->gpr[31]);
        saved_current_context = 1;
    } else {
        /* 0x1000 is the BIOS closed/free state and must retire the host fiber.
         * Other non-runnable states can still have a suspended generated C
         * stack that must be resumed later. */
        if (psx_tcb_state(cpu, current_tcb) == 0x1000u) {
            current->closed = 1;
        }
        debug_server_log_thread_event(6, cpu, current_tcb, target_tcb, 0);
    }

    HostThreadFiber* target = psx_get_or_create_host_thread(cpu, target_tcb);
    if (!target) {
        debug_server_log_thread_event(7, cpu, current_tcb, target_tcb, 0);
        return 0;
    }

    target->return_fiber = current->fiber;
    target->return_tcb = current_tcb;

    psx_set_current_tcb(cpu, target_tcb);
    (void)psx_restore_context_from_tcb(cpu, target_tcb);
    debug_server_log_thread_event(8, cpu, current_tcb, target_tcb, 0);
    psx_fiber_switch(target->fiber);

    /* The switch returns on the original native stack, but CPUState is
     * shared globally and still contains the fiber that just yielded back.
     * Restore the TCB we saved above before continuing on this stack. */
    if (saved_current_context && psx_is_valid_tcb(cpu, current_tcb)) {
        psx_set_current_tcb(cpu, current_tcb);
        (void)psx_restore_context_from_tcb(cpu, current_tcb);
    }

    /* If a non-owner fiber requested an exception longjmp while we were
     * suspended, it deferred it by SwitchToFiber'ing back to us (the
     * owner). Honor it now from the correct stack. */
    extern void* g_exception_owner_fiber;
    extern int   g_pending_exception_longjmp;
    extern jmp_buf exception_jmpbuf;
    if (g_pending_exception_longjmp && psx_fiber_current() == g_exception_owner_fiber) {
        int code = g_pending_exception_longjmp;
        g_pending_exception_longjmp = 0;
        /* Same shadow-frame hardening as deferred_exception_longjmp: this is
         * the deferred-honor site — the actual unwind of the OWNER fiber's
         * frames happens here, so a live shadow frame on this stack must have
         * its globals restored before the jump. */
        extern uint64_t g_exc_setjmp_epoch;
        extern void overlay_loader_shadow_escape_fixup(uint64_t target_epoch);
        overlay_loader_shadow_escape_fixup(g_exc_setjmp_epoch);
        longjmp(exception_jmpbuf, code);
    }

    debug_server_log_thread_event(9, cpu, target_tcb, current_tcb, 0);
    g_pc0_reason = PSX_PC0_FIBER_SWITCH;
    cpu->pc = 0;
    return 1;
}

/* ── Deterministic TCB scheduler — LIVE (carve-out, replaces the fiber bridge) ─
 *
 * psx_request_thread_switch performs a cooperative thread switch by committing
 * the yielding thread's full context to its TCB, repointing dword_108->entry at
 * the target, and longjmp-ing to the outer scheduler loop (psx_scheduler_run),
 * which restores the target TCB and re-dispatches its EPC. No host fibers, no
 * per-thread native stacks, no cpu->pc=0 host signal — thread selection is a
 * pure function of guest TCB state (HLE_SCHEDULER_CARVEOUT_PLAN.md §9).
 *
 * This only runs with in_exception==0 (psx_syscall case 3 gates it), so the
 * longjmp never unwinds through a live setjmp(exception_jmpbuf) frame — the sole
 * native state skipped is nested psx_dispatch_impl frames, whose only invariant
 * (g_psx_dispatch_depth + g_psx_call_bail) the scheduler loop resets on landing.
 *
 * INVARIANT (psx_scheduler.h): all guest thread context lives in CPUState / RAM
 * / the TCB reg array — never in a host stack. The yielding thread resumes by
 * re-dispatching its saved EPC, which therefore MUST be a dispatchable block
 * leader (CLASS-B every-leader re-entry guarantees this); psx_is_dispatchable
 * fails loud if it ever is not. */
static int psx_request_thread_switch(CPUState* cpu, uint32_t target_tcb)
{
    uint32_t current_tcb = psx_current_tcb_ptr(cpu);
    debug_server_log_thread_event(3, cpu, current_tcb, target_tcb, 0);
    if (!psx_is_valid_tcb(cpu, current_tcb) || !psx_is_valid_tcb(cpu, target_tcb)) {
        debug_server_log_thread_event(4, cpu, current_tcb, target_tcb, 0);
        return 0; /* invalid TCB(s): caller falls back to the manual-RFE path */
    }
    if (current_tcb == target_tcb) {
        /* Same thread — not a switch. The syscall wrapper returns 0 and falls
         * through to its own jr $ra (CPS); the thread simply continues. No
         * cpu->pc=0 host signal (removed with the fiber bridge). */
        debug_server_log_thread_event(5, cpu, current_tcb, target_tcb, cpu->gpr[31]);
        return 1;
    }
    /* The target's entire context already lives in its TCB; it must be runnable
     * (0x4000). A non-runnable target is not a cooperative yield — let the
     * manual-RFE fallback handle it rather than dispatch a stale EPC. */
    if (psx_tcb_state(cpu, target_tcb) != 0x4000u) {
        debug_server_log_thread_event(7, cpu, current_tcb, target_tcb, 0);
        return 0;
    }
    /* Contain a native-only shadow divergence before it mutates TCB/RAM,
     * scheduler targets, or crosses run_shadow_diff with a longjmp. */
    {
        extern int overlay_loader_shadow_native_thread_switch_bail(void);
        if (overlay_loader_shadow_native_thread_switch_bail()) return -1;
    }
    /* Commit the yielding thread's context so it can be re-dispatched later.
     * resume PC = its return address (resume right after the ChangeThread call).
     * Only a still-running (0x4000) thread keeps a live context; a closed/free
     * (0x1000) thread is being torn down and must not be saved. */
    if (psx_tcb_state(cpu, current_tcb) == 0x4000u) {
        psx_save_context_to_tcb(cpu, current_tcb, cpu->gpr[31]);
    }
    psx_set_current_tcb(cpu, target_tcb);
    g_sched_return_tcb        = current_tcb; /* one-level switch-back safety net */
    g_sched_escape.target_tcb = target_tcb;
    g_sched_escape.resume_pc  = 0;
    g_sched_escape.reason     = PSX_RUN_YIELD_TO_TCB;
    sched_escape_ring_log(cpu, PSX_RUN_YIELD_TO_TCB, current_tcb, target_tcb, 0);
    debug_server_log_thread_event(8, cpu, current_tcb, target_tcb, 0);
    if (parity_trace_is_armed())
        parity_trace_record(PARITY_KIND_YIELD, cpu->pc, cpu->gpr[31],
                            cpu->gpr[29], target_tcb, traps_parity_rw, cpu);
    longjmp(g_scheduler_jmpbuf, 1); /* unwind to psx_scheduler_run; never returns */
    return 1;                        /* unreachable */
}

/* Save-state restore hook: unwind the entire guest native stack back to
 * psx_scheduler_run and re-dispatch at `resume_pc` (the PC stored in the just-
 * restored CPUState). Reuses the same structured-escape mechanism a same-thread
 * RFE uses (PSX_RUN_RESUME_CURRENT) — abandons every suspended CPS continuation
 * on the old stack (safe: the restored state is complete in CPUState/RAM/devices
 * and every block leader is re-enterable). MUST be called on the scheduler fiber
 * (HLE mode, default) at a block-leader boundary with in_exception == 0. */
void psx_scheduler_resume_at(uint32_t resume_pc)
{
    if (!psx_is_dispatchable(resume_pc)) {
        char b[96];
        snprintf(b, sizeof(b),
                 "savestate restore: non-dispatchable resume PC 0x%08X", (unsigned)resume_pc);
        trap_crash(b);
        return;
    }
    g_sched_escape.target_tcb = 0;
    g_sched_escape.resume_pc  = resume_pc;
    g_sched_escape.reason     = PSX_RUN_RESUME_CURRENT;
    longjmp(g_scheduler_jmpbuf, 1); /* unwind to psx_scheduler_run; never returns */
}

/* Scheduler mode. HLE = the deterministic TCB scheduler
 * (psx_request_thread_switch, default); LLE = the legacy host-fiber bridge
 * (psx_change_thread_fiber). This is the HLE tier's standing SUBSYSTEM
 * REPLACEMENT (CLAUDE.md §0 amendments 2026-06-29 + 2026-07-02): unlike the
 * opt-in call-HLE in bios_hle.c it defaults ON in BOTH backends, because the
 * LLE path it replaces (host fibers) has a genuine landmine — host-side
 * non-determinism with no hardware analog. Config default via
 * psx_hle_scheduler_set_default ([runtime] hle_scheduler, main.cpp);
 * PSX_HLE_SCHEDULER env wins over config. Latched at first query so the mode
 * can't flip mid-run — a thread suspended under one scheduler cannot be
 * resumed under the other (fibers keep a native stack; the TCB scheduler
 * keeps none). */
static int s_hle_sched_default = 1;

void psx_hle_scheduler_set_default(int on)
{
    s_hle_sched_default = on ? 1 : 0;
}

int psx_hle_scheduler_enabled(void)
{
    static int s_mode = -1;
    if (s_mode < 0) {
        const char* e = getenv("PSX_HLE_SCHEDULER");
        if (e && e[0]) s_mode = (e[0] == '0') ? 0 : 1;
        else           s_mode = s_hle_sched_default;
    }
    return s_mode;
}

static int psx_change_thread(CPUState* cpu, uint32_t target_tcb)
{
    if (psx_hle_scheduler_enabled())
        return psx_request_thread_switch(cpu, target_tcb);
    return psx_change_thread_fiber(cpu, target_tcb);
}

static int g_in_scheduler_run = 0;
static int g_return_to_lobby_req = 0;

void psx_clear_return_to_lobby(void)
{
    g_return_to_lobby_req = 0;
}

int psx_return_to_lobby_requested(void)
{
    return g_return_to_lobby_req;
}

void psx_request_return_to_lobby(void)
{
    g_return_to_lobby_req = 1;
    if (!g_in_scheduler_run) {
        return;
    }
    g_sched_escape.target_tcb = 0;
    g_sched_escape.resume_pc = 0;
    g_sched_escape.reason = PSX_RUN_RETURN_TO_LOBBY;
    longjmp(g_scheduler_jmpbuf, 1);
}

void psx_scheduler_run(CPUState* cpu)
{
    extern int g_psx_dispatch_depth;
    g_in_scheduler_run = 1;
    g_sched_escape.reason = PSX_RUN_CONTINUE;
    for (;;) {
        if (setjmp(g_scheduler_jmpbuf) != 0) {
            /* A structured escape unwound the native stack to here. Reset the
             * invariants the skipped psx_dispatch_impl frames would otherwise
             * own. No exception_jmpbuf frame is ever skipped (switch fires only
             * with in_exception==0), so interrupt state needs no fixup here. */
            extern void overlay_loader_shadow_scheduler_escape_fixup(void);
            overlay_loader_shadow_scheduler_escape_fixup();
            g_psx_dispatch_depth = 0;
            g_psx_call_bail      = 0;
            /* Soft-exit / yield longjmps out of vblank inside
             * psx_devices_service_to_now; without this the re-entrancy guard
             * stays set and every later advance skips device service forever. */
            { extern int psx_in_device_service; psx_in_device_service = 0; }
            /* A longjmp-out through a nested call unit (overlay_loader_call_native
             * or dispatch_nonlocal_call) would skip its depth restore; clear the
             * nested-unit gate so IRQ checks are not wedged-off after the escape
             * (backstop for the Ape memcard native<->interp fix). */
            { extern int g_call_unit_depth; g_call_unit_depth = 0; }
        }

        switch (g_sched_escape.reason) {
            case PSX_RUN_FATAL:
                g_in_scheduler_run = 0;
                trap_crash("scheduler: structured FATAL escape");
                return;
            case PSX_RUN_GUEST_EXIT:
                g_in_scheduler_run = 0;
                return; /* abnormal top-level exit — main.cpp dumps diagnostics */
            case PSX_RUN_RETURN_TO_LOBBY:
                g_in_scheduler_run = 0;
                return; /* netplay soft-exit — main.cpp reopens lobby UI */
            default:
                break;  /* CONTINUE / YIELD_TO_TCB / RESUME_CURRENT */
        }

        uint32_t run_pc;
        if (g_sched_escape.reason == PSX_RUN_RESUME_CURRENT &&
            g_sched_escape.resume_pc != 0u) {
            /* Same-thread RFE: GPRs already committed by the RFE; just re-dispatch. */
            run_pc = g_sched_escape.resume_pc;
        } else {
            uint32_t cur = psx_current_tcb_ptr(cpu);
            if (psx_is_valid_tcb(cpu, cur)) {
                run_pc = psx_restore_context_from_tcb(cpu, cur);
                if (!psx_is_dispatchable(run_pc)) {
                    char b[112];
                    snprintf(b, sizeof(b),
                        "scheduler: TCB 0x%08X has non-dispatchable resume PC 0x%08X",
                        (unsigned)cur, (unsigned)run_pc);
                    trap_crash(b);
                    return;
                }
            } else {
                run_pc = cpu->pc; /* boot / pre-TCB: run the raw PC */
            }
        }

        cpu->pc = run_pc;
        g_sched_escape.reason = PSX_RUN_CONTINUE;
        psx_dispatch(cpu, run_pc);

        /* psx_dispatch returned with NO structured escape => the outermost
         * dispatch saw cpu->pc==0. If a thread yielded to us and then ran to
         * completion, hand control back to its yielder (one-level safety net)
         * instead of tearing down the whole run. Otherwise it's the legacy
         * top-level abnormal exit. */
        if (psx_is_valid_tcb(cpu, g_sched_return_tcb) &&
            g_sched_return_tcb != psx_current_tcb_ptr(cpu)) {
            uint32_t yielder = g_sched_return_tcb;
            g_sched_return_tcb = 0;
            psx_set_current_tcb(cpu, yielder);
            g_sched_escape.reason = PSX_RUN_YIELD_TO_TCB;
            sched_escape_ring_log(cpu, PSX_RUN_YIELD_TO_TCB,
                                  psx_current_tcb_ptr(cpu), yielder, 0);
            continue;
        }
        g_sched_escape.reason = PSX_RUN_GUEST_EXIT;
        g_in_scheduler_run = 0;
        return;
    }
}

int psx_syscall(CPUState* cpu, uint32_t code) {
    /*
     * PS1 BIOS SYSCALL convention:
     *   $a0 = 1: EnterCriticalSection — disable interrupts, return old SR
     *   $a0 = 2: ExitCriticalSection  — enable interrupts, return old SR
     *   $a0 = 3: ReturnFromException  — restore full TCB state + RFE
     *
     * Syscalls 1 and 2 are always handled directly — they only touch IEc
     * in SR and don't need the full exception mechanism.
     *
     * Syscall 3 and unknown numbers route through the real BIOS exception
     * handler once it's installed, because ReturnFromException must restore
     * the full register state from the current thread's TCB (including the
     * saved SR which carries IM[2]).
     */
    uint32_t func = cpu->gpr[4]; /* $a0 = syscall function number */
    uint32_t sr = cpu->cop0[12];

    switch (func) {
        case 1: /* EnterCriticalSection: disable interrupts */
            cpu->cop0[12] = sr & ~1u; /* clear IEc (bit 0) */
            cpu->gpr[2] = sr & 1u; /* return old IEc */
            g_pc0_reason = PSX_PC0_CRIT_SECTION;
            cpu->pc = 0;
            /* Directly handled "void" syscall. Return 0: under CPS the caller
             * (`if (psx_syscall(...)) return;`) falls through to the inline
             * post-syscall code (the guest's own jr $ra), which returns to the
             * caller via the flat trampoline — matching real-HW resume at
             * EPC+4. Legacy callers ignore this and rely on cpu->pc==0. */
            return 0;

        case 2: /* ExitCriticalSection: enable interrupts */
            cpu->cop0[12] = sr | 0x0401u; /* set IEc (bit 0) + IM[2] (bit 10) */
            cpu->gpr[2] = 0;
            g_pc0_reason = PSX_PC0_CRIT_SECTION;
            cpu->pc = 0;
            return 0;

        case 3: { /* ChangeThread / ReturnFromException */
            uint32_t target_tcb = cpu->gpr[5];
            /* Tomba2 loader-thread diagnosis (Patch 1, trace-only): record every
             * syscall-3 (ChangeThread/RFE) at its decision point. kind 20 = entered
             * with in_exception==0 (eligible for psx_change_thread); kind 24 = entered
             * with in_exception==1 (will be forced down the manual-RFE path, NOT a
             * thread switch). target_tcb is the requested switch target; its state
             * word is auto-captured as target_state. This tells us whether the loader
             * ChangeThread is ever requested (Case A) and, if so, whether in_exception
             * diverts it (Case B). */
            debug_server_log_thread_event(psx_get_in_exception() ? 24u : 20u, cpu,
                                          psx_current_tcb_ptr(cpu), target_tcb, cpu->pc);
            int switch_result = 0;
            if (!psx_get_in_exception())
                switch_result = psx_change_thread(cpu, target_tcb);
            if (switch_result < 0) {
                /* Native shadow validation requested an impossible thread
                 * switch. Its bail flag unwinds the speculative call; return
                 * true so CPS syscall wrappers stop this block immediately. */
                return 1;
            }
            if (switch_result > 0) {
                /* Thread switched (and this thread has since been resumed): the
                 * fiber scheduler leaves cpu->pc == 0. Under CPS that must NOT
                 * be a transfer — return 0 so the syscall wrapper falls through
                 * to its own jr $ra and this thread resumes at its caller via
                 * the flat trampoline. Legacy ignores the return value (it uses
                 * cpu->pc == 0 + the nested-dispatch C-return to resume). */
                return 0;
            }

            uint32_t tcb_ptr_addr = cpu->read_word(0x00000108u);
            if (tcb_ptr_addr != 0) {
                uint32_t save_area = cpu->read_word(tcb_ptr_addr);
                if (save_area != 0) {
                    save_area += 8; /* handler adds 8 before saving */
                    uint32_t saved_epc = cpu->read_word(save_area + 128);
                    uint32_t saved_sr  = cpu->read_word(save_area + 140);
                    /* Restore ALL GPRs from TCB save area.
                     * Layout: offset 0 = $zero (skip), 4 = $at, ... 124 = $ra,
                     *         128 = EPC, 132 = HI, 136 = LO, 140 = SR. */
                    for (int i = 1; i < 32; i++) {
                        cpu->gpr[i] = cpu->read_word(save_area + i * 4);
                    }
                    cpu->hi = cpu->read_word(save_area + 132);
                    cpu->lo = cpu->read_word(save_area + 136);
                    /* RFE pop on saved SR (clears bits [5:0], shifts [5:2]→[3:0]). */
                    cpu->cop0[12] = (saved_sr & 0xFFFFFFC0u) | ((saved_sr >> 2) & 0x0Fu);
                    cpu->pc = saved_epc;
                    if (psx_get_in_exception()) {
                        /* Fix B: saved_epc is the REAL guest EPC restored from the TCB
                         * (never the sentinel). Mark the escape reason so the landing in
                         * psx_check_interrupts does NOT clobber the resumed thread's
                         * TCB-restored GPRs with the entering thread's saved_gpr. */
                        extern int g_exc_escape_reason; extern int g_rfe_escape_pending;
                        g_exc_escape_reason  = PSX_EXC_ESCAPE_SYSCALL_RETURN;
                        g_rfe_escape_pending = 0;
                        psx_exception_longjmp(); /* unwind handler */
                    }
                    return 1;
                }
            }
            /* Fallback: simple RFE on current SR. */
            cpu->cop0[12] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
            cpu->pc = cpu->cop0[14];
            return 1;
        }

        default:
            break;
    }

    /* Unknown syscalls: route through the real BIOS exception handler. */
    uint32_t handler_word = cpu->read_word(0x80000080u);
    if (handler_word != 0) {
        /* Route through the real BIOS exception handler. */
        cpu->cop0[14] = cpu->pc;  /* EPC */
        cpu->cop0[13] = (cpu->cop0[13] & ~0x7Cu) | (0x08u << 2); /* Cause: Syscall */
        cpu->cop0[12] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2); /* SR push */
        uint32_t vector = (sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
        psx_dispatch(cpu, vector);
        return 1;
    }

    /* Early boot fallback for syscall 3. */
    if (func == 3) {
        cpu->cop0[12] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
        cpu->pc = cpu->cop0[14];
        return 1;
    }

    /* Unknown syscall number and no handler — fatal. */
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "SYSCALL %u with no handler @ PC=0x%08X",
                 func, cpu->pc);
        trap_crash(buf);
        fprintf(stderr, "%s\n", buf); fflush(stderr);
        exit(1);
    }
    return 1;
}

void psx_break(CPUState* cpu, uint32_t code, uint32_t pc) {
    char buf[128];
    snprintf(buf, sizeof(buf), "BREAK @ PC=0x%08X, code=0x%05X", pc, code);
    trap_crash(buf);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    exit(1);
}

void psx_arith_overflow(CPUState* cpu) {
    char buf[128];
    snprintf(buf, sizeof(buf), "OVERFLOW @ PC=0x%08X", cpu->pc);
    trap_crash(buf);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    exit(1);
}

void psx_unaligned_access(CPUState* cpu, uint32_t addr, uint32_t pc) {
    /* Survivable fault: log diagnostics and let the generated code's
     * `return;` skip the rest of the current function. */
    static int count = 0;
    if (count < 3) {
        char buf[1024];
        int n = snprintf(buf, sizeof(buf),
            "ADEL: addr=0x%08X PC=0x%08X ra=0x%08X v0=0x%08X a0=0x%08X t9=0x%08X (hit #%d)\n"
            "  Callback table at 0x800DFEE0:",
            addr, pc, cpu->gpr[31], cpu->gpr[2], cpu->gpr[4], cpu->gpr[25], count + 1);
        for (int i = 0; i < 11; i++) {
            uint32_t cb = cpu->read_word(0x800DFEE0u + (uint32_t)(i * 4));
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " [%d]=0x%08X", i, cb);
        }
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
            "\n  Mask=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X",
            cpu->read_word(0x800DFF0Cu),
            cpu->gpr[16], cpu->gpr[17], cpu->gpr[18]);
        trap_crash(buf);
        count++;
    }
}

void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys) {
    /* NULL dispatch: address 0 is never a valid function target.
     * Silently absorb — this can happen when a corrupt function pointer
     * or uninitialized callback slot is dispatched. */
    if (addr == 0) {
        g_pc0_reason = PSX_PC0_DISPATCH_MISS; /* guest jumped/returned to a null (0) target */
        cpu->pc = 0;
        return;
    }

    /* Detect ReturnFromException: when the recompiled B0:0x17 function
     * (or handler epilogue) has already restored all registers from the
     * TCB and done RFE, it sets cpu->pc = saved_epc = our sentinel
     * 0x80000048.  The dispatch loop tail-calls here.  If we're inside
     * the exception handler, longjmp back to psx_check_interrupts to
     * properly unwind the handler call tree. */
    if (addr == 0x80000048u && psx_get_in_exception()) {
        cpu->pc = 0;
        psx_exception_longjmp(); /* does not return */
    }
    /* Async ReturnFromException (Tomba 2 frame-1997 fix): a game-installed handler
     * called ReturnFromException OUTSIDE our synchronous exception window (in_exception
     * already 0). The recompiled BIOS RFE restored the interrupted GPRs from the TCB and
     * set pc = saved EPC = sentinel; with in_exception==0 the longjmp above does not fire.
     * Previously this fell through to pc=0 -> abnormal "execution completed" exit. The
     * interrupted code was dirty-interpreted, so a real guest resume PC was latched at
     * exception entry (g_async_rfe_resume_pc) — resume there instead of exiting. */
    if (addr == 0x80000048u) {
        extern uint32_t g_async_rfe_resume_pc;
        extern uint64_t g_async_rfe_fire_count;
        extern uint64_t g_sentinel_reach_traps;
        extern uint32_t g_sentinel_reach_async;
        g_sentinel_reach_traps++;
        g_sentinel_reach_async = g_async_rfe_resume_pc;
        if (g_async_rfe_resume_pc != 0u) {
            g_async_rfe_fire_count++;
            cpu->pc = g_async_rfe_resume_pc;
            return;
        }
    }

    /* Exception handler chain-walk continuation: 0xBFC10910 (phys 0x00000E10)
     * is the return address set by jalr $s1 in the exception handler chain
     * walker.  In the merged exception handler function, the continuation
     * code at BFC10910 follows the psx_dispatch call as a C fall-through.
     * When the chain handler returns via jr $ra (compiled as C `return;`),
     * the fall-through handles the continuation.  But if external code
     * dispatches to BFC10910 directly (e.g., the trampoline resolver or
     * the psx_dispatch tail-call loop picking up $ra), it's a no-op — the
     * continuation was already handled by the merged function. */
    if (phys == 0x00000E10u) {
        cpu->pc = 0;
        return;
    }

    /* Reject non-word-aligned targets — corrupt function pointer. Hard fail. */
    if (addr & 3) {
        char buf[16384];
        int pos = snprintf(buf, sizeof(buf),
            "DISPATCH FATAL: misaligned target 0x%08X\n"
            "  aligned form: 0x%08X\n"
            "  physical:     0x%08X\n"
            "  cpu->pc:      0x%08X\n"
            "  $ra:          0x%08X\n"
            "  $t9:          0x%08X\n"
            "  $v0:          0x%08X\n"
            "  $a0:          0x%08X\n"
            "  $a1:          0x%08X\n"
            "  $a2:          0x%08X\n"
            "  $a3:          0x%08X\n"
            "  COP0_EPC:     0x%08X\n"
            "  COP0_SR:      0x%08X\n"
            "  COP0_Cause:   0x%08X\n",
            addr, addr & ~3u, phys,
            cpu->pc, cpu->gpr[31], cpu->gpr[25],
            cpu->gpr[2], cpu->gpr[4], cpu->gpr[5],
            cpu->gpr[6], cpu->gpr[7],
            cpu->cop0[14], cpu->cop0[12], cpu->cop0[13]);
        /* Include the dispatch chain that led to the corrupt target. */
        {
            extern uint32_t crash_trace_dispatch_ring_get(int idx);
            extern uint64_t crash_trace_dispatch_seq_get(void);
            uint64_t total = crash_trace_dispatch_seq_get();
            int count = total < 32 ? (int)total : 32;
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                            "\n  dispatch_tail (last %d of %llu):\n", count,
                            (unsigned long long)total);
            uint64_t start = total - (uint64_t)count;
            for (int i = 0; i < count && pos < (int)sizeof(buf) - 48; i++) {
                uint32_t dispatch = crash_trace_dispatch_ring_get(
                    (int)((start + (uint64_t)i) & 0xFFFFu));
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                                "    [%3d] 0x%08X\n", i, dispatch);
            }
        }

        /* Locate stored copies of both the bad pointer and its aligned form.
         * This is diagnostic-only and runs immediately before the fatal trap. */
        {
            extern uint8_t *memory_get_ram_ptr(void);
            uint8_t *ram = memory_get_ram_ptr();
            uint32_t needles[2] = { addr, addr & ~3u };
            int needle_count = needles[0] == needles[1] ? 1 : 2;
            for (int n = 0; n < needle_count; n++) {
                int hits = 0;
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                                "\n  RAM scan for 0x%08X:\n", needles[n]);
                for (uint32_t off = 0; off + 4 <= 2u * 1024u * 1024u; off += 4) {
                    uint32_t word;
                    memcpy(&word, ram + off, sizeof(word));
                    if (word != needles[n]) continue;
                    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                                    "    RAM[0x%08X] = 0x%08X\n",
                                    0x80000000u + off, word);
                    if (++hits == 16 || pos >= (int)sizeof(buf) - 64) break;
                }
                if (!hits)
                    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                                    "    (not found in 2MB PSX RAM)\n");
            }
        }

        /* stderr is emitted before trap_crash enters halt-and-serve. */
        fprintf(stderr, "%s", buf);
        fflush(stderr);
        trap_crash(buf);
        exit(1);
    }

    /*
     * The BIOS writes small jump trampolines into RAM at runtime
     * (e.g., at 0xA0, 0xB0, 0xC0 for the A0/B0/C0 vectors).
     * Pattern: lui rN, hi / addiu rN, rN, lo / jr rN / nop
     * Since we can't execute RAM instructions, resolve the pattern
     * and re-dispatch to the computed target.
     */
    {
        uint32_t w0 = cpu->read_word(addr);
        uint32_t w1 = cpu->read_word(addr + 4);
        uint32_t w2 = cpu->read_word(addr + 8);
        uint32_t op0 = (w0 >> 26) & 0x3F;
        uint32_t op1 = (w1 >> 26) & 0x3F;

        /* Trampoline resolution: set cpu->pc so the dispatch loop re-dispatches.
         * This avoids growing the native stack for RAM trampoline chains. */

        /* Pattern 1: J target */
        if (op0 == 2) {
            uint32_t target = (addr & 0xF0000000u) | ((w0 & 0x03FFFFFFu) << 2);
            cpu->pc = target;
            return;
        }

        /* Pattern 2: JR rs (single instruction) */
        if (op0 == 0 && (w0 & 0x3F) == 0x08) {
            uint32_t rs = (w0 >> 21) & 0x1F;
            cpu->pc = cpu->gpr[rs];
            return;
        }

        /* Pattern 3: addiu rN, $zero, imm / jr rN (small address trampoline) */
        if (op0 == 0x09) { /* ADDIU */
            uint32_t rs0 = (w0 >> 21) & 0x1F;
            uint32_t rt0 = (w0 >> 16) & 0x1F;
            if (rs0 == 0) { /* addiu rN, $zero, imm = li rN, imm */
                int16_t imm = (int16_t)(w0 & 0xFFFF);
                uint32_t target = (uint32_t)(int32_t)imm;
                /* w1 should be jr rN */
                if ((w1 & 0xFC1FFFFF) == 0x00000008) { /* jr rs */
                    uint32_t jr_rs = (w1 >> 21) & 0x1F;
                    if (jr_rs == rt0) {
                        /* w2 is delay slot — execute it as load of $t1 */
                        uint32_t ds_op = (w2 >> 26) & 0x3F;
                        if (ds_op == 0x09) { /* ADDIU in delay slot */
                            uint32_t ds_rs = (w2 >> 21) & 0x1F;
                            uint32_t ds_rt = (w2 >> 16) & 0x1F;
                            int16_t ds_imm = (int16_t)(w2 & 0xFFFF);
                            if (ds_rs == 0) {
                                cpu->gpr[ds_rt] = (uint32_t)(int32_t)ds_imm;
                            }
                        }
                        cpu->pc = target;
                        return;
                    }
                }
            }
        }

        /* Pattern 4: lui rN, hi / addiu|ori rN, rN, lo / jr rN / nop
         * All three instructions must be present. Without the jr check,
         * any function prologue that loads a constant via lui+ori would
         * be misidentified as a trampoline (see 0xBFC3DF90 incident). */
        if (op0 == 0x0F) { /* LUI */
            uint32_t rt0 = (w0 >> 16) & 0x1F;
            uint32_t hi_val = (w0 & 0xFFFF) << 16;
            uint32_t computed = 0;
            int have_target = 0;

            if (op1 == 0x09) { /* ADDIU */
                uint32_t rs1 = (w1 >> 21) & 0x1F;
                uint32_t rt1 = (w1 >> 16) & 0x1F;
                if (rs1 == rt0 && rt1 == rt0) {
                    int16_t lo_val = (int16_t)(w1 & 0xFFFF);
                    computed = hi_val + (uint32_t)(int32_t)lo_val;
                    have_target = 1;
                }
            }
            if (!have_target && op1 == 0x0D) { /* ORI */
                uint32_t rs1 = (w1 >> 21) & 0x1F;
                uint32_t rt1 = (w1 >> 16) & 0x1F;
                if (rs1 == rt0 && rt1 == rt0) {
                    computed = hi_val | (w1 & 0xFFFF);
                    have_target = 1;
                }
            }
            /* Only resolve if w2 is jr rN targeting the same register. */
            if (have_target && (w2 & 0xFC1FFFFF) == 0x00000008) {
                uint32_t jr_rs = (w2 >> 21) & 0x1F;
                if (jr_rs == rt0) {
                    cpu->pc = computed;
                    return;
                }
            }

            /* Pattern 5: BIOS vector dispatch table.
             * lui  rN, hi             w0
             * addiu rN, rN, lo        w1  (base = hi|lo)
             * sll  rM, rM, 2          w2  (index <<= 2)
             * addu rN, rN, rM         w3  (ptr = base + index*4)
             * lw   rN, 0(rN)          w4  (func = *ptr)
             * jr   rN                 w5
             * This is the A0/B0/C0 dispatch pattern the BIOS writes at
             * 0x500+.  rM holds the function number (set by the A0/B0/C0
             * trampoline delay slot before we get here). */
            if (have_target) {
                uint32_t w3 = cpu->read_word(addr + 12);
                uint32_t w4 = cpu->read_word(addr + 16);
                uint32_t w5 = cpu->read_word(addr + 20);

                /* w2: sll rM, rM, 2  (opcode 0, func 0, sa 2) */
                if ((w2 & 0xFC0007FF) == 0x00000080) { /* SLL with sa=2 */
                    uint32_t idx_rt = (w2 >> 16) & 0x1F;
                    uint32_t idx_rd = (w2 >> 11) & 0x1F;
                    /* w3: add/addu rN, rN, rM */
                    if ((w3 & 0xFC0007FE) == 0x00000020) { /* ADD or ADDU */
                        uint32_t a_rs = (w3 >> 21) & 0x1F;
                        uint32_t a_rt = (w3 >> 16) & 0x1F;
                        uint32_t a_rd = (w3 >> 11) & 0x1F;
                        if (a_rs == rt0 && a_rt == idx_rd && a_rd == rt0) {
                            /* w4: lw rN, 0(rN) */
                            if ((w4 & 0xFFFF0000) == (0x8C000000u | ((uint32_t)rt0 << 21) | ((uint32_t)rt0 << 16))) {
                                /* w5: jr rN — or nop then jr rN (load delay slot) */
                                uint32_t jr_word = w5;
                                if (w5 == 0x00000000u) {
                                    jr_word = cpu->read_word(addr + 24);
                                }
                                if ((jr_word & 0xFC1FFFFF) == 0x00000008 && ((jr_word >> 21) & 0x1F) == rt0) {
                                    uint32_t index_val = cpu->gpr[idx_rt];
                                    uint32_t table_addr = computed + (index_val << 2);
                                    uint32_t func_ptr = cpu->read_word(table_addr);
                                    cpu->pc = func_ptr;
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    {
        /* Always-on ring buffer of dispatch misses. Queryable via the
         * `unknown_dispatch_log` debug command. Replaces the prior
         * file-based log per CLAUDE.md §3. */
        extern void psx_unknown_dispatch_record(uint32_t addr, uint32_t phys,
                                                 uint32_t ra, uint32_t a0,
                                                 uint32_t a1);
        psx_unknown_dispatch_record(addr, phys, cpu->gpr[31],
                                    cpu->gpr[4], cpu->gpr[5]);

        /* LOUD BY DEFAULT. A miss that reaches this point is a real,
         * unresolvable dispatch target — a recompiler discovery bug (ROM
         * code the static pass never found) or corrupt guest state. The
         * old default silently no-op'd it, leaving stale register state
         * and a broken call chain; that masked the MMX6 memset miss
         * (A0:2B -> ROM 0xBFC02B8C) for weeks while the symptom was
         * chased as an event/timing bug (2026-07-02). Per CLAUDE.md §0
         * a function is either fully implemented or it aborts fatally —
         * so the first miss dumps a full crash report and halts.
         *
         * PSX_FAIL_FAST_UNKNOWN_DISPATCH=0 opts OUT for diagnostic
         * sessions that need to survive misses to enumerate them (the
         * miss is still ring-recorded either way; see
         * unknown_dispatch_log). No interp fallback here on purpose:
         * the dirty-RAM interpreter is reserved for runtime-written
         * code (Rule 18); interpreting around a static-discovery hole
         * would hide it exactly like the silent no-op did. */
        static int s_fail_fast = -1;
        if (s_fail_fast < 0) {
            const char *e = getenv("PSX_FAIL_FAST_UNKNOWN_DISPATCH");
            s_fail_fast = (e && *e == '0') ? 0 : 1;
        }
        if (s_fail_fast) {
            extern void psx_crash_trace_dump(const char *reason, void *seh_info);
            psx_crash_trace_dump("fail_fast_unknown_dispatch", NULL);
            char msg[256];
            snprintf(msg, sizeof(msg),
                "FAIL-FAST unknown dispatch: addr=0x%08X phys=0x%08X ra=0x%08X "
                "a0=0x%08X a1=0x%08X — see psx_last_run_report.json\n"
                "(recompiler discovery gap: if addr is BIOS ROM, seed it — "
                "recompiler/seeds/ — and regen; PSX_FAIL_FAST_UNKNOWN_DISPATCH=0 "
                "to survive-and-log instead)\n",
                addr, phys, cpu->gpr[31], cpu->gpr[4], cpu->gpr[5]);
            trap_crash(msg);
            exit(1);
        }

        /* Opt-out path: return without executing (function is a no-op),
         * ring-recorded above. Stale registers WILL corrupt the caller —
         * diagnostic use only. */
        g_pc0_reason = PSX_PC0_DISPATCH_MISS;
        cpu->pc = 0;
    }
}
