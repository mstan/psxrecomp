/* parity_trace.c — general two-process control-flow parity trace.
 * See parity_trace.h. Self-contained (no runtime internals) so the SAME TU
 * compiles into both psx-runtime and psx-beetle and they emit identical rows. */
#include "parity_trace.h"
#include "psx_bss.h"
#include <string.h>

#define PARITY_RING_CAP 131072u  /* power of two; ~128K thread1 events of history
                                  * so the parked-thread1 divergence is in-window,
                                  * not just the wedge tail (~7.5 MB ring). */

static PSX_BSS ParityEntry s_ring[PARITY_RING_CAP];
static uint64_t    s_seq      = 0;
static uint32_t    s_armed    = 0;
static uint32_t    s_frozen   = 0;

static uint32_t    s_watched_tcb   = 0;
static uint32_t    s_trigger       = 0;
static uint32_t    s_epc_off       = 0x88u;
static uint32_t    s_state_off     = 0x00u;
static PSX_BSS uint32_t    s_watch[PARITY_WATCH_MAX];
static int         s_watch_count   = 0;

/* Per-watch-word last-writer table (see parity_trace_note_write). */
static PSX_BSS uint32_t    s_wpc[PARITY_WATCH_MAX];
static PSX_BSS uint64_t    s_wcycle[PARITY_WATCH_MAX];
static PSX_BSS uint32_t    s_wframe[PARITY_WATCH_MAX];
static PSX_BSS uint32_t    s_wtcb[PARITY_WATCH_MAX];
static uint32_t    s_last_current_tcb = 0; /* writer-TCB attribution for note_write */

/* Dedicated READ ring (see parity_trace_note_read). One ParityEntry per read of a
 * watched word, byte-identical wire format to the write ring so tools diff both.
 * Each row carries reader provenance (pc/cycle/frame + addr in epc, value in target
 * & the matched watch slot) AND the current last-WRITER metadata for that slot, so
 * a single read row answers "who read what value, that whom last wrote". */
static PSX_BSS ParityEntry s_rring[PARITY_RING_CAP];
static uint64_t    s_rseq = 0;

/* Frame + guest-cycle stamps: each host process defines these accessors
 * (decoupled so this single TU compiles into both). cycle = absolute guest CPU
 * cycles since boot — the deterministic ruler for native<->Beetle drift. */
extern uint32_t parity_host_frame(void);
extern uint64_t parity_host_cycle(void);

void parity_trace_config(uint32_t watched_tcb, uint32_t trigger_target,
                         uint32_t tcb_epc_off, uint32_t tcb_state_off,
                         const uint32_t* watch_addrs, int watch_count)
{
    s_watched_tcb = watched_tcb;
    s_trigger     = trigger_target;
    s_epc_off     = tcb_epc_off;
    s_state_off   = tcb_state_off;
    if (watch_count < 0) watch_count = 0;
    if (watch_count > PARITY_WATCH_MAX) watch_count = PARITY_WATCH_MAX;
    s_watch_count = watch_count;
    for (int i = 0; i < PARITY_WATCH_MAX; i++) {
        s_watch[i] = (watch_addrs && i < watch_count) ? watch_addrs[i] : 0;
        s_wpc[i] = 0; s_wcycle[i] = 0; s_wframe[i] = 0; s_wtcb[i] = 0;
    }
    s_rseq = 0;
}

void parity_trace_arm(int on)        { s_armed = on ? 1u : 0u; }
void parity_trace_reset(void)        {
    s_seq = 0; s_frozen = 0; memset(s_ring, 0, sizeof(s_ring));
    s_rseq = 0; memset(s_rring, 0, sizeof(s_rring));
    for (int i = 0; i < PARITY_WATCH_MAX; i++) {
        s_wpc[i] = 0; s_wcycle[i] = 0; s_wframe[i] = 0; s_wtcb[i] = 0;
    }
    s_last_current_tcb = 0;
}
int  parity_trace_is_armed(void)     { return (int)s_armed; }
int  parity_trace_is_frozen(void)    { return (int)s_frozen; }
uint64_t parity_trace_total(void)    { return s_seq; }

const char* parity_kind_str(uint32_t kind)
{
    switch (kind) {
        case PARITY_KIND_DISPATCH:     return "dispatch";
        case PARITY_KIND_CHANGETHREAD: return "changethread";
        case PARITY_KIND_RFE:          return "rfe";
        case PARITY_KIND_YIELD:        return "yield";
        case PARITY_KIND_SAVE:         return "save";
        case PARITY_KIND_RESTORE:      return "restore";
        case PARITY_KIND_TRIGGER:      return "trigger";
        default:                       return "?";
    }
}

void parity_trace_record(parity_kind_t kind, uint32_t pc, uint32_t ra,
                         uint32_t sp, uint32_t target,
                         parity_read_word_fn read_word, void* ctx)
{
    if (!s_armed || s_frozen || !read_word) return;

    /* current TCB = *(*(0x108)) — the PSX kernel TCBH chain. */
    uint32_t tcbh = read_word(ctx, 0x00000108u);
    uint32_t current_tcb = tcbh ? read_word(ctx, tcbh) : 0u;

    /* Track the running thread for note_write's writer-TCB attribution. Updated
     * even for non-watched TCBs so a write by another thread is attributed to it. */
    s_last_current_tcb = current_tcb;

    if (s_watched_tcb && current_tcb != s_watched_tcb) return;

    ParityEntry* e = &s_ring[s_seq & (PARITY_RING_CAP - 1u)];
    e->seq         = s_seq;
    e->frame       = parity_host_frame();
    e->cycle       = parity_host_cycle();
    e->kind        = (uint32_t)kind;
    e->current_tcb = current_tcb;
    e->pc          = pc;
    e->ra          = ra;
    e->sp          = sp;
    e->target      = target;
    e->epc         = s_watched_tcb ? read_word(ctx, s_watched_tcb + s_epc_off)   : 0u;
    e->tcb_state   = s_watched_tcb ? read_word(ctx, s_watched_tcb + s_state_off) : 0u;
    for (int i = 0; i < PARITY_WATCH_MAX; i++) {
        e->watch[i]        = (i < s_watch_count && s_watch[i]) ? read_word(ctx, s_watch[i]) : 0u;
        e->watch_wpc[i]    = s_wpc[i];
        e->watch_wcycle[i] = s_wcycle[i];
        e->watch_wframe[i] = s_wframe[i];
        e->watch_wtcb[i]   = s_wtcb[i];
    }
    s_seq++;

    /* Latch on the trigger so the pre-divergence window survives the wedge. The
     * trigger row itself is recorded (above) before freezing. */
    if (s_trigger && target == s_trigger) {
        /* re-tag the just-written row as the trigger for easy diffing */
        e->kind = (uint32_t)PARITY_KIND_TRIGGER;
        s_frozen = 1;
    }
}

void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t writer_pc)
{
    if (!s_armed || s_frozen) return;
    uint32_t pa    = addr & 0x1FFFFFFFu;
    uint32_t pa_hi = pa + (width ? width : 1u);
    for (int i = 0; i < s_watch_count; i++) {
        if (!s_watch[i]) continue;
        uint32_t wa = s_watch[i] & 0x1FFFFFFFu;   /* watch word occupies [wa, wa+4) */
        if (pa < wa + 4u && pa_hi > wa) {
            s_wpc[i]    = writer_pc;
            s_wcycle[i] = parity_host_cycle();
            s_wframe[i] = parity_host_frame();
            s_wtcb[i]   = s_last_current_tcb;
        }
    }
}

void parity_trace_note_read(uint32_t addr, uint32_t value, uint32_t reader_pc)
{
    if (!s_armed || s_frozen) return;
    uint32_t pa = addr & 0x1FFFFFFFu;
    for (int i = 0; i < s_watch_count; i++) {
        if (!s_watch[i]) continue;
        uint32_t wa = s_watch[i] & 0x1FFFFFFFu;   /* watch word occupies [wa, wa+4) */
        if (pa < wa + 4u && pa + 4u > wa) {
            ParityEntry* e = &s_rring[s_rseq & (PARITY_RING_CAP - 1u)];
            memset(e, 0, sizeof(*e));
            e->seq    = s_rseq;
            e->frame  = parity_host_frame();
            e->cycle  = parity_host_cycle();
            e->kind   = (uint32_t)PARITY_KIND_DISPATCH; /* reads share the dispatch
                          * wire; the reads=1 dump names them — no read-only kind. */
            e->pc     = reader_pc;   /* the load instruction */
            e->epc    = addr;        /* read address (carried in the epc slot) */
            e->target = value;       /* the loaded word */
            /* Mirror value into the matched watch slot and attach that slot's
             * current last-WRITER provenance, so one read row shows read + write. */
            e->watch[i]        = value;
            e->watch_wpc[i]    = s_wpc[i];
            e->watch_wcycle[i] = s_wcycle[i];
            e->watch_wframe[i] = s_wframe[i];
            e->watch_wtcb[i]   = s_wtcb[i];
            s_rseq++;
            return; /* one row per read even if it straddles two watch words */
        }
    }
}

uint32_t parity_trace_get(ParityEntry* out, uint32_t max_rows)
{
    if (!out || max_rows == 0) return 0;
    uint64_t total = s_seq;
    uint32_t avail = (total < PARITY_RING_CAP) ? (uint32_t)total : PARITY_RING_CAP;
    uint32_t n = (avail < max_rows) ? avail : max_rows;
    uint64_t start = total - n;
    for (uint32_t i = 0; i < n; i++)
        out[i] = s_ring[(start + i) & (PARITY_RING_CAP - 1u)];
    return n;
}

uint64_t parity_trace_reads_total(void) { return s_rseq; }

uint32_t parity_trace_reads_get(ParityEntry* out, uint32_t max_rows)
{
    if (!out || max_rows == 0) return 0;
    uint64_t total = s_rseq;
    uint32_t avail = (total < PARITY_RING_CAP) ? (uint32_t)total : PARITY_RING_CAP;
    uint32_t n = (avail < max_rows) ? avail : max_rows;
    uint64_t start = total - n;
    for (uint32_t i = 0; i < n; i++)
        out[i] = s_rring[(start + i) & (PARITY_RING_CAP - 1u)];
    return n;
}
