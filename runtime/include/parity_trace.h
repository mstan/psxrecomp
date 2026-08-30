/* parity_trace.h — general two-process control-flow parity trace.
 *
 * Purpose: end the "somewhere upstream" blindness. A thread/TCB-scoped,
 * freeze-on-trigger ring with an IDENTICAL struct + JSON wire format on BOTH
 * psx-runtime (code under test) and psx-beetle (independent oracle), so a
 * generic sequence-diff tool (tools/parity_diff.py) can locate the FIRST logical
 * divergence between the two timelines — not a frame diff (PRINCIPLES.md "find
 * the first divergence" + "is the value WRONG or is the behavior MISSING").
 *
 * Model (CLAUDE.md ring-buffer doctrine): armed from boot (so the pre-divergence
 * window is always covered — never arm-then-time), records one row per relevant
 * control event while current_tcb == watched_tcb (or all TCBs if watched==0),
 * and LATCHES (freezes) the moment a configured trigger target is dispatched, so
 * the lead-up to the wedge survives instead of being evicted by the wedge loop.
 *
 * Generality: point it at any watched TCB + freeze trigger + up to six watch-word
 * addresses. The MMX6 cutscene→gameplay freeze is the first user, but nothing
 * here is MMX6-specific — it is the reusable substrate for a functional-parity
 * audit between recomp and oracle.
 */
#ifndef PARITY_TRACE_H
#define PARITY_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PARITY_KIND_DISPATCH = 0,   /* psx_dispatch iteration (per-leader) */
    PARITY_KIND_CHANGETHREAD,   /* syscall-3 ChangeThread decision point */
    PARITY_KIND_RFE,            /* ReturnFromException */
    PARITY_KIND_YIELD,          /* scheduler cross-thread switch committed */
    PARITY_KIND_SAVE,           /* context saved to a TCB */
    PARITY_KIND_RESTORE,        /* context restored from a TCB */
    PARITY_KIND_TRIGGER,        /* the freeze trigger fired (final row) */
} parity_kind_t;

#define PARITY_WATCH_MAX 6

typedef struct {
    uint64_t seq;
    uint32_t frame;
    uint32_t kind;
    uint64_t cycle;      /* absolute guest CPU cycles since boot (the deterministic
                          * ruler; native psx_get_cycle_count / Beetle
                          * beetle_core_get_guest_cycles — directly comparable) */
    uint32_t current_tcb;
    uint32_t pc;
    uint32_t ra;
    uint32_t sp;
    uint32_t epc;        /* resume PC read from watched TCB (epc slot) */
    uint32_t tcb_state;  /* state/status word read from watched TCB */
    uint32_t target;     /* dispatch / switch target */
    uint32_t watch[PARITY_WATCH_MAX];
    /* Per-watch-word LAST-WRITER provenance (set by parity_trace_note_write):
     * the exact store PC, guest cycle, frame, and (approx) writing TCB of the
     * most recent write to each watch word. This is what turns "the value
     * diverged" into "who/when produced the divergent value" — diff these across
     * native vs Beetle to name the early producer of a gate input. */
    uint32_t watch_wpc[PARITY_WATCH_MAX];
    uint64_t watch_wcycle[PARITY_WATCH_MAX];
    uint32_t watch_wframe[PARITY_WATCH_MAX];
    uint32_t watch_wtcb[PARITY_WATCH_MAX];
} ParityEntry;

/* Each process supplies its own RAM reader (recomp: cpu->read_word; Beetle:
 * PS_CPU peek). ctx is passed back verbatim. */
typedef uint32_t (*parity_read_word_fn)(void* ctx, uint32_t addr);

/* Configure. watched_tcb==0 => record all TCBs. trigger_target==0 => no freeze.
 * tcb_epc_off / tcb_state_off are watched-TCB-relative byte offsets (PSX BIOS:
 * epc slot = tcb+0x88, state word = tcb+0x00). watch_addrs/watch_count: up to
 * PARITY_WATCH_MAX guest word addresses snapshotted on every row. Does not arm. */
void parity_trace_config(uint32_t watched_tcb, uint32_t trigger_target,
                         uint32_t tcb_epc_off, uint32_t tcb_state_off,
                         const uint32_t* watch_addrs, int watch_count);
void parity_trace_arm(int on);     /* on=1 begins recording (idempotent) */
void parity_trace_reset(void);     /* clear ring + unfreeze (keeps config) */
int  parity_trace_is_armed(void);  /* hot-path gate: cheap one-branch check */
int  parity_trace_is_frozen(void);

/* Record one event. read_word+ctx source current_tcb (via TCBH 0x108), the
 * watched-TCB epc/state, and the watch words. No-op unless armed, not frozen,
 * and (watched==0 || current_tcb==watched). Latches on target==trigger. */
void parity_trace_record(parity_kind_t kind, uint32_t pc, uint32_t ra,
                         uint32_t sp, uint32_t target,
                         parity_read_word_fn read_word, void* ctx);

/* Note a guest memory write so the per-watch-word last-writer table stays
 * current. addr may be guest-virtual or physical (masked internally); width is
 * the access size in bytes (1/2/4); writer_pc is the EXACT store instruction PC
 * (native: g_debug_last_store_pc; Beetle: the store pc from the wtrace hook).
 * Updates a watch slot iff [addr,addr+width) overlaps that watch word. No-op
 * unless armed and not frozen. Called from BOTH processes' write paths so the
 * provenance is directly comparable. The writing TCB is taken from the most
 * recent recorded control event (a cheap, accurate-in-practice approximation —
 * the writer is the running thread). */
void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t writer_pc);

/* Read-side counterpart of parity_trace_note_write: record the EXACT load PC that
 * last consumed each watch word (read provenance), so a diverging value can be
 * traced to the last reader as well as the last writer. addr may be guest-virtual
 * or physical (masked internally); value is the loaded word; reader_pc is the load
 * instruction PC. No-op unless armed and not frozen. Called from both processes'
 * read paths so read provenance is directly comparable. */
void parity_trace_note_read(uint32_t addr, uint32_t value, uint32_t reader_pc);

/* Readback (matches the beetle_fntrace_get pattern): copies up to max_rows
 * NEWEST entries, oldest-first, into out[]; returns the count copied. */
uint32_t parity_trace_get(ParityEntry* out, uint32_t max_rows);
uint64_t parity_trace_total(void);
const char* parity_kind_str(uint32_t kind);

/* Readback for the dedicated READ ring (parity_trace_note_read). Same shape as
 * parity_trace_get/total: newest rows oldest-first. Each row's pc = load PC,
 * epc = read address, target/watch[slot] = loaded value, watch_w* = the last
 * writer of that word at read time. Backs the debug server's parity_dump reads=1. */
uint32_t parity_trace_reads_get(ParityEntry* out, uint32_t max_rows);
uint64_t parity_trace_reads_total(void);

#ifdef __cplusplus
}
#endif

#endif /* PARITY_TRACE_H */
