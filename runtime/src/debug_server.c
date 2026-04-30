/*
 * debug_server.c -- TCP debug server for PSX recomp v4
 *
 * Single-threaded, non-blocking TCP server polled once per vblank.
 * JSON-over-newline protocol on localhost:4370.
 *
 * Same function names and protocol as nesrecomp/snesrecomp versions
 * so TCP.md and DEBUG.md are reusable across projects.
 */
#include "debug_server.h"
#include "cpu_state.h"
#include "dma.h"
#include "gpu.h"
#include "sio.h"
#include "memcard.h"
#include "interrupts.h"
#include "psx_cycles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* ---- Platform sockets ---- */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
   static int sock_error(void) { return WSAGetLastError(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
   static int sock_error(void) { return errno; }
#endif

#include <SDL.h>

/* ---- Externs from runtime ---- */
extern uint32_t i_stat;
extern uint32_t i_mask;

/* Memory access (from memory.c) */
extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);
extern uint8_t  psx_read_byte(uint32_t addr);
extern void     psx_write_byte(uint32_t addr, uint8_t val);

/* ---- Server state ---- */
static sock_t s_listen  = SOCK_INVALID;
static sock_t s_client  = SOCK_INVALID;
static int    s_port    = 4370;

#define RECV_BUF_SIZE 8192
static char s_recv_buf[RECV_BUF_SIZE];
static int  s_recv_len = 0;

/* ---- Frame counter (set by record_frame caller) ---- */
static uint64_t s_frame_count = 0;

/* ---- CPU state pointer (set at init) ---- */
static CPUState *s_cpu = NULL;

/* ---- Pause / step ---- */
static volatile int s_paused     = 0;
static int          s_step_count = 0;
static uint32_t     s_run_to     = 0;

/* ---- Input override ---- */
static int s_input_override = -1;
static int s_input_frames   = 0;

/* ---- Ring buffer (heap-allocated) ---- */
static PSXFrameRecord *s_frame_history = NULL;
static uint64_t        s_history_count = 0;

/* ---- Snapshot regions (configurable via set_snapshot command) ---- */
static uint32_t s_snapshot_addrs[RAM_SNAPSHOT_REGIONS];
static int      s_snapshot_active[RAM_SNAPSHOT_REGIONS];

/* ---- Watchpoints ---- */
#define MAX_WATCHPOINTS 8
typedef struct {
    uint32_t addr;
    uint8_t  prev_val;
    int      active;
} Watchpoint;
static Watchpoint s_watchpoints[MAX_WATCHPOINTS];

/* ---- Write trace (Tier 1 reverse debugger) ----
 * Records every RAM write matching one of up to 8 configurable address ranges.
 * 1M-entry ring buffer, heap-allocated in debug_server_init(). */
#define WRITE_TRACE_CAP (1 << 20)  /* 1M entries = 32 MB */
typedef struct {
    uint64_t seq;        /* monotonic sequence number */
    uint32_t addr;       /* physical RAM address */
    uint32_t old_val;    /* pre-write value */
    uint32_t new_val;    /* post-write value */
    uint32_t ra;         /* $ra (caller return address) */
    uint32_t func_addr;  /* dispatch target (which recompiled function) */
    uint32_t frame;      /* VBlank frame number */
    uint8_t  width;      /* 1, 2, or 4 */
    uint8_t  pad[3];     /* align to 32 bytes */
} WriteTraceEntry;
static WriteTraceEntry *s_wtrace = NULL;
static uint64_t s_wtrace_seq  = 0;  /* total writes ever recorded */
static uint32_t s_wtrace_head = 0;

/* Multi-range filter: up to 8 [lo, hi) address ranges. */
#define WTRACE_MAX_RANGES 16
static struct { uint32_t lo, hi; } s_wtrace_ranges[WTRACE_MAX_RANGES];
static int s_wtrace_range_count = 0;

/* Function attribution global — set by psx_dispatch() before each call. */
uint32_t g_debug_current_func_addr = 0;

/* Last store PC — set by recompiler emitter before every store instruction. */
uint32_t g_debug_last_store_pc = 0;

/* ---- SIO write PC tracer ring ----
 * Captures (pc, addr, value, byte_seq, ctr) for every write to a SIO
 * register, attributing the exact writing instruction.  Used to find what
 * code is putting bytes on the SIO bus when chain-dispatcher attribution
 * (g_debug_current_func_addr) is too coarse. 1<<16 = 64K entries. */
#define SIO_PC_TRACE_CAP (1 << 16)
typedef struct {
    uint64_t seq;
    uint32_t pc;            /* g_debug_last_store_pc at the moment of write */
    uint32_t func;          /* g_debug_current_func_addr — outer frame */
    uint32_t addr;          /* full MMIO address written */
    uint32_t value;         /* value written (low 16/32 bits) */
    uint32_t byte_seq;      /* sio_get_seq() — cross-ref with sio_trace */
    uint8_t  width;         /* 1=byte, 2=half, 4=word */
    uint8_t  pad[3];
} SioPcTraceEntry;
static SioPcTraceEntry s_sio_pc_trace[SIO_PC_TRACE_CAP];
static uint64_t s_sio_pc_trace_seq = 0;

void debug_server_log_sio_write(uint32_t addr, uint32_t value, uint8_t width) {
    SioPcTraceEntry *e = &s_sio_pc_trace[s_sio_pc_trace_seq % SIO_PC_TRACE_CAP];
    e->seq      = s_sio_pc_trace_seq++;
    e->pc       = g_debug_last_store_pc;
    e->func     = g_debug_current_func_addr;
    e->addr     = addr;
    e->value    = value;
    e->byte_seq = sio_get_seq();
    e->width    = width;
}

/* ---- Dispatch trace ring buffer ----
 * Records every dispatched function address for post-mortem analysis.
 * 64K entries, stack-allocated (256 KB). */
#define DISPATCH_TRACE_CAP (1 << 16)
static uint32_t s_dispatch_ring[DISPATCH_TRACE_CAP];
static uint64_t s_dispatch_seq = 0;

/* Unique dispatch set — tracks every unique function address ever dispatched.
 * Simple hash set with linear probing. */
#define DISPATCH_UNIQUE_CAP 4096
static uint32_t s_dispatch_unique[DISPATCH_UNIQUE_CAP];
static int s_dispatch_unique_count = 0;

static void dispatch_unique_add(uint32_t addr) {
    uint32_t idx = (addr >> 2) % DISPATCH_UNIQUE_CAP;
    for (int i = 0; i < DISPATCH_UNIQUE_CAP; i++) {
        uint32_t slot = (idx + i) % DISPATCH_UNIQUE_CAP;
        if (s_dispatch_unique[slot] == addr) return; /* already present */
        if (s_dispatch_unique[slot] == 0) {
            s_dispatch_unique[slot] = addr;
            s_dispatch_unique_count++;
            return;
        }
    }
}

static int dispatch_trace_contains(uint32_t target) {
    uint32_t idx = (target >> 2) % DISPATCH_UNIQUE_CAP;
    for (int i = 0; i < DISPATCH_UNIQUE_CAP; i++) {
        uint32_t slot = (idx + i) % DISPATCH_UNIQUE_CAP;
        if (s_dispatch_unique[slot] == target) return 1;
        if (s_dispatch_unique[slot] == 0) return 0;
    }
    return 0;
}

/* ---- Chain-dispatch return-v0 ring ----
 * Each entry pairs (just-completed dispatch target, return v0 captured from
 * cpu->gpr[2]) with the chain counter (mem[0x7514]) and the SIO byte index.
 * Size 4096 — only writes when prev target was a chain state (states 1..13
 * read or 1..4 detection). Other dispatches go through trace_dispatch
 * untouched. */
/* 64K entries × ~24 B ≈ 1.5 MB; holds tens of minutes of chain transitions. */
#define CHAIN_TRACE_CAP (1 << 16)
typedef struct {
    uint64_t seq;
    uint32_t prev_target;     /* phys addr of the dispatch that just returned */
    uint32_t v0;              /* cpu->gpr[2] AT the next-dispatch trace point */
    uint32_t counter_7514;    /* mem[0x7514] at this moment */
    uint32_t flag_7520;       /* mem[0x7520] success flag */
    uint32_t mc_byte_seq;     /* sio_get_seq() for cross-ref */
} ChainTraceEntry;
static ChainTraceEntry s_chain_trace[CHAIN_TRACE_CAP];
static uint64_t s_chain_trace_seq = 0;
static uint32_t s_prev_dispatch_target = 0;

/* Track whether we're currently INSIDE a chain-state subtree.
 * Set when chain state entry dispatched; cleared when chain epilogue
 * (0x5B54 or 0x5B58) is dispatched. v0 captured at the post-epilogue
 * dispatch is the state's final return value. */
static uint32_t s_chain_state_active = 0;  /* 0 if not in a state, else state addr */

/* ---- Function ENTRY / EXIT trace rings (Tier 1 reverse-debugger) ----
 *
 * Two parallel always-on rings, hooked off psx_dispatch via
 * debug_server_trace_dispatch. Together they provide:
 *
 *   Entry ring  — every dispatch in CALL ORDER, with $a0..$a3, $ra, $t1
 *                 (the B0 function index when target is a B0 vector).
 *   Exit  ring  — every function in FINISH ORDER, with $v0, $v1 and a
 *                 link back to the entry seq.
 *
 * Exit detection uses a shadow call stack: each entry pushes (target, ra);
 * when a later dispatch's TARGET == a stack frame's RA, we pop intermediate
 * frames as "exited" and record their exit values. This is heuristic
 * (longjmp / setjmp / chain-handler tail-jumps via jr $t8 will skip), but
 * covers ~95% of normal call/return.
 *
 * Optional address filter so the ring isn't drowned by hot kernel funcs.
 * Default: trace everything; user sets [lo, hi) range to focus.
 *
 * Memory: 1M entries × 32B = 32 MB per ring × 2 = 64 MB. Heap-allocated. */
/* Entry ring sized for ≥3 min of unrotated history at peak capture rate
 * (~600k/sec under a wide direct-call filter): 1<<27 ≈ 134M entries × 56 B
 * ≈ 7 GB.  Lazily resident — only pages actually written use RAM, so the
 * footprint grows from 0 toward the cap as the ring fills.
 * Exit ring stays modest (1<<22, ~192 MB) — exits are auxiliary.  */
#define FN_TRACE_CAP        (1 << 27)
#define FN_EXIT_TRACE_CAP   (1 << 22)
#define FN_STACK_DEPTH 4096

typedef struct {
    uint64_t seq;
    uint64_t paired_exit_seq; /* set later when matching exit recorded; 0 = open */
    uint32_t func_addr;
    uint32_t ra;
    uint32_t a0, a1, a2, a3;
    uint32_t t1;              /* B0/A0/C0 function index when target is BIOS vector */
    uint32_t depth;           /* shadow stack depth at entry */
    uint32_t frame;
} FnEntryEntry;

typedef struct {
    uint64_t seq;
    uint64_t entry_seq;       /* link back to entry */
    uint32_t func_addr;
    uint32_t v0, v1;
    uint32_t depth;           /* shadow stack depth at exit */
    uint32_t frame;
    uint32_t pad;
} FnExitEntry;

static FnEntryEntry *s_fn_entry      = NULL;
static FnExitEntry  *s_fn_exit       = NULL;
static uint64_t      s_fn_entry_seq  = 0;
static uint64_t      s_fn_exit_seq   = 0;

/* Shadow call stack: tracks open call frames. */
typedef struct {
    uint32_t func_addr;
    uint32_t ra;
    uint64_t entry_seq;       /* index in s_fn_entry */
} FnStackFrame;
static FnStackFrame s_fn_stack[FN_STACK_DEPTH];
static int          s_fn_stack_top = 0;
static uint32_t     s_fn_prev_ra   = 0;   /* last seen $ra; new JAL changes this */
/* Stats: how many shadow-stack pops we couldn't match (interference signal). */
static uint64_t     s_fn_unmatched_returns = 0;
static uint64_t     s_fn_stack_overflows   = 0;
static uint64_t     s_fn_tail_calls        = 0;

/* ---- EvCB walk ring ----
 *
 * Captures the full kernel EvCB table on every entry to and exit from
 * DeliverEvent (RAM 0x1C5C). EvCB base ptr lives at RAM 0x124, total
 * bytes at RAM 0x120 (per disasm InitEvents at BFC04678).
 *
 * Per-entry tag identifies whether the snapshot was taken at DeliverEvent
 * entry or exit; pairing them lets the operator see exactly which entries
 * changed status (FIRED) during one DeliverEvent call.
 *
 * Memory: 256 snapshots × ~960B = ~240 KB. */
#define EVCB_DELIVER_EVENT_ADDR 0x00001C5Cu  /* func_00001C5C, RAM */
/* 4096 snapshots × 32 entries × ~28 B ≈ 3.7 MB — holds many minutes. */
#define EVCB_RING_CAP           4096
#define EVCB_MAX_ENTRIES        32
#define EVCB_ENTRY_SIZE         28           /* sizeof EvCB on real PSX */

typedef struct {
    uint32_t cls;
    uint32_t status;
    uint32_t spec;
    uint32_t mode;
    uint32_t fhandler;
    uint32_t pad1;
    uint32_t pad2;
} EvCBRec;

typedef enum { EVCB_TAG_ENTRY = 0, EVCB_TAG_EXIT = 1 } EvCBTag;

typedef struct {
    uint64_t seq;
    uint64_t fn_entry_seq;       /* DeliverEvent fn_entry seq this snapshot is paired with */
    uint8_t  tag;                /* EvCBTag */
    uint8_t  pad[3];
    uint32_t evcb_base;          /* RAM addr of EvCB table */
    uint32_t evcb_total_bytes;
    uint32_t entry_count;        /* number of EvCB entries actually snapshotted */
    uint32_t a0, a1;             /* DeliverEvent args ($a0, $a1) when known */
    uint32_t v0;                 /* return value (only valid for EXIT snapshots) */
    uint32_t counter_7514;       /* card chain counter */
    uint32_t flag_755A;          /* card chain abort flag */
    uint32_t flag_75C0;          /* state-11 success marker (B0:6380) */
    uint32_t frame;
    EvCBRec  entries[EVCB_MAX_ENTRIES];
} EvCBSnapshot;

static EvCBSnapshot *s_evcb_ring = NULL;
static uint64_t      s_evcb_ring_seq = 0;
static uint64_t      s_evcb_ring_entry_count = 0; /* DeliverEvent entries seen */
static uint64_t      s_evcb_ring_exit_count  = 0; /* DeliverEvent exits seen */

/* DeliverEvent return tracking — bypasses shadow stack since the kernel
 * uses psx_restore_state_escape() longjmp, which leaves orphaned frames.
 * Watch for the next dispatch matching the entry's stored RA; if a new
 * DeliverEvent entry happens first, the previous one was unwound. */
static uint32_t s_evcb_pending_exit_ra   = 0;
static uint64_t s_evcb_pending_entry_seq = 0;
static int      s_evcb_pending_active    = 0;
static uint64_t s_evcb_unwound_count     = 0; /* DeliverEvent calls that were longjmp'd over */

extern uint8_t psx_read_byte(uint32_t addr);

static uint32_t evcb_read_u32_ram(uint32_t ram_addr) {
    return  (uint32_t)psx_read_byte(ram_addr)
          | ((uint32_t)psx_read_byte(ram_addr + 1) << 8)
          | ((uint32_t)psx_read_byte(ram_addr + 2) << 16)
          | ((uint32_t)psx_read_byte(ram_addr + 3) << 24);
}

/* Snapshot the EvCB table into the ring. Reads kernel ptr [0x120] and
 * size [0x124] (per nocash kernel ToT layout: 0x120=EvCB ptr, 0x124=EvCB
 * size). Walks up to EVCB_MAX_ENTRIES entries. */
static void evcb_snapshot_capture(EvCBTag tag, uint64_t fn_entry_seq) {
    if (!s_evcb_ring || !debug_cpu_ptr) return;

    uint32_t base_ptr   = evcb_read_u32_ram(0x00000120);
    uint32_t total_bytes = evcb_read_u32_ram(0x00000124);

    /* Convert RAM base ptr to RAM offset (kernel uses cached + uncached
     * mirrors; mask off region bits). */
    uint32_t base_ram = base_ptr & 0x001FFFFFu;

    EvCBSnapshot *e = &s_evcb_ring[s_evcb_ring_seq % EVCB_RING_CAP];
    e->seq              = s_evcb_ring_seq;
    e->fn_entry_seq     = fn_entry_seq;
    e->tag              = (uint8_t)tag;
    e->evcb_base        = base_ptr;
    e->evcb_total_bytes = total_bytes;
    e->a0               = debug_cpu_ptr->gpr[4];
    e->a1               = debug_cpu_ptr->gpr[5];
    e->v0               = debug_cpu_ptr->gpr[2];
    e->counter_7514     = evcb_read_u32_ram(0x00007514);
    /* 0x755A is a single byte (chain abort flag). Read its byte directly,
     * not the dword at 0x7558. */
    e->flag_755A        = (uint32_t)psx_read_byte(0x0000755A);
    e->flag_75C0        = evcb_read_u32_ram(0x000075C0);
    e->frame            = (uint32_t)s_frame_count;

    uint32_t n_entries = (total_bytes / EVCB_ENTRY_SIZE);
    if (n_entries > EVCB_MAX_ENTRIES) n_entries = EVCB_MAX_ENTRIES;
    e->entry_count = n_entries;

    for (uint32_t i = 0; i < n_entries; i++) {
        uint32_t off = base_ram + i * EVCB_ENTRY_SIZE;
        if (off + EVCB_ENTRY_SIZE > 0x00200000u) break;
        e->entries[i].cls      = evcb_read_u32_ram(off + 0);
        e->entries[i].status   = evcb_read_u32_ram(off + 4);
        e->entries[i].spec     = evcb_read_u32_ram(off + 8);
        e->entries[i].mode     = evcb_read_u32_ram(off + 12);
        e->entries[i].fhandler = evcb_read_u32_ram(off + 16);
        e->entries[i].pad1     = evcb_read_u32_ram(off + 20);
        e->entries[i].pad2     = evcb_read_u32_ram(off + 24);
    }
    s_evcb_ring_seq++;
    if (tag == EVCB_TAG_ENTRY) s_evcb_ring_entry_count++;
    else                       s_evcb_ring_exit_count++;
}

/* Optional [lo, hi) physical-address filter; default = trace all. */
static uint32_t s_fn_trace_filter_lo = 0u;
static uint32_t s_fn_trace_filter_hi = 0xFFFFFFFFu;

static int fn_trace_in_filter(uint32_t phys) {
    return phys >= s_fn_trace_filter_lo && phys < s_fn_trace_filter_hi;
}

/* Helper: record an exit event for a popped frame. */
static void fn_record_exit(FnStackFrame *f) {
    if (!fn_trace_in_filter(f->func_addr)) return;
    FnExitEntry *e = &s_fn_exit[s_fn_exit_seq % FN_EXIT_TRACE_CAP];
    e->seq        = s_fn_exit_seq;
    e->entry_seq  = f->entry_seq;
    e->func_addr  = f->func_addr;
    e->v0         = debug_cpu_ptr->gpr[2];
    e->v1         = debug_cpu_ptr->gpr[3];
    e->depth      = (uint32_t)s_fn_stack_top;
    e->frame      = (uint32_t)s_frame_count;
    /* Back-fill entry's paired_exit_seq if still in ring. */
    if (f->entry_seq != (uint64_t)-1
        && s_fn_entry_seq > f->entry_seq
        && s_fn_entry_seq - f->entry_seq <= FN_TRACE_CAP) {
        s_fn_entry[f->entry_seq % FN_TRACE_CAP].paired_exit_seq = e->seq;
    }
    s_fn_exit_seq++;
}

/* Called from debug_server_trace_dispatch on every dispatch.
 *
 * Classify the dispatch into one of:
 *   RETURN     — target == some frame's saved RA → pop frames as exited
 *   TAIL CALL  — $ra unchanged from previous dispatch → replace top frame
 *   NEW CALL   — $ra changed (fresh JAL) → push new frame
 *
 * Distinguishing TAIL CALL from NEW CALL via $ra-change is what keeps the
 * shadow stack bounded under heavy code that uses fall-through dispatch. */
static void function_trace_record(uint32_t target) {
    if (!s_fn_entry || !s_fn_exit || !debug_cpu_ptr) return;

    uint32_t cur_ra = debug_cpu_ptr->gpr[31];

    /* RETURN check first: walk stack from top down. Match deepest first, since
     * deeper frames may have stale RAs that coincidentally match. */
    int return_idx = -1;
    for (int i = s_fn_stack_top - 1; i >= 0; i--) {
        if (s_fn_stack[i].ra == target) { return_idx = i; break; }
    }
    if (return_idx >= 0) {
        while (s_fn_stack_top > return_idx) {
            s_fn_stack_top--;
            fn_record_exit(&s_fn_stack[s_fn_stack_top]);
        }
        s_fn_prev_ra = cur_ra;
        return;
    }

    /* TAIL CALL: $ra unchanged from the previous dispatch and we have an open
     * frame. Treat as replacing the top frame's func_addr. Also record an
     * exit for the previous func + an entry for the new func, so the user
     * sees the chain of tail calls. */
    if (s_fn_stack_top > 0 && cur_ra == s_fn_prev_ra && cur_ra != 0) {
        s_fn_tail_calls++;
        fn_record_exit(&s_fn_stack[s_fn_stack_top - 1]);
        /* Replace top frame in place (don't push). */
        s_fn_stack[s_fn_stack_top - 1].func_addr = target;
        s_fn_stack[s_fn_stack_top - 1].entry_seq = s_fn_entry_seq;
    }
    /* NEW CALL or first-ever dispatch: push new frame. */
    else {
        if (s_fn_stack_top < FN_STACK_DEPTH) {
            s_fn_stack[s_fn_stack_top].func_addr = target;
            s_fn_stack[s_fn_stack_top].ra        = cur_ra;
            s_fn_stack[s_fn_stack_top].entry_seq = s_fn_entry_seq;
            s_fn_stack_top++;
        } else {
            /* Stack full — drop OLDEST frame (rotate) so we keep tracking
             * recent activity instead of stalling forever. The dropped frame
             * never gets an exit recorded; that's acceptable for unbounded
             * recursion / longjmp pathology. */
            for (int i = 0; i < FN_STACK_DEPTH - 1; i++) s_fn_stack[i] = s_fn_stack[i + 1];
            s_fn_stack[FN_STACK_DEPTH - 1].func_addr = target;
            s_fn_stack[FN_STACK_DEPTH - 1].ra        = cur_ra;
            s_fn_stack[FN_STACK_DEPTH - 1].entry_seq = s_fn_entry_seq;
            s_fn_stack_overflows++;
        }
    }

    /* Record entry (always, when target passes filter). */
    uint64_t this_entry_seq = (uint64_t)-1;
    if (fn_trace_in_filter(target)) {
        FnEntryEntry *e = &s_fn_entry[s_fn_entry_seq % FN_TRACE_CAP];
        e->seq        = s_fn_entry_seq;
        e->paired_exit_seq = 0;
        e->func_addr  = target;
        e->ra         = cur_ra;
        e->a0         = debug_cpu_ptr->gpr[4];
        e->a1         = debug_cpu_ptr->gpr[5];
        e->a2         = debug_cpu_ptr->gpr[6];
        e->a3         = debug_cpu_ptr->gpr[7];
        e->t1         = debug_cpu_ptr->gpr[9];
        e->depth      = (uint32_t)s_fn_stack_top;
        e->frame      = (uint32_t)s_frame_count;
        this_entry_seq = s_fn_entry_seq;
        s_fn_entry_seq++;

        /* Update the just-pushed shadow frame's entry_seq so the EXIT
         * capture in fn_record_exit can pair correctly. */
        if (s_fn_stack_top > 0
            && s_fn_stack[s_fn_stack_top - 1].func_addr == target) {
            s_fn_stack[s_fn_stack_top - 1].entry_seq = this_entry_seq;
        }
    }

    /* EvCB ring: capture on DeliverEvent entry. Track the return RA so
     * we can record an EXIT snapshot when execution next reaches it.
     * cur_ra has the kernel-mode bit set (0x80000000); psx_dispatch passes
     * `target` already masked to physical (& 0x1FFFFFFF). Mask before
     * comparing. If a new DeliverEvent entry arrives first, the previous
     * call was longjmp'd over (psx_restore_state_escape) — count it. */
    if (target == EVCB_DELIVER_EVENT_ADDR) {
        if (s_evcb_pending_active) {
            s_evcb_unwound_count++;
        }
        s_evcb_pending_exit_ra   = cur_ra & 0x1FFFFFFFu;
        s_evcb_pending_entry_seq = this_entry_seq;
        s_evcb_pending_active    = 1;
        evcb_snapshot_capture(EVCB_TAG_ENTRY, this_entry_seq);
    }
    /* DeliverEvent exit: dispatch matching the stored return RA (phys). */
    else if (s_evcb_pending_active && target == s_evcb_pending_exit_ra) {
        evcb_snapshot_capture(EVCB_TAG_EXIT, s_evcb_pending_entry_seq);
        s_evcb_pending_active = 0;
    }

    s_fn_prev_ra = cur_ra;
}

static int is_chain_state_entry(uint32_t phys) {
    static const uint32_t entries[] = {
        /* Read chain (table at 0x6c98) */
        0x000056E8u, 0x00005768u, 0x0000579Cu, 0x00005834u, 0x00005870u,
        0x000058B4u, 0x000058E8u, 0x00005918u, 0x00005954u, 0x00005990u,
        0x00005A00u, 0x00005A58u, 0x00005AB0u,
        /* Detection chain (table at 0x6ccc) */
        0x00005BA4u, 0x00005C24u, 0x00005C58u, 0x00005D48u };
    for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]); i++)
        if (entries[i] == phys) return 1;
    return 0;
}

/* Chain epilogue (common). All chain states fall through to bfc15654 = RAM 0x5B54.
 * func_00005B54 is the lw $ra; jr $ra block. After it runs, the chain dispatcher
 * cascade reads $v0. */
static int is_chain_epilogue(uint32_t phys) {
    return phys == 0x00005B54u || phys == 0x00005B58u;
}

/* Direct-call entry hook — emitted by the recompiler at the top of every
 * generated function.  Captures into the fn_entry ring without touching the
 * shadow stack, so we see direct-jal call paths that never go through
 * psx_dispatch (e.g. shell -> firstfile -> bu_read -> card_read).  The shadow
 * stack is owned by function_trace_record and is only meaningful for
 * indirect dispatches; direct calls return through the native C stack. */
void debug_server_log_call_entry(uint32_t func_addr) {
    if (!s_fn_entry || !debug_cpu_ptr) return;
    if (!fn_trace_in_filter(func_addr)) return;
    FnEntryEntry *e = &s_fn_entry[s_fn_entry_seq % FN_TRACE_CAP];
    e->seq             = s_fn_entry_seq;
    e->paired_exit_seq = 0;
    e->func_addr       = func_addr;
    e->ra              = debug_cpu_ptr->gpr[31];
    e->a0              = debug_cpu_ptr->gpr[4];
    e->a1              = debug_cpu_ptr->gpr[5];
    e->a2              = debug_cpu_ptr->gpr[6];
    e->a3              = debug_cpu_ptr->gpr[7];
    e->t1              = debug_cpu_ptr->gpr[9];
    e->depth           = (uint32_t)s_fn_stack_top;
    e->frame           = (uint32_t)s_frame_count;
    s_fn_entry_seq++;
}

void debug_server_trace_dispatch(uint32_t func_addr) {
    /* Function entry/exit rings (always-on, hooked here so every dispatch
     * is recorded with args and a return value when the call unwinds). */
    function_trace_record(func_addr);

    /* Track when we ENTER a chain state subtree. */
    if (is_chain_state_entry(func_addr)) {
        s_chain_state_active = func_addr;
    }

    /* Capture v0 when we LEAVE a chain state subtree, identified by the dispatch
     * that immediately follows the chain epilogue (0x5B54). The dispatch right
     * AFTER the epilogue is the chain dispatcher's cascade input. */
    if (s_prev_dispatch_target != 0
        && is_chain_epilogue(s_prev_dispatch_target)
        && s_chain_state_active != 0
        && debug_cpu_ptr) {
        ChainTraceEntry *e = &s_chain_trace[s_chain_trace_seq % CHAIN_TRACE_CAP];
        e->seq = s_chain_trace_seq++;
        e->prev_target = s_chain_state_active; /* the state, not the epilogue */
        e->v0 = debug_cpu_ptr->gpr[2];
        extern uint8_t psx_read_byte(uint32_t addr);
        e->counter_7514 = (uint32_t)psx_read_byte(0x7514)
                        | ((uint32_t)psx_read_byte(0x7515) << 8)
                        | ((uint32_t)psx_read_byte(0x7516) << 16)
                        | ((uint32_t)psx_read_byte(0x7517) << 24);
        e->flag_7520    = (uint32_t)psx_read_byte(0x7520)
                        | ((uint32_t)psx_read_byte(0x7521) << 8)
                        | ((uint32_t)psx_read_byte(0x7522) << 16)
                        | ((uint32_t)psx_read_byte(0x7523) << 24);
        e->mc_byte_seq = sio_get_trace(NULL, NULL);
        s_chain_state_active = 0;
    }
    s_prev_dispatch_target = func_addr;

    s_dispatch_ring[s_dispatch_seq % DISPATCH_TRACE_CAP] = func_addr;
    s_dispatch_seq++;
    dispatch_unique_add(func_addr);
}

static int json_get_int(const char *json, const char *key, int def);
static const char *json_get_str(const char *json, const char *key,
                                char *out, int out_sz);
static uint32_t hex_to_u32(const char *s);
static void handle_dirty_ram_stats(int id, const char *json);
void debug_server_send_fmt(const char *fmt, ...);
#define send_fmt debug_server_send_fmt

static void handle_chain_trace(int id, const char *json)
{
    int count = json_get_int(json, "count", 200);
    int total = (int)(s_chain_trace_seq < CHAIN_TRACE_CAP
                      ? s_chain_trace_seq : CHAIN_TRACE_CAP);
    if (count > total) count = total;
    if (count < 0) count = 0;

    char buf[128 * 1024];
    int n = snprintf(buf, sizeof(buf),
                     "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%d,"
                     "\"entries\":[",
                     id, (unsigned long long)s_chain_trace_seq, total);

    int start_idx = (int)(s_chain_trace_seq - (uint64_t)count);
    for (int i = 0; i < count; i++) {
        int idx = (start_idx + i) % CHAIN_TRACE_CAP;
        ChainTraceEntry *e = &s_chain_trace[idx];
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"seq\":%llu,\"prev\":\"0x%08X\","
                      "\"v0\":\"0x%08X\",\"counter\":%u,\"flag_7520\":%u,"
                      "\"mc_seq\":%u}",
                      i == 0 ? "" : ",",
                      (unsigned long long)e->seq, e->prev_target,
                      e->v0, e->counter_7514, e->flag_7520, e->mc_byte_seq);
        if (n >= (int)sizeof(buf) - 256) break;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    send_fmt("%s", buf);
}

/* ---- Dirty-RAM interpreter counters ---- */
#include "dirty_ram_interp.h"
static void handle_dirty_ram_stats(int id, const char *json)
{
    extern uint64_t g_dirty_ram_blocks_run;
    extern uint64_t g_dirty_ram_insns_run;
    extern uint64_t g_dirty_ram_aborts;
    extern uint32_t dirty_ram_get_bitmap(void);
    (void)json;

    char buf[16 * 1024];
    int n = snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"ok\":true,\"blocks_run\":%llu,"
             "\"insns_run\":%llu,\"aborts\":%llu,"
             "\"dirty_bitmap\":\"0x%08X\",\"per_pc\":[",
             id,
             (unsigned long long)g_dirty_ram_blocks_run,
             (unsigned long long)g_dirty_ram_insns_run,
             (unsigned long long)g_dirty_ram_aborts,
             (unsigned)dirty_ram_get_bitmap());

    int first = 1;
    for (int i = 0; i < DIRTY_RAM_PC_TABLE_SIZE; i++) {
        DirtyRamPcEntry *e = &g_dirty_ram_pc_table[i];
        if (e->pc == 0 || e->hits == 0) continue;
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"pc\":\"0x%08X\",\"hits\":%llu,\"insns\":%llu}",
                      first ? "" : ",",
                      (unsigned)e->pc,
                      (unsigned long long)e->hits,
                      (unsigned long long)e->insns);
        first = 0;
        if (n >= (int)sizeof(buf) - 128) break;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}\n");
    send_fmt("%s", buf);
}

/* ---- SIO write PC tracer dump ----
 * Returns the most recent N entries from the SIO PC tracer ring.
 * Optional addr_lo/addr_hi (hex-string) filter restricts to a single
 * MMIO register range — typical use: filter for SIO_DATA (0x1F801040)
 * or SIO_CTRL (0x1F80104A) writes only. */
static void handle_sio_pc_trace(int id, const char *json)
{
    int count = json_get_int(json, "count", 200);
    char addr_lo_buf[32], addr_hi_buf[32];
    uint32_t addr_lo = 0, addr_hi = 0;
    if (json_get_str(json, "addr_lo", addr_lo_buf, sizeof(addr_lo_buf)))
        addr_lo = hex_to_u32(addr_lo_buf);
    if (json_get_str(json, "addr_hi", addr_hi_buf, sizeof(addr_hi_buf)))
        addr_hi = hex_to_u32(addr_hi_buf);
    int filter = (addr_hi > addr_lo);

    int total = (int)(s_sio_pc_trace_seq < SIO_PC_TRACE_CAP
                      ? s_sio_pc_trace_seq : SIO_PC_TRACE_CAP);
    if (count > total) count = total;
    if (count < 0) count = 0;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%d,"
             "\"entries\":[",
             id, (unsigned long long)s_sio_pc_trace_seq, total);

    int start_idx = (int)(s_sio_pc_trace_seq - (uint64_t)count);
    int emitted = 0;
    for (int i = 0; i < count; i++) {
        int idx = (start_idx + i) % SIO_PC_TRACE_CAP;
        SioPcTraceEntry *e = &s_sio_pc_trace[idx];
        if (filter && (e->addr < addr_lo || e->addr >= addr_hi)) continue;
        send_fmt("%s{\"seq\":%llu,\"pc\":\"0x%08X\","
                 "\"func\":\"0x%08X\",\"addr\":\"0x%08X\","
                 "\"value\":\"0x%08X\",\"byte_seq\":%u,\"width\":%u}",
                 emitted == 0 ? "" : ",",
                 (unsigned long long)e->seq, e->pc, e->func,
                 e->addr, e->value, e->byte_seq, e->width);
        emitted++;
    }
    send_fmt("]}\n");
}

/* ---- MMIO write trace (separate ring buffer) ----
 * Records every write to 0x1F801xxx MMIO registers. Unconditional (no filtering).
 * 64K entries, heap-allocated in debug_server_init(). */
#define MMIO_TRACE_CAP (1 << 16)  /* 64K entries = 2 MB */
typedef struct {
    uint64_t seq;
    uint32_t addr;       /* 0x1F801xxx */
    uint32_t val;        /* value written */
    uint32_t func_addr;  /* dispatch target */
    uint32_t ra;         /* $ra */
    uint32_t frame;      /* VBlank frame */
    uint8_t  width;      /* 1, 2, or 4 */
    uint8_t  pad[3];
} MmioTraceEntry;
static MmioTraceEntry *s_mmio_trace = NULL;
static uint64_t s_mmio_trace_seq  = 0;
static uint32_t s_mmio_trace_head = 0;

/* ---- Platform helpers ---- */
static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* ---- JSON helpers ---- */

static const char *json_get_str(const char *json, const char *key,
                                 char *out, int out_sz)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
    {
        int i = 0;
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
}

static int json_get_int(const char *json, const char *key, int def)
{
    char buf[64];
    if (!json_get_str(json, key, buf, sizeof(buf))) return def;
    return atoi(buf);
}

static uint32_t hex_to_u32(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (uint32_t)strtoul(s, NULL, 16);
}

/* ---- Send helpers ---- */

static void send_all_blocking(sock_t sock, const char *data, size_t len)
{
#ifdef _WIN32
    u_long mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
#endif
    /* send() takes int on Win32; chunk so we can transmit > 2 GB payloads. */
    size_t sent = 0;
    while (sent < len) {
        size_t want = len - sent;
        if (want > (1u << 30)) want = (1u << 30);
        int n = send(sock, data + sent, (int)want, 0);
        if (n > 0) { sent += (size_t)n; continue; }
        break;
    }
#ifdef _WIN32
    mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    fcntl(sock, F_SETFL, flags);
#endif
}

void debug_server_send_line(const char *json)
{
    if (s_client == SOCK_INVALID) return;
    size_t len = strlen(json);
    send_all_blocking(s_client, json, len);
    send_all_blocking(s_client, "\n", 1);
}

void debug_server_send_fmt(const char *fmt, ...)
{
    char buf[65536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    debug_server_send_line(buf);
}

#define send_line  debug_server_send_line
#define send_fmt   debug_server_send_fmt

static void send_ok(int id)
{
    send_fmt("{\"id\":%d,\"ok\":true}", id);
}

static void send_err(int id, const char *msg)
{
    send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"%s\"}", id, msg);
}

/* ---- Command handlers ---- */

static void handle_ping(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu}",
             id, (unsigned long long)s_frame_count);
}

static void handle_frame(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu}",
             id, (unsigned long long)s_frame_count);
}

static void handle_get_registers(int id, const char *json)
{
    (void)json;
    if (!s_cpu) { send_err(id, "no cpu"); return; }

    char *buf = (char *)malloc(4096);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 4096,
        "{\"id\":%d,\"ok\":true,\"frame\":%llu,"
        "\"gpr\":[",
        id, (unsigned long long)s_frame_count);

    for (int i = 0; i < 32; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, 4096 - pos, "\"0x%08X\"", s_cpu->gpr[i]);
    }

    pos += snprintf(buf + pos, 4096 - pos,
        "],\"hi\":\"0x%08X\",\"lo\":\"0x%08X\","
        "\"cop0_sr\":\"0x%08X\",\"cop0_cause\":\"0x%08X\",\"cop0_epc\":\"0x%08X\","
        "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
        "\"pc\":\"0x%08X\"}",
        s_cpu->hi, s_cpu->lo,
        s_cpu->cop0[12], s_cpu->cop0[13], s_cpu->cop0[14],
        i_stat, i_mask,
        s_cpu->pc);

    send_line(buf);
    free(buf);
}

static void handle_read_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    /* Effectively the entire 2 MB RAM in one shot.  Response uses a heap-
     * sized envelope so we don't truncate. */
    if (len > 0x200000) len = 0x200000;

    /* Heap buffer for hex chars + JSON envelope.  Each byte = 2 hex chars. */
    size_t env = 256;
    size_t total = (size_t)len * 2 + env;
    char *out = (char *)malloc(total);
    if (!out) { send_err(id, "alloc failed"); return; }
    int hdr = snprintf(out, env,
                       "{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"",
                       id, addr, len);
    char *hex = out + hdr;
    for (int i = 0; i < len; i++)
        snprintf(hex + (size_t)i * 2, 3, "%02x", psx_read_byte(addr + (uint32_t)i));
    char *tail = hex + (size_t)len * 2;
    memcpy(tail, "\"}", 3);
    debug_server_send_line(out);
    free(out);
}

static void handle_dump_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 256);
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;

    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > 256) chunk = 256;
        char hex[513];
        for (int i = 0; i < chunk; i++)
            snprintf(hex + i * 2, 3, "%02x", psx_read_byte(addr + offset + i));
        send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"offset\":%d,\"len\":%d,\"hex\":\"%s\"}",
                 id, addr + offset, offset, chunk, hex);
        offset += chunk;
    }
}

static void handle_write_ram(int id, const char *json)
{
    char addr_str[32], val_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    if (!json_get_str(json, "val", val_str, sizeof(val_str))) {
        send_err(id, "missing val"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    uint8_t val = (uint8_t)hex_to_u32(val_str);
    psx_write_byte(addr, val);
    send_ok(id);
}

static void handle_gpu_state(int id, const char *json)
{
    (void)json;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    uint32_t gpustat = gpu_read_gpustat();

    GpuDrawArea da;
    gpu_get_draw_area(&da);
    uint64_t nop, fill, draw, env, copy;
    gpu_get_gp0_stats(&nop, &fill, &draw, &env, &copy);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"display_x\":%d,\"display_y\":%d,"
             "\"width\":%d,\"height\":%d,"
             "\"disabled\":%d,"
             "\"gpustat\":\"0x%08X\","
             "\"gp0_writes\":%llu,"
             "\"gp0_nop\":%llu,\"gp0_fill\":%llu,\"gp0_draw\":%llu,\"gp0_env\":%llu,\"gp0_copy\":%llu,"
             "\"draw_area\":[%u,%u,%u,%u],"
             "\"draw_offset\":[%d,%d]}",
             id, di.display_x, di.display_y,
             di.width, di.height,
             di.disabled,
             gpustat,
             (unsigned long long)gpu_get_gp0_count(),
             (unsigned long long)nop, (unsigned long long)fill,
             (unsigned long long)draw, (unsigned long long)env,
             (unsigned long long)copy,
             da.left, da.top, da.right, da.bottom,
             da.offset_x, da.offset_y);
}

static void handle_irq_state(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
             "\"pending\":\"0x%08X\","
             "\"cop0_sr\":\"0x%08X\","
             "\"IEc\":%d,\"IM2\":%d,\"BEV\":%d,"
             "\"dpcr\":\"0x%08X\",\"dicr\":\"0x%08X\"}",
             id, i_stat, i_mask, i_stat & i_mask,
             s_cpu ? s_cpu->cop0[12] : 0,
             s_cpu ? (s_cpu->cop0[12] & 1) : 0,
             s_cpu ? ((s_cpu->cop0[12] >> 10) & 1) : 0,
             s_cpu ? ((s_cpu->cop0[12] >> 22) & 1) : 0,
             dma_get_dpcr(), dma_get_dicr());
}

/* GPU opcode counter — defined in gpu.c */
extern uint32_t gpu_get_opcode_count(uint8_t op);

extern int gpu_get_a0_count(void);
extern int gpu_get_a0_history(int index, int *x, int *y, int *w, int *h,
                              uint32_t *fw0, uint32_t *fw1, int *wcount);
extern int gpu_get_a0_extra(int index, uint32_t *func, uint32_t *sp, uint32_t *ra,
                            uint32_t *s1, uint32_t *stack10);

static void handle_a0_history(int id, const char *json)
{
    (void)json;
    int count = gpu_get_a0_count();
    /* Use dynamic allocation for large output */
    int bufsz = 65536;
    char *buf = (char*)malloc(bufsz);
    if (!buf) { send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"OOM\"}", id); return; }
    int pos = snprintf(buf, bufsz, "{\"id\":%d,\"ok\":true,\"count\":%d,\"uploads\":[", id, count);
    for (int i = 0; i < count && pos < bufsz - 500; i++) {
        int x, y, w, h, wcount;
        uint32_t fw0, fw1, func, sp, ra, s1, stk[10];
        gpu_get_a0_history(i, &x, &y, &w, &h, &fw0, &fw1, &wcount);
        gpu_get_a0_extra(i, &func, &sp, &ra, &s1, stk);
        pos += snprintf(buf + pos, bufsz - pos,
            "%s{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
            "\"fw0\":\"0x%08X\",\"fw1\":\"0x%08X\",\"words\":%d,"
            "\"func\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"s1\":\"0x%08X\","
            "\"stk\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\"]}",
            i ? "," : "", i, x, y, w, h, fw0, fw1, wcount,
            func, sp, ra, s1,
            stk[0], stk[1], stk[2], stk[3], stk[4], stk[5], stk[6], stk[7],
            stk[8], stk[9]);
    }
    pos += snprintf(buf + pos, bufsz - pos, "]}");
    send_fmt("%s", buf);
    free(buf);
}

extern int gpu_get_c0_count(void);
extern int gpu_get_c0_history(int index, int *x, int *y, int *w, int *h,
                              uint32_t *func, uint32_t *sp, uint32_t *s1,
                              uint32_t *fw0, uint32_t *fw1, int *rcount);

static void handle_c0_history(int id, const char *json)
{
    (void)json;
    int count = gpu_get_c0_count();
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"count\":%d,\"reads\":[", id, count);
    for (int i = 0; i < count && pos < (int)sizeof(buf) - 300; i++) {
        int x, y, w, h, rcount;
        uint32_t func, sp, s1, fw0, fw1;
        gpu_get_c0_history(i, &x, &y, &w, &h, &func, &sp, &s1, &fw0, &fw1, &rcount);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
            "\"func\":\"0x%08X\",\"sp\":\"0x%08X\",\"s1\":\"0x%08X\","
            "\"fw0\":\"0x%08X\",\"fw1\":\"0x%08X\",\"reads\":%d}",
            i ? "," : "", i, x, y, w, h, func, sp, s1, fw0, fw1, rcount);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

static void handle_gpu_opcodes(int id, const char *json)
{
    (void)json;
    /* Report non-zero GP0 opcode counts */
    char buf[4096];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"opcodes\":{", id);
    int first = 1;
    for (int i = 0; i < 256; i++) {
        uint32_t cnt = gpu_get_opcode_count((uint8_t)i);
        if (cnt > 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%02X\":%u",
                           first ? "" : ",", i, cnt);
            first = 0;
        }
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
    send_fmt("%s", buf);
}

static void handle_capture_quads(int id, const char *json)
{
    (void)json;
    gpu_arm_shaded_quad_capture();
    send_ok(id);
}

static void handle_get_quads(int id, const char *json)
{
    (void)json;
    const GpuSqCapEntry *entries;
    int count = gpu_get_shaded_quad_capture(&entries);
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"count\":%d,\"quads\":[", id, count);
    for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++) {
        const GpuSqCapEntry *e = &entries[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"v\":[%d,%d,%d,%d,%d,%d,%d,%d],\"c\":[\"0x%06X\",\"0x%06X\",\"0x%06X\",\"0x%06X\"]}",
            i ? "," : "",
            e->vx[0], e->vy[0], e->vx[1], e->vy[1],
            e->vx[2], e->vy[2], e->vx[3], e->vy[3],
            e->color[0], e->color[1], e->color[2], e->color[3]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

extern uint64_t gte_get_exec_count(void);

static void handle_gte_state(int id, const char *json)
{
    (void)json;
    if (!s_cpu) { send_err(id, "no cpu"); return; }
    char buf[2048];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"gte_exec\":%llu,\"gte_ctrl\":[",
                       id, (unsigned long long)gte_get_exec_count());
    for (int i = 0; i < 32; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%08X\"",
                       i ? "," : "", s_cpu->gte_ctrl[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"gte_data\":[");
    for (int i = 0; i < 32; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%08X\"",
                       i ? "," : "", s_cpu->gte_data[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

static void handle_sio_state(int id, const char *json)
{
    (void)json;
    extern int sio_get_mc_probe_count(void);
    extern int sio_get_mc_ack_count(void);
    extern int sio_get_mc_cmd_count(void);
    extern int sio_get_mc_read_count(void);
    extern int sio_get_mc_read_done(void);
    extern uint32_t sio_get_mc_last_caller(void);
    extern int sio_get_mc_abort_count(void);
    extern int sio_get_mc_abort_state(void);
    extern uint16_t sio_get_mc_abort_ctrl(void);
    extern int sio_get_mc_max_state(void);
    extern int sio_get_tx_writes(void);
    extern int sio_get_tx_gated(void);
    extern uint16_t sio_get_last_ctrl_on_tx(void);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"sio_stat\":\"0x%04X\","
             "\"sio_ctrl\":\"0x%04X\","
             "\"sio_rx\":\"0x%02X\","
             "\"pad_buttons\":\"0x%04X\","
             "\"mc_probes\":%d,"
             "\"mc_acks\":%d,"
             "\"mc_cmds\":%d,"
             "\"mc_reads\":%d,"
             "\"mc_read_done\":%d,"
             "\"mc_last_caller\":\"0x%08X\","
             "\"mc_aborts\":%d,"
             "\"mc_abort_state\":%d,"
             "\"mc_abort_ctrl\":\"0x%04X\","
             "\"mc_max_state\":%d,"
             "\"tx_writes\":%d,"
             "\"tx_gated\":%d,"
             "\"last_ctrl_on_tx\":\"0x%04X\"}",
             id,
             (uint16_t)sio_read(0x1F801044),
             (uint16_t)sio_read(0x1F80104A),
             (uint8_t)sio_read(0x1F801040),
             sio_get_pad_buttons(),
             sio_get_mc_probe_count(),
             sio_get_mc_ack_count(),
             sio_get_mc_cmd_count(),
             sio_get_mc_read_count(),
             sio_get_mc_read_done(),
             sio_get_mc_last_caller(),
             sio_get_mc_abort_count(),
             sio_get_mc_abort_state(),
             sio_get_mc_abort_ctrl(),
             sio_get_mc_max_state(),
             sio_get_tx_writes(),
             sio_get_tx_gated(),
             sio_get_last_ctrl_on_tx());
}

/* ---- Memory card disk-load status (per-slot) ---- */
static void handle_mc_status(int id, const char *json)
{
    (void)json;
    const char *p0 = "", *p1 = "";
    uint8_t m0[2] = {0,0}, m1[2] = {0,0};
    int pres0 = 0, pres1 = 0, dirty0 = 0, dirty1 = 0;
    memcard_debug_info(0, &p0, m0, &pres0, &dirty0);
    memcard_debug_info(1, &p1, m1, &pres1, &dirty1);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"slot0\":{\"present\":%s,\"dirty\":%s,\"path\":\"%s\","
             "\"magic\":\"%c%c\",\"magic_hex\":\"%02X%02X\"},"
             "\"slot1\":{\"present\":%s,\"dirty\":%s,\"path\":\"%s\","
             "\"magic\":\"%c%c\",\"magic_hex\":\"%02X%02X\"}}",
             id,
             pres0 ? "true" : "false", dirty0 ? "true" : "false", p0,
             (m0[0] >= 0x20 && m0[0] < 0x7F) ? m0[0] : '?',
             (m0[1] >= 0x20 && m0[1] < 0x7F) ? m0[1] : '?',
             m0[0], m0[1],
             pres1 ? "true" : "false", dirty1 ? "true" : "false", p1,
             (m1[0] >= 0x20 && m1[0] < 0x7F) ? m1[0] : '?',
             (m1[1] >= 0x20 && m1[1] < 0x7F) ? m1[1] : '?',
             m1[0], m1[1]);
}

/* ---- SIO IRQ-arm audit -----------------------------------------------
 * Reports counts of TX writes that reached the IRQ-arm decision in
 * sio_write SIO_TX_DATA, partitioned by active_device. Tells us at the
 * arm-time gate whether ACK + ACK_IRQ_EN were set, or which side blocked
 * the arm. Steps 3-4 of the IRQ-chain audit. */
extern void sio_get_card_arm_audit(uint32_t out[3][7]);
extern int  sio_get_card_arm_countdown_after(void);
extern void sio_get_burst_stats(uint64_t out[10]);
extern void sio_get_pace_state(uint64_t out[16]);
extern volatile int g_sio_timing_active;

/* Phase 1.0c-v2 telemetry: cycle-paced SIO state snapshot. Read-only.
 * In 1.0c-v2 the TX path is still synchronous, g_sio_timing_active
 * stays 0, and all dynamic shifter/ack fields stay zero. */
static void handle_pace_state(int id, const char *json)
{
    (void)json;
    uint64_t s[16];
    sio_get_pace_state(s);
    const char *model = s[0] ? "cycle_paced" : "access_legacy";
    const char *owner = s[7] == 0 ? "none" : s[7] == 1 ? "card"
                      : s[7] == 2 ? "pad"  : "unknown";
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"sio_model\":\"%s\","
             "\"sio_quantum_cycles\":%llu,"
             "\"timing_active\":%d,"
             "\"shift_active\":%llu,"
             "\"shift_remaining\":%llu,"
             "\"tx_buffered\":%llu,"
             "\"ack_active\":%llu,"
             "\"ack_remaining\":%llu,"
             "\"bus_owner\":\"%s\","
             "\"bus_byte_index\":%llu,"
             "\"tx_writes_buffered\":%llu,"
             "\"tx_writes_dropped_busy\":%llu,"
             "\"tx_writes_dropped_cross_device\":%llu,"
             "\"tx_buffer_promoted\":%llu,"
             "\"tx_buffer_promoted_during_card\":%llu,"
             "\"pad_byte_processed_in_card_data\":%llu,"
             "\"cross_device_pad_during_card\":%llu}\n",
             id, model,
             (unsigned long long)s[1],
             g_sio_timing_active,
             (unsigned long long)s[2], (unsigned long long)s[3],
             (unsigned long long)s[4], (unsigned long long)s[5],
             (unsigned long long)s[6],
             owner,
             (unsigned long long)s[8], (unsigned long long)s[9],
             (unsigned long long)s[10], (unsigned long long)s[11],
             (unsigned long long)s[12], (unsigned long long)s[13],
             (unsigned long long)s[14], (unsigned long long)s[15]);
}

static void handle_sio_burst_stats(int id, const char *json)
{
    (void)json;
    uint64_t s[10];
    sio_get_burst_stats(s);
    const char *reason_str =
        s[8] == 1 ? "idle" :
        s[8] == 2 ? "mode_clear" :
        s[8] == 3 ? "capped" : "n/a";
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"calls\":%llu,\"iters_total\":%llu,\"iter_max\":%llu,"
             "\"break_idle\":%llu,\"break_mode_clear\":%llu,\"break_capped\":%llu,"
             "\"fires_in_burst\":%llu,\"last_iters\":%llu,"
             "\"last_break_reason\":\"%s\"}\n",
             id,
             (unsigned long long)s[0], (unsigned long long)s[1], (unsigned long long)s[2],
             (unsigned long long)s[3], (unsigned long long)s[4], (unsigned long long)s[5],
             (unsigned long long)s[6], (unsigned long long)s[7],
             reason_str);
}

static void handle_sio_arm_audit(int id, const char *json)
{
    (void)json;
    uint32_t a[3][7];
    sio_get_card_arm_audit(a);
    int cd_after = sio_get_card_arm_countdown_after();
    const char *names[3] = { "card", "pad", "none" };
    send_fmt("{\"id\":%d,\"ok\":true,\"countdown_after_last_card\":%d,\"by_device\":{",
             id, cd_after);
    for (int i = 0; i < 3; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("\"%s\":{\"tx_total\":%u,\"armed\":%u,\"no_ack\":%u,\"no_ackirqen\":%u,"
                 "\"ctrl_last\":\"0x%04X\",\"stat_pre_last\":\"0x%04X\",\"stat_post_last\":\"0x%04X\"}",
                 names[i], a[i][0], a[i][1], a[i][2], a[i][3], a[i][4], a[i][5], a[i][6]);
    }
    send_fmt("}}\n");
}

/* ---- Memory card raw buffer dump (in-memory cards[].data) ----
 * Used by the audit harness to verify that what the runtime loaded matches
 * the on-disk file byte-for-byte. Returns hex string, chunked. */
static void handle_card_buffer_dump(int id, const char *json)
{
    int slot   = json_get_int(json, "slot",   0);
    int offset = json_get_int(json, "offset", 0);
    int len    = json_get_int(json, "len",    256);
    if (slot < 0 || slot > 1)            { send_err(id, "bad slot"); return; }
    if (offset < 0 || offset > 0x20000)  { send_err(id, "bad offset"); return; }
    if (len < 1)                         { send_err(id, "bad len"); return; }
    if (len > 0x20000)                   len = 0x20000;
    if (offset + len > 0x20000)          len = 0x20000 - offset;

    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { send_err(id, "alloc failed"); return; }
    int got = memcard_debug_read_buffer(slot, (uint32_t)offset, (uint32_t)len, buf);
    if (got <= 0) { free(buf); send_err(id, "slot empty or read failed"); return; }

    /* hex envelope: 2 chars/byte + ~256 envelope */
    size_t env = 256;
    size_t total = (size_t)got * 2 + env;
    char *out = (char *)malloc(total);
    if (!out) { free(buf); send_err(id, "alloc failed"); return; }
    int hdr = snprintf(out, env,
                       "{\"id\":%d,\"ok\":true,\"slot\":%d,\"offset\":%d,\"len\":%d,\"hex\":\"",
                       id, slot, offset, got);
    char *hex = out + hdr;
    for (int i = 0; i < got; i++)
        snprintf(hex + (size_t)i * 2, 3, "%02x", buf[i]);
    char *tail = hex + (size_t)got * 2;
    memcpy(tail, "\"}", 3);
    debug_server_send_line(out);
    free(out);
    free(buf);
}

/* ---- I_MASK bit 7 trace (card protocol flow) ---- */
typedef struct {
    uint32_t old_mask;
    uint32_t new_mask;
    uint32_t caller;
    uint32_t store_pc;
    uint8_t  width;
    uint8_t  bit7_set;
    uint8_t  bit7_clear;
    uint8_t  in_exc;
} ImaskTraceEntry;
extern int memory_get_imask_bit7_set_count(void);
extern int memory_get_imask_bit7_clear_count(void);
extern const ImaskTraceEntry *memory_get_imask_trace(int *idx_out, int *count_out);

static void handle_imask_trace(int id, const char *json)
{
    int count = json_get_int(json, "count", 64);
    int only_b7c = json_get_int(json, "only_b7c", 0);
    int idx, total;
    const ImaskTraceEntry *buf = memory_get_imask_trace(&idx, &total);
    int cap = 4096; /* IMASK_TRACE_CAP */
    int avail = total < cap ? total : cap;
    if (count > avail) count = avail;
    if (count < 1) count = 1;

    int start = (idx - count + cap) % cap;

    send_fmt("{\"id\":%d,\"ok\":true,\"bit7_sets\":%d,\"bit7_clears\":%d,"
             "\"total\":%d,\"count\":%d,\"entries\":[",
             id, memory_get_imask_bit7_set_count(),
             memory_get_imask_bit7_clear_count(), total, count);

    int first = 1;
    for (int i = 0; i < count; i++) {
        int ii = (start + i) % cap;
        const ImaskTraceEntry *e = &buf[ii];
        if (only_b7c && !e->bit7_clear) continue;
        if (!first) send_fmt(",");
        first = 0;
        send_fmt("{\"old\":\"0x%03X\",\"new\":\"0x%03X\","
                 "\"func\":\"0x%08X\",\"pc\":\"0x%08X\",\"w\":%d,"
                 "\"b7s\":%d,\"b7c\":%d,\"exc\":%d}",
                 e->old_mask, e->new_mask,
                 (unsigned)e->caller, (unsigned)e->store_pc, e->width,
                 e->bit7_set, e->bit7_clear, e->in_exc);
    }
    send_fmt("]}\n");
}

static void handle_sio_trace(int id, const char *json)
{
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > SIO_TRACE_CAP) count = SIO_TRACE_CAP;

    const SioTraceEntry *buf;
    int write_idx;
    uint32_t total_seq = sio_get_trace(&buf, &write_idx);

    /* How many entries are actually available? */
    int avail = (int)(total_seq < (uint32_t)SIO_TRACE_CAP ? total_seq : SIO_TRACE_CAP);
    if (count > avail) count = avail;

    /* Start reading from (write_idx - count) wrapped */
    int start = (write_idx - count + SIO_TRACE_CAP) % SIO_TRACE_CAP;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"count\":%d,\"entries\":[",
             id, (unsigned)total_seq, count);

    for (int i = 0; i < count; i++) {
        int idx = (start + i) % SIO_TRACE_CAP;
        const SioTraceEntry *e = &buf[idx];
        if (i > 0) send_fmt(",");
        send_fmt("{\"seq\":%u,\"tx\":\"0x%02X\",\"rx\":\"0x%02X\","
                 "\"mc_pre\":%d,\"mc_post\":%d,"
                 "\"dev_pre\":%d,\"dev_post\":%d,"
                 "\"ctrl\":\"0x%04X\",\"func\":\"0x%08X\","
                 "\"abort\":%d,\"irq_cd\":%d,\"in_exc\":%d,\"ctr\":%d,"
                 "\"sr\":\"0x%08X\","
                 "\"slot0\":%d,\"slot1\":%d}",
                 (unsigned)e->seq, e->tx, e->rx,
                 e->mc_state_pre, e->mc_state_post,
                 e->dev_pre, e->dev_post,
                 e->ctrl, (unsigned)e->func_addr,
                 e->was_abort, e->irq_countdown, e->in_exception,
                 e->counter_7514, (unsigned)e->cop0_sr,
                 e->slot0_state, e->slot1_state);
    }

    send_fmt("]}\n");
}

/* ---- Card transaction ring dump ----
 *
 * Returns the most recent N closed transactions plus the live (open) txn
 * if there is one. Optional slot filter restricts to a single card slot.
 * Each entry includes the full TX/RX byte stream for that transaction
 * (truncated to SIO_TXN_MAX_BYTES per entry). */
static const char *txn_end_reason_str(int reason) {
    switch (reason) {
    case SIO_TXN_END_OPEN:           return "open";
    case SIO_TXN_END_SUCCESS:        return "success";
    case SIO_TXN_END_ABORT_RESELECT: return "abort_reselect";
    case SIO_TXN_END_ABORT_RESET:    return "abort_reset";
    case SIO_TXN_END_ABORT_SLOT:     return "abort_slot";
    case SIO_TXN_END_ABORT_BAD_CMD:  return "abort_bad_cmd";
    case SIO_TXN_END_ABORT_OTHER:    return "abort_other";
    default:                         return "unknown";
    }
}

static void emit_card_txn_json(const SioTxnEntry *e, int is_live) {
    int n_bytes = e->byte_count;
    if (n_bytes > SIO_TXN_MAX_BYTES) n_bytes = SIO_TXN_MAX_BYTES;
    send_fmt("{\"txn_seq\":%u,\"slot\":%u,\"cmd\":\"0x%02X\","
             "\"sector\":\"0x%04X\",\"bytes\":%u,\"acks\":%u,"
             "\"start_byte_seq\":%u,\"end_byte_seq\":%u,"
             "\"start_func\":\"0x%08X\",\"end_func\":\"0x%08X\","
             "\"end_reason\":\"%s\",\"terminal_state\":%u,\"live\":%s,\"tx\":[",
             (unsigned)e->txn_seq, e->slot, e->cmd, e->sector,
             (unsigned)e->byte_count, (unsigned)e->ack_count,
             (unsigned)e->start_byte_seq, (unsigned)e->end_byte_seq,
             (unsigned)e->start_func, (unsigned)e->end_func,
             txn_end_reason_str(e->end_reason), e->terminal_state,
             is_live ? "true" : "false");
    for (int i = 0; i < n_bytes; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("\"0x%02X\"", e->tx[i]);
    }
    send_fmt("],\"rx\":[");
    for (int i = 0; i < n_bytes; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("\"0x%02X\"", e->rx[i]);
    }
    send_fmt("]}");
}

/* ---- EvCB ring TCP cmds ----
 *
 * evcb_snapshot       — one-shot read of current EvCB table from RAM
 * evcb_walk_dump      — recent always-on snapshots (paired entry + exit per
 *                       DeliverEvent call)
 * evcb_walk_stats     — counts + ring usage */
static void emit_evcb_snapshot_json(const EvCBSnapshot *e) {
    send_fmt("{\"seq\":%llu,\"fn_entry_seq\":%llu,\"tag\":\"%s\","
             "\"evcb_base\":\"0x%08X\",\"evcb_total_bytes\":%u,"
             "\"entry_count\":%u,\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
             "\"v0\":\"0x%08X\",\"counter_7514\":%u,\"flag_755A\":\"0x%08X\","
             "\"flag_75C0\":\"0x%08X\",\"frame\":%u,\"entries\":[",
             (unsigned long long)e->seq, (unsigned long long)e->fn_entry_seq,
             e->tag == EVCB_TAG_ENTRY ? "entry" : "exit",
             (unsigned)e->evcb_base, (unsigned)e->evcb_total_bytes,
             (unsigned)e->entry_count, (unsigned)e->a0, (unsigned)e->a1,
             (unsigned)e->v0, (unsigned)e->counter_7514,
             (unsigned)e->flag_755A, (unsigned)e->flag_75C0,
             (unsigned)e->frame);
    for (uint32_t i = 0; i < e->entry_count; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("{\"i\":%u,\"class\":\"0x%08X\",\"status\":\"0x%08X\","
                 "\"spec\":\"0x%08X\",\"mode\":\"0x%08X\","
                 "\"fhandler\":\"0x%08X\"}",
                 i, (unsigned)e->entries[i].cls,
                 (unsigned)e->entries[i].status,
                 (unsigned)e->entries[i].spec,
                 (unsigned)e->entries[i].mode,
                 (unsigned)e->entries[i].fhandler);
    }
    send_fmt("]}");
}

static void handle_evcb_snapshot(int id, const char *json)
{
    (void)json;
    /* Synthetic one-shot capture into a stack-local snapshot — does NOT
     * record into the ring (so manual probes don't drown the always-on
     * pairing). */
    EvCBSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    if (!debug_cpu_ptr) { send_err(id, "no cpu ptr"); return; }
    uint32_t base_ptr    = evcb_read_u32_ram(0x00000120);
    uint32_t total_bytes = evcb_read_u32_ram(0x00000124);
    snap.seq             = (uint64_t)-1; /* synthetic */
    snap.fn_entry_seq    = (uint64_t)-1;
    snap.tag             = EVCB_TAG_ENTRY;
    snap.evcb_base       = base_ptr;
    snap.evcb_total_bytes = total_bytes;
    snap.a0              = debug_cpu_ptr->gpr[4];
    snap.a1              = debug_cpu_ptr->gpr[5];
    snap.v0              = debug_cpu_ptr->gpr[2];
    snap.counter_7514    = evcb_read_u32_ram(0x00007514);
    snap.flag_755A       = (uint32_t)psx_read_byte(0x0000755A);
    snap.flag_75C0       = evcb_read_u32_ram(0x000075C0);
    snap.frame           = (uint32_t)s_frame_count;
    uint32_t base_ram    = base_ptr & 0x001FFFFFu;
    uint32_t n_entries   = (total_bytes / EVCB_ENTRY_SIZE);
    if (n_entries > EVCB_MAX_ENTRIES) n_entries = EVCB_MAX_ENTRIES;
    snap.entry_count     = n_entries;
    for (uint32_t i = 0; i < n_entries; i++) {
        uint32_t off = base_ram + i * EVCB_ENTRY_SIZE;
        if (off + EVCB_ENTRY_SIZE > 0x00200000u) break;
        snap.entries[i].cls      = evcb_read_u32_ram(off + 0);
        snap.entries[i].status   = evcb_read_u32_ram(off + 4);
        snap.entries[i].spec     = evcb_read_u32_ram(off + 8);
        snap.entries[i].mode     = evcb_read_u32_ram(off + 12);
        snap.entries[i].fhandler = evcb_read_u32_ram(off + 16);
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"snapshot\":", id);
    emit_evcb_snapshot_json(&snap);
    send_fmt("}\n");
}

static void handle_evcb_walk_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 8);
    if (count < 1) count = 1;
    if (count > EVCB_RING_CAP) count = EVCB_RING_CAP;

    if (!s_evcb_ring) { send_err(id, "evcb ring not allocated"); return; }

    int avail = (int)(s_evcb_ring_seq < (uint64_t)EVCB_RING_CAP
                      ? s_evcb_ring_seq : EVCB_RING_CAP);
    if (count > avail) count = avail;

    uint64_t start_seq = s_evcb_ring_seq - (uint64_t)count;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"entry_count\":%llu,"
             "\"exit_count\":%llu,\"shown\":%d,\"snapshots\":[",
             id, (unsigned long long)s_evcb_ring_seq,
             (unsigned long long)s_evcb_ring_entry_count,
             (unsigned long long)s_evcb_ring_exit_count, count);
    for (int i = 0; i < count; i++) {
        const EvCBSnapshot *e = &s_evcb_ring[(start_seq + (uint64_t)i) % EVCB_RING_CAP];
        if (i > 0) send_fmt(",");
        emit_evcb_snapshot_json(e);
    }
    send_fmt("]}\n");
}

static void handle_evcb_walk_stats(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"entry_count\":%llu,"
             "\"exit_count\":%llu,\"unwound_count\":%llu,"
             "\"pending_active\":%d,\"ring_cap\":%d,\"max_entries\":%d}\n",
             id, (unsigned long long)s_evcb_ring_seq,
             (unsigned long long)s_evcb_ring_entry_count,
             (unsigned long long)s_evcb_ring_exit_count,
             (unsigned long long)s_evcb_unwound_count,
             s_evcb_pending_active,
             EVCB_RING_CAP, EVCB_MAX_ENTRIES);
}

/* ---- SIO IRQ ring dump ----
 *
 * Returns the most recent N IRQ #7 fires with timing/source/state context.
 * Each entry shows: when it was scheduled (byte_seq) vs when it actually
 * fired, what the chain counter was, and whether mc_state was idle or in
 * the middle of a card protocol. */
static const char *sio_irq_src_str(int s) {
    switch (s) {
    case SIO_IRQ_SRC_CARD_ACK: return "card";
    case SIO_IRQ_SRC_PAD_ACK:  return "pad";
    default:                   return "unknown";
    }
}

static void handle_sio_irq_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 64);
    int src_filter = json_get_int(json, "src", -1); /* -1=all, else SioIrqSource value */
    if (count < 1) count = 1;
    if (count > SIO_IRQ_RING_CAP) count = SIO_IRQ_RING_CAP;

    const SioIrqEntry *buf;
    int write_idx;
    uint32_t total_seq = sio_get_irq_ring(&buf, &write_idx);

    int avail = (int)(total_seq < (uint32_t)SIO_IRQ_RING_CAP
                      ? total_seq : SIO_IRQ_RING_CAP);
    if (count > avail) count = avail;

    int start = (write_idx - count + SIO_IRQ_RING_CAP) % SIO_IRQ_RING_CAP;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"shown\":%d,"
             "\"src_filter\":%d,\"entries\":[",
             id, (unsigned)total_seq, count, src_filter);

    int emitted = 0;
    for (int i = 0; i < count; i++) {
        const SioIrqEntry *e = &buf[(start + i) % SIO_IRQ_RING_CAP];
        if (src_filter >= 0 && (int)e->source != src_filter) continue;
        if (emitted > 0) send_fmt(",");
        send_fmt("{\"seq\":%u,\"src\":\"%s\",\"slot\":%u,\"delay\":%u,"
                 "\"byte_seq\":%u,\"mc_state\":%u,\"active_device\":%u,"
                 "\"ctrl\":\"0x%04X\",\"func\":\"0x%08X\","
                 "\"counter_7514\":%u,"
                 "\"i_stat_before\":\"0x%08X\",\"i_stat_after\":\"0x%08X\"}",
                 (unsigned)e->seq, sio_irq_src_str(e->source),
                 e->slot, e->delay_applied,
                 (unsigned)e->byte_seq, (unsigned)e->mc_state,
                 (unsigned)e->active_device,
                 (unsigned)e->ctrl, (unsigned)e->func_addr,
                 (unsigned)e->counter_7514,
                 (unsigned)e->i_stat_before, (unsigned)e->i_stat_after);
        emitted++;
    }
    send_fmt("],\"emitted\":%d}\n", emitted);
}

static void handle_card_txn_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 16);
    int slot  = json_get_int(json, "slot", -1); /* -1 = all */
    if (count < 1) count = 1;
    if (count > SIO_TXN_CAP) count = SIO_TXN_CAP;

    const SioTxnEntry *buf;
    int write_idx, open_flag;
    uint32_t total_seq = sio_get_card_txns(&buf, &write_idx, &open_flag);

    int avail = (int)(total_seq < (uint32_t)SIO_TXN_CAP
                      ? total_seq : SIO_TXN_CAP);
    if (count > avail) count = avail;

    int start = (write_idx - count + SIO_TXN_CAP) % SIO_TXN_CAP;

    send_fmt("{\"id\":%d,\"ok\":true,\"total_closed\":%u,\"open\":%s,"
             "\"slot_filter\":%d,\"count\":%d,\"entries\":[",
             id, (unsigned)total_seq, open_flag ? "true" : "false",
             slot, count);

    int emitted = 0;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % SIO_TXN_CAP;
        const SioTxnEntry *e = &buf[idx];
        if (slot >= 0 && (int)e->slot != slot) continue;
        if (emitted > 0) send_fmt(",");
        emit_card_txn_json(e, 0);
        emitted++;
    }

    /* Also append the live (open) txn if present and matches the filter. */
    const SioTxnEntry *live = sio_get_card_txn_live();
    if (live && (slot < 0 || (int)live->slot == slot)) {
        if (emitted > 0) send_fmt(",");
        emit_card_txn_json(live, 1);
        emitted++;
    }

    send_fmt("],\"emitted\":%d}\n", emitted);
}

static void handle_watch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) {
            s_watchpoints[i].addr = addr;
            s_watchpoints[i].prev_val = psx_read_byte(addr);
            s_watchpoints[i].active = 1;
            send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"addr\":\"0x%08X\"}",
                     id, i, addr);
            return;
        }
    }
    send_err(id, "all watchpoint slots full (max 8)");
}

static void handle_unwatch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (s_watchpoints[i].active && s_watchpoints[i].addr == addr) {
            s_watchpoints[i].active = 0;
            send_ok(id);
            return;
        }
    }
    send_err(id, "watchpoint not found");
}

static void handle_set_input(int id, const char *json)
{
    char val_str[32];
    if (!json_get_str(json, "buttons", val_str, sizeof(val_str))) {
        send_err(id, "missing buttons"); return;
    }
    s_input_override = (int)hex_to_u32(val_str);
    s_input_frames = 0;
    send_ok(id);
}

static void handle_press(int id, const char *json)
{
    int buttons = json_get_int(json, "buttons", -1);
    int frames  = json_get_int(json, "frames", 2);
    if (buttons < 0) { send_err(id, "missing buttons"); return; }
    s_input_override = buttons;
    s_input_frames   = frames;
    send_ok(id);
}

/* Reports BOTH ours' and Beetle's current pad word so the operator can
 * empirically verify input dispatch symmetry. Note: ours' pad is what
 * sio_set_pad_state stored last; Beetle's is whatever the last frame
 * set s_joypad to. They should match when override is active. */
extern uint16_t sio_get_pad_buttons(void);
#if defined(ENABLE_BEETLE_PSX_ORACLE)
extern uint16_t beetle_get_pad(void);
#endif
static void handle_pad_status(int id, const char *json)
{
    (void)json;
    uint16_t ours = sio_get_pad_buttons();
#if defined(ENABLE_BEETLE_PSX_ORACLE)
    uint16_t beetle = beetle_get_pad();
#else
    uint16_t beetle = 0xFFFF;
#endif
    send_fmt("{\"id\":%d,\"ok\":true,\"ours\":\"0x%04X\",\"beetle\":\"0x%04X\","
             "\"override\":%d,\"override_frames\":%d,\"match\":%s}\n",
             id, ours, beetle, s_input_override, s_input_frames,
             (ours == beetle) ? "true" : "false");
}

static void handle_clear_input(int id, const char *json)
{
    (void)json;
    s_input_override = -1;
    s_input_frames   = 0;
    send_ok(id);
}

/* ---- display_source: toggle SDL window between our VRAM and Beetle's framebuffer ---- */
extern void display_source_set(int src);
extern int  display_source_get(void);
static void handle_display_source(int id, const char *json)
{
    char val[16] = {0};
    if (!json_get_str(json, "src", val, sizeof(val))) {
        /* No arg = report current */
        send_fmt("{\"id\":%d,\"ok\":true,\"src\":\"%s\"}",
                 id, display_source_get() ? "beetle" : "ours");
        return;
    }
    if (strcmp(val, "beetle") == 0)        display_source_set(1);
    else if (strcmp(val, "ours") == 0)     display_source_set(0);
    else { send_err(id, "src must be 'ours' or 'beetle'"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"src\":\"%s\"}",
             id, display_source_get() ? "beetle" : "ours");
}

static void handle_pause(int id, const char *json)
{
    (void)json;
    s_paused = 1;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":true,\"frame\":%llu}",
             id, (unsigned long long)s_frame_count);
}

static void handle_continue(int id, const char *json)
{
    (void)json;
    s_paused = 0;
    s_step_count = 0;
    s_run_to = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":false}", id);
}

static void handle_step(int id, const char *json)
{
    int n = json_get_int(json, "count", 1);
    if (n < 1) n = 1;
    s_step_count = n;
    s_paused = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"stepping\":%d}", id, n);
}

static void handle_run_to_frame(int id, const char *json)
{
    int target = json_get_int(json, "frame", 0);
    if (target <= (int)s_frame_count) {
        send_err(id, "target frame already passed"); return;
    }
    s_run_to = (uint32_t)target;
    s_paused = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"running_to\":%d}", id, target);
}

/* ---- Ring buffer queries ---- */

static void handle_history(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"oldest\":%llu,\"newest\":%llu}",
             id,
             (unsigned long long)s_history_count,
             (unsigned long long)oldest,
             (unsigned long long)(s_history_count > 0 ? s_history_count - 1 : 0));
}

static void handle_get_frame(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
        send_err(id, "frame not in buffer"); return;
    }

    uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
    const PSXFrameRecord *r = &s_frame_history[idx];
    if (r->frame_number != (uint32_t)f) {
        send_err(id, "frame record mismatch"); return;
    }

    char *buf = (char *)malloc(8192);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 8192,
        "{\"id\":%d,\"ok\":true,"
        "\"frame\":%u,\"verify_pass\":%d,\"diff_count\":%d,"
        "\"cop0_sr\":\"0x%08X\",\"cop0_cause\":\"0x%08X\",\"cop0_epc\":\"0x%08X\","
        "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
        "\"display\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"disabled\":%d},"
        "\"pad_buttons\":\"0x%04X\","
        "\"sio_stat\":\"0x%04X\",\"sio_ctrl\":\"0x%04X\","
        "\"dispatch_count\":%u,"
        "\"total_dispatches\":%llu,"
        "\"last_func\":\"%s\","
        "\"gpr\":[",
        id, r->frame_number, r->verify_pass, r->diff_count,
        r->cop0_sr, r->cop0_cause, r->cop0_epc,
        r->i_stat, r->i_mask,
        r->display_area_x, r->display_area_y, r->display_w, r->display_h,
        r->display_disabled,
        r->pad_buttons,
        r->sio_stat, r->sio_ctrl,
        r->dispatch_count,
        (unsigned long long)r->total_dispatches,
        r->last_func);

    for (int i = 0; i < 32; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, 8192 - pos, "\"0x%08X\"", r->gpr[i]);
    }

    pos += snprintf(buf + pos, 8192 - pos, "]}");
    send_line(buf);
    free(buf);
}

static void handle_frame_range(int id, const char *json)
{
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end", -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    char *buf = (char *)malloc(200 * 256 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"frames\":[", id);
    int first = 1;

    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;

        if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
            pos += snprintf(buf + pos, 128, "{\"frame\":%d,\"available\":false}", f);
            continue;
        }
        uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
        const PSXFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 128, "{\"frame\":%d,\"available\":false}", f);
            continue;
        }

        pos += snprintf(buf + pos, 256,
            "{\"frame\":%u,\"verify\":%d,"
            "\"sr\":\"0x%08X\",\"i_stat\":\"0x%08X\","
            "\"pad\":\"0x%04X\"}",
            r->frame_number, r->verify_pass,
            r->cop0_sr, r->i_stat,
            r->pad_buttons);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_frame_timeseries(int id, const char *json)
{
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end", -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    char *buf = (char *)malloc(200 * 320 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"ts\":[", id);
    int first = 1;

    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;

        if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
            pos += snprintf(buf + pos, 32, "null");
            continue;
        }
        uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
        const PSXFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 32, "null");
            continue;
        }

        pos += snprintf(buf + pos, 320,
            "{\"f\":%u,\"v\":%d,"
            "\"sr\":\"0x%08X\",\"ist\":\"0x%08X\",\"imk\":\"0x%08X\","
            "\"pad\":\"0x%04X\",\"dc\":%u}",
            r->frame_number, r->verify_pass,
            r->cop0_sr, r->i_stat, r->i_mask,
            r->pad_buttons, r->dispatch_count);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_first_failure(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    for (uint64_t f = oldest; f < s_history_count; f++) {
        uint32_t idx = (uint32_t)(f % FRAME_HISTORY_CAP);
        const PSXFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number == (uint32_t)f && r->verify_pass == 0) {
            send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u,\"diff_count\":%d}",
                     id, r->frame_number, r->diff_count);
            return;
        }
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":-1,\"message\":\"no failures found\"}", id);
}

static void handle_read_frame_ram(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }
    if (!s_frame_history) { send_err(id, "ring buffer not allocated"); return; }

    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 128) len = 128;

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
        send_err(id, "frame not in buffer"); return;
    }

    uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
    const PSXFrameRecord *r = &s_frame_history[idx];

    /* Find matching snapshot region */
    char hex[257];
    int found = 0;
    for (int i = 0; i < RAM_SNAPSHOT_REGIONS; i++) {
        if (r->snapshot_addr[i] == 0) continue;
        if (addr >= r->snapshot_addr[i] && addr + len <= r->snapshot_addr[i] + RAM_SNAPSHOT_SIZE) {
            uint32_t off = addr - r->snapshot_addr[i];
            for (int j = 0; j < len; j++)
                snprintf(hex + j * 2, 3, "%02x", r->snapshot_data[i][off + j]);
            found = 1;
            break;
        }
    }

    if (!found) {
        send_err(id, "address not in any snapshot region for this frame"); return;
    }

    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%d,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"%s\"}",
             id, f, addr, len, hex);
}

static void handle_set_snapshot(int id, const char *json)
{
    int slot = json_get_int(json, "slot", -1);
    if (slot < 0 || slot >= RAM_SNAPSHOT_REGIONS) {
        send_err(id, "invalid slot (0-3)"); return;
    }
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    s_snapshot_addrs[slot] = addr;
    s_snapshot_active[slot] = (addr != 0);
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"addr\":\"0x%08X\"}", id, slot, addr);
}

static void handle_get_snapshots(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"snapshots\":["
             "{\"slot\":0,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":1,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":2,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":3,\"addr\":\"0x%08X\",\"active\":%d}]}",
             id,
             s_snapshot_addrs[0], s_snapshot_active[0],
             s_snapshot_addrs[1], s_snapshot_active[1],
             s_snapshot_addrs[2], s_snapshot_active[2],
             s_snapshot_addrs[3], s_snapshot_active[3]);
}

static void handle_screenshot_file(int id, const char *json)
{
    (void)json;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.width == 0 || di.height == 0) {
        send_err(id, "display disabled"); return;
    }

    const uint16_t *vram = gpu_get_vram();
    uint32_t w = di.width;  if (w > 640) w = 640;
    uint32_t h = di.height; if (h > 512) h = 512;

    const char *path = "psx_screenshot.bmp";
    FILE *f = fopen(path, "wb");
    if (!f) { send_err(id, "cannot open file"); return; }

    /* BMP row stride: 3 bytes/pixel, padded to 4-byte boundary */
    uint32_t row_stride = (w * 3 + 3) & ~3u;
    uint32_t pixel_size = row_stride * h;
    uint32_t file_size = 14 + 40 + pixel_size;

    /* BITMAPFILEHEADER (14 bytes) */
    uint8_t bfh[14] = {0};
    bfh[0] = 'B'; bfh[1] = 'M';
    bfh[2] = file_size & 0xFF; bfh[3] = (file_size >> 8) & 0xFF;
    bfh[4] = (file_size >> 16) & 0xFF; bfh[5] = (file_size >> 24) & 0xFF;
    bfh[10] = 54; /* offset to pixel data */
    fwrite(bfh, 1, 14, f);

    /* BITMAPINFOHEADER (40 bytes) */
    uint8_t bih[40] = {0};
    bih[0] = 40; /* header size */
    bih[4] = w & 0xFF; bih[5] = (w >> 8) & 0xFF;
    /* Height negative = top-down */
    int32_t neg_h = -(int32_t)h;
    memcpy(bih + 8, &neg_h, 4);
    bih[12] = 1;  /* planes */
    bih[14] = 24; /* bits per pixel */
    fwrite(bih, 1, 40, f);

    /* Pixel data: top-down, BGR */
    uint8_t *row = (uint8_t *)malloc(row_stride);
    for (uint32_t y = 0; y < h; y++) {
        memset(row, 0, row_stride);
        uint32_t vy = (di.display_y + y) & 511;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t vx = (di.display_x + x) & 1023;
            uint16_t c = vram[vy * 1024 + vx];
            uint8_t r = (c & 0x1F) << 3;
            uint8_t g = ((c >> 5) & 0x1F) << 3;
            uint8_t b = ((c >> 10) & 0x1F) << 3;
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row, 1, row_stride, f);
    }
    free(row);
    fclose(f);

    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%u,\"height\":%u}",
             id, path, w, h);
}

static void handle_screenshot(int id, const char *json)
{
    (void)json;
    /* Read display area from GPU and encode as hex RGB555 */
    GpuDisplayInfo di;
    gpu_get_display_info(&di);

    if (di.disabled || di.width == 0 || di.height == 0) {
        send_err(id, "display disabled"); return;
    }

    const uint16_t *vram = gpu_get_vram();
    uint32_t w = di.width;
    uint32_t h = di.height;
    if (w > 640) w = 640;
    if (h > 512) h = 512;

    /* Send metadata first */
    send_fmt("{\"id\":%d,\"ok\":true,\"width\":%u,\"height\":%u,\"format\":\"rgb555\"}",
             id, w, h);

    /* Send rows as hex lines */
    char *hex = (char *)malloc(w * 4 + 32);
    if (!hex) return;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t vy = (di.display_y + y) & 511;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t vx = (di.display_x + x) & 1023;
            uint16_t pixel = vram[vy * 1024 + vx];
            snprintf(hex + x * 4, 5, "%04x", pixel);
        }
        send_fmt("{\"row\":%u,\"hex\":\"%s\"}", y, hex);
    }
    free(hex);
}

static void handle_vram_peek(int id, const char *json)
{
    int x = json_get_int(json, "x", 0);
    int y = json_get_int(json, "y", 0);
    int w = json_get_int(json, "w", 8);
    int h = json_get_int(json, "h", 1);
    if (w > 64) w = 64;
    if (h > 64) h = 64;
    char hex[64*64*4+1];
    int pos = 0;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t p = gpu_vram_peek(x + col, y + row);
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%04x", p);
        }
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"hex\":\"%s\"}",
             id, x, y, w, h, hex);
}

/* ---- Write trace: hook + handlers (Tier 1 reverse debugger) ---- */
extern CPUState *debug_cpu_ptr;

/* Record a single write into the RAM trace ring buffer. */
static void wtrace_record(uint32_t phys, uint32_t old_val, uint32_t new_val, uint8_t width)
{
    if (!s_wtrace) return;
    uint32_t ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    WriteTraceEntry *e = &s_wtrace[s_wtrace_head];
    e->seq       = s_wtrace_seq++;
    e->addr      = phys;
    e->old_val   = old_val;
    e->new_val   = new_val;
    e->ra        = ra;
    e->func_addr = g_debug_current_func_addr;
    e->frame     = (uint32_t)s_frame_count;
    e->width     = width;
    s_wtrace_head = (s_wtrace_head + 1) % WRITE_TRACE_CAP;
}

/* Multi-range check called from memory.c write paths.
 * Iterates up to 8 ranges; records if any match. */
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width)
{
    if (s_wtrace_range_count == 0) return;
    for (int i = 0; i < s_wtrace_range_count; i++) {
        if (phys >= s_wtrace_ranges[i].lo && phys < s_wtrace_ranges[i].hi) {
            wtrace_record(phys, old_val, new_val, width);
            return;
        }
    }
}

/* MMIO write trace — called from memory.c mmio_write32/16/8. */
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width)
{
    if (!s_mmio_trace) return;
    uint32_t ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    MmioTraceEntry *e = &s_mmio_trace[s_mmio_trace_head];
    e->seq       = s_mmio_trace_seq++;
    e->addr      = addr;
    e->val       = val;
    e->func_addr = g_debug_current_func_addr;
    e->ra        = ra;
    e->frame     = (uint32_t)s_frame_count;
    e->width     = width;
    s_mmio_trace_head = (s_mmio_trace_head + 1) % MMIO_TRACE_CAP;
}

static void handle_wtrace_range(int id, const char *json)
{
    /* Backward compat: sets slot 0, clears all other slots. */
    char lo_str[32], hi_str[32];
    if (!json_get_str(json, "lo", lo_str, sizeof(lo_str))) { send_err(id, "missing lo"); return; }
    if (!json_get_str(json, "hi", hi_str, sizeof(hi_str))) { send_err(id, "missing hi"); return; }
    uint32_t lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    uint32_t hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;
    s_wtrace_ranges[0].lo = lo;
    s_wtrace_ranges[0].hi = hi;
    s_wtrace_range_count = (lo != hi) ? 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
             id, lo, hi);
}

static void handle_wtrace_add(int id, const char *json)
{
    if (s_wtrace_range_count >= WTRACE_MAX_RANGES) {
        send_err(id, "max ranges reached (8)"); return;
    }
    char lo_str[32], hi_str[32];
    if (!json_get_str(json, "lo", lo_str, sizeof(lo_str))) { send_err(id, "missing lo"); return; }
    if (!json_get_str(json, "hi", hi_str, sizeof(hi_str))) { send_err(id, "missing hi"); return; }
    int slot = s_wtrace_range_count++;
    s_wtrace_ranges[slot].lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    s_wtrace_ranges[slot].hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
             id, slot, s_wtrace_ranges[slot].lo, s_wtrace_ranges[slot].hi);
}

static void handle_wtrace_del(int id, const char *json)
{
    int slot = json_get_int(json, "slot", -1);
    if (slot < 0 || slot >= s_wtrace_range_count) {
        send_err(id, "invalid slot"); return;
    }
    /* Compact: shift remaining slots down. */
    for (int i = slot; i < s_wtrace_range_count - 1; i++)
        s_wtrace_ranges[i] = s_wtrace_ranges[i + 1];
    s_wtrace_range_count--;
    send_ok(id);
}

static void handle_wtrace_ranges(int id, const char *json)
{
    (void)json;
    const size_t BUF_SZ = 2048;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"count\":%d,\"ranges\":[", id, s_wtrace_range_count);
    for (int i = 0; i < s_wtrace_range_count; i++) {
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"slot\":%d,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
                        (i == 0) ? "" : ",",
                        i, s_wtrace_ranges[i].lo, s_wtrace_ranges[i].hi);
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

/* Liveness/freeze diagnostic: returns a single snapshot of every counter
 * that distinguishes "stuck in a tight handler loop" from "just slow" or
 * "starved on TCP poll".  Pass {"window":N} (default 256) to also include
 * a top-N histogram of function entries within the last `window` slots
 * of the fn_entry ring, and a sample of the last `window` SIO IRQ ring
 * entries (delay_applied + i_stat_after).
 *
 * Discriminators:
 *   - exc_entries delta between two snapshots  → handler dispatch rate
 *   - exc_reentry_blocks > 0                   → check_interrupts called
 *                                                 from inside the handler
 *   - dirty_ram blocks/insns delta              → install-stub interp loop
 *   - fn_entry histogram dominance               → stuck-in-N-functions loop
 *   - dispatch_count change between calls       → recompiled code progressing
 *   - sio.irq_pending + countdown stuck         → IRQ pacing breakdown */
static void handle_freeze_check(int id, const char *json)
{
    int window = json_get_int(json, "window", 256);
    if (window < 1) window = 1;
    if (window > 65536) window = 65536;

    extern int sio_get_mc_max_state(void);
    extern int sio_get_mc_abort_count(void);
    extern int sio_get_mc_read_done(void);
    extern int sio_get_tx_writes(void);

    uint64_t total_checks = 0;
    uint32_t dispatch_count = 0;
    int in_exc = 0;
    int post_cool = 0;
    uint64_t exc_entries = 0;
    uint64_t exc_reentry = 0;
    psx_get_freeze_diag(&total_checks, &dispatch_count, &in_exc,
                        &post_cool, &exc_entries, &exc_reentry);

    int sio_irq_pending = 0;
    int sio_irq_countdown = 0;
    uint16_t sio_stat = 0;
    uint16_t sio_ctrl = 0;
    int card_active = 0;
    sio_get_freeze_diag(&sio_irq_pending, &sio_irq_countdown,
                        &sio_stat, &sio_ctrl, &card_active);

    extern uint64_t g_dirty_ram_blocks_run;
    extern uint64_t g_dirty_ram_insns_run;
    extern uint64_t g_dirty_ram_aborts;

    /* Top-K fn_entry histogram over the last `window` slots. */
    typedef struct { uint32_t func; uint32_t count; } HistBucket;
    enum { HIST_CAP = 16 };
    HistBucket hist[HIST_CAP];
    int hist_n = 0;
    uint32_t recent_total = 0;
    uint32_t recent_min_func = 0xFFFFFFFFu, recent_max_func = 0;
    if (s_fn_entry && s_fn_entry_seq > 0) {
        uint64_t end_seq = s_fn_entry_seq;
        uint64_t start_seq = (end_seq > (uint64_t)window) ? end_seq - window : 0;
        for (uint64_t i = start_seq; i < end_seq; i++) {
            FnEntryEntry *e = &s_fn_entry[i % FN_TRACE_CAP];
            uint32_t f = e->func_addr;
            if (f < recent_min_func) recent_min_func = f;
            if (f > recent_max_func) recent_max_func = f;
            recent_total++;
            int found = 0;
            for (int k = 0; k < hist_n; k++) {
                if (hist[k].func == f) { hist[k].count++; found = 1; break; }
            }
            if (!found) {
                if (hist_n < HIST_CAP) {
                    hist[hist_n].func = f;
                    hist[hist_n].count = 1;
                    hist_n++;
                } else {
                    /* Replace lowest count with this one if the lowest is 1
                     * (rare bucket, may be noise).  Keeps the top-K mostly
                     * stable for repeating-function-loop diagnosis. */
                    int min_idx = 0;
                    for (int k = 1; k < hist_n; k++)
                        if (hist[k].count < hist[min_idx].count) min_idx = k;
                    if (hist[min_idx].count <= 1) {
                        hist[min_idx].func = f;
                        hist[min_idx].count = 1;
                    }
                }
            }
        }
    }
    /* Sort hist descending by count (insertion sort, n<=16). */
    for (int i = 1; i < hist_n; i++) {
        HistBucket cur = hist[i];
        int j = i - 1;
        while (j >= 0 && hist[j].count < cur.count) {
            hist[j+1] = hist[j];
            j--;
        }
        hist[j+1] = cur;
    }

    /* Recent SIO IRQ ring sample. */
    const SioIrqEntry *irq_buf = NULL;
    int irq_widx = 0;
    uint32_t irq_total = sio_get_irq_ring(&irq_buf, &irq_widx);

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"id\":%d,\"ok\":true,"
                    "\"current_func\":\"0x%08X\","
                    "\"last_store_pc\":\"0x%08X\","
                    "\"total_checks\":%llu,"
                    "\"dispatch_count\":%u,"
                    "\"in_exception\":%d,"
                    "\"post_exception_cooldown\":%d,"
                    "\"exception_entries\":%llu,"
                    "\"exception_reentry_blocks\":%llu,"
                    "\"sio_irq_pending\":%d,"
                    "\"sio_irq_countdown\":%d,"
                    "\"sio_stat\":\"0x%04X\","
                    "\"sio_ctrl\":\"0x%04X\","
                    "\"sio_card_active\":%d,"
                    "\"i_stat\":\"0x%08X\","
                    "\"i_mask\":\"0x%08X\","
                    "\"dirty_ram_blocks\":%llu,"
                    "\"dirty_ram_insns\":%llu,"
                    "\"dirty_ram_aborts\":%llu,"
                    "\"fn_entry_total\":%llu,"
                    "\"sio_irq_total\":%u,"
                    "\"sio_byte_seq\":%u,"
                    "\"mc_max_state\":%d,"
                    "\"mc_aborts\":%d,"
                    "\"mc_read_done\":%d,"
                    "\"tx_writes\":%d,"
                    "\"psx_cycle_count\":%llu,"
                    "\"window\":%d,"
                    "\"recent_func_min\":\"0x%08X\","
                    "\"recent_func_max\":\"0x%08X\","
                    "\"recent_total\":%u,"
                    "\"hist\":[",
                    id,
                    g_debug_current_func_addr,
                    g_debug_last_store_pc,
                    (unsigned long long)total_checks,
                    dispatch_count,
                    in_exc,
                    post_cool,
                    (unsigned long long)exc_entries,
                    (unsigned long long)exc_reentry,
                    sio_irq_pending,
                    sio_irq_countdown,
                    (unsigned)sio_stat,
                    (unsigned)sio_ctrl,
                    card_active,
                    i_stat, i_mask,
                    (unsigned long long)g_dirty_ram_blocks_run,
                    (unsigned long long)g_dirty_ram_insns_run,
                    (unsigned long long)g_dirty_ram_aborts,
                    (unsigned long long)s_fn_entry_seq,
                    irq_total,
                    sio_get_seq(),
                    sio_get_mc_max_state(),
                    sio_get_mc_abort_count(),
                    sio_get_mc_read_done(),
                    sio_get_tx_writes(),
                    (unsigned long long)psx_get_cycle_count(),
                    window,
                    (recent_total ? recent_min_func : 0),
                    recent_max_func,
                    recent_total);
    for (int i = 0; i < hist_n && pos < sizeof(buf) - 64; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"func\":\"0x%08X\",\"count\":%u}",
                        i ? "," : "", hist[i].func, hist[i].count);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

static void handle_wtrace_stats(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_wtrace_seq <= WRITE_TRACE_CAP) ? 0 : s_wtrace_seq - WRITE_TRACE_CAP;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%d,"
             "\"oldest_seq\":%llu,\"newest_seq\":%llu,\"ranges\":%d}",
             id, (unsigned long long)s_wtrace_seq, WRITE_TRACE_CAP,
             (unsigned long long)oldest,
             (unsigned long long)(s_wtrace_seq > 0 ? s_wtrace_seq - 1 : 0),
             s_wtrace_range_count);
}

static void handle_wtrace_clear(int id, const char *json)
{
    (void)json;
    s_wtrace_seq = 0;
    s_wtrace_head = 0;
    if (s_wtrace) memset(s_wtrace, 0, (size_t)WRITE_TRACE_CAP * sizeof(WriteTraceEntry));
    send_ok(id);
}

static void handle_wtrace_dump(int id, const char *json)
{
    if (!s_wtrace) { send_err(id, "trace not initialized"); return; }

    /* Optional post-hoc address filter. */
    char lo_str[32], hi_str[32];
    uint32_t filter_lo = 0, filter_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "addr_lo", lo_str, sizeof(lo_str)))
        filter_lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", hi_str, sizeof(hi_str)))
        filter_hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;

    uint64_t total = s_wtrace_seq;
    uint32_t avail = (total < WRITE_TRACE_CAP) ? (uint32_t)total : WRITE_TRACE_CAP;
    uint32_t start = (total < WRITE_TRACE_CAP) ? 0 : s_wtrace_head;

    /* 1 GB response buffer; emit up to MAX_OUT (effectively whole ring). */
    const uint32_t MAX_OUT = WRITE_TRACE_CAP;
    const size_t BUF_SZ = (size_t)1 * 1024 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)total, avail);
    for (uint32_t i = 0; i < avail && emitted < MAX_OUT && pos < BUF_SZ - 256; i++) {
        uint32_t idx = (start + i) % WRITE_TRACE_CAP;
        WriteTraceEntry *e = &s_wtrace[idx];
        if (e->addr < filter_lo || e->addr >= filter_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"old\":\"0x%08X\","
                        "\"new\":\"0x%08X\",\"ra\":\"0x%08X\",\"func\":\"0x%08X\","
                        "\"frame\":%u,\"w\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->old_val, e->new_val, e->ra, e->func_addr,
                        e->frame, (unsigned)e->width);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%u}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

/* ---- MMIO trace handlers ---- */

static void handle_mmio_dump(int id, const char *json)
{
    if (!s_mmio_trace) { send_err(id, "mmio trace not initialized"); return; }

    /* Optional address filter. */
    char addr_str[32];
    uint32_t filter_addr = 0;
    int has_filter = json_get_str(json, "addr", addr_str, sizeof(addr_str)) != NULL;
    if (has_filter) filter_addr = hex_to_u32(addr_str);

    uint64_t total = s_mmio_trace_seq;
    uint32_t avail = (total < MMIO_TRACE_CAP) ? (uint32_t)total : MMIO_TRACE_CAP;
    uint32_t start = (total < MMIO_TRACE_CAP) ? 0 : s_mmio_trace_head;

    /* 1 GB response buffer; emit up to MAX_OUT (effectively whole ring). */
    const uint32_t MAX_OUT = MMIO_TRACE_CAP;
    const size_t BUF_SZ = (size_t)1 * 1024 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)total, avail);
    for (uint32_t i = 0; i < avail && emitted < MAX_OUT && pos < BUF_SZ - 256; i++) {
        uint32_t idx = (start + i) % MMIO_TRACE_CAP;
        MmioTraceEntry *e = &s_mmio_trace[idx];
        if (has_filter && e->addr != filter_addr) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"val\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"ra\":\"0x%08X\",\"frame\":%u,\"w\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->val, e->func_addr, e->ra,
                        e->frame, (unsigned)e->width);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%u}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_mmio_clear(int id, const char *json)
{
    (void)json;
    s_mmio_trace_seq = 0;
    s_mmio_trace_head = 0;
    if (s_mmio_trace) memset(s_mmio_trace, 0, (size_t)MMIO_TRACE_CAP * sizeof(MmioTraceEntry));
    send_ok(id);
}

static void handle_quit(int id, const char *json)
{
    (void)json;
    send_ok(id);
    debug_server_shutdown();
    exit(0);
}

/* ---- Command dispatch table ---- */

/* dispatch_check: check if a specific address was ever dispatched */
static void handle_dispatch_check(int id, const char *json) {
    char abuf[32] = {0};
    json_get_str(json, "addr", abuf, sizeof(abuf));
    uint32_t addr = (uint32_t)strtoul(abuf, NULL, 0);
    int found = dispatch_trace_contains(addr);
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"found\":%s,\"total\":%llu}\n",
        id, addr, found ? "true" : "false", (unsigned long long)s_dispatch_seq);
}

/* dispatch_tail: dump the last N dispatched function addresses */
static void handle_dispatch_tail(int id, const char *json) {
    int count = 64;
    { /* try to parse count */
        char buf[32];
        if (json_get_str(json, "count", buf, sizeof(buf)))
            count = atoi(buf);
    }
    if (count > 4096) count = 4096;
    if ((uint64_t)count > s_dispatch_seq) count = (int)s_dispatch_seq;

    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"count\":%d,\"addrs\":[",
        id, (unsigned long long)s_dispatch_seq, count);
    uint64_t start = s_dispatch_seq - count;
    for (int i = 0; i < count; i++) {
        uint32_t a = s_dispatch_ring[(start + i) % DISPATCH_TRACE_CAP];
        if (i > 0) debug_server_send_fmt(",");
        debug_server_send_fmt("\"0x%08X\"", a);
    }
    debug_server_send_fmt("]}\n");
}

/* ---- Function entry/exit trace handlers ---- */

static void handle_fn_filter(int id, const char *json) {
    char buf[32];
    if (json_get_str(json, "lo", buf, sizeof(buf))) s_fn_trace_filter_lo = hex_to_u32(buf);
    if (json_get_str(json, "hi", buf, sizeof(buf))) s_fn_trace_filter_hi = hex_to_u32(buf);
    send_fmt("{\"id\":%d,\"ok\":true,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}\n",
             id, s_fn_trace_filter_lo, s_fn_trace_filter_hi);
}

static void handle_fn_clear(int id, const char *json) {
    (void)json;
    s_fn_entry_seq = 0;
    s_fn_exit_seq  = 0;
    s_fn_stack_top = 0;
    s_fn_prev_ra   = 0;
    s_fn_unmatched_returns = 0;
    s_fn_stack_overflows   = 0;
    s_fn_tail_calls        = 0;
    if (s_fn_entry) memset(s_fn_entry, 0, (size_t)FN_TRACE_CAP * sizeof(FnEntryEntry));
    if (s_fn_exit)  memset(s_fn_exit,  0, (size_t)FN_EXIT_TRACE_CAP * sizeof(FnExitEntry));
    send_ok(id);
}

static void handle_fn_stats(int id, const char *json) {
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"entry_total\":%llu,\"exit_total\":%llu,"
             "\"stack_top\":%d,\"unmatched_returns\":%llu,"
             "\"stack_overflows\":%llu,\"tail_calls\":%llu,"
             "\"entry_capacity\":%llu,\"exit_capacity\":%llu,"
             "\"filter_lo\":\"0x%08X\",\"filter_hi\":\"0x%08X\"}\n",
             id,
             (unsigned long long)s_fn_entry_seq,
             (unsigned long long)s_fn_exit_seq,
             s_fn_stack_top,
             (unsigned long long)s_fn_unmatched_returns,
             (unsigned long long)s_fn_stack_overflows,
             (unsigned long long)s_fn_tail_calls,
             (unsigned long long)FN_TRACE_CAP,
             (unsigned long long)FN_EXIT_TRACE_CAP,
             s_fn_trace_filter_lo, s_fn_trace_filter_hi);
}

/* Helper: parse filter / range / count for fn_*_dump. */
static void fn_dump_parse(const char *json, uint64_t total,
                         uint64_t *out_seq_lo, uint64_t *out_seq_hi,
                         uint32_t *out_addr_lo, uint32_t *out_addr_hi,
                         int *out_max) {
    char buf[32];
    *out_seq_lo  = 0;
    *out_seq_hi  = total;
    *out_addr_lo = 0;
    *out_addr_hi = 0xFFFFFFFFu;
    /* Default: emit up to ~4M filtered hits.  Caller can override.  Trust the
     * client; we have plenty of RAM and a 1 GB response buffer. */
    *out_max     = 4 * 1024 * 1024;
    if (json_get_str(json, "seq_lo", buf, sizeof(buf))) *out_seq_lo = strtoull(buf, NULL, 0);
    if (json_get_str(json, "seq_hi", buf, sizeof(buf))) *out_seq_hi = strtoull(buf, NULL, 0);
    if (json_get_str(json, "addr_lo", buf, sizeof(buf))) *out_addr_lo = hex_to_u32(buf) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", buf, sizeof(buf))) *out_addr_hi = hex_to_u32(buf) & 0x1FFFFFFFu;
    *out_max = json_get_int(json, "count", *out_max);
    if (*out_max < 1) *out_max = 1;
    /* No artificial upper bound; trust the caller and the response buffer. */
    /* No "tail-N" auto-trim: scan the entire visible range by default so we
     * never silently miss filtered hits earlier in the ring. */
}

static void handle_fn_entry_dump(int id, const char *json) {
    if (!s_fn_entry) { send_err(id, "not initialized"); return; }
    uint64_t seq_lo, seq_hi;
    uint32_t addr_lo, addr_hi;
    int max_count;
    fn_dump_parse(json, s_fn_entry_seq, &seq_lo, &seq_hi, &addr_lo, &addr_hi, &max_count);

    /* Earliest available seq in ring. */
    uint64_t oldest = (s_fn_entry_seq > FN_TRACE_CAP) ? s_fn_entry_seq - FN_TRACE_CAP : 0;
    if (seq_lo < oldest) seq_lo = oldest;
    if (seq_hi > s_fn_entry_seq) seq_hi = s_fn_entry_seq;

    /* 1 GB response buffer.  Each entry serializes to ~256 bytes JSON, so we
     * can emit up to ~4M entries per dump.  No artificial truncation. */
    const size_t BUF_SZ = (size_t)1 * 1024 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,"
                    "\"seq_lo\":%llu,\"seq_hi\":%llu,\"entries\":[",
                    id, (unsigned long long)s_fn_entry_seq,
                    (unsigned long long)oldest,
                    (unsigned long long)seq_lo, (unsigned long long)seq_hi);
    int emitted = 0;
    int first = 1;
    for (uint64_t s = seq_lo; s < seq_hi && emitted < max_count && pos < BUF_SZ - 512; s++) {
        FnEntryEntry *e = &s_fn_entry[s % FN_TRACE_CAP];
        if (e->func_addr < addr_lo || e->func_addr >= addr_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"func\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
            "\"t1\":\"0x%08X\",\"depth\":%u,\"frame\":%u,\"exit_seq\":%llu}",
            first ? "" : ",",
            (unsigned long long)e->seq,
            e->func_addr, e->ra, e->a0, e->a1, e->a2, e->a3, e->t1,
            e->depth, e->frame,
            (unsigned long long)e->paired_exit_seq);
        first = 0;
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_fn_exit_dump(int id, const char *json) {
    if (!s_fn_exit) { send_err(id, "not initialized"); return; }
    uint64_t seq_lo, seq_hi;
    uint32_t addr_lo, addr_hi;
    int max_count;
    fn_dump_parse(json, s_fn_exit_seq, &seq_lo, &seq_hi, &addr_lo, &addr_hi, &max_count);

    uint64_t oldest = (s_fn_exit_seq > FN_EXIT_TRACE_CAP) ? s_fn_exit_seq - FN_EXIT_TRACE_CAP : 0;
    if (seq_lo < oldest) seq_lo = oldest;
    if (seq_hi > s_fn_exit_seq) seq_hi = s_fn_exit_seq;

    const size_t BUF_SZ = (size_t)1 * 1024 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,"
                    "\"seq_lo\":%llu,\"seq_hi\":%llu,\"entries\":[",
                    id, (unsigned long long)s_fn_exit_seq,
                    (unsigned long long)oldest,
                    (unsigned long long)seq_lo, (unsigned long long)seq_hi);
    int emitted = 0;
    int first = 1;
    for (uint64_t s = seq_lo; s < seq_hi && emitted < max_count && pos < BUF_SZ - 512; s++) {
        FnExitEntry *e = &s_fn_exit[s % FN_EXIT_TRACE_CAP];
        if (e->func_addr < addr_lo || e->func_addr >= addr_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"entry_seq\":%llu,\"func\":\"0x%08X\","
            "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\",\"depth\":%u,\"frame\":%u}",
            first ? "" : ",",
            (unsigned long long)e->seq, (unsigned long long)e->entry_seq,
            e->func_addr, e->v0, e->v1, e->depth, e->frame);
        first = 0;
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

typedef void (*CmdHandler)(int id, const char *json);
typedef struct { const char *name; CmdHandler handler; } CmdEntry;

static const CmdEntry s_commands[] = {
    { "ping",              handle_ping },
    { "frame",             handle_frame },
    { "get_registers",     handle_get_registers },
    { "read_ram",          handle_read_ram },
    { "dump_ram",          handle_dump_ram },
    { "write_ram",         handle_write_ram },
    { "gpu_state",         handle_gpu_state },
    { "vram_peek",         handle_vram_peek },
    { "irq_state",         handle_irq_state },
    { "sio_state",         handle_sio_state },
    { "mc_status",         handle_mc_status },
    { "card_buffer_dump",  handle_card_buffer_dump },
    { "sio_arm_audit",     handle_sio_arm_audit },
    { "sio_burst_stats",   handle_sio_burst_stats },
    { "pace_state",        handle_pace_state },
    { "chain_trace",       handle_chain_trace },
    { "sio_trace",         handle_sio_trace },
    { "sio_pc_trace",      handle_sio_pc_trace },
    { "dirty_ram_stats",   handle_dirty_ram_stats },
    { "card_txn_dump",     handle_card_txn_dump },
    { "sio_irq_dump",      handle_sio_irq_dump },
    { "evcb_snapshot",     handle_evcb_snapshot },
    { "evcb_walk_dump",    handle_evcb_walk_dump },
    { "evcb_walk_stats",   handle_evcb_walk_stats },
    { "imask_trace",       handle_imask_trace },
    { "watch",             handle_watch },
    { "unwatch",           handle_unwatch },
    { "wtrace_range",      handle_wtrace_range },
    { "wtrace_add",        handle_wtrace_add },
    { "wtrace_del",        handle_wtrace_del },
    { "wtrace_ranges",     handle_wtrace_ranges },
    { "wtrace_dump",       handle_wtrace_dump },
    { "wtrace_clear",      handle_wtrace_clear },
    { "wtrace_stats",      handle_wtrace_stats },
    { "freeze_check",      handle_freeze_check },
    { "mmio_dump",         handle_mmio_dump },
    { "mmio_clear",        handle_mmio_clear },
    { "set_input",         handle_set_input },
    { "press",             handle_press },
    { "pad_status",        handle_pad_status },
    { "clear_input",       handle_clear_input },
    { "display_source",    handle_display_source },
    { "pause",             handle_pause },
    { "continue",          handle_continue },
    { "step",              handle_step },
    { "run_to_frame",      handle_run_to_frame },
    { "history",           handle_history },
    { "get_frame",         handle_get_frame },
    { "frame_range",       handle_frame_range },
    { "frame_timeseries",  handle_frame_timeseries },
    { "first_failure",     handle_first_failure },
    { "read_frame_ram",    handle_read_frame_ram },
    { "set_snapshot",      handle_set_snapshot },
    { "get_snapshots",     handle_get_snapshots },
    { "screenshot",        handle_screenshot },
    { "screenshot_file",   handle_screenshot_file },
    { "gpu_opcodes",       handle_gpu_opcodes },
    { "a0_history",        handle_a0_history },
    { "c0_history",        handle_c0_history },
    { "capture_quads",     handle_capture_quads },
    { "get_quads",         handle_get_quads },
    { "gte_state",         handle_gte_state },
    { "quit",              handle_quit },
    { "dispatch_check",    handle_dispatch_check },
    { "dispatch_tail",     handle_dispatch_tail },
    { "fn_filter",         handle_fn_filter },
    { "fn_clear",          handle_fn_clear },
    { "fn_stats",          handle_fn_stats },
    { "fn_entry_dump",     handle_fn_entry_dump },
    { "fn_exit_dump",      handle_fn_exit_dump },
    { NULL, NULL }
};

static void process_command(const char *line)
{
    char cmd[64];
    if (!json_get_str(line, "cmd", cmd, sizeof(cmd))) {
        strncpy(cmd, line, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        int len = (int)strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\r' || cmd[len-1] == ' '))
            cmd[--len] = '\0';
    }

    int id = json_get_int(line, "id", 0);

    for (const CmdEntry *e = s_commands; e->name; e++) {
        if (strcmp(cmd, e->name) == 0) {
            e->handler(id, line);
            return;
        }
    }

    /* Oracle commands (find_first_divergence, emu_read_ram, etc.) */
#if defined(ENABLE_DUCKSTATION_ORACLE) || defined(ENABLE_BEETLE_PSX_ORACLE)
    {
        extern int psx_oracle_handle_cmd(const char *cmd, int id, const char *json);
        if (psx_oracle_handle_cmd(cmd, id, line)) return;
    }
#endif

    send_err(id, "unknown command");
}

/* ---- Public API ---- */

/* Extended init that accepts a CPU state pointer for register queries. */
static CPUState *s_init_cpu = NULL;
CPUState *debug_cpu_ptr = NULL; /* Global, used by memory.c watchpoints */

void debug_server_set_cpu(CPUState *cpu)
{
    s_cpu = cpu;
    debug_cpu_ptr = cpu;
}

void debug_server_init(int port)
{
    if (port > 0) s_port = port;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == SOCK_INVALID) return;

    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)s_port);

    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
        return;
    }

    listen(s_listen, 1);
    set_nonblocking(s_listen);

    if (!s_frame_history) {
        s_frame_history = (PSXFrameRecord *)calloc(FRAME_HISTORY_CAP, sizeof(PSXFrameRecord));
    }
    s_history_count = 0;

    /* Tier 1: heap-allocate write trace ring buffer (32 MB). */
    if (!s_wtrace) {
        s_wtrace = (WriteTraceEntry *)calloc(WRITE_TRACE_CAP, sizeof(WriteTraceEntry));
    }
    s_wtrace_seq = 0;
    s_wtrace_head = 0;

    /* Function entry/exit ring buffers (32 MB each, 64 MB total). */
    if (!s_fn_entry) s_fn_entry = (FnEntryEntry *)calloc(FN_TRACE_CAP, sizeof(FnEntryEntry));
    if (!s_fn_exit)  s_fn_exit  = (FnExitEntry *)calloc(FN_EXIT_TRACE_CAP, sizeof(FnExitEntry));
    s_fn_entry_seq = 0;
    s_fn_exit_seq  = 0;
    s_fn_stack_top = 0;
    s_fn_unmatched_returns = 0;
    s_fn_stack_overflows   = 0;

    /* EvCB walk ring (~240 KB). */
    if (!s_evcb_ring) s_evcb_ring = (EvCBSnapshot *)calloc(EVCB_RING_CAP, sizeof(EvCBSnapshot));
    s_evcb_ring_seq = 0;
    s_evcb_ring_entry_count = 0;
    s_evcb_ring_exit_count  = 0;
    s_evcb_pending_active   = 0;
    s_evcb_unwound_count    = 0;

    /* Phase 4.5: watch EB4 area + DF8/DFC area + EvCB slot 1 status + state machine
     * + spiral texture buffer. */
    s_wtrace_ranges[0].lo = 0x00079EB0u;
    s_wtrace_ranges[0].hi = 0x00079EC0u;
    s_wtrace_ranges[1].lo = 0x00079DF0u;
    s_wtrace_ranges[1].hi = 0x00079E04u;
    s_wtrace_ranges[2].lo = 0x0000E044u;  /* EvCB slot 1 (class+status+spec) */
    s_wtrace_ranges[2].hi = 0x0000E054u;
    s_wtrace_ranges[3].lo = 0x00066940u;  /* shell state machine 0x80066940 */
    s_wtrace_ranges[3].hi = 0x00066954u;
    s_wtrace_ranges[4].lo = 0x001B6810u;  /* spiral texture buffer (heap @ 0x801B6814) */
    s_wtrace_ranges[4].hi = 0x001B6830u;
    /* Card-chain visibility (always-on per global rule "use ring buffers, never
     * sample"): 0x7514 is the shared chain counter, 0x7528[0..1] are the per-slot
     * chain handler ptrs. Reading 0x7528 = 0x5688 means read-chain installed;
     * = 0x5B64 means detection-chain installed. Catching every write to these
     * tells us deterministically when card-read paths get armed. */
    s_wtrace_ranges[5].lo = 0x00007514u;
    s_wtrace_ranges[5].hi = 0x00007518u;
    s_wtrace_ranges[6].lo = 0x00007528u;  /* 0x7528[0] (slot 0 chain handler ptr) */
    s_wtrace_ranges[6].hi = 0x00007530u;  /* through 0x7528[1] (slot 1) */
    /* 0x755A is the chain abort flag: D1 sets =1, outer coord clears+aborts on !=0 */
    s_wtrace_ranges[7].lo = 0x0000755Au;
    s_wtrace_ranges[7].hi = 0x0000755Cu;
    /* 0x7520 is the success flag (state-3 sets, dispatcher v0=-1 cascade reads) */
    s_wtrace_ranges[8].lo = 0x00007520u;
    s_wtrace_ranges[8].hi = 0x00007524u;
    /* 0x74A4 is the chain status flag */
    s_wtrace_ranges[9].lo = 0x000074A4u;
    s_wtrace_ranges[9].hi = 0x000074A8u;
    /* 0x75C0 = SIO data-mode flag (set 1 by func_6380, cleared 0 by 0xBFC15EBC).
     * 0x75C4 = current buffer pointer.  The data-byte handler at RAM 0x641C
     * exits early via beq v0,zero if [0x75C0]==0; tracking these tells us
     * whether the install-stub data path is engaging at all. */
    s_wtrace_ranges[10].lo = 0x000075C0u;
    s_wtrace_ranges[10].hi = 0x000075C8u;
    /* 0x74B8 = pad-poll gate: if 0, pad poll function at BFC144BC SKIPS pad
     * polling. 0x74BC = card-running gate: if non-zero, calls 0xBFC14B00
     * (outer card coordinator). Tracking both tells us whether the BIOS is
     * attempting the gate-flip serialization that prevents pad/card races. */
    s_wtrace_ranges[11].lo = 0x000074B8u;
    s_wtrace_ranges[11].hi = 0x000074C0u;
    /* 0x72F0 = data-byte counter (halfword). Install handler at RAM 0x641C
     * increments this per SIO IRQ during data phase; should reach 128 for a
     * full sector read. Handoff says it stops at ~16. Track every write
     * (including any abort-clear) to identify the byte-count regression PC. */
    s_wtrace_ranges[12].lo = 0x000072F0u;
    s_wtrace_ranges[12].hi = 0x000072F4u;
    s_wtrace_range_count = 13;

    /* Tier 1: heap-allocate MMIO trace ring buffer (2 MB). */
    if (!s_mmio_trace) {
        s_mmio_trace = (MmioTraceEntry *)calloc(MMIO_TRACE_CAP, sizeof(MmioTraceEntry));
    }
    s_mmio_trace_seq = 0;
    s_mmio_trace_head = 0;

    memset(s_watchpoints, 0, sizeof(s_watchpoints));
    memset(s_snapshot_addrs, 0, sizeof(s_snapshot_addrs));
    memset(s_snapshot_active, 0, sizeof(s_snapshot_active));
}

void debug_server_poll(void)
{
    if (s_listen == SOCK_INVALID) return;

    if (s_client == SOCK_INVALID) {
        struct sockaddr_in caddr;
        int clen = sizeof(caddr);
        sock_t c = accept(s_listen, (struct sockaddr *)&caddr, &clen);
        if (c != SOCK_INVALID) {
            s_client = c;
            set_nonblocking(s_client);
            s_recv_len = 0;
        }
        return;
    }

    int space = RECV_BUF_SIZE - s_recv_len - 1;
    if (space > 0) {
        int n = recv(s_client, s_recv_buf + s_recv_len, space, 0);
        if (n > 0) {
            s_recv_len += n;
            s_recv_buf[s_recv_len] = '\0';
        } else if (n == 0) {
            sock_close(s_client);
            s_client = SOCK_INVALID;
            s_paused = 0;
            return;
        } else {
            int err = sock_error();
#ifdef _WIN32
            if (err != WSAEWOULDBLOCK) {
#else
            if (err != EAGAIN && err != EWOULDBLOCK) {
#endif
                sock_close(s_client);
                s_client = SOCK_INVALID;
                s_paused = 0;
                s_input_override = -1;
                return;
            }
        }
    }

    char *nl;
    while ((nl = strchr(s_recv_buf, '\n')) != NULL) {
        *nl = '\0';
        if (nl > s_recv_buf && *(nl - 1) == '\r')
            *(nl - 1) = '\0';
        if (s_recv_buf[0] != '\0')
            process_command(s_recv_buf);
        int consumed = (int)(nl - s_recv_buf) + 1;
        s_recv_len -= consumed;
        memmove(s_recv_buf, nl + 1, s_recv_len + 1);
    }
}

void debug_server_record_frame(void)
{
    if (!s_frame_history) return;
    if (!s_cpu) return;

    uint32_t idx = (uint32_t)(s_frame_count % FRAME_HISTORY_CAP);
    PSXFrameRecord *r = &s_frame_history[idx];

    r->frame_number = (uint32_t)s_frame_count;
    r->verify_pass = -1;
    r->diff_count  = 0;
    memset(r->diffs, 0, sizeof(r->diffs));

    /* MIPS CPU state */
    memcpy(r->gpr, s_cpu->gpr, sizeof(r->gpr));
    r->hi = s_cpu->hi;
    r->lo = s_cpu->lo;
    r->cop0_sr    = s_cpu->cop0[12];
    r->cop0_cause = s_cpu->cop0[13];
    r->cop0_epc   = s_cpu->cop0[14];

    /* Interrupt state */
    r->i_stat = i_stat;
    r->i_mask = i_mask;

    /* GPU display state */
    {
        GpuDisplayInfo di;
        gpu_get_display_info(&di);
        r->display_area_x = (uint16_t)di.display_x;
        r->display_area_y = (uint16_t)di.display_y;
        r->display_w      = (uint16_t)di.width;
        r->display_h      = (uint16_t)di.height;
        r->display_disabled = di.disabled;
    }

    /* SIO state */
    r->pad_buttons = sio_get_pad_buttons();
    r->sio_stat = (uint16_t)sio_read(0x1F801044);
    r->sio_ctrl = (uint16_t)sio_read(0x1F80104A);

    /* Timing */
    r->dispatch_count = 0; /* filled externally if needed */
    r->total_dispatches = s_frame_count;

    /* Snapshot regions */
    for (int i = 0; i < RAM_SNAPSHOT_REGIONS; i++) {
        r->snapshot_addr[i] = s_snapshot_addrs[i];
        if (s_snapshot_active[i] && s_snapshot_addrs[i] != 0) {
            for (int j = 0; j < RAM_SNAPSHOT_SIZE; j++)
                r->snapshot_data[i][j] = psx_read_byte(s_snapshot_addrs[i] + j);
        } else {
            memset(r->snapshot_data[i], 0, RAM_SNAPSHOT_SIZE);
        }
    }

    /* Game-specific data */
    memset(r->game_data, 0, sizeof(r->game_data));

    /* Last function */
    strcpy(r->last_func, "(no tracking)");

    s_history_count = s_frame_count + 1;
    s_frame_count++;

    /* Step mode */
    if (s_step_count > 0) {
        s_step_count--;
        if (s_step_count == 0) {
            s_paused = 1;
            send_fmt("{\"event\":\"step_done\",\"frame\":%llu}",
                     (unsigned long long)(s_frame_count - 1));
        }
    }

    /* Run-to-frame */
    if (s_run_to > 0 && s_frame_count - 1 >= s_run_to) {
        s_paused = 1;
        s_run_to = 0;
        send_fmt("{\"event\":\"run_to_done\",\"frame\":%llu}",
                 (unsigned long long)(s_frame_count - 1));
    }
}

void debug_server_wait_if_paused(void)
{
    while (s_paused) {
        debug_server_poll();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) exit(0);
        }

        SDL_Delay(5);
    }
}

void debug_server_check_watchpoints(void)
{
    if (s_client == SOCK_INVALID) return;

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) continue;
        uint8_t cur = psx_read_byte(s_watchpoints[i].addr);
        if (cur != s_watchpoints[i].prev_val) {
            send_fmt("{\"event\":\"watchpoint\","
                     "\"addr\":\"0x%08X\",\"old\":\"0x%02X\",\"new\":\"0x%02X\","
                     "\"frame\":%llu}",
                     s_watchpoints[i].addr,
                     s_watchpoints[i].prev_val, cur,
                     (unsigned long long)s_frame_count);
            s_watchpoints[i].prev_val = cur;
        }
    }
}

void debug_server_shutdown(void)
{
    if (s_client != SOCK_INVALID) {
        sock_close(s_client);
        s_client = SOCK_INVALID;
    }
    if (s_listen != SOCK_INVALID) {
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

int debug_server_is_connected(void)
{
    return s_client != SOCK_INVALID;
}

int debug_server_get_input_override(void)
{
    if (s_input_override >= 0 && s_input_frames > 0) {
        if (--s_input_frames == 0)
            s_input_override = -1;
    }
    return s_input_override;
}
