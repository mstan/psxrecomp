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
#include "dma.h"
#include "cpu_state.h"
#include "debug_server.h"
#include "event_ring.h"
#include "lockstep.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

/* Event-timeline ring: execution-mode flag owned by dirty_ram_interp.c. The
 * static BIOS exception handler is NOT interp code, so we clear it around the
 * handler dispatch (see psx_check_interrupts). */
extern int g_dirty_interp_active;
extern uint32_t g_dirty_safe_resume_pc;

/* IRQ-delivery context ring (MMX6 VSync-vs-CD-DMA hunt; dumped via `irqctx_ring`). */
#define IRQCTX_RING_CAP 4096u
typedef struct {
    uint64_t seq, cycle;
    uint32_t frame, istat, imask, sr, d44, cdrom_active, is_vblank;
    int      dma_depth;
    /* Exception-EXIT half (Tomba menu IRQ-resume wedge): filled when this
     * delivery returns to the interrupted code, so the ring shows for EVERY
     * delivery which escape path fired and whether the interrupted GPRs were
     * restored. take_pc/real_epc are the entry-side resume selection. */
    uint32_t take_pc;      /* resume PC selected at entry (0 = boundary/sentinel) */
    uint32_t real_epc;     /* g_exception_real_epc installed at entry */
    uint32_t exit_pc;      /* cpu->pc at the restore decision */
    uint32_t exit_reason;  /* g_exc_escape_reason at exit */
    uint32_t same_thread;  /* same_thread_resume discriminator result */
    uint32_t restored;     /* saved_gpr restore fired */
    uint32_t v1_exit;      /* cpu->gpr[3] at exit before restore decision */
    uint32_t v1_saved;     /* saved_gpr[3] (interrupted code's v1) */
    uint32_t ra_exit;      /* cpu->gpr[31] at exit before restore decision */
    uint32_t ra_saved;     /* saved_gpr[31] */
    uint32_t redirects;    /* jmp_val==2 RestoreState redirects in this delivery */
    /* Ape memcard native<->interp resume-desync probe: the live sp and the
     * dirty pump-site at IRQ ENTRY. The interrupted-thread TCB save captures
     * exactly this (resume_pc=take_pc, sp=entry_sp); a pair where take_pc is a
     * dirty return point but entry_sp belongs to a native callee's frame is the
     * corrupt snapshot (0x801384AC paired with 0x801EFE80). */
    uint32_t entry_sp;     /* cpu->gpr[29] at exception entry */
    uint32_t pump_site;    /* g_cosim_dirty_pump_site at entry (which delivery path) */
} IrqCtxEntry;
IrqCtxEntry g_irqctx_ring[IRQCTX_RING_CAP];
uint64_t    g_irqctx_seq = 0;
extern uint64_t psx_get_cycle_count(void);

/* Record an interrupt-delivery decision into the event ring. GATE outcomes are
 * edge-suppressed (they repeat every block while blocked); DELIVER is always
 * recorded (once per interrupt); the not-pending (idle) case emits nothing but
 * still updates the edge key so the next gate/deliver is captured. */
static void irq_record_outcome(uint8_t kind, uint8_t detail, uint32_t aux) {
    static uint16_t s_last = 0xFFFFu;
    uint16_t key = ((uint16_t)kind << 8) | detail;
    int repeat = (key == s_last);
    s_last = key;
    if (kind == EV_NONE) return;                 /* idle: update key only */
    /* IRQ_DELIVER: aux carries the architectural TAKE-PC (the resume PC), so the
     * interrupt-take point can be compared across backends at the SAME guest
     * cycle — native takes the IRQ at a basic-block boundary, the dirty-interp
     * per-instruction. A take-PC that diverges at identical cycle is the
     * take-point-granularity signature (PRINCIPLES "Control Flow Semantics":
     * model the interrupt frame explicitly; the class fix is precise event
     * slicing). The independent Beetle oracle records the same field. */
    if (kind == EV_IRQ_DELIVER) { event_ring_record_aux(EV_IRQ_DELIVER, detail, aux); return; }
    if (!repeat) event_ring_record(kind, detail);/* GATE: edge only */
}

/* COP0 register indices */
#define COP0_SR    12
#define COP0_CAUSE 13
#define COP0_EPC   14

/* I_STAT and I_MASK are owned by memory.c */
extern uint32_t i_stat;
extern uint32_t i_mask;

/* Device-event cycle ring (device_trace.c): every hardware IRQ-raise edge is
 * recorded here with its guest cycle. */
#include "device_trace.h"

/* Central IRQ-raise choke point. All device sources call this to set their
 * I_STAT bit so the device-event ring sees every raise from one place with the
 * exact guest cycle. Pure addition over `i_stat |= (1<<bit)` — identical effect
 * on i_stat, plus the trace note (no-op unless the ring is armed). */
void psx_irq_raise(uint32_t bit, uint32_t detail)
{
    i_stat |= (1u << bit);
    device_trace_note(bit, detail);
}

/* Dispatch counter for vblank scheduling. */
#define VBLANK_INTERVAL 50000        /* legacy: dispatch-count fallback (unused for VBlank gating now) */
#define VBLANK_CYCLES   564480u      /* 33.8688 MHz / 60 Hz — real PSX NTSC VBlank period */
#define VBLANK_DEFER_STALE_CYCLES (VBLANK_CYCLES * 10ull)
static uint32_t dispatch_count;
static uint64_t total_checks;
static uint32_t cycles_since_vblank;  /* incremented by interrupts_advance_cycles */
extern uint64_t g_vblank_raise_count;
extern int g_cosim_dirty_pump_site;

/* Reentrancy guard: prevent interrupt handler from triggering interrupts. */
static int in_exception;
/* Guest-cycle deadline: IRQ delivery is blocked while psx_get_cycle_count() is
 * below this. 0 = no cooldown active. Counted in guest cycles (not calls) so both
 * backends make the identical delivery decision — see the cooldown constants. */
static uint64_t post_exception_cooldown_until;

static uint32_t last_sio_seq_seen;
static uint64_t last_sio_progress_cycle;

#ifdef PSX_COSIM
#define COSIM_IRQ_RING_CAP 4096u
typedef struct {
    uint64_t seq;
    uint64_t cycle;
    uint32_t kind;
    uint32_t cpu_pc;
    uint32_t take_pc;
    uint32_t dirty_resume_pc;
    uint32_t compiled_resume_pc;
    uint32_t epc;
    uint32_t sr_before;
    uint32_t sr_after;
    uint32_t cause;
    uint32_t istat;
    uint32_t imask;
    uint32_t func;
    uint32_t block;
    uint32_t native;
    int32_t cooldown;
    int32_t dirty_site;
} CosimIrqEntry;
static CosimIrqEntry s_cosim_irq_ring[COSIM_IRQ_RING_CAP];
static uint64_t s_cosim_irq_seq;
extern int g_cosim_dirty_pump_site;
extern uint32_t g_debug_current_func_addr;
extern uint32_t cosim_last_block(void);
extern uint32_t overlay_loader_get_inprogress(void);

static void cosim_irq_note(CPUState *cpu,
                           uint32_t kind,
                           uint32_t take_pc,
                           uint32_t dirty_resume_pc,
                           uint32_t compiled_resume_pc,
                           uint32_t sr_before)
{
    CosimIrqEntry *e = &s_cosim_irq_ring[s_cosim_irq_seq & (COSIM_IRQ_RING_CAP - 1u)];
    e->seq = s_cosim_irq_seq++;
    e->cycle = psx_get_cycle_count();
    e->kind = kind;
    e->cpu_pc = cpu ? cpu->pc : 0;
    e->take_pc = take_pc;
    e->dirty_resume_pc = dirty_resume_pc;
    e->compiled_resume_pc = compiled_resume_pc;
    e->epc = cpu ? cpu->cop0[COP0_EPC] : 0;
    e->sr_before = sr_before;
    e->sr_after = cpu ? cpu->cop0[COP0_SR] : 0;
    e->cause = cpu ? cpu->cop0[COP0_CAUSE] : 0;
    e->istat = i_stat;
    e->imask = i_mask;
    e->func = g_debug_current_func_addr;
    e->block = cosim_last_block();
    e->native = overlay_loader_get_inprogress();
    {   /* remaining cooldown cycles (0 = inactive) for the cosim ring display */
        uint64_t now = psx_get_cycle_count();
        e->cooldown = (post_exception_cooldown_until > now)
                    ? (int32_t)(post_exception_cooldown_until - now) : 0;
    }
    e->dirty_site = g_cosim_dirty_pump_site;
}

void interrupts_cosim_irq_dump(char *out, int cap)
{
    if (!out || cap <= 0) return;
    char *p = out;
    size_t rem = (size_t)cap;
    uint64_t total = s_cosim_irq_seq < COSIM_IRQ_RING_CAP ? s_cosim_irq_seq : COSIM_IRQ_RING_CAP;
    uint64_t count = total;
    if (count > 16u) count = 16u;
    int w = snprintf(p, rem, "irqtrace count %llu",
                     (unsigned long long)count);
    if (w < 0 || (size_t)w >= rem) { out[cap - 1] = 0; return; }
    p += w; rem -= (size_t)w;
    for (uint64_t i = 0; i < count && rem > 1; i++) {
        uint64_t seq = s_cosim_irq_seq - count + i;
        CosimIrqEntry *e = &s_cosim_irq_ring[seq & (COSIM_IRQ_RING_CAP - 1u)];
        w = snprintf(p, rem,
                     " ; seq %llu kind %u cyc %llu pc %08x take %08x dirty %08x compiled %08x epc %08x sr0 %08x sr1 %08x cause %08x istat %08x imask %08x func %08x block %08x native %08x cool %d site %d",
                     (unsigned long long)e->seq,
                     e->kind,
                     (unsigned long long)e->cycle,
                     e->cpu_pc, e->take_pc, e->dirty_resume_pc,
                     e->compiled_resume_pc, e->epc, e->sr_before,
                     e->sr_after, e->cause, e->istat, e->imask,
                     e->func, e->block, e->native,
                     e->cooldown, e->dirty_site);
        if (w < 0 || (size_t)w >= rem) { break; }
        p += w; rem -= (size_t)w;
    }
    w = snprintf(p, rem, " | deliveries");
    if (w >= 0 && (size_t)w < rem) { p += w; rem -= (size_t)w; }
    uint64_t emitted = 0;
    for (uint64_t scanned = 0; scanned < total && emitted < 16u && rem > 1; scanned++) {
        uint64_t seq = s_cosim_irq_seq - 1u - scanned;
        CosimIrqEntry *e = &s_cosim_irq_ring[seq & (COSIM_IRQ_RING_CAP - 1u)];
        if (e->kind != 1u) continue;
        w = snprintf(p, rem,
                     " ; seq %llu cyc %llu pc %08x take %08x dirty %08x compiled %08x epc %08x sr0 %08x sr1 %08x cause %08x istat %08x imask %08x func %08x block %08x native %08x cool %d site %d",
                     (unsigned long long)e->seq,
                     (unsigned long long)e->cycle,
                     e->cpu_pc, e->take_pc, e->dirty_resume_pc,
                     e->compiled_resume_pc, e->epc, e->sr_before,
                     e->sr_after, e->cause, e->istat, e->imask,
                     e->func, e->block, e->native,
                     e->cooldown, e->dirty_site);
        if (w < 0 || (size_t)w >= rem) { break; }
        p += w; rem -= (size_t)w;
        emitted++;
    }
    uint64_t latest_delivery = UINT64_MAX;
    for (uint64_t scanned = 0; scanned < total; scanned++) {
        uint64_t seq = s_cosim_irq_seq - 1u - scanned;
        CosimIrqEntry *e = &s_cosim_irq_ring[seq & (COSIM_IRQ_RING_CAP - 1u)];
        if (e->kind == 1u) { latest_delivery = e->seq; break; }
    }
    if (latest_delivery != UINT64_MAX && rem > 1) {
        w = snprintf(p, rem, " | pre_delivery");
        if (w >= 0 && (size_t)w < rem) { p += w; rem -= (size_t)w; }
        uint64_t first = (latest_delivery >= 15u) ? (latest_delivery - 15u) : 0u;
        for (uint64_t seq = first; seq <= latest_delivery && rem > 1; seq++) {
            CosimIrqEntry *e = &s_cosim_irq_ring[seq & (COSIM_IRQ_RING_CAP - 1u)];
            w = snprintf(p, rem,
                         " ; seq %llu kind %u cyc %llu pc %08x take %08x dirty %08x compiled %08x epc %08x sr0 %08x sr1 %08x cause %08x istat %08x imask %08x func %08x block %08x native %08x cool %d site %d",
                         (unsigned long long)e->seq,
                         e->kind,
                         (unsigned long long)e->cycle,
                         e->cpu_pc, e->take_pc, e->dirty_resume_pc,
                         e->compiled_resume_pc, e->epc, e->sr_before,
                         e->sr_after, e->cause, e->istat, e->imask,
                         e->func, e->block, e->native,
                         e->cooldown, e->dirty_site);
            if (w < 0 || (size_t)w >= rem) { break; }
            p += w; rem -= (size_t)w;
        }
    }
    if (rem >= 2) {
        p[0] = '\n';
        p[1] = 0;
    } else if (cap == 1) {
        out[0] = 0;
    } else {
        out[cap - 2] = '\n';
        out[cap - 1] = 0;
    }
}
#endif

static void note_sio_progress_cycle(void) {
    uint32_t cur_sio_seq = sio_get_seq();
    if (cur_sio_seq != last_sio_seq_seen) {
        last_sio_seq_seen = cur_sio_seq;
        last_sio_progress_cycle = psx_get_cycle_count();
    }
}

static int should_defer_vblank_for_sio(void) {
    if (!sio_card_protocol_active()) return 0;
    uint64_t now = psx_get_cycle_count();
    uint64_t since_progress = now >= last_sio_progress_cycle
                            ? now - last_sio_progress_cycle
                            : 0;
    return since_progress < VBLANK_DEFER_STALE_CYCLES;
}

static void fire_vblank_edge(void) {
    /* Subtract one VBlank period rather than reset to 0 so cycle overshoot
     * carries forward. Prevents long-running blocks from rounding multiple
     * VBlanks together. */
    cycles_since_vblank -= VBLANK_CYCLES;
    dispatch_count = 0;
    /* DEQUEUE: this VBlank fired. ENQUEUE: next VBlank scheduled one period out. */
    event_ring_record_aux(EV_DEQ, (uint8_t)SRC_VBLANK,
                          (uint32_t)psx_get_cycle_count());
    event_ring_record_aux(EV_ENQ, (uint8_t)SRC_VBLANK,
                          (uint32_t)(psx_get_cycle_count() + VBLANK_CYCLES));
    psx_irq_raise(IRQ_VBLANK, 0);
    g_vblank_raise_count++;
    event_ring_record(EV_ISTAT_RAISE, IRQ_VBLANK);
    gpu_vblank_tick();  /* Toggle LCF (GPUSTAT bit 31) */
#ifndef PSX_ENABLE_BLOCK_CYCLES
    timers_tick(33868); /* ~1 NTSC frame worth of cycles */
    cdrom_tick();      /* Process pending CDROM responses */
#endif
}

void interrupts_service_scheduled_events(void) {
    note_sio_progress_cycle();
    if (in_exception) return;
    while (cycles_since_vblank >= VBLANK_CYCLES) {
        if (should_defer_vblank_for_sio()) return;
        fire_vblank_edge();
    }
}

uint32_t interrupts_cycles_to_vblank(void) {
    if (cycles_since_vblank >= VBLANK_CYCLES) return 0;
    return VBLANK_CYCLES - cycles_since_vblank;
}

void interrupts_advance_cycles(uint32_t cycles) {
    cycles_since_vblank += cycles;
    interrupts_service_scheduled_events();
}

/* Diagnostic: total times the exception handler was entered (in_exception
 * transitioned 0->1).  Diff this between two snapshots to measure handler
 * dispatch rate. */
static uint64_t exception_entries_total;

/* Diagnostic: psx_check_interrupts called while already in_exception (the
 * `return` path at line ~159).  Real hardware can't double-fault into the
 * same handler; on our model this should stay near zero — non-zero means
 * something is calling psx_check_interrupts from inside the recompiled
 * exception handler tree. */
static uint64_t exception_reentry_blocks;
/* Nested-exception depth (0 = not in exception). in_exception stays the
 * boolean the rest of the runtime reads; this tracks how deep the
 * synchronous-handler recursion is so the nested-delivery path (hardware
 * IEc-re-enabled semantics, see the gate below) can cap host-stack use. */
static int exception_nest_depth;
/* Nested-delivery gate veto attribution (freeze_check). One counter per guard
 * in the refinement gate below; first-true-wins ordering matches the gate. */
uint64_t g_nestgate_depth, g_nestgate_rfepend, g_nestgate_escreason, g_nestgate_iec;

/* After the exception handler returns, suppress the next interrupt delivery
 * to give the interrupted code at least one block of execution — matching
 * real hardware where at least one instruction runs after RFE before the
 * pending interrupt can re-fire.  Without this, unhandled interrupts cause
 * a livelock: the handler runs, doesn't clear I_STAT, returns, and the
 * very next psx_check_interrupts re-enters immediately. */
/* IRQ raise/deliver/ack telemetry (Tomba 2 exception-reentry-storm diagnosis).
 * Always-on, surfaced in the freeze heartbeat. Distinguishes:
 *   raise≈deliver≈ack≈1/frame  → healthy
 *   deliver≫ack                → handler runs but doesn't clear i_stat (re-delivered)
 *   raise≫1/frame              → something keeps RE-RAISING the bit (cycle/condition bug)
 * g_vblank_ack_count is incremented in memory.c at the I_STAT write that clears
 * the VBLANK bit. The others are incremented here. */
uint64_t g_vblank_raise_count   = 0;  /* bit0 set at the cycle-paced raise site */
uint64_t g_vblank_deliver_count = 0;  /* VBLANK delivered to the guest (exception taken) */
uint64_t g_irq_deliver_count    = 0;  /* ANY hardware interrupt delivered */
uint64_t g_cdrom_deliver_count  = 0;  /* CD IRQ (IRQ_CDROM) delivered to the guest — FMV dispatch probe */
extern uint64_t g_vblank_ack_count;   /* defined in memory.c */

/* Blocks of guaranteed main-code forward progress imposed after a CLAIMED
 * non-SIO interrupt (DMA/VBLANK/timer/...). With cooldown 0 (the prior blanket
 * policy, commit 6d2cb65) the block leader at the interrupted PC re-takes the
 * exception before the block body executes, so under a fast-disc DMA flood the
 * main code is pinned at one PC forever (reentry-storm freeze). A few blocks
 * guarantee the interrupted block — and the field loop — advance between
 * deliveries. SIO is exempt (card reads need immediate back-to-back IRQs).
 *
 * FAITHFULNESS (MMX5 cp-3259 fork, 2026-07-04): the cooldown window is counted
 * in GUEST CYCLES, not psx_check_interrupts CALLS. A call is a backend-dependent
 * unit — the compiled path checks at block edges, the dirty-RAM interp checks
 * per-instruction, and compiled has extra check sites (e.g. at a `jr ra` return
 * transition) the interp lacks. A per-call countdown therefore reached zero at a
 * DIFFERENT guest cycle in each backend → IRQ delivered at a different cycle/EPC
 * → the two backends forked (cosim first-divergence cp 3259 @ cyc ~213.5M). Guest
 * cycles are advanced identically by both backends (shared cycle-cost model, ruler-
 * validated), so a guest-cycle deadline makes the delivery decision backend-
 * independent. Magnitudes below preserve the old behavior's breathing room: the
 * claimed window ~= the old "8 blocks", the unclaimed ~= the old "500 blocks"
 * (a compiled block edge ~= one psx_check_interrupts call ~= ~15 guest cycles). */
#define POST_EXC_CLAIMED_COOLDOWN_CYCLES   128u   /* claimed non-SIO: a few blocks */
#define POST_EXC_UNCLAIMED_COOLDOWN_CYCLES 8192u  /* unclaimed: generous boot window */

/* setjmp target for ReturnFromException during handler dispatch.
 *
 * IMPORTANT: longjmp(exception_jmpbuf, ...) MUST execute on the same
 * Windows fiber that called setjmp; otherwise RSP is restored to that
 * fiber's stack while the OS still tracks a different fiber as current,
 * corrupting fiber state and eventually deadlocking SwitchToFiber.
 *
 * s_exception_owner_fiber records which fiber called setjmp. If a
 * longjmp request originates on a different fiber, the caller must:
 *   (1) set s_pending_exception_longjmp = code
 *   (2) SwitchToFiber back to s_exception_owner_fiber
 * That fiber's wrapped SwitchToFiber call will observe the flag on
 * return and execute the longjmp on the correct stack. */
jmp_buf exception_jmpbuf;  /* non-static so traps.c can deferred-longjmp */
/* Monotonic epoch of the exception setjmp frame: bumped each time the setjmp
 * loop is (re)armed at exception entry. Lets an observer holding a frame on
 * the C stack (the overlay shadow-diff, run_shadow_diff) decide whether a
 * longjmp targets a frame BELOW it (armed before it — the unwind escapes its
 * frame and skips its epilogue) or a frame armed after it (contained). The
 * single global exception_jmpbuf always holds the latest-armed frame, so the
 * current epoch identifies every longjmp's target. */
uint64_t g_exc_setjmp_epoch = 0;
uint64_t psx_exception_setjmp_epoch(void) { return g_exc_setjmp_epoch; }
/* The fiber that owns the current exception setjmp. A longjmp must run on
 * that same fiber/stack, so a non-owner defers by switching back to it
 * first (see deferred_exception_longjmp). Used on all platforms now that
 * the thread scheduler is fiber-based everywhere. */
void* g_exception_owner_fiber = NULL;
int   g_pending_exception_longjmp = 0;
extern int g_psx_dispatch_depth;

/* Set by psx_check_interrupts_at while a compiled block-leader interrupt check
 * is in progress. If the delivered handler later RFEs to the sentinel outside
 * the synchronous host window, dirty_ram_dispatch can resume at this guest PC
 * instead of treating the sentinel as pc=0 termination. */
static uint32_t s_compiled_interrupt_resume_pc = 0;
static uint32_t s_last_interrupt_check_pc = 0;
static uint64_t s_last_interrupt_check_cycle = UINT64_MAX;

/* Deferred cooperative thread switch from nested exception delivery.
 *
 * A genuine in-exception ChangeThread (kind-30, escape site below) must be
 * honored at the OUTERMOST dispatch boundary, never mid-nested-host-dispatch.
 * Only at the outermost boundary is the outgoing thread's guest state fully
 * materialized in CPUState (its true block PC is live, GPRs current); nested
 * inside a call unit / dirty pump the PC lives only in the resume-PC latch,
 * which desyncs from the live GPRs when the IRQ was taken deep inside a nested
 * call (the async-poll callback), so switching there saves a poisoned context
 * (latched PC ahead of the callback's registers) → the resumed thread dispatches
 * a jumptable with a smeared index → misaligned DISPATCH FATAL. When a switch is
 * detected nested, we record it here, let the outgoing thread resume
 * transparently, and honor it at the next outermost block-leader boundary (top
 * of psx_check_interrupts) where the thread can be re-saved cleanly. */
static int      s_defer_switch_pending = 0;
static uint32_t s_defer_switch_target  = 0;  /* TCB PCB[0] should name after the switch */
static uint32_t s_defer_switch_from    = 0;  /* the interrupted thread to re-save cleanly */

/* A/B toggle: PSX_DEFER_SWITCH=0 forces the legacy immediate-switch behavior
 * (longjmp the instant a mid-exception ChangeThread is detected, regardless of
 * nesting) so the deferred-switch fix can be compared against the baseline
 * without a rebuild. Default ON. Latched at first query. */
static int defer_switch_enabled(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("PSX_DEFER_SWITCH"); s = (e && e[0] == '0') ? 0 : 1; }
    return s;
}

static int same_guest_pc(uint32_t a, uint32_t b) {
    return (((a ^ b) & 0x1FFFFFFFu) == 0);
}

int psx_get_in_exception(void) { return in_exception; }

/* Co-sim (COSIM_ORACLE.md): fold the GENUINE guest-timing interrupt statics into the
 * state hash. Deliberately EXCLUDES total_checks / dispatch_count / post_exception_
 * cooldown / s_compiled_interrupt_resume_pc — those are counted in psx_check_interrupts
 * CALLS or are backend-internal, so they legitimately differ between the compiled and
 * interp backends by call frequency and would be a false first-divergence. The one
 * quantity that is a real guest-cycle timing value is cycles_since_vblank (drives the
 * VBLANK deadline); in_exception mirrors the architectural handler-active state. */
uint64_t interrupts_cosim_hash(uint64_t h) {
    const uint64_t P = 1099511628211ULL;
    uint32_t v = cycles_since_vblank;
    for (int i = 0; i < 4; i++) { h ^= (uint8_t)(v >> (i*8)); h *= P; }
    uint64_t sp = last_sio_progress_cycle;
    for (int i = 0; i < 8; i++) { h ^= (uint8_t)(sp >> (i*8)); h *= P; }
    h ^= (uint8_t)in_exception; h *= P;
    return h;
}
/* Co-sim field dump: expose the exact irqctl-timing fields so a divergence can be
 * field-diffed (is it cycles_since_vblank = VBLANK-raise-timing, or in_exception, ...). */
void interrupts_cosim_dump(uint32_t *csv, int *inexc) {
    if (csv)   *csv   = cycles_since_vblank;
    if (inexc) *inexc = in_exception;
}

/* ===== Fix B: faithful exception-return escape state (see psx_runtime.h) ===== */
int      g_rfe_escape_pending = 0;
int      g_exc_escape_reason  = PSX_EXC_ESCAPE_NONE;
uint32_t g_exception_real_epc = 0;
extern void psx_exception_longjmp(void);

/* Called by the recompiled `rfe` opcode (after it pops the COP0 SR stack). When we
 * are inside the SYNCHRONOUS exception-handler window (in_exception) AND the handler
 * is returning to a REAL EPC (not the legacy boundary-sentinel fallback), arm the
 * host escape so the trampoline unwinds back to psx_check_interrupts after the jr
 * commits cpu->pc = real EPC. On a fiber/thread resume (in_exception==0) this is a
 * no-op and the real EPC is dispatched directly — that is how a suspended thread
 * resumes at its own PC. */
void psx_rfe_mark_escape(void) {
    if (in_exception && g_exc_escape_reason != PSX_EXC_ESCAPE_LEGACY_SENTINEL) {
        g_rfe_escape_pending = 1;
        g_exc_escape_reason  = PSX_EXC_ESCAPE_RFE_RETURN;
    }
}

/* Called in the dispatch trampoline after a function returns (cpu->pc holds the
 * real resume EPC). If an RFE armed the escape inside the synchronous handler,
 * longjmp back to psx_check_interrupts (cpu->pc preserved across the longjmp). */
void psx_rfe_escape_check(CPUState* cpu) {
    (void)cpu;
    /* Escape ONLY when this RFE is the synchronous handler completing on the SAME
     * fiber that set up the exception (the owner of exception_jmpbuf). in_exception
     * is a single global, so a thread RESUMED on a different fiber (its own real EPC
     * was restored and it later RFEs) must NOT longjmp here — that would be a
     * cross-fiber jump to a stale jmpbuf (crash). For that case we fall through and
     * the trampoline keeps dispatching cpu->pc = the real resume EPC, which is
     * exactly how a suspended thread continues at its own PC. */
    extern void* psx_fiber_current(void); /* psx_fiber.h is included further down */
    if (g_rfe_escape_pending && in_exception &&
        psx_fiber_current() == g_exception_owner_fiber) {
        g_rfe_escape_pending = 0;
        /* Same shadow-frame hardening as deferred_exception_longjmp. */
        extern void overlay_loader_shadow_escape_fixup(uint64_t target_epoch);
        overlay_loader_shadow_escape_fixup(g_exc_setjmp_epoch);
        longjmp(exception_jmpbuf, 1); /* same fiber: safe; lands in psx_check_interrupts */
    }
}

void psx_get_freeze_diag(uint64_t *out_total_checks,
                         uint32_t *out_dispatch_count,
                         int *out_in_exception,
                         int *out_post_exc_cooldown,
                         uint64_t *out_exc_entries,
                         uint64_t *out_exc_reentry_blocks) {
    if (out_total_checks)        *out_total_checks       = total_checks;
    if (out_dispatch_count)      *out_dispatch_count     = dispatch_count;
    if (out_in_exception)        *out_in_exception       = in_exception;
    if (out_post_exc_cooldown) {
        uint64_t now = psx_get_cycle_count();
        *out_post_exc_cooldown = (post_exception_cooldown_until > now)
                               ? (int)(post_exception_cooldown_until - now) : 0;
    }
    if (out_exc_entries)         *out_exc_entries        = exception_entries_total;
    if (out_exc_reentry_blocks)  *out_exc_reentry_blocks = exception_reentry_blocks;
}

void interrupts_init(void) {
    dispatch_count = 0;
    in_exception = 0;
    exception_nest_depth = 0;
    g_psx_dispatch_depth = 0;
    total_checks = 0;
    post_exception_cooldown_until = 0;
    exception_entries_total = 0;
    exception_reentry_blocks = 0;
    cycles_since_vblank = 0;
    last_sio_seq_seen = sio_get_seq();
    last_sio_progress_cycle = psx_get_cycle_count();
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
#include "psx_fiber.h"
/* Defer a longjmp to the fiber that owns the current exception setjmp.
 * If we're on the owning fiber already, longjmp immediately. Otherwise
 * record the requested code, switch back to the owner, and the post-switch
 * check in traps.c psx_change_thread_fiber executes the longjmp on the
 * correct stack. */
static void deferred_exception_longjmp(int code) {
    /* If this unwind blows through a live shadow-diff frame (a frame armed
     * BEFORE the shadow started), restore the shadow-scoped globals its
     * skipped epilogue would have restored — otherwise the diff instrument
     * silently dies for the rest of the run (s_in_shadow stuck). No-op when
     * no shadow is live or the target frame was armed inside the shadow. */
    extern void overlay_loader_shadow_escape_fixup(uint64_t target_epoch);
    overlay_loader_shadow_escape_fixup(g_exc_setjmp_epoch);
    if (!g_exception_owner_fiber || psx_fiber_current() == g_exception_owner_fiber) {
        longjmp(exception_jmpbuf, code);
    }
    g_pending_exception_longjmp = code;
    psx_fiber_switch(g_exception_owner_fiber);
    /* If we end up back here, the owner didn't honor the flag (bug).
     * Fall through to direct longjmp as a last resort — even though
     * the stack is wrong, the alternative is hanging silently. */
    longjmp(exception_jmpbuf, code);
}

void psx_exception_longjmp(void) {
    debug_server_log_restore_event(2, debug_cpu_ptr ? debug_cpu_ptr->pc : 0, 1);
    deferred_exception_longjmp(1);
}

void psx_restore_state_escape(void) {
    if (in_exception) {
        debug_server_log_restore_event(1, debug_cpu_ptr ? debug_cpu_ptr->pc : 0, 2);
        deferred_exception_longjmp(2);
    }
    /* Not in exception context — return normally, let caller's `return;` handle it. */
}

/* Cycle-budgeted precise event slicing — single source of truth.
 *
 * Returns the minimum guest-CPU-cycle distance to the next DELIVERABLE hardware
 * interrupt: a source raises its I_STAT bit AND that bit is unmasked in i_mask
 * (CPU-side SR/IE gating is checked separately at take time). UINT32_MAX means
 * no maskable hardware event is currently scheduled.
 *
 * The two-tier block executor uses this at each block leader: if a whole block
 * fits inside this distance (and the block has no IRQ-visibility side effects),
 * it runs as fast compiled C; otherwise it runs through the per-instruction
 * interpreter so the interrupt is taken at its exact architectural cycle (see
 * PRECISE_IRQ_SLICE.md). Each peripheral owns its own next-IRQ query; this just
 * takes the min. Every query is a conservative UNDER-estimate (smaller distance
 * => slice more => never run past an event), so this is too. */
uint32_t cycles_to_next_event(void) {
    uint32_t best = 0xFFFFFFFFu;
    /* VBLANK is paced here (interrupts.c owns cycles_since_vblank). The deferred
     * card-SIO case only pushes VBlank LATER, so this estimate stays a safe
     * under-estimate. */
    if (i_mask & (1u << IRQ_VBLANK)) {
        uint32_t d = (cycles_since_vblank >= VBLANK_CYCLES)
                       ? 0u : (VBLANK_CYCLES - cycles_since_vblank);
        if (d < best) best = d;
    }
    uint32_t t = timers_cycles_to_irq(i_mask); if (t < best) best = t;
    uint32_t c = cdrom_cycles_to_irq(i_mask);  if (c < best) best = c;
    uint32_t d = dma_cycles_to_irq(i_mask);    if (d < best) best = d;
    uint32_t s = sio_cycles_to_irq(i_mask);    if (s < best) best = s;
    return best;
}

static uint64_t s_need_defer, s_need_irq, s_skip_none, s_skip_sr;
static uint64_t s_skip_cooldown, s_skip_nested;

void psx_interrupt_delivery_diag(uint64_t *need_defer, uint64_t *need_irq,
                                 uint64_t *skip_none, uint64_t *skip_sr,
                                 uint64_t *skip_cooldown, uint64_t *skip_nested) {
    if (need_defer)    *need_defer    = s_need_defer;
    if (need_irq)      *need_irq      = s_need_irq;
    if (skip_none)     *skip_none     = s_skip_none;
    if (skip_sr)       *skip_sr       = s_skip_sr;
    if (skip_cooldown) *skip_cooldown = s_skip_cooldown;
    if (skip_nested)   *skip_nested   = s_skip_nested;
}

int psx_interrupt_delivery_needed(const CPUState* cpu) {
    if (s_defer_switch_pending) { s_need_defer++; return 1; }
    if ((i_stat & i_mask) == 0) { s_skip_none++; return 0; }

    uint32_t sr = cpu->cop0[COP0_SR];
    if (!(sr & 0x01u) || !(sr & (1u << 10))) { s_skip_sr++; return 0; }

    if (post_exception_cooldown_until != 0 &&
        psx_get_cycle_count() < post_exception_cooldown_until) {
        s_skip_cooldown++;
        return 0;
    }

    if (in_exception &&
        (exception_nest_depth >= 2 ||
         g_rfe_escape_pending ||
         g_exc_escape_reason != PSX_EXC_ESCAPE_NONE)) {
        s_skip_nested++;
        return 0;
    }
    s_need_irq++;
    return 1;
}

void psx_check_interrupts(CPUState* cpu) {
    extern int g_ls_suppress_record;
#define PSX_CHECK_INTERRUPTS_RETURN() do { if (g_ls_suppress_record > 0) g_ls_suppress_record--; return; } while (0)
#ifdef PSX_COSIM
    extern uint32_t g_dirty_safe_resume_pc;
#define COSIM_IRQ_TAKE_PC() (g_dirty_safe_resume_pc ? g_dirty_safe_resume_pc : s_compiled_interrupt_resume_pc)
#define COSIM_IRQ_NOTE(kind_) cosim_irq_note(cpu, (kind_), COSIM_IRQ_TAKE_PC(), g_dirty_safe_resume_pc, s_compiled_interrupt_resume_pc, cpu->cop0[COP0_SR])
#endif

    /* Genuine entry fast paths. Generated resident code calls this at every
     * basic-block edge, which can mean tens of millions of calls inside an FMV
     * polling loop. psx_advance_cycles() has already serviced every device
     * deadline before this point, so no-pending and IEc-clear checks have no
     * architectural work to do. The old fast paths came after diagnostics and
     * savestate_poll, making a harmless callback expensive enough to reduce
     * Tomba 2's Whoopee FMV to ~1 guest fps.
     *
     * Poll host maintenance periodically so save/load and the debug socket stay
     * responsive even when the guest spends a long time in this path. At the
     * observed 1M+ block edges/s this is sub-frame latency. */
    static uint32_t s_fast_maintenance;
    if (g_idle_skip_enabled < 0) (void)psx_idle_skip_is_enabled();
    if (!in_exception && (i_stat & i_mask) == 0 &&
        !s_defer_switch_pending && g_idle_skip_enabled == 0) {
        if ((++s_fast_maintenance & 0x3FFFu) == 0) {
            extern void savestate_poll(CPUState* cpu, uint32_t resume_pc);
            savestate_poll(cpu, s_compiled_interrupt_resume_pc);
            debug_server_poll();
        }
        return;
    }
    if (in_exception && !(cpu->cop0[COP0_SR] & 0x01u)) return;

    {
        uint32_t check_pc = g_dirty_safe_resume_pc ? g_dirty_safe_resume_pc
                                                   : s_compiled_interrupt_resume_pc;
        s_last_interrupt_check_pc = check_pc;
        s_last_interrupt_check_cycle = psx_get_cycle_count();
    }
    g_ls_suppress_record++;
    total_checks++;
    if ((total_checks & 0x3FFFu) == 0) {
        debug_server_poll();
    }

    /* SIO delayed IRQ delivery removed from here.
     * sio_tick() is now called only from SIO register accesses
     * (sio_read/sio_write) and I_STAT reads (memory.c).  The BIOS
     * pad detection sequence clears I_STAT bit 7 then polls I_STAT
     * waiting for it to re-appear.  If we tick here, the IRQ fires
     * during the delay loop BEFORE the clear, and the BIOS never
     * sees it. */

    interrupts_service_scheduled_events();

    /* Idle-loop cycle skip (psx_cycles.c): detect a side-effect-free poll
     * loop by its repeated same-PC checks and fast-forward guest time to the
     * next internal device event in whole loop quanta. Runs BEFORE the
     * deliverability evaluation below so an event raised by the skip is
     * delivered in this same check, exactly at the boundary real execution
     * would have taken it. No-ops in exception context / precise mode /
     * lockstep (gated inside). */
    psx_idle_note_check(cpu, s_last_interrupt_check_pc);

    /* User save states: this is a block-leader boundary with a known resume PC;
     * only act outside the exception handler so a restore's stack-unwind can't
     * strand a half-finished handler. Near-free when nothing is staged. A load
     * longjmps to the scheduler and never returns here. */
    if (!in_exception) {
        extern void savestate_poll(CPUState* cpu, uint32_t resume_pc);
        savestate_poll(cpu, s_last_interrupt_check_pc);
    }

    /* Deferred cooperative thread switch: honor at the next real thread-save
     * boundary (see s_defer_switch_* above). A
     * genuine in-exception ChangeThread was deferred because it was detected at a
     * poisoned point (the IRQ fired deep inside a nested call, e.g. the
     * async-poll callback, so the resume-PC latch desynced from the live GPRs).
     *
     * The dirty-RAM interpreter also pumps interrupts at synthetic transfer and
     * call-return sites. Those sites are precise enough to deliver hardware IRQs,
     * but not necessarily safe to snapshot a cooperative thread: the interpreter
     * may have committed a candidate resume PC while the live CPUState stack/GPRs
     * still describe the previous local frame. Keep the deferral pending there and
     * honor it only once execution reaches a normal interrupt poll with site 0.
     * Near-free when nothing is pending. */
    if (s_defer_switch_pending && !in_exception) {
        if (psx_hle_scheduler_enabled()) {
            uint32_t from_tcb  = s_defer_switch_from;
            uint32_t to_tcb    = s_defer_switch_target;
            uint32_t resume_pc = s_last_interrupt_check_pc; /* materialized block PC */
            if (from_tcb == 0u || to_tcb == 0u ||
                psx_sched_current_tcb(cpu) != from_tcb) {
                /* STALE: the guest already re-scheduled cooperatively (PCB[0] no
                 * longer names the deferred thread) - abandon the deferral. This is
                 * the ONLY case we clear pending without honoring; dropping when a
                 * clean resume PC merely isn't available yet would LOSE the guest's
                 * ChangeThread and wedge (the bug this replaces). */
                s_defer_switch_pending = 0;
                s_defer_switch_target  = 0;
                s_defer_switch_from    = 0;
                debug_server_log_thread_event(33, cpu, from_tcb, to_tcb, resume_pc);
            } else if (g_cosim_dirty_pump_site != 0) {
                /* Not a real suspend boundary: dirty interpreter pump sites expose
                 * committed PCs for IRQ timing, but the matching CPUState may not
                 * be materialized yet. */
            } else if (resume_pc != 0u && (resume_pc & 0x3u) == 0u &&
                       psx_is_dispatchable(resume_pc)) {
                /* Materialized clean boundary: re-save the deferred thread cleanly
                 * (this OVERWRITES any poisoned/sentinel EPC the guest handler wrote
                 * for it while nested), re-point PCB[0], and honor the switch. */
                s_defer_switch_pending = 0;
                s_defer_switch_target  = 0;
                s_defer_switch_from    = 0;
                psx_sched_save_context(cpu, from_tcb, resume_pc);
                psx_sched_set_current_tcb(cpu, to_tcb);
                debug_server_log_thread_event(32, cpu, from_tcb, to_tcb, resume_pc);
                g_dirty_interp_active = 0;
                s_compiled_interrupt_resume_pc = 0;
                g_sched_escape.target_tcb = to_tcb;
                g_sched_escape.resume_pc  = 0;
                g_sched_escape.reason     = PSX_RUN_YIELD_TO_TCB;
                longjmp(g_scheduler_jmpbuf, 1); /* unwind to psx_scheduler_run */
            }
            /* else: KEEP pending - this outermost boundary has no materialized
             * resume PC (both latches 0, e.g. a scheduled-event check not tied to a
             * block leader). Wait for the next block-leader boundary that does. */
        }
    }

    /* Dispatch-loop maintenance only when NOT inside the exception handler. */
    if (!in_exception) {
#if SIO_MODEL_CYCLE_PACED
        /* Builds without block-cycle accounting need a small dispatch-loop
         * SIO quantum. With PSX_ENABLE_BLOCK_CYCLES, psx_advance_cycles()
         * already drives SIO timing, so the fixed quantum would double-count.
         * arms shift/ack), so this gate never opens — per-call cost on
         * this hot path is one volatile load + one branch. */
#ifndef PSX_ENABLE_BLOCK_CYCLES
        if (g_sio_timing_active) {
            sio_tick_quantum();
        }
#endif
#endif
        dispatch_count++;
    }

    /* Event ring: generic i_stat-edge backstop. Catches raises from sites we
     * don't instrument precisely (memory.c MMIO acks, SIO, SPU). Bounded by
     * actual transitions, not by check frequency. */
    {
        static uint32_t s_last_istat = 0;
        if (i_stat != s_last_istat) {
            s_last_istat = i_stat;
            event_ring_record(EV_ISTAT_CHANGE, 0);
        }
    }

    /* Check if any interrupts are pending. */
    if ((i_stat & i_mask) == 0) { irq_record_outcome(EV_NONE, 0, 0); PSX_CHECK_INTERRUPTS_RETURN(); }
    /* Nested delivery (hardware semantics). Real R3000A has no 'in exception'
     * gate — delivery is governed by SR alone. The handler normally runs with
     * IEc=0 (hardware bit-shift on entry), so the SR gates below block
     * re-entry faithfully on their own. A game that RE-ENABLES IEc inside the
     * handler is asking for a nested interrupt and real hardware delivers it:
     * Tomba2's lava attract demo parks in the kernel event-poll INSIDE the
     * VBlank handler waiting for the NEXT VBlank — the old unconditional
     * in_exception block starved it to ~1 fps (VBlank pending+unmasked,
     * ~1100 reentry blocks/s, 1 delivery/s). The synchronous-handler
     * machinery below is nesting-safe: the interrupted-context save
     * (saved_gpr/hi/lo) and owner state (prev_owner_fiber, prev_pending,
     * dispatch depth) are host-stack locals restored on unwind, and
     * in_exception is restored to its pre-delivery value at the epilogue.
     * Depth cap: a pathological IEc-re-enable loop would recurse the host
     * stack; real hardware would wedge the guest instead — we stop nesting
     * there (counted) so the rings expose it. */
    if (in_exception) {
        /* Refinements over plain SR gating (first attempt wedged BOOT at
         * f436): (1) never nest while an RFE/escape unwind is in flight —
         * the guest's RFE pops SR (IEc back to 1) BEFORE the host escape
         * completes, and a delivery in that window lands in half-unwound
         * machinery; (2) one nesting level only — enough for the
         * wait-inside-handler idiom, no host-stack pyramids. */
        if (exception_nest_depth >= 2 ||
            g_rfe_escape_pending ||
            g_exc_escape_reason != PSX_EXC_ESCAPE_NONE ||
            !(cpu->cop0[COP0_SR] & 0x01)) {
            /* Per-guard veto attribution (always-on counters, freeze_check):
             * which refinement is actually vetoing nested delivery. A wedge
             * where ONE guard dominates for thousands of frames names the
             * false veto (Tomba2 Whoopee-logo forensics 2026-07-03). */
            if (exception_nest_depth >= 2)                        g_nestgate_depth++;
            else if (g_rfe_escape_pending)                        g_nestgate_rfepend++;
            else if (g_exc_escape_reason != PSX_EXC_ESCAPE_NONE)  g_nestgate_escreason++;
            else                                                  g_nestgate_iec++;
            exception_reentry_blocks++;
            irq_record_outcome(EV_IRQ_GATE, GATE_IN_EXCEPTION, 0);
#ifdef PSX_COSIM
            COSIM_IRQ_NOTE(2u);
#endif
            PSX_CHECK_INTERRUPTS_RETURN();
        }
        /* IEc re-enabled inside the handler: fall through to the SR gates
         * and deliver a nested exception, as hardware would. */
    }

    /* Post-exception cooldown: let at least one block of guest time elapse after
     * RFE before the next delivery. Gated on the guest-cycle deadline (not a
     * per-call countdown) so compiled and interp agree on the delivery cycle. */
    if (post_exception_cooldown_until != 0) {
        if (psx_get_cycle_count() < post_exception_cooldown_until) {
            irq_record_outcome(EV_IRQ_GATE, GATE_COOLDOWN, 0);
#ifdef PSX_COSIM
            COSIM_IRQ_NOTE(3u);
#endif
            PSX_CHECK_INTERRUPTS_RETURN();
        }
        post_exception_cooldown_until = 0;  /* window elapsed */
    }

    /* Check COP0 SR: IEc (bit 0) must be set, and IM2 (bit 10) must be set. */
    uint32_t sr = cpu->cop0[COP0_SR];
    if (!(sr & 0x01)) {
        irq_record_outcome(EV_IRQ_GATE, GATE_SR_IE, 0);
#ifdef PSX_COSIM
        COSIM_IRQ_NOTE(4u);
#endif
        PSX_CHECK_INTERRUPTS_RETURN();
    }   /* Interrupts globally disabled */
    if (!(sr & (1 << 10))) {
        irq_record_outcome(EV_IRQ_GATE, GATE_SR_IM2, 0);
#ifdef PSX_COSIM
        COSIM_IRQ_NOTE(5u);
#endif
        PSX_CHECK_INTERRUPTS_RETURN();
    } /* Hardware interrupt bit not enabled */

    /* Architectural take-PC = the resume PC (same selection the async-RFE block
     * below uses): the dirty-interp commits the exact interrupted instruction in
     * g_dirty_safe_resume_pc; compiled code passes its block-entry PC via
     * psx_check_interrupts_at -> s_compiled_interrupt_resume_pc. Combined with the
     * event's `mode`, this is the take-point for the cross-backend diff. */
    {
        extern uint32_t g_dirty_safe_resume_pc;
        uint32_t take_pc = g_dirty_safe_resume_pc ? g_dirty_safe_resume_pc
                                                  : s_compiled_interrupt_resume_pc;
        irq_record_outcome(EV_IRQ_DELIVER, 0, take_pc);
    }
    g_irq_deliver_count++;
    if ((i_stat & i_mask) & (1u << IRQ_VBLANK)) g_vblank_deliver_count++;
    if ((i_stat & i_mask) & (1u << IRQ_CDROM))  g_cdrom_deliver_count++;
    /* IRQ-delivery context ring (MMX6 VSync-vs-CD-DMA hunt). Capture, at every IRQ
     * delivery, whether the kernel VSync callback-block word at 0x80079D44 is the
     * clobbered game value AND whether a CD DMA (ch3) is mid-transfer / a DMA is
     * executing. If VBlank gets delivered with d44 already==0x016F0110 while the CD
     * DMA is active, that is ChatGPT's class (c)+(#2): IRQ delivered in the DMA
     * clobber window because the CPU isn't stalled / the event wasn't torn down. */
    {
        extern int dma_cdrom_transfer_active(void);
        extern int g_dma_exec_depth;
        extern uint64_t s_frame_count;
        g_irqctx_ring[g_irqctx_seq & (IRQCTX_RING_CAP - 1u)] = (IrqCtxEntry){
            .seq = g_irqctx_seq, .cycle = psx_get_cycle_count(),
            .frame = (uint32_t)s_frame_count, .istat = i_stat, .imask = i_mask,
            .sr = cpu->cop0[COP0_SR], .d44 = cpu->read_word(0x80079D44u),
            .cdrom_active = (uint32_t)dma_cdrom_transfer_active(),
            .dma_depth = g_dma_exec_depth,
            .is_vblank = (uint32_t)(((i_stat & i_mask) & (1u << IRQ_VBLANK)) != 0),
        };
        g_irqctx_seq++;
    }
    ls_note_exception_entry();
    /* Nested delivery: remember whether we interrupted an OUTER handler so
     * the epilogue restores in_exception to the pre-delivery value instead
     * of clearing it (a nested level-2 exit must leave level-1 flagged). */
    int prev_in_exception = in_exception;
    in_exception = 1;
    exception_nest_depth++;
    exception_entries_total++;
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
     * kernel data pointer area (0x100+), and not used by the BIOS.
     *
     * The sentinel is the HOST escape token for the SYNCHRONOUS handler: when the
     * recompiled handler RFEs to it while in_exception, psx_unknown_dispatch /
     * dirty_ram_dispatch longjmp back here and resume the interrupted code via the
     * host-GPR-restore below. This is unchanged (Tomba 1 / MMX6 / Ape rely on it). */
    /* Fix B: COP0.EPC = the REAL interrupted resume PC, NOT a sentinel. The recompiled
     * BIOS exception handler saves COP0.EPC into the interrupted thread's TCB EPC slot,
     * so a thread suspended here (ChangeThread) resumes at its OWN real PC — never via a
     * single global that the next thread's IRQs overwrite (the MMX6 cooperative-thread
     * freeze). The host escape out of the nested synchronous handler is keyed on the
     * RFE-pending flag (psx_rfe_mark_escape / psx_rfe_escape_check), not on pc==sentinel.
     *
     * real_pc = the committed interrupted PC, latched by the delivery site (the dirty
     * pump exposes g_dirty_safe_resume_pc; a compiled block-leader check exposes
     * s_compiled_interrupt_resume_pc). When BOTH are 0 the IRQ was taken at a clean
     * trampoline boundary (the interrupted function already returned, pc was 0) — there
     * is no mid-function resume PC, so fall back to the legacy sentinel + saved_gpr path
     * for THIS delivery only (reason = LEGACY_SENTINEL gates the saved_gpr restore and
     * disarms the RFE-flag escape). */
    g_rfe_escape_pending = 0;
    {
        uint32_t real_pc = g_dirty_safe_resume_pc ? g_dirty_safe_resume_pc
                                                  : s_compiled_interrupt_resume_pc;
        if (real_pc != 0u && (real_pc & 0x3u) == 0u &&
            (real_pc & 0x1FFFFFFFu) < 0x00200000u) {
            cpu->cop0[COP0_EPC]  = real_pc;     /* architectural: the real resume PC */
            g_exception_real_epc = real_pc;
            g_exc_escape_reason  = PSX_EXC_ESCAPE_NONE; /* set at the actual RFE/SYSCALL return */
        } else {
            uint32_t sentinel = PSX_EXC_SENTINEL_PC;
            cpu->write_word(sentinel, 0x00000000u); /* NOP, read by the handler's BD check */
            cpu->cop0[COP0_EPC]  = sentinel;
            g_exception_real_epc = sentinel;
            g_exc_escape_reason  = PSX_EXC_ESCAPE_LEGACY_SENTINEL;
        }
#ifdef PSX_COSIM
        cosim_irq_note(cpu, 1u, real_pc, g_dirty_safe_resume_pc,
                       s_compiled_interrupt_resume_pc, sr);
#endif
        /* Ring exit-half init: record the entry-side resume selection now; the
         * exit fields are filled at the restore decision below. No nesting is
         * possible between here and there (in_exception blocks re-entry), so
         * (g_irqctx_seq - 1) is this delivery's entry. */
        {
            IrqCtxEntry *e = &g_irqctx_ring[(g_irqctx_seq - 1u) & (IRQCTX_RING_CAP - 1u)];
            e->take_pc  = real_pc;
            e->real_epc = g_exception_real_epc;
            e->exit_pc = 0; e->exit_reason = 0; e->same_thread = 0;
            e->restored = 0; e->v1_exit = 0; e->v1_saved = 0;
            e->ra_exit = 0; e->ra_saved = 0; e->redirects = 0;
            e->entry_sp   = cpu->gpr[29];
            { extern int g_cosim_dirty_pump_site;
              e->pump_site = (uint32_t)g_cosim_dirty_pump_site; }
        }
    }

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

    /* Same-thread discriminator input (mmx6_card_load_regression_state): the
     * kernel's CURRENT-TCB pointer at exception ENTRY. PCB ptr lives at kernel
     * 0x108; PCB[0] = running thread's TCB. ChangeThread inside the handler
     * moves PCB[0] to the new thread, so entry-vs-exit TCB equality is the
     * STRUCTURAL "no thread switch happened" test — unlike PC equality, it is
     * immune to two threads parked at the SAME guest PC (shared spin loops:
     * MMX6's card poll), where the PC heuristic force-restores the OLD thread's
     * GPRs into the NEW thread. */
    extern uint32_t psx_read_word(uint32_t addr);   /* memory.c (plain RAM read) */
    uint32_t entry_pcb = psx_read_word(0x108u);
    uint32_t entry_tcb = entry_pcb ? psx_read_word(entry_pcb & 0x1FFFFFFFu) : 0u;

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
        /* The RAM exception vector is a LUI/ADDIU pair that materializes the
         * installed handler address in $k0 before transferring to it.  The
         * host-side fast path decodes that pair and dispatches straight to the
         * target, so it must also commit the pair's architectural register
         * result.  Vigilante 8's handler uses $k0 as its table base on its very
         * first instruction; leaving the interrupted value in $k0 made it load
         * a BIOS instruction word (0xAD400000) as a jump target. */
        uint32_t vector_reg = (w1 >> 16) & 31u;
        if (vector_reg != 0u) cpu->gpr[vector_reg] = target_pc;
    }

    /* Record which fiber owns this setjmp. Any subsequent longjmp must
     * happen on this same fiber; if a non-owner fiber needs to longjmp
     * it must switch back here first (see deferred_exception_longjmp). */
    void *prev_owner_fiber = g_exception_owner_fiber;
    int   prev_pending = g_pending_exception_longjmp;
    g_exception_owner_fiber = psx_fiber_current();
    g_pending_exception_longjmp = 0;
    /* A bail unwind can never be in flight at exception entry: bail-mode
     * returns skip every block leader, so psx_check_interrupts is never
     * reached while g_psx_call_bail is set.  If it ever is, count the
     * anomaly and clear so the handler dispatch isn't poisoned. */
    if (g_psx_call_bail) {
        g_psx_bail_anomaly++;
        g_psx_call_bail = 0;
    }
    /* The static BIOS exception handler is not dirty-RAM-interp code. Clear the
     * interp mode flag across the handler dispatch so events recorded inside it
     * are tagged STATIC. The restore sits after the loop the longjmp lands in,
     * so the EPC-sentinel longjmp can't leave the flag wrong. */
    int prev_interp_active = g_dirty_interp_active;
    g_dirty_interp_active = 0;
    /* Ape memcard fix #3 (2026-07-07): clear the dirty-interp resume-PC latch
     * across the handler dispatch, exactly like g_dirty_interp_active above.
     *
     * The interrupted dirty block set g_dirty_safe_resume_pc = its committed PC
     * and left it LIVE across its own psx_check_interrupts call (the dirty pump
     * only restores it AFTER that call returns). Without clearing it here, a
     * NESTED interrupt taken inside the handler — e.g. a VBLANK delivered while
     * the kernel's async-poll memory-card callback runs (in_exception, IEc
     * re-enabled) — inherits the OUTER interrupted block's stale resume PC as its
     * EPC. same-thread-restore then matches (cpu->pc == that stale EPC) and
     * resumes the OUTER block with the CALLBACK's registers: the Ape memcard
     * consumer at 0x801382CC gets v1 (jumptable index) = the callback's pointer
     * 0x800B4E30, bounds-check already passed, `jr jumptable[garbage]` faults
     * (DISPATCH FATAL misaligned 0x3) — or, in the soft variant, drives a wrong
     * branch that skips DeliverEvent(0xF0000011,4) and the card scene wedges.
     * (irqctx_ring proof: exit_pc=0x801382CC, ra_saved=0x80010094 [callback],
     * v1_saved=0x800B4E30, restored=1.)
     *
     * Cleared here so a nested delivery latches the ACTUALLY-running code's PC:
     * the callback's own dirty pump re-sets g_dirty_safe_resume_pc for its
     * blocks, and compiled handler code uses s_compiled_interrupt_resume_pc.
     * Restored after the dispatch loop (below), which the RFE/escape longjmp
     * lands in — so the escape can't leave the latch wrong. A/B off:
     * PSX_EXC_CLEAR_RESUME_LATCH=0. */
    static int s_clear_resume_latch = -1;
    if (s_clear_resume_latch < 0) {
        const char *e = getenv("PSX_EXC_CLEAR_RESUME_LATCH");
        s_clear_resume_latch = (e && *e) ? atoi(e) : 1;
    }
    uint32_t saved_dirty_resume_pc = g_dirty_safe_resume_pc;
    if (s_clear_resume_latch) g_dirty_safe_resume_pc = 0;
    /* Wall-time sampler phase: the handler dispatch runs recompiled static
     * BIOS text. Same save/restore contract as g_dirty_interp_active above —
     * the restore sits after the setjmp loop, covering longjmp landings. */
    extern int g_exec_phase;
    int prev_exec_phase = g_exec_phase;
    g_exec_phase = 3;
    /* Dispatch-depth contract across the async handler (Tomba 2 splash, post
     * overlay-floor fix). The interrupt can be delivered from a NESTED dispatch
     * (the local-flow pump runs inside dirty_ram_dispatch at depth > 0). The
     * handler must run as its OWN outermost context (so its tail-call trampoline
     * flattens), and a ReturnFromException longjmp skips the handler frames'
     * `--g_psx_dispatch_depth` decrements — so we cannot leave the counter at 0.
     * Save the interrupted code's nesting here and RESTORE it after the handler;
     * otherwise the outer frames unwind below zero (dispatch_depth -> -1), the
     * `outermost` test misfires, and the top-level dispatch returns to PC=0
     * ("execution completed" abnormal exit). */
    int saved_dispatch_depth = g_psx_dispatch_depth;
    g_psx_dispatch_depth = 0;
    g_exc_setjmp_epoch++;   /* new setjmp frame armed (see decl above) */
    for (;;) {
        int jmp_val = setjmp(exception_jmpbuf);
        if (jmp_val == 2) {
            /* RestoreState redirect: re-dispatch to cpu->pc.
             * GPRs were already set by RestoreState — do NOT restore.
             * Stay in exception context so ReturnFromException works. */
            g_irqctx_ring[(g_irqctx_seq - 1u) & (IRQCTX_RING_CAP - 1u)].redirects++;
            g_psx_dispatch_depth = 0;
            debug_server_log_restore_event(3, cpu->pc, (uint32_t)jmp_val);
            target_pc = cpu->pc;
            continue;
        }
        if (jmp_val == 1) {
            g_psx_dispatch_depth = 0;
            debug_server_log_restore_event(4, cpu->pc, (uint32_t)jmp_val);
        }
        if (jmp_val == 0) {
            /* Normal entry (or after RestoreState redirect): dispatch. */
            psx_dispatch(cpu, target_pc);
        }
        /* jmp_val 0 (normal return) or 1 (ReturnFromException): done. */
        break;
    }
    /* Restore previous exception-owner state. Supports nested exceptions
     * if they ever arise (uncommon but harmless). */
    g_exception_owner_fiber = prev_owner_fiber;
    g_pending_exception_longjmp = prev_pending;
    g_dirty_interp_active = prev_interp_active;
    /* Ape memcard fix #3: restore the resume-PC latch cleared at handler entry
     * (see above). Sits with the other post-dispatch-loop restores so an
     * RFE/RestoreState longjmp — which lands in the setjmp loop above — can't
     * strand it at 0. */
    if (s_clear_resume_latch) g_dirty_safe_resume_pc = saved_dirty_resume_pc;
    g_exec_phase = prev_exec_phase;
    /* Restore the interrupted code's dispatch nesting (see save above). The
     * handler's frames were unwound by longjmp without decrementing, so the
     * live counter is meaningless here — overwrite, don't decrement. */
    g_psx_dispatch_depth = saved_dispatch_depth;

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
     * exception push — we do RFE to pop the SR stack.
     *
     * Fix B: only restore saved_gpr on the LEGACY (jmpbuf / boundary-sentinel)
     * exit. On a real RFE/ReturnFromException the recompiled BIOS handler already
     * restored the RESUMED thread's GPRs from its TCB; overwriting them with our
     * saved_gpr would clobber the resumed thread with the state of the thread that
     * ENTERED the exception (the cross-thread corruption fix B exists to prevent). */
    /* Fix B refinement (2026-07-01, Tomba pause-menu wedge): restore the
     * interrupted GPRs whenever this resume returns to the SAME thread it
     * interrupted — either the LEGACY sentinel exit, OR a real-EPC RFE that
     * resumes at exactly the PC we installed as EPC (no ChangeThread happened).
     *
     * Real hardware saves and restores ALL GPRs across every exception, so a
     * transparent same-thread interrupt must give the interrupted code its exact
     * pre-exception registers back. The original Fix B skipped this for all
     * real-EPC resumes, trusting the recompiled BIOS handler's TCB restore — but
     * for a same-thread compiled resume that restore is incomplete, so a live
     * value held in a register across a block-leader IRQ check gets clobbered by
     * the handler. Concretely: Tomba's pause-menu frame-wait spin holds its loop
     * bound (256) in gpr[3] across 0x80016588; a VBLANK there returned gpr[3]=1,
     * so the wait exited early every frame and the menu never opened.
     *
     * A GENUINE ChangeThread resumes a DIFFERENT thread at its own PC (!= our
     * EPC); there the BIOS handler restored the NEW thread's GPRs from its TCB
     * and saved_gpr holds the thread that ENTERED the exception — restoring it
     * would reintroduce the MMX6 cooperative-thread corruption Fix B prevents. */
    /* Discriminator selector (verification + candidate fix,
     * mmx6_card_load_regression_state — the PC heuristic below was BISECTED as
     * the "A game data could not be found" regression B on MMX6 while being
     * the fix for Tomba's pause menu):
     *   PSX_SAME_THREAD_RESTORE=0  original Fix B (legacy-sentinel-only restore)
     *   PSX_SAME_THREAD_RESTORE=1  PC-equality heuristic (13c5e0c behavior)
     *   PSX_SAME_THREAD_RESTORE=2  kernel current-TCB equality (default;
     *                              structural same-thread test — a genuine
     *                              ChangeThread moves PCB[0] even when the new
     *                              thread resumes at the SAME guest PC) */
    static int s_str_mode = -1;
    if (s_str_mode < 0) {
        const char *e = getenv("PSX_SAME_THREAD_RESTORE");
        s_str_mode = (e && *e) ? atoi(e) : 1;   /* default = 13c5e0c behavior;
            mode 2 (TCB check) is the hardening candidate, pending a Tomba
            pause-menu gate. The MMX6 "not found" this selector was built to
            verify turned out to be a STALE GENERATED IMAGE artifact. */
        if (s_str_mode < 0 || s_str_mode > 2) s_str_mode = 1;
    }
    extern uint32_t psx_read_word(uint32_t addr);   /* memory.c (plain RAM read) */
    uint32_t exit_pcb = psx_read_word(0x108u);
    uint32_t exit_tcb = exit_pcb ? psx_read_word(exit_pcb & 0x1FFFFFFFu) : 0u;
    int same_thread_resume = 0;
    if (s_str_mode == 1) {
        same_thread_resume =
            (g_exc_escape_reason != PSX_EXC_ESCAPE_LEGACY_SENTINEL) &&
            g_exception_real_epc != 0u &&
            same_guest_pc(cpu->pc, g_exception_real_epc);
    } else if (s_str_mode == 2) {
        same_thread_resume =
            (g_exc_escape_reason != PSX_EXC_ESCAPE_LEGACY_SENTINEL) &&
            g_exception_real_epc != 0u &&
            same_guest_pc(cpu->pc, g_exception_real_epc) &&
            entry_tcb != 0u && entry_tcb == exit_tcb;
    }
    int do_restore =
        (g_exc_escape_reason == PSX_EXC_ESCAPE_LEGACY_SENTINEL || same_thread_resume);
    /* Ring exit-half: every delivery records which escape path it took and
     * whether the interrupted GPRs were restored (Tomba menu wedge evidence). */
    {
        IrqCtxEntry *e = &g_irqctx_ring[(g_irqctx_seq - 1u) & (IRQCTX_RING_CAP - 1u)];
        e->exit_pc     = cpu->pc;
        e->exit_reason = (uint32_t)g_exc_escape_reason;
        e->same_thread = (uint32_t)same_thread_resume;
        e->restored    = (uint32_t)do_restore;
        e->v1_exit     = cpu->gpr[3];
        e->v1_saved    = saved_gpr[3];
        e->ra_exit     = cpu->gpr[31];
        e->ra_saved    = saved_gpr[31];
    }
    if (do_restore) {
        for (int i = 0; i < 32; i++) cpu->gpr[i] = saved_gpr[i];
        cpu->hi = saved_hi;
        cpu->lo = saved_lo;
    }
    g_rfe_escape_pending = 0;
    g_exc_escape_reason  = PSX_EXC_ESCAPE_NONE;

    if (!(cpu->cop0[COP0_SR] & 0x01)) {
        uint32_t sr2 = cpu->cop0[COP0_SR];
        cpu->cop0[COP0_SR] = (sr2 & 0xFFFFFFC0u) | ((sr2 >> 2) & 0x0Fu);
    }

    /* Nested delivery: a level-2 exit must leave level-1 flagged. */
    in_exception = prev_in_exception;
    if (exception_nest_depth > 0) exception_nest_depth--;

    /* Adaptive cooldown: if the handler acknowledged the interrupt (cleared
     * some I_STAT bits), the interrupt won't immediately re-fire and we need
     * no cooldown.  If I_STAT is unchanged (no handler claimed the interrupt),
     * give the main code a generous window to make progress — e.g. to let
     * the shell finish installing handlers.  On real hardware, the CPU
     * executes at least one instruction between exceptions; in our model
     * each "block" is many instructions, but the handler also consumes
     * hundreds of sub-dispatches per invocation. */
    if ((i_stat & i_mask) != 0 && i_stat == pre_handler_istat) {
        /* unclaimed: give main code guest-time to install handlers */
        post_exception_cooldown_until = psx_get_cycle_count() + POST_EXC_UNCLAIMED_COOLDOWN_CYCLES;
    } else {
        /* Claimed: the handler acknowledged at least one I_STAT bit. Per-source
         * policy (this used to be a blanket cooldown=0 — commit 6d2cb65 — which
         * pins main code under a fast-disc DMA flood: the interrupted block's
         * leader re-takes the exception before its body runs):
         *   - SIO (card reads): 128 consecutive SIO IRQs must fire within one
         *     blocking wait; any gap stalls the card protocol → re-fire now.
         *   - DMA/VBLANK/timer/etc: guarantee a few blocks of main-code
         *     progress between deliveries so a flood can't starve the loop. */
        uint32_t claimed = pre_handler_istat & ~i_stat;            /* bits handler cleared */
        uint32_t sio_active = (claimed | (i_stat & i_mask)) & (1u << IRQ_SIO0);
        post_exception_cooldown_until = sio_active
            ? 0  /* SIO: no cooldown — card reads need immediate back-to-back IRQs */
            : psx_get_cycle_count() + POST_EXC_CLAIMED_COOLDOWN_CYCLES;
    }
    if (g_ls_suppress_record > 0) g_ls_suppress_record--;

    /* In-exception thread switch (Ape Escape NEW GAME memcard scene).
     *
     * A game can run its OWN cooperative scheduler inside the kernel exception
     * handler: its VBlank/exception callback moves dword_108->entry (PCB[0], the
     * current-thread pointer) to the next thread, and ReturnFromException (0xF40)
     * then restores THAT thread. On real hardware the RFE atomically switches
     * execution to the new thread. (docs/psx_bios_disasm.txt: both ExceptionHandler
     * 0xC80 and ReturnFromException 0xF40 key off dword_108->entry.)
     *
     * Under the HLE TCB scheduler the interrupted thread is running via a
     * structured dispatch on the host stack. Returning normally here does NOT
     * transfer to the new thread — the generated block leader just falls through
     * to the interrupted thread's next block (it never re-reads cpu->pc). The
     * interrupted thread therefore keeps physically executing while PCB[0] already
     * names a different thread; a subsequent exception then saves the running
     * (old) thread's EPC into the NEW thread's TCB, corrupting it. That is the
     * Ape "Checking… MEMORY CARD" wedge: the transition thread's ec8 PC is written
     * into main's TCB EPC, main is later RFE'd there with main's regs, and its
     * store lands on the transition thread's TCB status word (kill → target_missing
     * forever). See ape_thread_smear_rootcause.md.
     *
     * Honor the switch exactly as a cooperative ChangeThread is honored: unwind to
     * psx_scheduler_run and dispatch whatever PCB[0] now points at, from its
     * committed TCB context (the guest handler already saved the interrupted
     * thread into ITS own TCB before the switch). Gated to the OUTERMOST exception
     * (prev_in_exception==0 → no live outer exception_jmpbuf frame to skip) and to
     * the HLE scheduler (the legacy fiber bridge has no g_scheduler_jmpbuf and
     * switches threads by fiber). */
    if (prev_in_exception == 0 && psx_hle_scheduler_enabled() &&
        entry_tcb != 0u && exit_tcb != 0u && entry_tcb != exit_tcb) {
        extern uint32_t psx_read_word(uint32_t addr);   /* memory.c (plain RAM read) */
        uint32_t new_state = psx_read_word(exit_tcb & 0x1FFFFFFFu);
        if (new_state == 0x4000u) {   /* the new current thread must be runnable */
            /* Fix #2: honor the switch ONLY at the outermost dispatch boundary,
             * where the outgoing thread's guest state is fully materialized in
             * CPUState. Nested inside a host call unit / dirty pump, the outgoing
             * thread's PC lives only in the resume-PC latch, which desyncs from the
             * live GPRs when the IRQ was taken deep inside a nested call (the
             * async-poll callback) — switching there commits a poisoned TCB context
             * and the resumed thread faults on a smeared jumptable index. */
            extern int g_call_unit_depth;
            int at_outermost = (g_psx_dispatch_depth == 0 && g_call_unit_depth == 0);
            /* Defer only when nested AND the outgoing thread has a valid resume PC
             * of its OWN (g_exception_real_epc — the architectural EPC latched for
             * THIS delivery). The guest handler's in-exception ReturnFromException
             * (traps.c syscall 3, in_exception path) has already restored the TARGET
             * thread's GPRs and set cpu->pc to the target's EPC; to resume the
             * OUTGOING thread transparently we must put cpu->pc back to ITS resume PC
             * as well, not leave it pointing at the target. If we have no clean
             * outgoing resume PC (sentinel / clean-trampoline boundary), or the
             * toggle is off, or we are already outermost, switch immediately. */
            /* Low BIOS/kernel code is already scheduler code; deferring it can
             * starve a target thread by re-entering the same VBlank EPC forever. */
            uint32_t epc_phys = g_exception_real_epc & 0x1FFFFFFFu;
            int low_kernel_epc = (epc_phys < 0x00010000u);
            int can_defer = defer_switch_enabled() && !low_kernel_epc && !at_outermost &&
                            g_exception_real_epc != 0u &&
                            (g_exception_real_epc & 0x3u) == 0u &&
                            g_exception_real_epc != (uint32_t)PSX_EXC_SENTINEL_PC &&
                            psx_is_dispatchable(g_exception_real_epc);
            if (!can_defer) {
                /* Outermost / no clean deferral: honor immediately — the guest
                 * handler's save of the outgoing thread is consistent here (or this
                 * is the pre-existing baseline path). */
                debug_server_log_thread_event(30, cpu, entry_tcb, exit_tcb, cpu->pc);
                /* The longjmp skips psx_check_interrupts_at's restore of the compiled
                 * resume-PC latch; clear it so the next thread's block-leader delivery
                 * recomputes real_pc cleanly rather than inheriting a stale value. */
                s_compiled_interrupt_resume_pc = 0;
                /* This escape uses g_scheduler_jmpbuf (unwinding to psx_scheduler_run),
                 * NOT exception_jmpbuf — so it BYPASSES the landing above that restores
                 * g_dirty_interp_active. From the dirty-RAM entry poll the flag is 1
                 * and would LEAK across the unwind; we abandon the dirty-interp
                 * dispatch here, so it MUST be 0 (dirty_ram_interp.c ~205-210 contract;
                 * no-op from compiled code where the flag was already 0). */
                g_dirty_interp_active = 0;
                g_sched_escape.target_tcb = exit_tcb;
                g_sched_escape.resume_pc  = 0;
                g_sched_escape.reason     = PSX_RUN_YIELD_TO_TCB;
                longjmp(g_scheduler_jmpbuf, 1); /* unwind to psx_scheduler_run; never returns */
            }
            /* Nested in host dispatch: DEFER. The guest handler has already saved a
             * POISONED context for the outgoing thread (latched PC ahead of the live
             * registers), but we do NOT switch off it. Instead resume the outgoing
             * thread transparently — restoring its clean pre-exception GPRs/HI/LO/SR
             * (do_restore above ran false for this genuine cross-thread case) — so it
             * finishes its nested calls; record the switch as pending and honor it at
             * the next outermost boundary (top of this function), re-saving the thread
             * cleanly there. Keep PCB[0] naming the STILL-RUNNING thread during the
             * interim so any interim exception saves into the right TCB (closes the
             * mis-save window the immediate switch used to avoid); the guest's intent
             * to run exit_tcb is preserved as the pending switch. Coalesces: the newest
             * guest ChangeThread wins the target. */
            for (int i = 0; i < 32; i++) cpu->gpr[i] = saved_gpr[i];
            cpu->hi = saved_hi;
            cpu->lo = saved_lo;
            cpu->cop0[COP0_SR] = sr;   /* pre-exception SR (IEc was set — gate above) */
            cpu->pc = g_exception_real_epc; /* resume the OUTGOING thread at ITS own PC,
                                             * not the target PC the in-exception RFE
                                             * left in cpu->pc — else it runs the target's
                                             * code under the outgoing thread and never
                                             * reaches its own block leaders to be honored. */
            psx_sched_set_current_tcb(cpu, entry_tcb);
            s_defer_switch_from    = entry_tcb;
            s_defer_switch_target  = exit_tcb;
            s_defer_switch_pending = 1;
            s_compiled_interrupt_resume_pc = 0;
            debug_server_log_thread_event(31, cpu, entry_tcb, exit_tcb, cpu->pc);
            /* fall through: return normally — the outgoing thread resumes. */
        }
    }
#ifdef PSX_COSIM
#undef COSIM_IRQ_NOTE
#undef COSIM_IRQ_TAKE_PC
#endif
#undef PSX_CHECK_INTERRUPTS_RETURN
}

/* Compatibility shim: the ape-flavored generated code calls
 * psx_check_interrupts_at(cpu, resume_pc); the mmx6-fw baseline runtime delivers
 * interrupts via psx_check_interrupts (cpu->pc / scratch sentinel). Forwarding
 * here gives the mmx6 baseline interrupt behavior — sufficient to build+run the
 * current generated code on the good baseline for instrumented comparison. */
void psx_check_interrupts_at(CPUState* cpu, uint32_t resume_pc) {
    uint32_t prev = s_compiled_interrupt_resume_pc;
    s_compiled_interrupt_resume_pc = resume_pc;
    psx_check_interrupts(cpu);
    s_compiled_interrupt_resume_pc = prev;
}

int psx_interrupts_checked_at_current_cycle(uint32_t resume_pc) {
    return s_last_interrupt_check_cycle == psx_get_cycle_count() &&
           same_guest_pc(s_last_interrupt_check_pc, resume_pc);
}

void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t resume_pc) {
    if (psx_interrupts_checked_at_current_cycle(resume_pc)) {
        return;
    }
    psx_check_interrupts_at(cpu, resume_pc);
}
