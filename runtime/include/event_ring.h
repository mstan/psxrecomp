/*
 * event_ring.h — Always-on unified event-timeline ring.
 *
 * Purpose: capture the interleaved timeline of interrupt-delivery decisions,
 * I_STAT transitions, and DMA kick/complete events, each tagged with the
 * current guest cycle, PC, enclosing recompiled function, current execution
 * mode (native-overlay / dirty-RAM-interp / static), and the in-progress
 * native overlay function. The goal is to diff two runs of the SAME route
 * (native-overlay OFF vs ON) and find the FIRST interrupt/DMA ordering
 * divergence relative to game code (PRINCIPLES.md #3, handoff Priority 2).
 *
 * Design: ALWAYS-ON (Release too), eviction-bounded ring. Records EDGES /
 * EVENTS, never per-poll samples — native code calls psx_check_interrupts at
 * every block, so polling would evict the window instantly. The interrupt
 * decision path emits an event only when the decision OUTCOME changes
 * (idle -> gated(reason) -> delivered), and raises/DMA are emitted at their
 * source sites. Dump-on-demand; the ring survives a blue screen because the
 * debug-server thread stays alive and queries it.
 *
 * This is OBSERVABILITY, not a fix. It does not alter timing.
 */
#ifndef PSXRECOMP_EVENT_RING_H
#define PSXRECOMP_EVENT_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ALWAYS-ON in every build, production included. The ring records EDGES
 * (IRQ decisions, I_STAT raises, DMA/CD/timer schedule+fire), not per-poll
 * samples, so the hot-path cost is a handful of stores per device event.
 * The TCP debug server is compiled into every build too (production runs
 * opt the listener in via PSX_DEBUG_SERVER=1), so the ring is readable on
 * the exact production binary that exhibits a bug — the Tomba 2 lost
 * CD-completion race was only attributable because of this. */
#ifndef EVENT_RING_ENABLED
#define EVENT_RING_ENABLED 1
#endif

/* 64K entries * 48 bytes ~= 3 MB. Covers many frames of transition activity. */
#define EVENT_RING_CAP (1u << 16)

typedef enum {
    EV_NONE        = 0,
    EV_IRQ_DELIVER = 1,  /* interrupt actually delivered (in_exception <- 1)   */
    EV_IRQ_GATE    = 2,  /* pending & masked-in but blocked; detail = gate code */
    EV_ISTAT_RAISE = 3,  /* known raise site fired; detail = IRQ bit            */
    EV_ISTAT_CHANGE= 4,  /* generic backstop: i_stat changed since last check   */
    EV_DMA_KICK    = 5,  /* DMA transfer start;    detail = channel, aux = chcr */
    EV_DMA_DONE    = 6,  /* DMA transfer complete; detail = channel, aux = chcr */
    EV_MODE        = 7,  /* execution-mode transition; mode = new mode          */
    EV_DMA_SCHED   = 8,  /* delayed (async) completion scheduled; detail=chan, aux=chcr */
    EV_ENQ         = 9,  /* event SCHEDULED/queued; detail = EventSource, aux = due_cycle (or payload) */
    EV_DEQ         = 10, /* event FIRED/consumed;   detail = EventSource, aux = payload (type/scheduled-due) */
} EventKind;

/* Source id for EV_ENQ / EV_DEQ (the `detail` field). Lets us independently
 * track when each event is scheduled vs when it fires — a MISSING enqueue (an
 * event scheduled in the working run but never in the broken run) is the prime
 * suspect for "content never loads", and is invisible if you only watch fires. */
typedef enum {
    SRC_NONE     = 0,
    SRC_DMA0     = 1,  /* SRC_DMA0+ch for DMA channels 0..6 (1..7) */
    SRC_VBLANK   = 16, /* enqueue=next VBlank scheduled, aux=due_cycle */
    SRC_CD_CMD   = 17, /* enqueue=CD command issued, aux=cmd byte */
    SRC_CD_READ  = 18, /* enqueue=sector read scheduled, aux=read_delay cycles */
    SRC_CD_IRQ   = 19, /* dequeue=CD response/data IRQ fired, aux=cd irq type */
    SRC_CD_PEND  = 20, /* enqueue=delayed CD command scheduled, aux=delay cycles */
    SRC_TIMER0   = 24, /* SRC_TIMER0+n for timers 0..2 (enqueue=armed, dequeue=fired) */
    SRC_SIO      = 28, /* enqueue=shift/ack countdown armed, dequeue=SIO IRQ fired */
} EventSource;

/* detail codes for EV_IRQ_GATE (why a pending+masked IRQ was NOT delivered) */
typedef enum {
    GATE_NONE         = 0,
    GATE_IN_EXCEPTION = 1,  /* already inside an exception                     */
    GATE_COOLDOWN     = 2,  /* post-exception cooldown block                   */
    GATE_SR_IE        = 3,  /* COP0 SR IEc clear (interrupts globally off)     */
    GATE_SR_IM2       = 4,  /* COP0 SR IM2 clear (HW int line masked)          */
    GATE_CALL_DEPTH   = 5,  /* ape-fw: inside nested generated call, not a safe point */
    GATE_BAD_RESUME   = 6,  /* ape-fw: no valid guest resume PC at this check  */
} EventGateReason;

typedef enum {
    MODE_UNKNOWN       = 0,
    MODE_STATIC        = 1,  /* statically-recompiled ROM/EXE code             */
    MODE_NATIVE_OVERLAY= 2,  /* native overlay DLL function                    */
    MODE_INTERP        = 3,  /* dirty-RAM interpreter                          */
} EventExecMode;

typedef struct {
    uint64_t seq;        /* monotonic sequence                                 */
    uint64_t cycle;      /* psx_get_cycle_count() at event                     */
    uint32_t pc;         /* g_debug_last_store_pc (best "where" signal)        */
    uint32_t func;       /* g_debug_current_func_addr                          */
    uint32_t overlay_fn; /* in-progress native overlay fn (0 if none)          */
    uint32_t i_stat;
    uint32_t i_mask;
    uint32_t aux;        /* per-kind payload: DMA events -> chcr value; else 0  */
    uint16_t kind;       /* EventKind                                          */
    uint8_t  mode;       /* EventExecMode at event time                        */
    uint8_t  detail;     /* channel / gate reason / IRQ bit, per kind          */
} EventEntry;

/* Record one event. Captures cycle/pc/func/overlay_fn/i_stat/i_mask/mode from
 * runtime globals internally, so call sites are a single line. No-op when the
 * ring is disabled. Safe to call from any runtime path. */
void event_ring_record(uint16_t kind, uint8_t detail);
/* As above, plus a per-kind aux payload (DMA events pass the CHCR value). */
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux);

/* Write the whole live window (oldest->newest) as a JSON array to `path`.
 * Returns entries written, or -1 on open failure. No TCP size limit. */
int  event_ring_dump_file(const char *path);

/* Emit a bounded JSON tail (most-recent `max_entries`, oldest->newest) into
 * `out`. Returns bytes written. For quick TCP inspection. */
int  event_ring_dump_json(char *out, int cap, int max_entries);

/* Clear the ring (e.g. to isolate a fresh window before a run). */
void event_ring_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_EVENT_RING_H */
