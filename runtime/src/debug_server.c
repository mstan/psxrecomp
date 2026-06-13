/*
 * debug_server.c -- TCP debug server for PSX recomp v4
 *
 * Single-threaded, non-blocking TCP server polled once per vblank.
 * JSON-over-newline protocol on localhost:4370.
 *
 * Same function names and protocol as nesrecomp/snesrecomp versions
 * so TCP.md and DEBUG.md are reusable across projects.
 */
/* Expose POSIX clock_gettime()/CLOCK_MONOTONIC (used by monotonic_ms) on
 * glibc — must precede any system header. Harmless on Windows/macOS. */
#ifdef __APPLE__
#  define _DARWIN_C_SOURCE 1   /* full BSD+POSIX namespace: clock_gettime AND INADDR_LOOPBACK/timeval */
#elif !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>
#include "debug_server.h"
#include "overlay_loader.h"
#include "overlay_capture.h"
#include "cpu_state.h"
#include "dma.h"
#include "gpu.h"
#include "cdrom.h"
#include "sio.h"
#include "memcard.h"
#include "spu.h"
#include "mdec.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "timers.h"
#include "dirty_ram_interp.h"
#include "card_read_summary.h"
#include "card_data_writes.h"
#include "crash_trace.h"
#include "gpu_gl_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#ifndef DEFAULT_DEBUG_PORT
#error DEFAULT_DEBUG_PORT must be defined by the runtime target.
#endif

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
#  include <sys/time.h>     /* struct timeval — socket send/recv timeouts */
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
static int    s_port    = DEFAULT_DEBUG_PORT;

#define RECV_BUF_SIZE 8192
static char s_recv_buf[RECV_BUF_SIZE];
static int  s_recv_len = 0;

/* ---- Frame counter (set by record_frame caller) ---- */
/* Non-static so other instrumentation (e.g. dirty_ram_interp.c) can stamp
 * ring-buffer entries with the current frame for cross-correlation. */
uint64_t s_frame_count = 0;

/* ---- CPU state pointer (set at init) ---- */
static CPUState *s_cpu = NULL;

/* ---- Pause / step ---- */
static volatile int s_paused     = 0;
static int          s_step_count = 0;
static uint32_t     s_run_to     = 0;

/* ---- Dirty-RAM one-shot break ---- */
static volatile int s_dirty_break_active = 0;
static uint32_t s_dirty_break_lo = 0;
static uint32_t s_dirty_break_hi = 0;
static uint32_t s_dirty_break_target = 0;
static uint32_t s_dirty_break_ra = 0;
static uint32_t s_dirty_break_a0 = 0;
static uint32_t s_dirty_break_a1 = 0;
static uint32_t s_dirty_break_a2 = 0;
static uint32_t s_dirty_break_a3 = 0;
static uint32_t s_dirty_break_sp = 0;
static uint32_t s_dirty_break_frame = 0;
static uint64_t s_dirty_break_hits = 0;

/* ---- Input override ---- */
static int s_input_override = -1;
static int s_input_frames   = 0;

/* ---- Frontend turbo override ---- */
static volatile int s_turbo_enabled = 0;

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
 * Records every RAM write matching one of the configurable address ranges.
 * 1M-entry ring buffer, heap-allocated in debug_server_init(). */
/* Ring caps were sized for hour-long captures; that's ~700 MB if every
 * ring is touched. Most diagnostic flows need seconds, not hours. Drop
 * to 256K-entry caps unless an explicit need for more arises — at 60Hz
 * and ~thousand events/frame that's still ~4 sec of coverage per ring,
 * and total runtime memory stays under ~100 MB worst-case. */
#define WRITE_TRACE_CAP (1 << 18)
typedef struct {
    uint64_t seq;        /* monotonic sequence number */
    uint32_t addr;       /* physical RAM address */
    uint32_t old_val;    /* pre-write value */
    uint32_t new_val;    /* post-write value */
    uint32_t ra;         /* $ra (caller return address) */
    uint32_t func_addr;  /* dispatch target (which recompiled function) */
    uint32_t pc;         /* g_debug_last_store_pc — exact PC of the SW/SH/SB */
    uint32_t cpu_pc;     /* CPUState::pc at the moment of the write */
    uint32_t sp;
    uint32_t v0;
    uint32_t v1;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t t0;
    uint32_t t1;
    uint32_t frame;      /* VBlank frame number */
    uint8_t  width;      /* 1, 2, or 4 */
    uint8_t  pad[3];     /* align to 8 bytes */
} WriteTraceEntry;
static WriteTraceEntry *s_wtrace = NULL;
static uint64_t s_wtrace_seq  = 0;  /* total writes ever recorded */
static uint32_t s_wtrace_head = 0;

/* Boot-pinned write trace. This is intentionally not a ring: it captures the
 * first writes to a few high-value startup ranges and then stops, so late
 * probes can answer "what initialized/reset this?" even after the normal trace
 * has rolled. */
#define WRITE_TRACE_BOOT_CAP (1 << 18)
static WriteTraceEntry *s_wtrace_boot = NULL;
static uint64_t s_wtrace_boot_total = 0;  /* matching writes ever seen */
static uint32_t s_wtrace_boot_count = 0;  /* entries retained */
#define WTRACE_BOOT_MAX_RANGES 12
static struct { uint32_t lo, hi; } s_wtrace_boot_ranges[WTRACE_BOOT_MAX_RANGES];
static int s_wtrace_boot_range_count = 0;

/* Multi-range filter: up to 64 [lo, hi) address ranges. Boot defaults
 * occupy ~15; investigative arms must always have headroom. */
#define WTRACE_MAX_RANGES 64
static struct { uint32_t lo, hi; } s_wtrace_ranges[WTRACE_MAX_RANGES];
static int s_wtrace_range_count = 0;

/* ---- wtrace_all ring (ALWAYS-ON; no filter; lean fields) ----
 * Parity with psx-beetle's s_wtrace_all. Every recompiled-code write
 * to RAM lands here unconditionally, so a probe that connects AFTER
 * an event can query the write history without needing to have pre-armed
 * a range. Lean record (no register window) keeps 4M entries at ~128 MB;
 * this is intentionally large enough to retain Tomba's boot-to-OPTIONS
 * window for post-hoc initialization questions. */
#define WRITE_TRACE_ALL_CAP (1 << 22)
typedef struct {
    uint64_t seq;
    uint32_t addr;
    uint32_t new_val;
    uint32_t pc;
    uint32_t ra;
    uint32_t frame;
    uint8_t  w;
    uint8_t  pad[3];
} WriteTraceAllEntry;
static WriteTraceAllEntry *s_wtrace_all = NULL;
static uint64_t s_wtrace_all_seq  = 0;
static uint32_t s_wtrace_all_head = 0;

/* ---- wtrace_transition ring (ALWAYS-ON; selected ranges; value changes only)
 * The catch-all write ring intentionally records every write, but hot paths can
 * roll it before a boot-to-menu transition is diagnosed. This ring keeps a
 * long-lived timeseries for high-value scheduler/render state by recording only
 * writes whose value changed. */
#define WRITE_TRACE_TRANS_CAP (1 << 20)
#define WTRACE_TRANS_MAX_RANGES 16
typedef struct {
    uint64_t seq;
    uint32_t addr;
    uint32_t old_val;
    uint32_t new_val;
    uint32_t pc;
    uint32_t func_addr;
    uint32_t ra;
    uint32_t sp;
    uint32_t s0;
    uint32_t s1;
    uint32_t s2;
    uint32_t s3;
    uint32_t stk20;
    uint32_t stk40;
    uint32_t frame;
    uint8_t  width;
    uint8_t  pad[3];
} WriteTraceTransEntry;
static WriteTraceTransEntry *s_wtrace_trans = NULL;
static uint64_t s_wtrace_trans_seq = 0;
static uint32_t s_wtrace_trans_head = 0;
static struct { uint32_t lo, hi; } s_wtrace_trans_ranges[WTRACE_TRANS_MAX_RANGES];
static int s_wtrace_trans_range_count = 0;

/* Function attribution global — set by psx_dispatch() before each call. */
uint32_t g_debug_current_func_addr = 0;

/* Last store PC — set by recompiler emitter before every store instruction. */
uint32_t g_debug_last_store_pc = 0;

/* Static dispatch hit counter — incremented by generated dispatch code on
 * every binary-search hit (i.e. successfully dispatched to static C). */
uint64_t g_dispatch_static_hits = 0;

/* ---- SIO write PC tracer ring ----
 * Captures (pc, addr, value, byte_seq, ctr) for every write to a SIO
 * register, attributing the exact writing instruction.  Used to find what
 * code is putting bytes on the SIO bus when chain-dispatcher attribution
 * (g_debug_current_func_addr) is too coarse.
 *
 * 1<<22 entries x 32 bytes = 128 MiB. This is intentionally much larger
 * than the old 64K ring because BIOS pad/card polling can burn through
 * tens of thousands of MMIO writes before a useful post-failure query. */
#define SIO_PC_TRACE_CAP (1 << 18)
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

/* Compact register sidecar for SIO_CTRL writes.  The broad SIO PC ring keeps
 * the long timeline; this smaller ring carries the CPU state needed to explain
 * BIOS chain-driver branch decisions around SELECT resets. */
#define SIO_CTRL_REG_TRACE_CAP (1 << 16)
typedef struct {
    uint64_t seq;
    uint32_t pc;
    uint32_t func;
    uint32_t value;
    uint32_t byte_seq;
    uint32_t cpu_pc;
    uint32_t ra;
    uint32_t sp;
    uint32_t v0;
    uint32_t v1;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t sr;
    uint32_t epc;
    uint32_t istat;
    uint32_t imask;
    uint8_t  width;
    uint8_t  in_exception;
    uint8_t  counter_7514;
    uint8_t  pad;
} SioCtrlRegTraceEntry;
static SioCtrlRegTraceEntry s_sio_ctrl_reg_trace[SIO_CTRL_REG_TRACE_CAP];
static uint64_t s_sio_ctrl_reg_trace_seq = 0;

/* RestoreState / exception longjmp trace.  This is intentionally compact:
 * the high-volume SIO/MMIO rings show what happened on the bus, while this
 * ring shows whether exception nonlocal control flow skipped a callback's
 * normal return-value cleanup. */
#define RESTORE_TRACE_CAP (1 << 16)
typedef struct {
    uint64_t seq;
    uint32_t kind;
    uint32_t jmp_val;
    uint32_t target_pc;
    uint32_t cpu_pc;
    uint32_t func;
    uint32_t last_store_pc;
    uint32_t byte_seq;
    uint32_t ra;
    uint32_t sp;
    uint32_t v0;
    uint32_t v1;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t s0;
    uint32_t s1;
    uint32_t sr;
    uint32_t epc;
    uint32_t istat;
    uint32_t imask;
    uint32_t frame;
    uint8_t  in_exception;
    uint8_t  pad[3];
} RestoreTraceEntry;
static RestoreTraceEntry s_restore_trace[RESTORE_TRACE_CAP];
static uint64_t s_restore_trace_seq = 0;

#define THREAD_TRACE_CAP (1 << 16)
typedef struct {
    uint64_t seq;
    uint32_t kind;
    uint32_t current_tcb;
    uint32_t target_tcb;
    uint32_t current_state;
    uint32_t target_state;
    uint32_t current_tcb_ptr;
    uint32_t target_pc;
    uint32_t func;
    uint32_t last_store_pc;
    uint32_t ra;
    uint32_t sp;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t s0;
    uint32_t s1;
    uint32_t s2;
    uint32_t s3;
    uint32_t sr;
    uint32_t epc;
    uint32_t saved_a0;
    uint32_t saved_a1;
    uint32_t saved_a2;
    uint32_t saved_a3;
    uint32_t saved_s0;
    uint32_t saved_s1;
    uint32_t saved_s2;
    uint32_t saved_s3;
    uint32_t saved_sp;
    uint32_t saved_ra;
    uint32_t saved_pc;
    uint32_t saved_sr;
    uint32_t task_ptr;
    uint32_t task_state;
    uint32_t task_mode;
    uint32_t task_submode;
    uint32_t istat;
    uint32_t imask;
    uint32_t frame;
    uint8_t  in_exception;
    uint8_t  pad[3];
} ThreadTraceEntry;
static ThreadTraceEntry s_thread_trace[THREAD_TRACE_CAP];
static uint64_t s_thread_trace_seq = 0;

#define SREG_TRACE_CAP (1 << 18)
typedef struct {
    uint64_t seq;
    uint32_t tcb;
    uint32_t func;
    uint32_t ra;
    uint32_t sp;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
    uint32_t prev_s0, prev_s1, prev_s2, prev_s3;
    uint32_t a0, a1, a2, a3;
    uint32_t stack10, stack14, stack18, stack1c;
    uint32_t stack20, stack28, stack40;
    uint32_t task_ptr;
    uint32_t task_state;
    uint32_t task_mode;
    uint32_t task_submode;
    uint32_t frame;
    uint8_t  reason;
    uint8_t  pad[3];
} SregTraceEntry;

typedef struct {
    uint32_t tcb;
    uint32_t s[8];
    int valid;
} SregLastEntry;

static SregTraceEntry s_sreg_trace[SREG_TRACE_CAP];
static uint64_t s_sreg_trace_seq = 0;
static SregLastEntry s_sreg_last[32];

#define PROBE_TRACE_CAP (1 << 16)
typedef struct {
    uint64_t seq;
    uint32_t pc;
    uint32_t func;
    uint32_t last_store_pc;
    uint32_t byte_seq;
    uint32_t ra;
    uint32_t sp;
    uint32_t v0;
    uint32_t v1;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t sr;
    uint32_t epc;
    uint32_t istat;
    uint32_t imask;
    uint32_t frame;
    uint8_t  in_exception;
    uint8_t  pad[3];
} ProbeTraceEntry;
static ProbeTraceEntry s_probe_trace[PROBE_TRACE_CAP];
static uint64_t s_probe_trace_seq = 0;

void debug_server_log_probe(uint32_t pc, CPUState *cpu)
{
#ifdef PSX_NO_DEBUG_TOOLS
    (void)pc; (void)cpu;
    return;
#else
    if (!cpu) return;
    ProbeTraceEntry *e = &s_probe_trace[s_probe_trace_seq % PROBE_TRACE_CAP];
    e->seq          = s_probe_trace_seq++;
    e->pc           = pc;
    e->func         = g_debug_current_func_addr;
    e->last_store_pc = g_debug_last_store_pc;
    e->byte_seq     = sio_get_seq();
    e->ra           = cpu->gpr[31];
    e->sp           = cpu->gpr[29];
    e->v0           = cpu->gpr[2];
    e->v1           = cpu->gpr[3];
    e->a0           = cpu->gpr[4];
    e->a1           = cpu->gpr[5];
    e->a2           = cpu->gpr[6];
    e->a3           = cpu->gpr[7];
    e->sr           = cpu->cop0[12];
    e->epc          = cpu->cop0[14];
    e->istat        = i_stat;
    e->imask        = i_mask;
    e->frame        = (uint32_t)s_frame_count;
    e->in_exception = (uint8_t)psx_get_in_exception();
#endif /* PSX_NO_DEBUG_TOOLS */
}

void debug_server_log_restore_event(uint32_t kind, uint32_t target_pc, uint32_t jmp_val)
{
#ifdef PSX_NO_DEBUG_TOOLS
    (void)kind; (void)target_pc; (void)jmp_val;
    return;
#endif
    RestoreTraceEntry *e =
        &s_restore_trace[s_restore_trace_seq % RESTORE_TRACE_CAP];
    CPUState *cpu = s_cpu;
    e->seq           = s_restore_trace_seq++;
    e->kind          = kind;
    e->jmp_val       = jmp_val;
    e->target_pc     = target_pc;
    e->cpu_pc        = cpu ? cpu->pc      : 0;
    e->func          = g_debug_current_func_addr;
    e->last_store_pc = g_debug_last_store_pc;
    e->byte_seq      = sio_get_seq();
    e->ra            = cpu ? cpu->gpr[31] : 0;
    e->sp            = cpu ? cpu->gpr[29] : 0;
    e->v0            = cpu ? cpu->gpr[2]  : 0;
    e->v1            = cpu ? cpu->gpr[3]  : 0;
    e->a0            = cpu ? cpu->gpr[4]  : 0;
    e->a1            = cpu ? cpu->gpr[5]  : 0;
    e->a2            = cpu ? cpu->gpr[6]  : 0;
    e->a3            = cpu ? cpu->gpr[7]  : 0;
    e->s0            = cpu ? cpu->gpr[16] : 0;
    e->s1            = cpu ? cpu->gpr[17] : 0;
    e->sr            = cpu ? cpu->cop0[12] : 0;
    e->epc           = cpu ? cpu->cop0[14] : 0;
    e->istat         = i_stat;
    e->imask         = i_mask;
    e->frame         = (uint32_t)s_frame_count;
    e->in_exception  = (uint8_t)psx_get_in_exception();
}

static uint32_t trace_read_word(CPUState *cpu, uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys > 0x001FFFFCu &&
        (phys < 0x1F800000u || phys > 0x1F8003FCu)) {
        return 0;
    }
    return cpu ? cpu->read_word(addr) : 0;
}

static uint32_t trace_read_half(CPUState *cpu, uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys > 0x001FFFFEu &&
        (phys < 0x1F800000u || phys > 0x1F8003FEu)) {
        return 0;
    }
    return cpu ? cpu->read_half(addr) : 0;
}

static int sreg_trace_focus_func(uint32_t func)
{
    switch (func) {
        case 0x800171D4u:
        case 0x8001A51Cu:
        case 0x8001A670u:
        case 0x8001A774u:
        case 0x8001A954u:
        case 0x8001CE80u:
        case 0x8001DE24u:
        case 0x80021C24u:
        case 0x80021CC8u:
        case 0x800222B8u:
        case 0x800223E0u:
        case 0x8005B40Cu:
            return 1;
        default:
            return 0;
    }
}

static SregLastEntry *sreg_last_slot(uint32_t tcb)
{
    int free_idx = -1;
    for (int i = 0; i < (int)(sizeof(s_sreg_last) / sizeof(s_sreg_last[0])); i++) {
        if (s_sreg_last[i].valid && s_sreg_last[i].tcb == tcb) {
            return &s_sreg_last[i];
        }
        if (!s_sreg_last[i].valid && free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        free_idx = (int)(tcb % (uint32_t)(sizeof(s_sreg_last) / sizeof(s_sreg_last[0])));
    }
    memset(&s_sreg_last[free_idx], 0, sizeof(s_sreg_last[free_idx]));
    s_sreg_last[free_idx].valid = 1;
    s_sreg_last[free_idx].tcb = tcb;
    return &s_sreg_last[free_idx];
}

static void sreg_trace_record(uint32_t func_addr)
{
    CPUState *cpu = debug_cpu_ptr;
    if (!cpu) return;

    uint32_t tcb_ptr_addr = trace_read_word(cpu, 0x00000108u);
    uint32_t tcb = tcb_ptr_addr ? trace_read_word(cpu, tcb_ptr_addr) : 0;
    uint32_t tcb_phys = tcb & 0x1FFFFFFFu;
    if (tcb == 0 || tcb_phys < 0x0000E000u || tcb_phys >= 0x0000F000u) {
        return;
    }
    int focus_func = sreg_trace_focus_func(func_addr);
    if (!focus_func) {
        return;
    }
    uint32_t task_ptr = trace_read_word(cpu, 0x1F8001D4u);
    uint32_t task_state = task_ptr ? trace_read_word(cpu, task_ptr) : 0;
    uint32_t task_mode = task_ptr ? trace_read_half(cpu, task_ptr + 72u) : 0;
    uint32_t task_submode = task_ptr ? trace_read_half(cpu, task_ptr + 74u) : 0;

    SregLastEntry *last = sreg_last_slot(tcb);
    uint32_t cur[8] = {
        cpu->gpr[16], cpu->gpr[17], cpu->gpr[18], cpu->gpr[19],
        cpu->gpr[20], cpu->gpr[21], cpu->gpr[22], cpu->gpr[23]
    };
    uint8_t reason = 0;
    if (!last->valid) {
        reason |= 1u;
    }
    for (int i = 0; i < 8; i++) {
        if (last->s[i] != cur[i]) {
            reason |= 2u;
            break;
        }
    }
    if (focus_func) {
        reason |= 4u;
    }
    if (!reason) {
        return;
    }

    SregTraceEntry *e = &s_sreg_trace[s_sreg_trace_seq % SREG_TRACE_CAP];
    e->seq = s_sreg_trace_seq++;
    e->tcb = tcb;
    e->func = func_addr;
    e->ra = cpu->gpr[31];
    e->sp = cpu->gpr[29];
    e->s0 = cur[0]; e->s1 = cur[1]; e->s2 = cur[2]; e->s3 = cur[3];
    e->s4 = cur[4]; e->s5 = cur[5]; e->s6 = cur[6]; e->s7 = cur[7];
    e->prev_s0 = last->s[0];
    e->prev_s1 = last->s[1];
    e->prev_s2 = last->s[2];
    e->prev_s3 = last->s[3];
    e->a0 = cpu->gpr[4];
    e->a1 = cpu->gpr[5];
    e->a2 = cpu->gpr[6];
    e->a3 = cpu->gpr[7];
    e->stack10 = trace_read_word(cpu, cpu->gpr[29] + 0x10u);
    e->stack14 = trace_read_word(cpu, cpu->gpr[29] + 0x14u);
    e->stack18 = trace_read_word(cpu, cpu->gpr[29] + 0x18u);
    e->stack1c = trace_read_word(cpu, cpu->gpr[29] + 0x1Cu);
    e->stack20 = trace_read_word(cpu, cpu->gpr[29] + 0x20u);
    e->stack28 = trace_read_word(cpu, cpu->gpr[29] + 0x28u);
    e->stack40 = trace_read_word(cpu, cpu->gpr[29] + 0x40u);
    e->task_ptr = task_ptr;
    e->task_state = task_state;
    e->task_mode = task_mode;
    e->task_submode = task_submode;
    e->frame = (uint32_t)s_frame_count;
    e->reason = reason;

    memcpy(last->s, cur, sizeof(cur));
}

void debug_server_log_thread_event(uint32_t kind, CPUState *cpu,
                                   uint32_t current_tcb,
                                   uint32_t target_tcb,
                                   uint32_t target_pc)
{
#ifdef PSX_NO_DEBUG_TOOLS
    (void)kind; (void)cpu; (void)current_tcb; (void)target_tcb; (void)target_pc;
    return;
#endif
    if (!cpu) return;
    ThreadTraceEntry *e =
        &s_thread_trace[s_thread_trace_seq % THREAD_TRACE_CAP];
    uint32_t tcb_ptr_addr = trace_read_word(cpu, 0x00000108u);
    uint32_t save = target_tcb + 8u;
    e->seq             = s_thread_trace_seq++;
    e->kind            = kind;
    e->current_tcb     = current_tcb;
    e->target_tcb      = target_tcb;
    e->current_state   = current_tcb ? trace_read_word(cpu, current_tcb) : 0;
    e->target_state    = target_tcb ? trace_read_word(cpu, target_tcb) : 0;
    e->current_tcb_ptr = tcb_ptr_addr ? trace_read_word(cpu, tcb_ptr_addr) : 0;
    e->target_pc       = target_pc;
    e->func            = g_debug_current_func_addr;
    e->last_store_pc   = g_debug_last_store_pc;
    e->ra              = cpu->gpr[31];
    e->sp              = cpu->gpr[29];
    e->a0              = cpu->gpr[4];
    e->a1              = cpu->gpr[5];
    e->a2              = cpu->gpr[6];
    e->a3              = cpu->gpr[7];
    e->s0              = cpu->gpr[16];
    e->s1              = cpu->gpr[17];
    e->s2              = cpu->gpr[18];
    e->s3              = cpu->gpr[19];
    e->sr              = cpu->cop0[12];
    e->epc             = cpu->cop0[14];
    e->saved_a0        = target_tcb ? trace_read_word(cpu, save + 16u) : 0;
    e->saved_a1        = target_tcb ? trace_read_word(cpu, save + 20u) : 0;
    e->saved_a2        = target_tcb ? trace_read_word(cpu, save + 24u) : 0;
    e->saved_a3        = target_tcb ? trace_read_word(cpu, save + 28u) : 0;
    e->saved_s0        = target_tcb ? trace_read_word(cpu, save + 64u) : 0;
    e->saved_s1        = target_tcb ? trace_read_word(cpu, save + 68u) : 0;
    e->saved_s2        = target_tcb ? trace_read_word(cpu, save + 72u) : 0;
    e->saved_s3        = target_tcb ? trace_read_word(cpu, save + 76u) : 0;
    e->saved_sp        = target_tcb ? trace_read_word(cpu, save + 116u) : 0;
    e->saved_ra        = target_tcb ? trace_read_word(cpu, save + 124u) : 0;
    e->saved_pc        = target_tcb ? trace_read_word(cpu, save + 128u) : 0;
    e->saved_sr        = target_tcb ? trace_read_word(cpu, save + 140u) : 0;
    e->task_ptr        = trace_read_word(cpu, 0x1F8001D4u);
    e->task_state      = e->task_ptr ? trace_read_word(cpu, e->task_ptr) : 0;
    e->task_mode       = e->task_ptr ? trace_read_half(cpu, e->task_ptr + 72u) : 0;
    e->task_submode    = e->task_ptr ? trace_read_half(cpu, e->task_ptr + 74u) : 0;
    e->istat           = i_stat;
    e->imask           = i_mask;
    e->frame           = (uint32_t)s_frame_count;
    e->in_exception    = (uint8_t)psx_get_in_exception();
}

static void debug_server_log_sio_ctrl_regs(uint32_t value, uint8_t width,
                                           uint32_t byte_seq) {
    SioCtrlRegTraceEntry *e =
        &s_sio_ctrl_reg_trace[s_sio_ctrl_reg_trace_seq % SIO_CTRL_REG_TRACE_CAP];
    CPUState *cpu = s_cpu;
    e->seq      = s_sio_ctrl_reg_trace_seq++;
    e->pc       = g_debug_last_store_pc;
    e->func     = g_debug_current_func_addr;
    e->value    = value;
    e->byte_seq = byte_seq;
    e->cpu_pc   = cpu ? cpu->pc      : 0;
    e->ra       = cpu ? cpu->gpr[31] : 0;
    e->sp       = cpu ? cpu->gpr[29] : 0;
    e->v0       = cpu ? cpu->gpr[2]  : 0;
    e->v1       = cpu ? cpu->gpr[3]  : 0;
    e->a0       = cpu ? cpu->gpr[4]  : 0;
    e->a1       = cpu ? cpu->gpr[5]  : 0;
    e->a2       = cpu ? cpu->gpr[6]  : 0;
    e->a3       = cpu ? cpu->gpr[7]  : 0;
    e->sr       = cpu ? cpu->cop0[12] : 0;
    e->epc      = cpu ? cpu->cop0[14] : 0;
    e->istat    = i_stat;
    e->imask    = i_mask;
    e->width    = width;
    e->in_exception = (uint8_t)psx_get_in_exception();
    e->counter_7514 = psx_read_byte(0x7514);
}

void debug_server_log_sio_write(uint32_t addr, uint32_t value, uint8_t width) {
#ifdef PSX_NO_DEBUG_TOOLS
    (void)addr; (void)value; (void)width;
    return;
#endif
    SioPcTraceEntry *e = &s_sio_pc_trace[s_sio_pc_trace_seq % SIO_PC_TRACE_CAP];
    uint32_t byte_seq = sio_get_seq();
    e->seq      = s_sio_pc_trace_seq++;
    e->pc       = g_debug_last_store_pc;
    e->func     = g_debug_current_func_addr;
    e->addr     = addr;
    e->value    = value;
    e->byte_seq = byte_seq;
    e->width    = width;
    if (addr == 0x1F80104Au)
        debug_server_log_sio_ctrl_regs(value, width, byte_seq);
}

/* ---- Dispatch trace ring buffer ----
 * Records every dispatched function address for post-mortem analysis.
 * 64K entries, stack-allocated (256 KB). */
#define DISPATCH_TRACE_CAP (1 << 16)
static uint32_t s_dispatch_ring[DISPATCH_TRACE_CAP];
static uint64_t s_dispatch_seq = 0;

/* ---- Unknown-dispatch ring buffer ----
 * Always-on log of every psx_dispatch target that doesn't resolve to a
 * generated function AND doesn't match any trampoline pattern in
 * traps.c. Used to identify functions the recompiler missed.
 * 64K entries × 32 bytes = 2 MB. Replaces the prior file-based log. */
#define UNKNOWN_DISPATCH_CAP (1 << 16)
typedef struct {
    uint64_t seq;
    uint32_t addr;
    uint32_t phys;
    uint32_t ra;
    uint32_t a0;
    uint32_t a1;
    uint32_t frame;
    uint32_t pad;
} UnknownDispatchEntry;
static UnknownDispatchEntry s_unknown_ring[UNKNOWN_DISPATCH_CAP];
static uint64_t s_unknown_seq = 0;

/* Crash-trace accessor: returns entry at the given seq number (modulo cap).
 * Layout matches crash_trace.c's UnknownDispatchEntry typedef. */
UnknownDispatchEntry crash_trace_unknown_get(uint64_t seq) {
    return s_unknown_ring[seq & (UNKNOWN_DISPATCH_CAP - 1u)];
}
uint64_t crash_trace_unknown_seq_get(void) { return s_unknown_seq; }
/* Per-target hit count — bounded set, ~N unique targets typically. */
#define UNKNOWN_UNIQUE_CAP 1024
typedef struct { uint32_t phys; uint64_t count; } UnknownUniqueEntry;
static UnknownUniqueEntry s_unknown_unique[UNKNOWN_UNIQUE_CAP];
static int s_unknown_unique_count = 0;

void psx_unknown_dispatch_record(uint32_t addr, uint32_t phys,
                                  uint32_t ra, uint32_t a0, uint32_t a1)
{
    uint64_t seq = s_unknown_seq++;
    UnknownDispatchEntry *e = &s_unknown_ring[seq & (UNKNOWN_DISPATCH_CAP - 1u)];
    e->seq   = seq;
    e->addr  = addr;
    e->phys  = phys;
    e->ra    = ra;
    e->a0    = a0;
    e->a1    = a1;
    e->frame = (uint32_t)s_frame_count;
    /* Per-target count (linear probe). */
    uint32_t idx = (phys >> 2) % UNKNOWN_UNIQUE_CAP;
    for (int i = 0; i < UNKNOWN_UNIQUE_CAP; i++) {
        uint32_t slot = (idx + i) % UNKNOWN_UNIQUE_CAP;
        if (s_unknown_unique[slot].phys == phys) {
            s_unknown_unique[slot].count++;
            return;
        }
        if (s_unknown_unique[slot].phys == 0) {
            s_unknown_unique[slot].phys = phys;
            s_unknown_unique[slot].count = 1;
            s_unknown_unique_count++;
            return;
        }
    }
}

/* Crash-trace accessors (used by crash_trace.c). The unknown-ring
 * accessor is defined later in this file, after UnknownDispatchEntry. */
uint32_t crash_trace_dispatch_ring_get(int idx) {
    return s_dispatch_ring[idx & (DISPATCH_TRACE_CAP - 1)];
}
uint64_t crash_trace_dispatch_seq_get(void) { return s_dispatch_seq; }

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
#define FN_TRACE_CAP        (1 << 18)
#define FN_EXIT_TRACE_CAP   (1 << 18)
#define FN_STACK_DEPTH 4096

typedef struct {
    uint64_t seq;
    uint64_t paired_exit_seq; /* set later when matching exit recorded; 0 = open */
    uint32_t func_addr;
    uint32_t ra;
    uint32_t a0, a1, a2, a3;
    uint32_t t1;              /* B0/A0/C0 function index when target is BIOS vector */
    uint32_t s0, s1, s2, s3;
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

/* Focused menu/render manager call ring. This is always-on, but deliberately
 * narrow: it records the Tomba title-menu and render-object manager functions
 * involved in NEW GAME / LOAD GAME / OPTIONS transitions. The generic function
 * ring is either filtered or too hot; this ring preserves the boot-to-menu
 * call history without one-shot arming. */
#define CALL_FOCUS_CAP (1 << 20)
typedef struct {
    uint64_t seq;
    uint32_t func_addr;
    uint32_t ra;
    uint32_t pc;
    uint32_t frame;
    uint32_t sp;
    uint32_t v0, v1;
    uint32_t a0, a1, a2, a3;
    uint32_t t0, t1;
    uint32_t s0, s1, s2, s3;
    uint32_t stk10, stk14, stk18, stk20, stk40;
    uint32_t obj;
    uint32_t obj_10;
    uint32_t obj_14;
    uint32_t obj_18;
    uint32_t obj_30;
    uint32_t obj_30_0;
    uint32_t obj_30_1;
    uint32_t obj_34;
    uint32_t obj_35;
    uint32_t obj_36;
    uint32_t obj_37;
    uint32_t obj_38;
    uint32_t obj_3c;
    uint32_t obj_40;
    uint32_t obj_44;
    uint32_t obj_45;
    uint32_t obj_46;
    uint32_t obj_49;
    uint32_t obj_4a;
    uint32_t obj_50;
    uint32_t obj_e0;
    uint32_t obj_e3;
    uint32_t obj_e4;
    uint32_t obj_e5;
    uint32_t obj_e6;
    uint32_t obj_e8;
    uint32_t obj_e9;
    uint32_t obj_ea;
} CallFocusEntry;
static CallFocusEntry *s_call_focus = NULL;
static uint64_t s_call_focus_seq = 0;

/* Narrow card-manager trace. The generic function-entry ring is too hot
 * during Tomba title/menu polling, so it rotates away the card-read setup
 * before we can inspect a later hang. This ring records only the BIOS public
 * card state machine and the low-level RAM card service boundary. */
#define CARD_MGR_TRACE_CAP 8192
typedef struct {
    uint64_t seq;
    uint32_t func_addr;
    uint32_t ra;
    uint32_t pc;
    uint32_t a0, a1, a2, a3;
    uint32_t v0, t0, t1;
    uint32_t frame;
    uint32_t state_9f20;
    uint32_t state_9f24;
    uint32_t state_9f28;
    uint32_t state_9f2c;
    uint32_t state_9f30;
    uint32_t state_9f34;
    uint32_t state_7258;
    uint32_t state_725c;
    uint32_t state_7264;
    uint32_t state_74bc;
    uint32_t state_7500;
    uint32_t state_7504;
    uint32_t state_7508;
    uint32_t state_750c;
    uint32_t state_7510;
    uint32_t state_7514;
    uint32_t state_7518;
    uint32_t state_751c;
    uint32_t state_7520;
    uint32_t state_7528;
    uint32_t state_752c;
    uint32_t state_7558;
    uint32_t state_7568;
    uint32_t state_756c;
    uint8_t  source; /* 0 = direct entry hook, 1 = dispatch hook */
    uint8_t  pad[3];
} CardMgrTraceEntry;
static CardMgrTraceEntry s_card_mgr_trace[CARD_MGR_TRACE_CAP];
static uint64_t s_card_mgr_trace_seq = 0;

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
 * DeliverEvent (RAM 0x1B44). EvCB base ptr lives at RAM 0x124, total
 * bytes at RAM 0x120 (per disasm InitEvents at BFC04678).
 *
 * Per-entry tag identifies whether the snapshot was taken at DeliverEvent
 * entry or exit; pairing them lets the operator see exactly which entries
 * changed status (FIRED) during one DeliverEvent call.
 *
 * Memory: 256 snapshots × ~960B = ~240 KB. */
#define EVCB_DELIVER_EVENT_ADDR 0x00001B44u  /* func_00001B44, RAM */
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

/* Optional [lo, hi) physical-address filter for the fn_entry/fn_exit rings.
 * Default disabled — recording every dispatch's args burns ~10 us per call
 * (stack walk + 9-field write) and crushes wall-clock simulation rate.
 * Enable via debug-server `fntrace_arm_filter` (sets lo/hi); investigators
 * pick a tight range so the per-dispatch cost only fires for code under
 * inspection. */
static int      s_fn_trace_active    = 0;
static uint32_t s_fn_trace_filter_lo = 0u;
static uint32_t s_fn_trace_filter_hi = 0xFFFFFFFFu;
static uint64_t s_fn_direct_seen = 0;
static uint64_t s_fn_direct_no_cpu = 0;
static uint64_t s_fn_direct_filtered = 0;

static int fn_trace_in_filter(uint32_t phys) {
    if (!s_fn_trace_active) return 0;
    return phys >= s_fn_trace_filter_lo && phys < s_fn_trace_filter_hi;
}

static void fn_trace_filter_from_env(const char *env_name) {
    const char *spec = getenv(env_name);
    if (!spec || !*spec) return;

    char *end = NULL;
    uint32_t lo = (uint32_t)strtoul(spec, &end, 0);
    if (end == spec) return;

    while (*end == ' ' || *end == '\t') end++;
    if (*end != ':' && *end != '-' && *end != ',' && *end != ';') return;
    end++;

    while (*end == ' ' || *end == '\t') end++;
    char *end2 = NULL;
    uint32_t hi = (uint32_t)strtoul(end, &end2, 0);
    if (end2 == end || hi <= lo) return;

    s_fn_trace_filter_lo = lo;
    s_fn_trace_filter_hi = hi;
    s_fn_trace_active = 1;
}

static int card_mgr_trace_target(uint32_t phys) {
    phys &= 0x1FFFFFFFu;
    if (phys >= 0x1FC0B600u && phys < 0x1FC0C240u) return 1;
    if (phys >= 0x00004900u && phys < 0x00006C80u) return 1;
    if (phys == 0x00001B44u) return 1;
    return 0;
}

static void card_mgr_trace_record(uint32_t func_addr, uint8_t source) {
    if (!debug_cpu_ptr) return;
    uint32_t phys = func_addr & 0x1FFFFFFFu;
    if (!card_mgr_trace_target(phys)) return;

    CardMgrTraceEntry *e = &s_card_mgr_trace[s_card_mgr_trace_seq % CARD_MGR_TRACE_CAP];
    e->seq       = s_card_mgr_trace_seq++;
    e->func_addr = phys;
    e->ra        = debug_cpu_ptr->gpr[31];
    e->pc        = debug_cpu_ptr->pc;
    e->a0        = debug_cpu_ptr->gpr[4];
    e->a1        = debug_cpu_ptr->gpr[5];
    e->a2        = debug_cpu_ptr->gpr[6];
    e->a3        = debug_cpu_ptr->gpr[7];
    e->v0        = debug_cpu_ptr->gpr[2];
    e->t0        = debug_cpu_ptr->gpr[8];
    e->t1        = debug_cpu_ptr->gpr[9];
    e->frame     = (uint32_t)s_frame_count;
    e->state_9f20 = psx_read_word(0x00009F20u);
    e->state_9f24 = psx_read_word(0x00009F24u);
    e->state_9f28 = psx_read_word(0x00009F28u);
    e->state_9f2c = psx_read_word(0x00009F2Cu);
    e->state_9f30 = psx_read_word(0x00009F30u);
    e->state_9f34 = psx_read_word(0x00009F34u);
    e->state_7258 = psx_read_word(0x00007258u);
    e->state_725c = psx_read_word(0x0000725Cu);
    e->state_7264 = psx_read_word(0x00007264u);
    e->state_74bc = psx_read_word(0x000074BCu);
    e->state_7500 = psx_read_word(0x00007500u);
    e->state_7504 = psx_read_word(0x00007504u);
    e->state_7508 = psx_read_word(0x00007508u);
    e->state_750c = psx_read_word(0x0000750Cu);
    e->state_7510 = psx_read_word(0x00007510u);
    e->state_7514 = psx_read_word(0x00007514u);
    e->state_7518 = psx_read_word(0x00007518u);
    e->state_751c = psx_read_word(0x0000751Cu);
    e->state_7520 = psx_read_word(0x00007520u);
    e->state_7528 = psx_read_word(0x00007528u);
    e->state_752c = psx_read_word(0x0000752Cu);
    e->state_7558 = psx_read_word(0x00007558u);
    e->state_7568 = psx_read_word(0x00007568u);
    e->state_756c = psx_read_word(0x0000756Cu);
    e->source    = source;
}

static int call_focus_target(uint32_t func)
{
    switch (func) {
        /* Title/menu state and input paths. */
        case 0x80028638u:
        case 0x80028728u:
        case 0x80028794u:
        case 0x800287F8u:
        case 0x800288C4u:
        case 0x80028A74u:
        case 0x80028B34u:
        case 0x80028CE4u:
        case 0x80028D70u:
        case 0x80028EF4u:
        /* Render-object manager and parser paths. */
        case 0x80068AA8u:
        case 0x80068DDCu:
        case 0x80068E5Cu:
        case 0x800694FCu:
        case 0x80069818u:
        case 0x8006995Cu:
        case 0x800699D0u:
        case 0x80069AC8u:
        case 0x80069B4Cu:
        case 0x80069C98u:
        case 0x80069CD0u:
        case 0x8006A0C0u:
        case 0x8006A128u:
        case 0x8006A144u:
        case 0x8006A378u:
        case 0x8006A38Cu:
        case 0x8006A3CCu:
        case 0x8006AB4Cu:
        case 0x8006ACA8u:
        case 0x8006ACB8u:
        case 0x8006AD74u:
        case 0x8006AFF0u:
        case 0x8006B028u:
        case 0x8006B080u:
        case 0x8006B154u:
        case 0x8006B3B8u:
        case 0x8006B494u:
            return 1;
        default:
            return 0;
    }
}

static uint32_t dbg_read_u16_phys(uint32_t phys)
{
    return (uint32_t)psx_read_byte(phys)
        | ((uint32_t)psx_read_byte(phys + 1u) << 8);
}

static uint32_t call_focus_object_ptr(uint32_t func, CPUState *cpu)
{
    if (!cpu) return 0;
    switch (func) {
        case 0x80068AA8u:
        case 0x80068DDCu:
        case 0x80068E5Cu:
            return 0x8009B3A0u + ((cpu->gpr[4] & 0x00F0u) ? 0xF0u : 0u);
        case 0x8006A0C0u:
        case 0x8006A128u:
        case 0x8006A144u:
        case 0x8006A378u:
        case 0x8006A38Cu:
        case 0x8006A3CCu:
        case 0x8006AB4Cu:
        case 0x8006ACA8u:
        case 0x8006ACB8u:
        case 0x8006AD74u:
        case 0x8006B080u:
        case 0x8006B154u:
        case 0x8006B3B8u:
        case 0x8006B494u:
        case 0x800694FCu:
        case 0x80069818u:
        case 0x8006995Cu:
        case 0x800699D0u:
        case 0x80069AC8u:
        case 0x80069B4Cu:
        case 0x80069C98u:
        case 0x80069CD0u:
            return cpu->gpr[4];
        default:
            return 0;
    }
}

static void call_focus_record(uint32_t func_addr)
{
    CPUState *cpu = debug_cpu_ptr;
    if (!s_call_focus || !cpu) return;
    if (!call_focus_target(func_addr)) return;

    CallFocusEntry *e = &s_call_focus[s_call_focus_seq % CALL_FOCUS_CAP];
    e->seq       = s_call_focus_seq++;
    e->func_addr = func_addr;
    e->ra        = cpu->gpr[31];
    e->pc        = cpu->pc;
    e->frame     = (uint32_t)s_frame_count;
    e->sp        = cpu->gpr[29];
    e->v0        = cpu->gpr[2];
    e->v1        = cpu->gpr[3];
    e->a0        = cpu->gpr[4];
    e->a1        = cpu->gpr[5];
    e->a2        = cpu->gpr[6];
    e->a3        = cpu->gpr[7];
    e->t0        = cpu->gpr[8];
    e->t1        = cpu->gpr[9];
    e->s0        = cpu->gpr[16];
    e->s1        = cpu->gpr[17];
    e->s2        = cpu->gpr[18];
    e->s3        = cpu->gpr[19];
    e->stk10     = trace_read_word(cpu, cpu->gpr[29] + 0x10u);
    e->stk14     = trace_read_word(cpu, cpu->gpr[29] + 0x14u);
    e->stk18     = trace_read_word(cpu, cpu->gpr[29] + 0x18u);
    e->stk20     = trace_read_word(cpu, cpu->gpr[29] + 0x20u);
    e->stk40     = trace_read_word(cpu, cpu->gpr[29] + 0x40u);
    e->obj       = 0;
    e->obj_10    = 0;
    e->obj_14    = 0;
    e->obj_18    = 0;
    e->obj_30    = 0;
    e->obj_30_0  = 0;
    e->obj_30_1  = 0;
    e->obj_34    = 0;
    e->obj_35    = 0;
    e->obj_36    = 0;
    e->obj_37    = 0;
    e->obj_38    = 0;
    e->obj_3c    = 0;
    e->obj_40    = 0;
    e->obj_44    = 0;
    e->obj_45    = 0;
    e->obj_46    = 0;
    e->obj_49    = 0;
    e->obj_4a    = 0;
    e->obj_50    = 0;
    e->obj_e0    = 0;
    e->obj_e3    = 0;
    e->obj_e4    = 0;
    e->obj_e5    = 0;
    e->obj_e6    = 0;
    e->obj_e8    = 0;
    e->obj_e9    = 0;
    e->obj_ea    = 0;

    uint32_t obj = call_focus_object_ptr(func_addr, cpu);
    uint32_t phys = obj & 0x1FFFFFFFu;
    if (phys >= 0x0009B300u && phys < 0x0009B700u) {
        e->obj       = obj;
        e->obj_10    = psx_read_word(phys + 0x10u);
        e->obj_14    = psx_read_word(phys + 0x14u);
        e->obj_18    = psx_read_word(phys + 0x18u);
        e->obj_30    = psx_read_word(phys + 0x30u);
        uint32_t out_phys = e->obj_30 & 0x1FFFFFFFu;
        if (out_phys < 0x00200000u - 1u) {
            e->obj_30_0 = psx_read_byte(out_phys);
            e->obj_30_1 = psx_read_byte(out_phys + 1u);
        }
        e->obj_34    = psx_read_byte(phys + 0x34u);
        e->obj_35    = psx_read_byte(phys + 0x35u);
        e->obj_36    = psx_read_byte(phys + 0x36u);
        e->obj_37    = psx_read_byte(phys + 0x37u);
        e->obj_38    = psx_read_byte(phys + 0x38u);
        e->obj_3c    = psx_read_word(phys + 0x3Cu);
        e->obj_40    = psx_read_word(phys + 0x40u);
        e->obj_44    = psx_read_byte(phys + 0x44u);
        e->obj_45    = psx_read_byte(phys + 0x45u);
        e->obj_46    = psx_read_byte(phys + 0x46u);
        e->obj_49    = psx_read_byte(phys + 0x49u);
        e->obj_4a    = psx_read_byte(phys + 0x4Au);
        e->obj_50    = psx_read_byte(phys + 0x50u);
        e->obj_e0    = psx_read_byte(phys + 0xE0u);
        e->obj_e3    = psx_read_byte(phys + 0xE3u);
        e->obj_e4    = psx_read_byte(phys + 0xE4u);
        e->obj_e5    = psx_read_byte(phys + 0xE5u);
        e->obj_e6    = dbg_read_u16_phys(phys + 0xE6u);
        e->obj_e8    = psx_read_byte(phys + 0xE8u);
        e->obj_e9    = psx_read_byte(phys + 0xE9u);
        e->obj_ea    = psx_read_byte(phys + 0xEAu);
    }
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
    if (!s_fn_trace_active) return;
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
        e->s0         = debug_cpu_ptr->gpr[16];
        e->s1         = debug_cpu_ptr->gpr[17];
        e->s2         = debug_cpu_ptr->gpr[18];
        e->s3         = debug_cpu_ptr->gpr[19];
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
#ifdef PSX_NO_DEBUG_TOOLS
    /* Hottest call site in the binary — called at the top of every
     * recompiled function. Early-return makes it a 1-instruction
     * function-call cost; the compiler will likely PLT it through. */
    (void)func_addr;
    return;
#endif
    s_fn_direct_seen++;
    if (!debug_cpu_ptr) {
        s_fn_direct_no_cpu++;
        return;
    }
    card_mgr_trace_record(func_addr, 0);
    sreg_trace_record(func_addr);
    call_focus_record(func_addr);
    if (!s_fn_entry) return;
    if (!fn_trace_in_filter(func_addr)) return;
    s_fn_direct_filtered++;
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
    e->s0              = debug_cpu_ptr->gpr[16];
    e->s1              = debug_cpu_ptr->gpr[17];
    e->s2              = debug_cpu_ptr->gpr[18];
    e->s3              = debug_cpu_ptr->gpr[19];
    e->depth           = (uint32_t)s_fn_stack_top;
    e->frame           = (uint32_t)s_frame_count;
    s_fn_entry_seq++;
}

/* Always-on A0/B0/C0 BIOS-call ring (ported from ape-fw for good-vs-bad
 * event-delivery comparison). Recorded at the central dispatch chokepoint. */
#define BIOSCALL_RING_CAP (1 << 16)
typedef struct {
    uint64_t seq; uint32_t table_base; uint32_t index; uint32_t func_ptr;
    uint32_t a0, a1, a2, a3; uint32_t ra; uint32_t current_func; uint32_t frame;
    uint8_t in_exception;
} BiosCallEntry;
static BiosCallEntry s_bioscall_ring[BIOSCALL_RING_CAP];
static uint64_t s_bioscall_seq = 0;
#define BIOSCALL_UNIQUE_CAP 2048
typedef struct { uint32_t table_base; uint32_t index; uint64_t count; } BiosCallUnique;
static BiosCallUnique s_bioscall_unique[BIOSCALL_UNIQUE_CAP];
static int s_bioscall_unique_count = 0;
void psx_bioscall_record(uint32_t table_base, uint32_t index, uint32_t func_ptr,
                         uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t ra)
{
    uint64_t seq = s_bioscall_seq++;
    BiosCallEntry *e = &s_bioscall_ring[seq & (BIOSCALL_RING_CAP - 1u)];
    e->seq = seq; e->table_base = table_base; e->index = index; e->func_ptr = func_ptr;
    e->a0 = a0; e->a1 = a1; e->a2 = a2; e->a3 = a3; e->ra = ra;
    e->current_func = g_debug_current_func_addr; e->frame = (uint32_t)s_frame_count;
    e->in_exception = (uint8_t)(psx_get_in_exception() ? 1u : 0u);
    uint32_t h = ((table_base >> 2) ^ (index * 2654435761u)) % BIOSCALL_UNIQUE_CAP;
    for (int i = 0; i < BIOSCALL_UNIQUE_CAP; i++) {
        uint32_t slot = (h + i) % BIOSCALL_UNIQUE_CAP;
        if (s_bioscall_unique[slot].count != 0 &&
            s_bioscall_unique[slot].table_base == table_base &&
            s_bioscall_unique[slot].index == index) { s_bioscall_unique[slot].count++; return; }
        if (s_bioscall_unique[slot].count == 0) {
            s_bioscall_unique[slot].table_base = table_base; s_bioscall_unique[slot].index = index;
            s_bioscall_unique[slot].count = 1; s_bioscall_unique_count++; return;
        }
    }
}

void debug_server_trace_dispatch(uint32_t func_addr) {
#ifdef PSX_NO_DEBUG_TOOLS
    (void)func_addr;
    return;
#endif
    card_mgr_trace_record(func_addr, 1);

    {
        uint32_t vphys = func_addr & 0x1FFFFFFFu;
        if ((vphys == 0xA0u || vphys == 0xB0u || vphys == 0xC0u) && debug_cpu_ptr) {
            psx_bioscall_record(vphys, debug_cpu_ptr->gpr[9], 0,
                                debug_cpu_ptr->gpr[4], debug_cpu_ptr->gpr[5],
                                debug_cpu_ptr->gpr[6], debug_cpu_ptr->gpr[7],
                                debug_cpu_ptr->gpr[31]);
        }
    }

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
static void handle_dirty_ram_unsupported(int id, const char *json);
static void send_err(int id, const char *msg);
static void send_ok(int id);
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
    extern uint64_t g_dirty_ram_guard_yields;
    extern uint32_t dirty_ram_get_bitmap(void);
    extern uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);
    extern uint32_t dirty_ram_get_bitmap_word_count(void);
    (void)json;

    char buf[16 * 1024];
    int n = snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"ok\":true,\"blocks_run\":%llu,"
             "\"insns_run\":%llu,\"aborts\":%llu,"
             "\"guard_yields\":%llu,"
             "\"dirty_bitmap\":\"0x%08X\",\"per_pc\":[",
             id,
             (unsigned long long)g_dirty_ram_blocks_run,
             (unsigned long long)g_dirty_ram_insns_run,
             (unsigned long long)g_dirty_ram_aborts,
             (unsigned long long)g_dirty_ram_guard_yields,
             (unsigned)dirty_ram_get_bitmap());

    int first = 1;
    for (int i = 0; i < DIRTY_RAM_PC_TABLE_SIZE; i++) {
        DirtyRamPcEntry *e = &g_dirty_ram_pc_table[i];
        if (e->pc == 0 || e->hits == 0) continue;
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"pc\":\"0x%08X\",\"hits\":%llu,\"insns\":%llu,"
                      "\"entries\":%llu}",
                      first ? "" : ",",
                      (unsigned)e->pc,
                      (unsigned long long)e->hits,
                      (unsigned long long)e->insns,
                      (unsigned long long)e->entry_hits);
        first = 0;
        if (n >= (int)sizeof(buf) - 128) break;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "],\"dirty_bitmap_words\":[");
    uint32_t word_count = dirty_ram_get_bitmap_word_count();
    for (uint32_t i = 0; i < word_count; i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s\"0x%08X\"",
                      i == 0 ? "" : ",",
                      (unsigned)dirty_ram_get_bitmap_word(i));
        if (n >= (int)sizeof(buf) - 64) break;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}\n");
    send_fmt("%s", buf);
}

static void handle_dirty_ram_unsupported(int id, const char *json)
{
    (void)json;
    const char *reason = g_dirty_ram_last_unsupported_reason;
    if (!reason) reason = "";
    send_fmt("{\"id\":%d,\"ok\":true,\"aborts\":%llu,"
             "\"midblock\":%llu,\"entry\":\"0x%08X\","
             "\"entry_ra\":\"0x%08X\",\"entry_sp\":\"0x%08X\","
             "\"entry_insns\":%u,\"last_pc\":\"0x%08X\","
             "\"last_insn\":\"0x%08X\",\"reason\":\"%s\"}\n",
             id,
             (unsigned long long)g_dirty_ram_aborts,
             (unsigned long long)g_dirty_ram_unsupported_midblock,
             (unsigned)g_dirty_ram_last_unsupported_entry,
             (unsigned)g_dirty_ram_last_unsupported_entry_ra,
             (unsigned)g_dirty_ram_last_unsupported_entry_sp,
             (unsigned)g_dirty_ram_last_unsupported_insns,
             (unsigned)g_dirty_ram_last_unsupported_pc,
             (unsigned)g_dirty_ram_last_unsupported_insn,
             reason);
}

/* ---- Dirty-RAM block-entry log: dump (target,ra,frame) tuples to find
 * the caller of any RAM-installed stub. Optional target_lo/target_hi
 * filters the response to a target-PC range; with no filter, dumps the
 * most recent `count` entries (default 256, max DIRTY_RAM_BLOCK_LOG_CAP). */
static void handle_dirty_block_log(int id, const char *json)
{
    char buf[32];
    uint32_t target_lo = 0, target_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "target_lo", buf, sizeof(buf))) target_lo = hex_to_u32(buf);
    if (json_get_str(json, "target_hi", buf, sizeof(buf))) target_hi = hex_to_u32(buf);
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > (int)DIRTY_RAM_BLOCK_LOG_CAP) count = DIRTY_RAM_BLOCK_LOG_CAP;

    uint64_t total = g_dirty_ram_block_log_seq;
    uint64_t avail = (total < DIRTY_RAM_BLOCK_LOG_CAP) ? total : DIRTY_RAM_BLOCK_LOG_CAP;
    uint64_t scan_start = (total > avail) ? (total - avail) : 0;

    /* Generous response buffer — 16K log entries * ~96 chars/entry < 2 MB. */
    const size_t BUF_SZ = 4 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ);
    if (!out) {
        send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"oom\"}\n", id);
        return;
    }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"target_lo\":\"0x%08X\",\"target_hi\":\"0x%08X\",\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail,
                    target_lo, target_hi);
    int emitted = 0;
    /* Walk newest-first so callers naturally get the latest dispatches.
     * Stop once we've emitted `count` matches or scanned the whole window. */
    for (uint64_t i = 0; i < avail && emitted < count; i++) {
        uint64_t seq = total - 1 - i;
        DirtyRamBlockLogEntry *e =
            &g_dirty_ram_block_log[seq & (DIRTY_RAM_BLOCK_LOG_CAP - 1u)];
        if (e->target < target_lo || e->target >= target_hi) continue;
        if (pos > BUF_SZ - 256) break;
        pos += snprintf(out + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"target\":\"0x%08X\","
                        "\"ra\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"sp\":\"0x%08X\","
                        "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\",\"t2\":\"0x%08X\","
                        "\"frame\":%u}",
                        emitted == 0 ? "" : ",",
                        (unsigned long long)e->seq,
                        e->target, e->ra, e->a0, e->a1, e->a2, e->a3,
                        e->sp, e->t0, e->t1, e->t2, e->frame);
        emitted++;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "],\"emitted\":%d}\n", emitted);
    debug_server_send_line(out);
    free(out);
}

static void handle_dirty_flow_log(int id, const char *json)
{
    char buf[32];
    uint32_t target_lo = 0, target_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "target_lo", buf, sizeof(buf))) target_lo = hex_to_u32(buf);
    if (json_get_str(json, "target_hi", buf, sizeof(buf))) target_hi = hex_to_u32(buf);
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > (int)DIRTY_RAM_FLOW_LOG_CAP) count = DIRTY_RAM_FLOW_LOG_CAP;

    uint64_t total = g_dirty_ram_flow_log_seq;
    uint64_t avail = (total < DIRTY_RAM_FLOW_LOG_CAP) ? total : DIRTY_RAM_FLOW_LOG_CAP;
    const size_t BUF_SZ = 2 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ);
    if (!out) {
        send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"oom\"}\n", id);
        return;
    }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"target_lo\":\"0x%08X\",\"target_hi\":\"0x%08X\",\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail,
                    target_lo, target_hi);
    int emitted = 0;
    for (uint64_t i = 0; i < avail && emitted < count; i++) {
        uint64_t seq = total - 1 - i;
        DirtyRamFlowLogEntry *e =
            &g_dirty_ram_flow_log[seq & (DIRTY_RAM_FLOW_LOG_CAP - 1u)];
        if (e->target < target_lo || e->target >= target_hi) continue;
        if (pos > BUF_SZ - 256) break;
        pos += snprintf(out + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"pc\":\"0x%08X\",\"target\":\"0x%08X\","
                        "\"ra\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"sp\":\"0x%08X\","
                        "\"frame\":%u}",
                        emitted == 0 ? "" : ",",
                        (unsigned long long)e->seq, e->pc, e->target,
                        e->ra, e->a0, e->a1, e->a2, e->a3, e->sp, e->frame);
        emitted++;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "],\"emitted\":%d}\n", emitted);
    debug_server_send_line(out);
    free(out);
}

static void handle_dirty_insn_log(int id, const char *json)
{
    char buf[32];
    uint32_t pc_lo = 0, pc_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "pc_lo", buf, sizeof(buf))) pc_lo = hex_to_u32(buf);
    if (json_get_str(json, "pc_hi", buf, sizeof(buf))) pc_hi = hex_to_u32(buf);
    int changed_only = json_get_int(json, "changed_only", 0);
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > (int)DIRTY_RAM_INSN_LOG_CAP) count = DIRTY_RAM_INSN_LOG_CAP;

    uint64_t total = g_dirty_ram_insn_log_seq;
    uint64_t avail = (total < DIRTY_RAM_INSN_LOG_CAP) ? total : DIRTY_RAM_INSN_LOG_CAP;
    const size_t BUF_SZ = 16 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ);
    if (!out) {
        send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"oom\"}\n", id);
        return;
    }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"pc_lo\":\"0x%08X\",\"pc_hi\":\"0x%08X\",\"changed_only\":%d,"
                    "\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail,
                    pc_lo, pc_hi, changed_only ? 1 : 0);
    int emitted = 0;
    for (uint64_t i = 0; i < avail && emitted < count; i++) {
        uint64_t seq = total - 1 - i;
        DirtyRamInsnLogEntry *e =
            &g_dirty_ram_insn_log[seq & (DIRTY_RAM_INSN_LOG_CAP - 1u)];
        if (e->pc < pc_lo || e->pc >= pc_hi) continue;
        if (changed_only && e->before_s0 == e->after_s0) continue;
        if (pos > BUF_SZ - 768) break;
        pos += snprintf(out + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"pc\":\"0x%08X\",\"insn\":\"0x%08X\","
                        "\"next_pc\":\"0x%08X\",\"target\":\"0x%08X\","
                        "\"before_s0\":\"0x%08X\",\"after_s0\":\"0x%08X\","
                        "\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
                        "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\","
                        "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                        "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\","
                        "\"t2\":\"0x%08X\","
                        "\"current_tcb\":\"0x%08X\",\"task_ptr\":\"0x%08X\","
                        "\"task_mode\":\"0x%08X\",\"task_submode\":\"0x%08X\","
                        "\"frame\":%u,\"transferred\":%u}",
                        emitted == 0 ? "" : ",",
                        (unsigned long long)e->seq,
                        e->pc, e->insn, e->next_pc, e->target,
                        e->before_s0, e->after_s0, e->sp, e->ra,
                        e->v0, e->v1, e->a0, e->a1, e->a2, e->a3,
                        e->t0, e->t1, e->t2,
                        e->current_tcb, e->task_ptr, e->task_mode,
                        e->task_submode, e->frame, (unsigned)e->transferred);
        emitted++;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "],\"emitted\":%d}\n", emitted);
    debug_server_send_line(out);
    free(out);
}

/* dirty_block_dump_file: write the WHOLE live block-log window (oldest->newest)
 * to a file. Inline dumps above ~2MB wedge the single-threaded server (seen
 * twice); file dumps are the only safe path for full-ring extraction. */
static void handle_dirty_block_dump_file(int id, const char *json)
{
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        snprintf(path, sizeof(path), "dirty_block_log.json");
    FILE *f = fopen(path, "w");
    if (!f) { send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"open failed\"}\n", id); return; }
    uint64_t total = g_dirty_ram_block_log_seq;
    uint64_t avail = (total < DIRTY_RAM_BLOCK_LOG_CAP) ? total : DIRTY_RAM_BLOCK_LOG_CAP;
    fputc('[', f);
    int first = 1, count = 0;
    for (uint64_t s = total - avail; s < total; s++) {
        DirtyRamBlockLogEntry *e =
            &g_dirty_ram_block_log[s & (DIRTY_RAM_BLOCK_LOG_CAP - 1u)];
        fprintf(f,
            "%s{\"seq\":%llu,\"target\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
            "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\",\"t2\":\"0x%08X\","
            "\"sp\":\"0x%08X\",\"frame\":%u}",
            first ? "" : ",\n",
            (unsigned long long)e->seq, e->target, e->ra,
            e->a0, e->a1, e->a2, e->a3, e->t0, e->t1, e->t2,
            e->sp, e->frame);
        first = 0; count++;
    }
    fputs("]\n", f);
    fclose(f);
    send_fmt("{\"id\":%d,\"ok\":true,\"file\":\"%s\",\"entries\":%d}\n",
             id, path, count);
}

/* dirty_insn_dump_file: write the WHOLE live insn-log window (oldest->newest)
 * as a JSON array to a file. The inline dirty_insn_log path serializes into a
 * bounded TCP buffer and wedges the server on multi-MB dumps — file dumps have
 * no size limit and leave the server responsive. */
static void handle_dirty_insn_dump_file(int id, const char *json)
{
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        snprintf(path, sizeof(path), "dirty_insn_log.json");
    FILE *f = fopen(path, "w");
    if (!f) { send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"open failed\"}\n", id); return; }
    uint64_t total = g_dirty_ram_insn_log_seq;
    uint64_t avail = (total < DIRTY_RAM_INSN_LOG_CAP) ? total : DIRTY_RAM_INSN_LOG_CAP;
    fputc('[', f);
    int first = 1, count = 0;
    for (uint64_t s = total - avail; s < total; s++) {
        DirtyRamInsnLogEntry *e =
            &g_dirty_ram_insn_log[s & (DIRTY_RAM_INSN_LOG_CAP - 1u)];
        fprintf(f,
            "%s{\"seq\":%llu,\"pc\":\"0x%08X\",\"insn\":\"0x%08X\","
            "\"next_pc\":\"0x%08X\",\"target\":\"0x%08X\","
            "\"before_s0\":\"0x%08X\",\"after_s0\":\"0x%08X\","
            "\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
            "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\",\"t2\":\"0x%08X\","
            "\"frame\":%u,\"transferred\":%u}",
            first ? "" : ",\n",
            (unsigned long long)e->seq,
            e->pc, e->insn, e->next_pc, e->target,
            e->before_s0, e->after_s0, e->sp, e->ra,
            e->v0, e->v1, e->a0, e->a1, e->a2, e->a3,
            e->t0, e->t1, e->t2, e->frame, (unsigned)e->transferred);
        first = 0; count++;
    }
    fputs("]\n", f);
    fclose(f);
    send_fmt("{\"id\":%d,\"ok\":true,\"file\":\"%s\",\"entries\":%d}\n",
             id, path, count);
}

/* ---- fntrace: always-on psx_dispatch call ring ----
 * Mirrors beetle_libretro.cpp's fntrace; covers every static-recomp +
 * dirty-RAM dispatch on this side. Use to find indirect callers, walk
 * argument-passing chains, and answer "who called X with what args"
 * across processes (psx-runtime port 4370, psx-beetle port 4380). */
#include "fntrace.h"

static void handle_fntrace_arm(int id, const char *json)
{
    char buf[32];
    if (!json_get_str(json, "target", buf, sizeof(buf))) {
        send_err(id, "missing target"); return;
    }
    uint32_t target = hex_to_u32(buf);
    if (target == 0) { fntrace_arm_clear(); send_ok(id); return; }
    fntrace_arm(target);
    send_fmt("{\"id\":%d,\"ok\":true,\"target\":\"0x%08X\",\"armed\":%u}",
             id, target, fntrace_arm_count());
}

static void handle_fntrace_arm_clear(int id, const char *json)
{
    (void)json;
    fntrace_arm_clear();
    send_ok(id);
}

static void handle_fntrace_armed(int id, const char *json)
{
    (void)json;
    const size_t BUF_SZ = 2048;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t n = fntrace_arm_count();
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"count\":%u,\"targets\":[", id, n);
    for (uint32_t i = 0; i < n; i++) {
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s\"0x%08X\"", (i == 0) ? "" : ",", fntrace_arm_get(i));
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

static void handle_fntrace_clear(int id, const char *json)
{
    (void)json;
    fntrace_clear();
    send_ok(id);
}

/* fntrace_dump: paginated tail of the ring.
 * Optional filters:
 *   - target_lo / target_hi:  half-open virtual-address range filter on `target`
 *   - count: max entries to return (default 256, cap = ring size)
 * Walks newest-first; emits up to `count` matches.  When no filter is
 * given, returns the most recent `count` entries verbatim. */
static void handle_fntrace_dump(int id, const char *json)
{
    char buf[32];
    uint32_t target_lo = 0, target_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "target_lo", buf, sizeof(buf))) target_lo = hex_to_u32(buf);
    if (json_get_str(json, "target_hi", buf, sizeof(buf))) target_hi = hex_to_u32(buf);
    uint64_t seq_lo = 0, seq_hi = 0;
    int have_seq_window = 0;
    if (json_get_str(json, "seq_lo", buf, sizeof(buf))) {
        seq_lo = strtoull(buf, NULL, 0);
        have_seq_window = 1;
    }
    if (json_get_str(json, "seq_hi", buf, sizeof(buf))) {
        seq_hi = strtoull(buf, NULL, 0);
        have_seq_window = 1;
    }
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > (int)FNTRACE_RING_CAP) count = FNTRACE_RING_CAP;

    uint64_t total = g_fntrace_seq;
    uint64_t avail = (total < FNTRACE_RING_CAP) ? total : FNTRACE_RING_CAP;
    uint64_t oldest = total - avail;
    if (have_seq_window) {
        if (seq_hi == 0 || seq_hi > total) seq_hi = total;
        if (seq_lo < oldest) seq_lo = oldest;
        if (seq_lo > seq_hi) seq_lo = seq_hi;
    }

    const size_t BUF_SZ = 4 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ);
    if (!out) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"seq_lo\":%llu,\"seq_hi\":%llu,"
                    "\"target_lo\":\"0x%08X\",\"target_hi\":\"0x%08X\","
                    "\"armed\":%u,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail,
                    (unsigned long long)(have_seq_window ? seq_lo : 0),
                    (unsigned long long)(have_seq_window ? seq_hi : 0),
                    target_lo, target_hi, fntrace_arm_count());
    int emitted = 0;
    uint64_t scan_count = have_seq_window ? (seq_hi - seq_lo) : avail;
    for (uint64_t i = 0; i < scan_count && emitted < count; i++) {
        uint64_t seq = have_seq_window ? (seq_lo + i) : (total - 1 - i);
        FntraceEntry *e = &g_fntrace_ring[seq & (FNTRACE_RING_CAP - 1u)];
        if (e->target < target_lo || e->target >= target_hi) continue;
        if (pos > BUF_SZ - 256) break;
        pos += snprintf(out + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"target\":\"0x%08X\","
                        "\"ra\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                        "\"s3\":\"0x%08X\",\"frame\":%u}",
                        emitted == 0 ? "" : ",",
                        (unsigned long long)seq,
                        e->target, e->ra, e->a0, e->a1, e->a2, e->a3,
                        e->s3, e->frame);
        emitted++;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "],\"emitted\":%d}\n", emitted);
    debug_server_send_line(out);
    free(out);
}

/* ---- unknown_dispatch_log: dump psx_unknown_dispatch hits ----
 * Two modes:
 *   - default: per-target count summary (sorted by hit count)
 *   - tail=N: most recent N entries from the ring */
static void handle_bioscall_dump(int id, const char *json)
{
    int tail = json_get_int(json, "tail", 0);
    long want_index = json_get_int(json, "index", -1);
    char tbuf[32] = {0}; uint32_t want_table = 0; int have_table = 0;
    if (json_get_str(json, "table", tbuf, sizeof tbuf)) { want_table = (uint32_t)strtoul(tbuf, NULL, 0); have_table = 1; }
    const size_t BUF_SZ = 2 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ); if (!out) { send_err(id, "oom"); return; }
    size_t pos = 0;
    if (tail > 0) {
        if (tail > (int)BIOSCALL_RING_CAP) tail = BIOSCALL_RING_CAP;
        uint64_t total = s_bioscall_seq;
        uint64_t avail = (total < BIOSCALL_RING_CAP) ? total : BIOSCALL_RING_CAP;
        if ((uint64_t)tail > avail) tail = (int)avail;
        pos += snprintf(out + pos, BUF_SZ - pos, "{\"id\":%d,\"ok\":true,\"total\":%llu,\"tail\":%d,\"entries\":[", id, (unsigned long long)total, tail);
        uint64_t start = total - (uint64_t)tail; int first = 1;
        for (int i = 0; i < tail; i++) {
            BiosCallEntry *e = &s_bioscall_ring[(start + i) & (BIOSCALL_RING_CAP - 1u)];
            if (want_index >= 0 && (long)e->index != want_index) continue;
            if (have_table && e->table_base != want_table) continue;
            if (pos > BUF_SZ - 512) break;
            pos += snprintf(out + pos, BUF_SZ - pos, "%s{\"seq\":%llu,\"table\":\"0x%08X\",\"index\":%u,\"func\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"ra\":\"0x%08X\",\"current_func\":\"0x%08X\",\"in_exc\":%u,\"frame\":%u}",
                            first ? "" : ",", (unsigned long long)e->seq, e->table_base, e->index, e->func_ptr, e->a0, e->a1, e->a2, e->a3, e->ra, e->current_func, e->in_exception, e->frame);
            first = 0;
        }
        pos += snprintf(out + pos, BUF_SZ - pos, "]}\n");
        debug_server_send_line(out); free(out); return;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "{\"id\":%d,\"ok\":true,\"total\":%llu,\"unique\":%d,\"counts\":[", id, (unsigned long long)s_bioscall_seq, s_bioscall_unique_count);
    int first = 1;
    for (int i = 0; i < BIOSCALL_UNIQUE_CAP; i++) {
        if (s_bioscall_unique[i].count == 0) continue;
        if (want_index >= 0 && (long)s_bioscall_unique[i].index != want_index) continue;
        if (have_table && s_bioscall_unique[i].table_base != want_table) continue;
        if (pos > BUF_SZ - 256) break;
        pos += snprintf(out + pos, BUF_SZ - pos, "%s{\"table\":\"0x%08X\",\"index\":%u,\"count\":%llu}", first ? "" : ",", s_bioscall_unique[i].table_base, s_bioscall_unique[i].index, (unsigned long long)s_bioscall_unique[i].count);
        first = 0;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "]}\n");
    debug_server_send_line(out); free(out);
}

static void handle_unknown_dispatch_log(int id, const char *json)
{
    int tail = json_get_int(json, "tail", 0);

    if (tail > 0) {
        if (tail > (int)UNKNOWN_DISPATCH_CAP) tail = UNKNOWN_DISPATCH_CAP;
        uint64_t total = s_unknown_seq;
        uint64_t avail = (total < UNKNOWN_DISPATCH_CAP) ? total : UNKNOWN_DISPATCH_CAP;
        if ((uint64_t)tail > avail) tail = (int)avail;

        const size_t BUF_SZ = 2 * 1024 * 1024;
        char *out = (char *)malloc(BUF_SZ);
        if (!out) { send_err(id, "oom"); return; }
        size_t pos = 0;
        pos += snprintf(out + pos, BUF_SZ - pos,
                        "{\"id\":%d,\"ok\":true,\"total\":%llu,\"unique\":%d,"
                        "\"tail\":%d,\"entries\":[",
                        id, (unsigned long long)total, s_unknown_unique_count, tail);
        uint64_t start = total - (uint64_t)tail;
        for (int i = 0; i < tail; i++) {
            UnknownDispatchEntry *e =
                &s_unknown_ring[(start + i) & (UNKNOWN_DISPATCH_CAP - 1u)];
            if (pos > BUF_SZ - 256) break;
            pos += snprintf(out + pos, BUF_SZ - pos,
                            "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"phys\":\"0x%08X\","
                            "\"ra\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                            "\"frame\":%u}",
                            i == 0 ? "" : ",",
                            (unsigned long long)e->seq,
                            e->addr, e->phys, e->ra, e->a0, e->a1, e->frame);
        }
        pos += snprintf(out + pos, BUF_SZ - pos, "]}\n");
        debug_server_send_line(out);
        free(out);
        return;
    }

    /* Summary mode: per-phys hit count, sorted descending. */
    const size_t BUF_SZ = 2 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ);
    if (!out) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"unique\":%d,"
                    "\"summary\":[",
                    id, (unsigned long long)s_unknown_seq, s_unknown_unique_count);
    /* Sort by count desc — small cap, simple selection. */
    int emitted = 0;
    for (int round = 0; round < UNKNOWN_UNIQUE_CAP && emitted < 200; round++) {
        uint64_t best_count = 0;
        int best_idx = -1;
        for (int i = 0; i < UNKNOWN_UNIQUE_CAP; i++) {
            if (s_unknown_unique[i].phys == 0) continue;
            if (s_unknown_unique[i].count > best_count) {
                best_count = s_unknown_unique[i].count;
                best_idx = i;
            }
        }
        if (best_idx < 0) break;
        if (pos > BUF_SZ - 128) break;
        pos += snprintf(out + pos, BUF_SZ - pos,
                        "%s{\"phys\":\"0x%08X\",\"count\":%llu}",
                        emitted == 0 ? "" : ",",
                        s_unknown_unique[best_idx].phys,
                        (unsigned long long)s_unknown_unique[best_idx].count);
        s_unknown_unique[best_idx].count = 0; /* mark consumed */
        emitted++;
    }
    /* Restore: re-walk the ring once to rebuild counts. Cheap since cap is 1024. */
    /* (We mutated counts above to drive selection; rebuild by replaying the
     * ring's most recent UNKNOWN_DISPATCH_CAP entries.) */
    for (int i = 0; i < UNKNOWN_UNIQUE_CAP; i++) s_unknown_unique[i].phys = 0;
    s_unknown_unique_count = 0;
    uint64_t avail = (s_unknown_seq < UNKNOWN_DISPATCH_CAP) ? s_unknown_seq : UNKNOWN_DISPATCH_CAP;
    uint64_t start = s_unknown_seq - avail;
    for (uint64_t i = 0; i < avail; i++) {
        UnknownDispatchEntry *e = &s_unknown_ring[(start + i) & (UNKNOWN_DISPATCH_CAP - 1u)];
        uint32_t phys = e->phys;
        uint32_t idx = (phys >> 2) % UNKNOWN_UNIQUE_CAP;
        for (int k = 0; k < UNKNOWN_UNIQUE_CAP; k++) {
            uint32_t slot = (idx + k) % UNKNOWN_UNIQUE_CAP;
            if (s_unknown_unique[slot].phys == phys) {
                s_unknown_unique[slot].count++;
                break;
            }
            if (s_unknown_unique[slot].phys == 0) {
                s_unknown_unique[slot].phys = phys;
                s_unknown_unique[slot].count = 1;
                s_unknown_unique_count++;
                break;
            }
        }
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "]}\n");
    debug_server_send_line(out);
    free(out);
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

static void handle_sio_pc_window(int id, const char *json)
{
    int seq = json_get_int(json, "byte_seq", -1);
    int before = json_get_int(json, "before", 8);
    int after = json_get_int(json, "after", 16);
    char addr_lo_buf[32], addr_hi_buf[32];
    uint32_t addr_lo = 0, addr_hi = 0;
    if (json_get_str(json, "addr_lo", addr_lo_buf, sizeof(addr_lo_buf)))
        addr_lo = hex_to_u32(addr_lo_buf);
    if (json_get_str(json, "addr_hi", addr_hi_buf, sizeof(addr_hi_buf)))
        addr_hi = hex_to_u32(addr_hi_buf);
    int filter = (addr_hi > addr_lo);
    if (seq < 0) { send_err(id, "missing byte_seq"); return; }
    if (before < 0) before = 0;
    if (after < 0) after = 0;

    uint32_t lo = (uint32_t)((seq > before) ? (seq - before) : 0);
    uint32_t hi = (uint32_t)(seq + after);
    uint64_t total = s_sio_pc_trace_seq;
    uint64_t avail = (total < SIO_PC_TRACE_CAP) ? total : SIO_PC_TRACE_CAP;
    uint64_t start_seq = total - avail;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
             "\"byte_seq\":%d,\"entries\":[",
             id, (unsigned long long)total, (unsigned long long)avail, seq);
    int emitted = 0;
    for (uint64_t i = 0; i < avail; i++) {
        uint64_t s = start_seq + i;
        SioPcTraceEntry *e = &s_sio_pc_trace[s % SIO_PC_TRACE_CAP];
        if (e->byte_seq < lo || e->byte_seq > hi) continue;
        if (filter && (e->addr < addr_lo || e->addr >= addr_hi)) continue;
        if (emitted > 0) send_fmt(",");
        send_fmt("{\"seq\":%llu,\"pc\":\"0x%08X\","
                 "\"func\":\"0x%08X\",\"addr\":\"0x%08X\","
                 "\"value\":\"0x%08X\",\"byte_seq\":%u,\"width\":%u}",
                 (unsigned long long)e->seq, e->pc, e->func,
                 e->addr, e->value, e->byte_seq, e->width);
        emitted++;
    }
    send_fmt("],\"emitted\":%d}\n", emitted);
}

static void emit_sio_ctrl_reg_entry(const SioCtrlRegTraceEntry *e, int first)
{
    send_fmt("%s{\"seq\":%llu,\"pc\":\"0x%08X\",\"func\":\"0x%08X\","
             "\"value\":\"0x%08X\",\"byte_seq\":%u,\"cpu_pc\":\"0x%08X\","
             "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\",\"v0\":\"0x%08X\","
             "\"v1\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
             "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"sr\":\"0x%08X\","
             "\"epc\":\"0x%08X\",\"istat\":\"0x%08X\",\"imask\":\"0x%08X\","
             "\"width\":%u,\"in_exc\":%u,\"ctr\":%u}",
             first ? "" : ",",
             (unsigned long long)e->seq, e->pc, e->func,
             e->value, e->byte_seq, e->cpu_pc, e->ra, e->sp,
             e->v0, e->v1, e->a0, e->a1, e->a2, e->a3,
             e->sr, e->epc, e->istat, e->imask,
             (unsigned)e->width, (unsigned)e->in_exception,
             (unsigned)e->counter_7514);
}

static void handle_sio_ctrl_reg_trace(int id, const char *json)
{
    int count = json_get_int(json, "count", 200);
    if (count < 0) count = 0;
    if (count > (int)SIO_CTRL_REG_TRACE_CAP) count = SIO_CTRL_REG_TRACE_CAP;

    uint64_t total = s_sio_ctrl_reg_trace_seq;
    uint64_t avail = (total < SIO_CTRL_REG_TRACE_CAP)
                   ? total : SIO_CTRL_REG_TRACE_CAP;
    if ((uint64_t)count > avail) count = (int)avail;
    uint64_t start = total - (uint64_t)count;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
             "\"entries\":[",
             id, (unsigned long long)total, (unsigned long long)avail);
    for (int i = 0; i < count; i++) {
        uint64_t s = start + (uint64_t)i;
        const SioCtrlRegTraceEntry *e =
            &s_sio_ctrl_reg_trace[s % SIO_CTRL_REG_TRACE_CAP];
        emit_sio_ctrl_reg_entry(e, i == 0);
    }
    send_fmt("]}\n");
}

static void handle_sio_ctrl_reg_window(int id, const char *json)
{
    int seq = json_get_int(json, "byte_seq", -1);
    int before = json_get_int(json, "before", 8);
    int after = json_get_int(json, "after", 16);
    if (seq < 0) { send_err(id, "missing byte_seq"); return; }
    if (before < 0) before = 0;
    if (after < 0) after = 0;

    uint32_t lo = (uint32_t)((seq > before) ? (seq - before) : 0);
    uint32_t hi = (uint32_t)(seq + after);
    uint64_t total = s_sio_ctrl_reg_trace_seq;
    uint64_t avail = (total < SIO_CTRL_REG_TRACE_CAP)
                   ? total : SIO_CTRL_REG_TRACE_CAP;
    uint64_t start_seq = total - avail;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
             "\"byte_seq\":%d,\"entries\":[",
             id, (unsigned long long)total, (unsigned long long)avail, seq);
    int emitted = 0;
    for (uint64_t i = 0; i < avail; i++) {
        uint64_t s = start_seq + i;
        const SioCtrlRegTraceEntry *e =
            &s_sio_ctrl_reg_trace[s % SIO_CTRL_REG_TRACE_CAP];
        if (e->byte_seq < lo || e->byte_seq > hi) continue;
        emit_sio_ctrl_reg_entry(e, emitted == 0);
        emitted++;
    }
    send_fmt("],\"emitted\":%d}\n", emitted);
}

static void handle_sio_ctrl_reg_clear(int id, const char *json)
{
    (void)json;
    memset(s_sio_ctrl_reg_trace, 0, sizeof(s_sio_ctrl_reg_trace));
    s_sio_ctrl_reg_trace_seq = 0;
    send_ok(id);
}

static const char *restore_kind_name(uint32_t kind)
{
    switch (kind) {
    case 1: return "restore_escape";
    case 2: return "rfe_escape";
    case 3: return "restore_resume";
    case 4: return "rfe_resume";
    default: return "unknown";
    }
}

static void emit_restore_entry(const RestoreTraceEntry *e, int first)
{
    send_fmt("%s{\"seq\":%llu,\"kind\":%u,\"name\":\"%s\",\"jmp\":%u,"
             "\"target\":\"0x%08X\",\"cpu_pc\":\"0x%08X\","
             "\"func\":\"0x%08X\",\"store_pc\":\"0x%08X\","
             "\"byte_seq\":%u,\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
             "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\",\"a0\":\"0x%08X\","
             "\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
             "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\",\"sr\":\"0x%08X\","
             "\"epc\":\"0x%08X\",\"istat\":\"0x%08X\",\"imask\":\"0x%08X\","
             "\"frame\":%u,\"in_exc\":%u}",
             first ? "" : ",",
             (unsigned long long)e->seq, e->kind, restore_kind_name(e->kind),
             e->jmp_val, e->target_pc, e->cpu_pc, e->func, e->last_store_pc,
             e->byte_seq, e->ra, e->sp, e->v0, e->v1, e->a0, e->a1, e->a2,
             e->a3, e->s0, e->s1, e->sr, e->epc, e->istat, e->imask,
             e->frame, (unsigned)e->in_exception);
}

static size_t append_restore_entry(char *buf, size_t pos, size_t cap,
                                   const RestoreTraceEntry *e, int first)
{
    if (pos >= cap) return pos;
    pos += snprintf(buf + pos, cap - pos,
                    "%s{\"seq\":%llu,\"kind\":%u,\"name\":\"%s\",\"jmp\":%u,"
                    "\"target\":\"0x%08X\",\"cpu_pc\":\"0x%08X\","
                    "\"func\":\"0x%08X\",\"store_pc\":\"0x%08X\","
                    "\"byte_seq\":%u,\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
                    "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\",\"a0\":\"0x%08X\","
                    "\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                    "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\",\"sr\":\"0x%08X\","
                    "\"epc\":\"0x%08X\",\"istat\":\"0x%08X\",\"imask\":\"0x%08X\","
                    "\"frame\":%u,\"in_exc\":%u}",
                    first ? "" : ",",
                    (unsigned long long)e->seq, e->kind, restore_kind_name(e->kind),
                    e->jmp_val, e->target_pc, e->cpu_pc, e->func, e->last_store_pc,
                    e->byte_seq, e->ra, e->sp, e->v0, e->v1, e->a0, e->a1, e->a2,
                    e->a3, e->s0, e->s1, e->sr, e->epc, e->istat, e->imask,
                    e->frame, (unsigned)e->in_exception);
    return pos;
}

static void handle_restore_trace(int id, const char *json)
{
    int count = json_get_int(json, "count", 200);
    if (count < 0) count = 0;
    if (count > (int)RESTORE_TRACE_CAP) count = RESTORE_TRACE_CAP;

    uint64_t total = s_restore_trace_seq;
    uint64_t avail = (total < RESTORE_TRACE_CAP) ? total : RESTORE_TRACE_CAP;
    if ((uint64_t)count > avail) count = (int)avail;
    uint64_t start = total - (uint64_t)count;

    const size_t BUF_SZ = 256u + (size_t)count * 512u;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail);
    for (int i = 0; i < count; i++) {
        uint64_t s = start + (uint64_t)i;
        const RestoreTraceEntry *e = &s_restore_trace[s % RESTORE_TRACE_CAP];
        if (pos > BUF_SZ - 512) break;
        pos = append_restore_entry(buf, pos, BUF_SZ, e, i == 0);
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

static void handle_restore_trace_window(int id, const char *json)
{
    int seq = json_get_int(json, "byte_seq", -1);
    int before = json_get_int(json, "before", 8);
    int after = json_get_int(json, "after", 16);
    if (seq < 0) { send_err(id, "missing byte_seq"); return; }
    if (before < 0) before = 0;
    if (after < 0) after = 0;

    uint32_t lo = (uint32_t)((seq > before) ? (seq - before) : 0);
    uint32_t hi = (uint32_t)(seq + after);
    uint64_t total = s_restore_trace_seq;
    uint64_t avail = (total < RESTORE_TRACE_CAP) ? total : RESTORE_TRACE_CAP;
    uint64_t start_seq = total - avail;

    const size_t BUF_SZ = 4 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"byte_seq\":%d,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail, seq);
    int emitted = 0;
    for (uint64_t i = 0; i < avail; i++) {
        uint64_t s = start_seq + i;
        const RestoreTraceEntry *e = &s_restore_trace[s % RESTORE_TRACE_CAP];
        if (e->byte_seq < lo || e->byte_seq > hi) continue;
        if (pos > BUF_SZ - 512) break;
        pos = append_restore_entry(buf, pos, BUF_SZ, e, emitted == 0);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_restore_trace_clear(int id, const char *json)
{
    (void)json;
    memset(s_restore_trace, 0, sizeof(s_restore_trace));
    s_restore_trace_seq = 0;
    send_ok(id);
}

static const char *thread_kind_name(uint32_t kind)
{
    switch (kind) {
        case 1: return "save";
        case 2: return "restore";
        case 3: return "change_enter";
        case 4: return "invalid";
        case 5: return "same";
        case 6: return "inactive_current";
        case 7: return "target_missing";
        case 8: return "switch_to";
        case 9: return "switch_back";
        case 10: return "fiber_entry";
        case 11: return "fiber_done";
        case 12: return "fiber_return_restore";
        default: return "unknown";
    }
}

static size_t append_thread_entry(char *buf, size_t pos, size_t cap,
                                  const ThreadTraceEntry *e, int first)
{
    if (pos >= cap) return pos;
    pos += snprintf(buf + pos, cap - pos,
                    "%s{\"seq\":%llu,\"kind\":%u,\"name\":\"%s\","
                    "\"current_tcb\":\"0x%08X\",\"target_tcb\":\"0x%08X\","
                    "\"current_state\":\"0x%08X\",\"target_state\":\"0x%08X\","
                    "\"current_ptr\":\"0x%08X\",\"target_pc\":\"0x%08X\","
                    "\"func\":\"0x%08X\",\"store_pc\":\"0x%08X\","
                    "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
                    "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                    "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                    "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\","
                    "\"s2\":\"0x%08X\",\"s3\":\"0x%08X\","
                    "\"sr\":\"0x%08X\",\"epc\":\"0x%08X\","
                    "\"saved_a0\":\"0x%08X\",\"saved_a1\":\"0x%08X\","
                    "\"saved_a2\":\"0x%08X\",\"saved_a3\":\"0x%08X\","
                    "\"saved_s0\":\"0x%08X\",\"saved_s1\":\"0x%08X\","
                    "\"saved_s2\":\"0x%08X\",\"saved_s3\":\"0x%08X\","
                    "\"saved_sp\":\"0x%08X\","
                    "\"saved_ra\":\"0x%08X\",\"saved_pc\":\"0x%08X\","
                    "\"saved_sr\":\"0x%08X\","
                    "\"task_ptr\":\"0x%08X\",\"task_state\":\"0x%08X\","
                    "\"task_mode\":\"0x%08X\",\"task_submode\":\"0x%08X\","
                    "\"istat\":\"0x%08X\","
                    "\"imask\":\"0x%08X\",\"frame\":%u,\"in_exc\":%u}",
                    first ? "" : ",",
                    (unsigned long long)e->seq, e->kind, thread_kind_name(e->kind),
                    e->current_tcb, e->target_tcb, e->current_state, e->target_state,
                    e->current_tcb_ptr, e->target_pc, e->func, e->last_store_pc,
                    e->ra, e->sp, e->a0, e->a1, e->a2, e->a3,
                    e->s0, e->s1, e->s2, e->s3, e->sr, e->epc,
                    e->saved_a0, e->saved_a1, e->saved_a2, e->saved_a3,
                    e->saved_s0, e->saved_s1, e->saved_s2, e->saved_s3,
                    e->saved_sp, e->saved_ra, e->saved_pc, e->saved_sr,
                    e->task_ptr, e->task_state, e->task_mode, e->task_submode,
                    e->istat, e->imask, e->frame,
                    (unsigned)e->in_exception);
    return pos;
}

/* Thread context save/restore ring — declared in traps.c. */
typedef struct ThreadCtxRingEntry {
    uint32_t seq;
    uint32_t frame;
    uint8_t  op;
    uint8_t  pad0[3];
    uint32_t tcb;
    uint32_t resume_pc;
    uint32_t gpr_29;
    uint32_t gpr_31;
    uint32_t cop0_sr;
    uint32_t cop0_epc;
} ThreadCtxRingEntry;
#define THREAD_CTX_RING_CAP_DS 256u
extern ThreadCtxRingEntry g_thread_ctx_ring[THREAD_CTX_RING_CAP_DS];
extern uint64_t           g_thread_ctx_ring_seq;

static void handle_thread_ctx_ring(int id, const char *json)
{
    int count = json_get_int(json, "count", 64);
    if (count < 0) count = 0;
    if (count > (int)THREAD_CTX_RING_CAP_DS) count = THREAD_CTX_RING_CAP_DS;

    uint64_t total = g_thread_ctx_ring_seq;
    uint64_t avail = (total < THREAD_CTX_RING_CAP_DS) ? total : THREAD_CTX_RING_CAP_DS;
    if ((uint64_t)count > avail) count = (int)avail;
    uint64_t start = total - (uint64_t)count;

    const size_t BUF_SZ = 256u + (size_t)count * 240u;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail);
    int emitted = 0;
    for (uint64_t s = start; s < total && emitted < count; s++) {
        const ThreadCtxRingEntry *e = &g_thread_ctx_ring[s & (THREAD_CTX_RING_CAP_DS - 1u)];
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%u,\"frame\":%u,\"op\":\"%s\",\"tcb\":\"0x%08X\","
                        "\"resume_pc\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
                        "\"sr\":\"0x%08X\",\"epc\":\"0x%08X\"}",
                        emitted == 0 ? "" : ",",
                        e->seq, e->frame,
                        e->op == 0 ? "save" : "restore",
                        e->tcb, e->resume_pc, e->gpr_29, e->gpr_31, e->cop0_sr, e->cop0_epc);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "]}\n");
    debug_server_send_line(buf);
    free(buf);
}

static void handle_thread_trace(int id, const char *json)
{
    int count = json_get_int(json, "count", 200);
    if (count < 0) count = 0;
    if (count > 2048) count = 2048;

    uint64_t total = s_thread_trace_seq;
    uint64_t avail = (total < THREAD_TRACE_CAP) ? total : THREAD_TRACE_CAP;
    if ((uint64_t)count > avail) count = (int)avail;
    uint64_t oldest = total - avail;
    uint64_t start = total - (uint64_t)count;
    uint32_t frame_lo = 0;
    uint32_t frame_hi = 0xFFFFFFFFu;
    uint32_t kind_filter = 0;
    uint32_t current_filter = 0;
    uint32_t target_filter = 0;
    uint32_t either_filter = 0;
    int has_kind = 0;
    int has_current = 0;
    int has_target = 0;
    int has_either = 0;
    int newest_first = json_get_int(json, "newest", 0) != 0;
    char seq_buf[32], val_buf[32];
    if (json_get_str(json, "seq_lo", seq_buf, sizeof(seq_buf))) {
        start = strtoull(seq_buf, NULL, 0);
        if (start < oldest) start = oldest;
        if (start > total) start = total;
        if (start + (uint64_t)count > total)
            count = (int)(total - start);
    }
    if (json_get_str(json, "frame_lo", val_buf, sizeof(val_buf)))
        frame_lo = (uint32_t)strtoul(val_buf, NULL, 0);
    if (json_get_str(json, "frame_hi", val_buf, sizeof(val_buf)))
        frame_hi = (uint32_t)strtoul(val_buf, NULL, 0);
    if (json_get_str(json, "kind", val_buf, sizeof(val_buf))) {
        kind_filter = (uint32_t)strtoul(val_buf, NULL, 0);
        has_kind = 1;
    }
    if (json_get_str(json, "current_tcb", val_buf, sizeof(val_buf))) {
        current_filter = (uint32_t)strtoul(val_buf, NULL, 0);
        has_current = 1;
    }
    if (json_get_str(json, "target_tcb", val_buf, sizeof(val_buf))) {
        target_filter = (uint32_t)strtoul(val_buf, NULL, 0);
        has_target = 1;
    }
    if (json_get_str(json, "tcb", val_buf, sizeof(val_buf))) {
        either_filter = (uint32_t)strtoul(val_buf, NULL, 0);
        has_either = 1;
    }

    int has_filter = has_kind || has_current || has_target || has_either ||
                     frame_lo != 0 || frame_hi != 0xFFFFFFFFu;
    if (has_filter && !json_get_str(json, "seq_lo", seq_buf, sizeof(seq_buf))) {
        start = oldest;
    }

    const size_t BUF_SZ = 256u + (size_t)count * 1152u;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail);

    int emitted = 0;
    if (newest_first) {
        uint64_t stop = (start > oldest) ? start : oldest;
        for (uint64_t s = total; s > stop && emitted < count; ) {
            s--;
            const ThreadTraceEntry *e = &s_thread_trace[s % THREAD_TRACE_CAP];
            if (has_filter) {
                if (e->frame < frame_lo || e->frame > frame_hi) continue;
                if (has_kind && e->kind != kind_filter) continue;
                if (has_current && e->current_tcb != current_filter) continue;
                if (has_target && e->target_tcb != target_filter) continue;
                if (has_either &&
                    e->current_tcb != either_filter &&
                    e->target_tcb != either_filter) continue;
            }
            if (pos > BUF_SZ - 1152) break;
            pos = append_thread_entry(buf, pos, BUF_SZ, e, emitted == 0);
            emitted++;
        }
    } else {
        for (uint64_t s = start; s < total && emitted < count; s++) {
            const ThreadTraceEntry *e = &s_thread_trace[s % THREAD_TRACE_CAP];
            if (has_filter) {
                if (e->frame < frame_lo || e->frame > frame_hi) continue;
                if (has_kind && e->kind != kind_filter) continue;
                if (has_current && e->current_tcb != current_filter) continue;
                if (has_target && e->target_tcb != target_filter) continue;
                if (has_either &&
                    e->current_tcb != either_filter &&
                    e->target_tcb != either_filter) continue;
            }
            if (pos > BUF_SZ - 1152) break;
            pos = append_thread_entry(buf, pos, BUF_SZ, e, emitted == 0);
            emitted++;
        }
    }
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "],\"emitted\":%d,\"oldest_seq\":%llu}",
                    emitted, (unsigned long long)oldest);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_thread_trace_clear(int id, const char *json)
{
    (void)json;
    memset(s_thread_trace, 0, sizeof(s_thread_trace));
    s_thread_trace_seq = 0;
    send_ok(id);
}

static void handle_sreg_trace_stats(int id, const char *json)
{
    (void)json;
    uint64_t total = s_sreg_trace_seq;
    uint64_t avail = (total < SREG_TRACE_CAP) ? total : SREG_TRACE_CAP;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
             "\"capacity\":%u}",
             id, (unsigned long long)total, (unsigned long long)avail,
             (unsigned)SREG_TRACE_CAP);
}

static void handle_sreg_trace_clear(int id, const char *json)
{
    (void)json;
    memset(s_sreg_trace, 0, sizeof(s_sreg_trace));
    memset(s_sreg_last, 0, sizeof(s_sreg_last));
    s_sreg_trace_seq = 0;
    send_ok(id);
}

static int sreg_trace_yield_func(uint32_t func)
{
    return func == 0x800223E0u || func == 0x800171D4u || func == 0x8005B40Cu;
}

static size_t append_sreg_compact(char *buf, size_t pos, size_t cap,
                                  const SregTraceEntry *e, int first)
{
    return pos + snprintf(buf + pos, cap - pos,
        "%s{\"seq\":%llu,\"frame\":%u,\"tcb\":\"0x%08X\","
        "\"func\":\"0x%08X\",\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
        "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\",\"s2\":\"0x%08X\",\"s3\":\"0x%08X\","
        "\"prev_s0\":\"0x%08X\",\"prev_s1\":\"0x%08X\","
        "\"stk10\":\"0x%08X\",\"stk14\":\"0x%08X\",\"stk18\":\"0x%08X\","
        "\"stk1c\":\"0x%08X\",\"stk20\":\"0x%08X\",\"stk40\":\"0x%08X\","
        "\"task_state\":\"0x%08X\",\"task_mode\":\"0x%08X\",\"reason\":%u}",
        first ? "" : ",",
        (unsigned long long)e->seq, e->frame, e->tcb,
        e->func, e->ra, e->sp,
        e->s0, e->s1, e->s2, e->s3,
        e->prev_s0, e->prev_s1,
        e->stack10, e->stack14, e->stack18,
        e->stack1c, e->stack20, e->stack40,
        e->task_state, e->task_mode, (unsigned)e->reason);
}

static void handle_sreg_trace_find(int id, const char *json)
{
    uint64_t total = s_sreg_trace_seq;
    uint64_t avail = (total < SREG_TRACE_CAP) ? total : SREG_TRACE_CAP;
    uint64_t oldest = total - avail;
    if (avail == 0) {
        send_fmt("{\"id\":%d,\"ok\":true,\"found\":false,\"total\":0,\"entries\":[]}", id);
        return;
    }

    char val[32];
    uint32_t tcb_filter = 0;
    int has_tcb = 0;
    if (json_get_str(json, "tcb", val, sizeof(val))) {
        tcb_filter = hex_to_u32(val);
        has_tcb = 1;
    }
    uint32_t frame_lo = 0;
    uint32_t frame_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "frame_lo", val, sizeof(val)))
        frame_lo = (uint32_t)strtoul(val, NULL, 0);
    if (json_get_str(json, "frame_hi", val, sizeof(val)))
        frame_hi = (uint32_t)strtoul(val, NULL, 0);

    int want_zero = json_get_int(json, "zero", 1) != 0;
    int yield_only = json_get_int(json, "yield_only", 1) != 0;
    int newest_first = json_get_int(json, "newest", 0) != 0;
    int window = json_get_int(json, "window", 24);
    if (window < 0) window = 0;
    if (window > 80) window = 80;

    uint64_t found = (uint64_t)-1;
    if (newest_first) {
        for (uint64_t s = total; s > oldest; ) {
            s--;
            const SregTraceEntry *e = &s_sreg_trace[s % SREG_TRACE_CAP];
            if (e->seq != s) continue;
            if (has_tcb && e->tcb != tcb_filter) continue;
            if (e->frame < frame_lo || e->frame > frame_hi) continue;
            if (yield_only && !sreg_trace_yield_func(e->func)) continue;
            if (want_zero && !(e->s0 == 0 && e->s1 == 0)) continue;
            found = s;
            break;
        }
    } else {
        for (uint64_t s = oldest; s < total; s++) {
            const SregTraceEntry *e = &s_sreg_trace[s % SREG_TRACE_CAP];
            if (e->seq != s) continue;
            if (has_tcb && e->tcb != tcb_filter) continue;
            if (e->frame < frame_lo || e->frame > frame_hi) continue;
            if (yield_only && !sreg_trace_yield_func(e->func)) continue;
            if (want_zero && !(e->s0 == 0 && e->s1 == 0)) continue;
            found = s;
            break;
        }
    }

    const size_t BUF_SZ = 512u + (size_t)(window * 2 + 1) * 640u;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"oldest_seq\":%llu,\"found\":%s",
                    id, (unsigned long long)total, (unsigned long long)avail,
                    (unsigned long long)oldest,
                    found == (uint64_t)-1 ? "false" : "true");
    if (found != (uint64_t)-1) {
        const SregTraceEntry *m = &s_sreg_trace[found % SREG_TRACE_CAP];
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        ",\"match_seq\":%llu,\"match_frame\":%u,\"entries\":[",
                        (unsigned long long)found, m->frame);
        uint64_t lo = (found > (uint64_t)window) ? found - (uint64_t)window : 0;
        uint64_t hi = found + (uint64_t)window + 1u;
        if (lo < oldest) lo = oldest;
        if (hi > total) hi = total;
        int first = 1;
        for (uint64_t s = lo; s < hi && pos < BUF_SZ - 640; s++) {
            const SregTraceEntry *e = &s_sreg_trace[s % SREG_TRACE_CAP];
            if (e->seq != s) continue;
            if (has_tcb && e->tcb != tcb_filter) continue;
            pos = append_sreg_compact(buf, pos, BUF_SZ, e, first);
            first = 0;
        }
        pos += snprintf(buf + pos, BUF_SZ - pos, "]}");
    } else {
        pos += snprintf(buf + pos, BUF_SZ - pos, ",\"entries\":[]}");
    }
    debug_server_send_line(buf);
    free(buf);
}

static void handle_sreg_trace_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 4096) count = 4096;

    uint64_t total = s_sreg_trace_seq;
    uint64_t avail = (total < SREG_TRACE_CAP) ? total : SREG_TRACE_CAP;
    uint64_t oldest = total - avail;
    uint64_t start = total > (uint64_t)count ? total - (uint64_t)count : 0;
    if (start < oldest) start = oldest;

    uint32_t tcb_filter = 0;
    uint32_t func_lo = 0;
    uint32_t func_hi = 0xFFFFFFFFu;
    uint32_t frame_lo = 0;
    uint32_t frame_hi = 0xFFFFFFFFu;
    int has_tcb = 0;
    int newest_first = json_get_int(json, "newest", 0) != 0;
    char val[32];
    if (json_get_str(json, "seq_lo", val, sizeof(val))) {
        start = strtoull(val, NULL, 0);
        if (start < oldest) start = oldest;
        if (start > total) start = total;
    }
    if (json_get_str(json, "tcb", val, sizeof(val))) {
        tcb_filter = hex_to_u32(val);
        has_tcb = 1;
    }
    if (json_get_str(json, "func_lo", val, sizeof(val)))
        func_lo = hex_to_u32(val);
    if (json_get_str(json, "func_hi", val, sizeof(val)))
        func_hi = hex_to_u32(val);
    if (json_get_str(json, "frame_lo", val, sizeof(val)))
        frame_lo = (uint32_t)strtoul(val, NULL, 0);
    if (json_get_str(json, "frame_hi", val, sizeof(val)))
        frame_hi = (uint32_t)strtoul(val, NULL, 0);

    int has_filter = has_tcb || func_lo != 0 || func_hi != 0xFFFFFFFFu ||
                     frame_lo != 0 || frame_hi != 0xFFFFFFFFu;
    if (has_filter && !json_get_str(json, "seq_lo", val, sizeof(val))) {
        start = oldest;
    }

    const size_t BUF_SZ = 256u + (size_t)count * 1024u;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"oldest_seq\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail,
                    (unsigned long long)oldest);

    if (newest_first) {
        for (uint64_t s = total; s > start && emitted < count && pos < BUF_SZ - 1024; ) {
            s--;
            const SregTraceEntry *e = &s_sreg_trace[s % SREG_TRACE_CAP];
            if (e->seq != s) continue;
            if (has_filter) {
                if (has_tcb && e->tcb != tcb_filter) continue;
                if (e->func < func_lo || e->func >= func_hi) continue;
                if (e->frame < frame_lo || e->frame > frame_hi) continue;
            }
            pos += snprintf(buf + pos, BUF_SZ - pos,
                "%s{\"seq\":%llu,\"tcb\":\"0x%08X\",\"func\":\"0x%08X\","
                "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
                "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\",\"s2\":\"0x%08X\",\"s3\":\"0x%08X\","
                "\"s4\":\"0x%08X\",\"s5\":\"0x%08X\",\"s6\":\"0x%08X\",\"s7\":\"0x%08X\","
                "\"prev_s0\":\"0x%08X\",\"prev_s1\":\"0x%08X\","
                "\"prev_s2\":\"0x%08X\",\"prev_s3\":\"0x%08X\","
                "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                "\"stk10\":\"0x%08X\",\"stk14\":\"0x%08X\",\"stk18\":\"0x%08X\",\"stk1c\":\"0x%08X\","
                "\"stk20\":\"0x%08X\",\"stk28\":\"0x%08X\",\"stk40\":\"0x%08X\","
                "\"task_ptr\":\"0x%08X\",\"task_state\":\"0x%08X\","
                "\"task_mode\":\"0x%08X\",\"task_submode\":\"0x%08X\","
                "\"frame\":%u,\"reason\":%u}",
                emitted == 0 ? "" : ",",
                (unsigned long long)e->seq, e->tcb, e->func, e->ra, e->sp,
                e->s0, e->s1, e->s2, e->s3, e->s4, e->s5, e->s6, e->s7,
                e->prev_s0, e->prev_s1, e->prev_s2, e->prev_s3,
                e->a0, e->a1, e->a2, e->a3,
                e->stack10, e->stack14, e->stack18, e->stack1c,
                e->stack20, e->stack28, e->stack40,
                e->task_ptr, e->task_state, e->task_mode, e->task_submode,
                e->frame, (unsigned)e->reason);
            emitted++;
        }
    } else {
        for (uint64_t s = start; s < total && emitted < count && pos < BUF_SZ - 1024; s++) {
            const SregTraceEntry *e = &s_sreg_trace[s % SREG_TRACE_CAP];
            if (e->seq != s) continue;
            if (has_filter) {
                if (has_tcb && e->tcb != tcb_filter) continue;
                if (e->func < func_lo || e->func >= func_hi) continue;
                if (e->frame < frame_lo || e->frame > frame_hi) continue;
            }
            pos += snprintf(buf + pos, BUF_SZ - pos,
                "%s{\"seq\":%llu,\"tcb\":\"0x%08X\",\"func\":\"0x%08X\","
                "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
                "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\",\"s2\":\"0x%08X\",\"s3\":\"0x%08X\","
                "\"s4\":\"0x%08X\",\"s5\":\"0x%08X\",\"s6\":\"0x%08X\",\"s7\":\"0x%08X\","
                "\"prev_s0\":\"0x%08X\",\"prev_s1\":\"0x%08X\","
                "\"prev_s2\":\"0x%08X\",\"prev_s3\":\"0x%08X\","
                "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                "\"stk10\":\"0x%08X\",\"stk14\":\"0x%08X\",\"stk18\":\"0x%08X\",\"stk1c\":\"0x%08X\","
                "\"stk20\":\"0x%08X\",\"stk28\":\"0x%08X\",\"stk40\":\"0x%08X\","
                "\"task_ptr\":\"0x%08X\",\"task_state\":\"0x%08X\","
                "\"task_mode\":\"0x%08X\",\"task_submode\":\"0x%08X\","
                "\"frame\":%u,\"reason\":%u}",
                emitted == 0 ? "" : ",",
                (unsigned long long)e->seq, e->tcb, e->func, e->ra, e->sp,
                e->s0, e->s1, e->s2, e->s3, e->s4, e->s5, e->s6, e->s7,
                e->prev_s0, e->prev_s1, e->prev_s2, e->prev_s3,
                e->a0, e->a1, e->a2, e->a3,
                e->stack10, e->stack14, e->stack18, e->stack1c,
                e->stack20, e->stack28, e->stack40,
                e->task_ptr, e->task_state, e->task_mode, e->task_submode,
                e->frame, (unsigned)e->reason);
            emitted++;
        }
    }

    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void emit_probe_entry(const ProbeTraceEntry *e, int first)
{
    send_fmt("%s{\"seq\":%llu,\"pc\":\"0x%08X\",\"func\":\"0x%08X\","
             "\"store_pc\":\"0x%08X\",\"byte_seq\":%u,"
             "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
             "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\","
             "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
             "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
             "\"sr\":\"0x%08X\",\"epc\":\"0x%08X\","
             "\"istat\":\"0x%08X\",\"imask\":\"0x%08X\","
             "\"frame\":%u,\"in_exc\":%u}",
             first ? "" : ",",
             (unsigned long long)e->seq, e->pc, e->func,
             e->last_store_pc, e->byte_seq, e->ra, e->sp,
             e->v0, e->v1, e->a0, e->a1, e->a2, e->a3,
             e->sr, e->epc, e->istat, e->imask,
             e->frame, (unsigned)e->in_exception);
}

static void handle_probe_trace(int id, const char *json)
{
    int count = json_get_int(json, "count", 200);
    if (count < 0) count = 0;
    if (count > (int)PROBE_TRACE_CAP) count = PROBE_TRACE_CAP;

    uint64_t total = s_probe_trace_seq;
    uint64_t avail = (total < PROBE_TRACE_CAP) ? total : PROBE_TRACE_CAP;
    if ((uint64_t)count > avail) count = (int)avail;
    uint64_t start = total - (uint64_t)count;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
             "\"entries\":[",
             id, (unsigned long long)total, (unsigned long long)avail);
    for (int i = 0; i < count; i++) {
        uint64_t s = start + (uint64_t)i;
        const ProbeTraceEntry *e = &s_probe_trace[s % PROBE_TRACE_CAP];
        emit_probe_entry(e, i == 0);
    }
    send_fmt("]}\n");
}

static void handle_probe_clear(int id, const char *json)
{
    (void)json;
    memset(s_probe_trace, 0, sizeof(s_probe_trace));
    s_probe_trace_seq = 0;
    send_ok(id);
}

/* ---- MMIO write trace (separate ring buffer) ----
 * Records every write to 0x1F801xxx MMIO registers. Unconditional (no filtering).
 * 1<<22 entries, heap-allocated in debug_server_init().
 * Kept in step with the SIO PC trace so generic MMIO history has enough
 * retention for post-failure queries. */
#define MMIO_TRACE_CAP (1 << 18)
typedef struct {
    uint64_t seq;
    uint32_t addr;       /* 0x1F801xxx */
    uint32_t val;        /* value written */
    uint32_t func_addr;  /* dispatch target */
    uint32_t pc;         /* g_debug_last_store_pc */
    uint32_t cpu_pc;     /* live CPUState.pc, useful across nonlocal returns */
    uint32_t ra;         /* $ra */
    uint32_t sp;         /* $sp */
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t sr;
    uint32_t epc;
    uint32_t istat;
    uint32_t imask;
    uint32_t frame;      /* VBlank frame */
    uint8_t  width;      /* 1, 2, or 4 */
    uint8_t  pad[3];
} MmioTraceEntry;
static MmioTraceEntry *s_mmio_trace = NULL;
static uint64_t s_mmio_trace_seq  = 0;
static uint32_t s_mmio_trace_head = 0;

/* ---- GP1 display-control trace (ALWAYS-ON, dedicated) ----
 * The general MMIO ring rolls over in well under a minute of gameplay
 * (SPU/DMA traffic), evicting the boot-window display history before a
 * post-hoc probe can ask "who toggled the display during the logo?".
 * GP1 writes are ~10/frame in-game (measured: Tomba attract ≈ 600/s),
 * so 512K entries ≈ 15 minutes — enough to attribute a boot window from
 * well after the title screen. ~40 MB heap.
 * Same entry layout as the general ring; dumped via `gp1_dump`. */
#define GP1_TRACE_CAP (1 << 19)
static MmioTraceEntry *s_gp1_trace = NULL;
static uint64_t s_gp1_trace_seq  = 0;
static uint32_t s_gp1_trace_head = 0;

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

/* TCP serve-stall telemetry. The server is pumped on the main thread, so
 * every millisecond spent inside send_all_blocking is a millisecond the
 * emulator did not run. Surfaced in the freeze heartbeat and wedge dumps
 * so a TCP-throttled run can never be misread as a guest-side bug
 * (2026-06-10: two pre-fix attract "degradations" were exactly this —
 * 6 fps crawl + final stall, all main-thread time inside WS2_32!send). */
static uint64_t s_tcp_send_stall_ms = 0;
static uint32_t s_tcp_clients_dropped = 0;

uint64_t debug_server_get_tcp_stall_ms(void)  { return s_tcp_send_stall_ms; }
uint32_t debug_server_get_tcp_drops(void)     { return s_tcp_clients_dropped; }

static uint64_t monotonic_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* Bounded blocking send. Returns 0 on success, -1 on failure/timeout.
 *
 * The server is pumped on the main thread, so an unbounded send to a
 * client that has stopped draining stalls the emulator until the
 * starvation watchdog (4 s) kills the whole process. Each chunk gets a
 * 2 s send timeout and a watchdog heartbeat. A slow-but-alive client is
 * additionally bounded by a TOTAL budget per call: a trickle-draining
 * client used to be able to throttle the emulator indefinitely (progress
 * heartbeats kept the watchdog quiet). Past the budget the client loses
 * its connection — never the runtime. Responses too big to send inside
 * the budget must use the *_dump_file commands instead. */
#define SEND_TOTAL_BUDGET_MS 15000u

static int send_all_blocking(sock_t sock, const char *data, size_t len)
{
    int ok = 0;
    uint64_t t_start = monotonic_ms();
#ifdef _WIN32
    u_long mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
    DWORD tmo = 2000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval tmo = { 2, 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
#endif
    size_t sent = 0;
    while (sent < len) {
        if (monotonic_ms() - t_start > SEND_TOTAL_BUDGET_MS) {
            ok = -1;
            break;
        }
        size_t want = len - sent;
        if (want > (1u << 20)) want = (1u << 20);
        int n = send(sock, data + sent, (int)want, 0);
        if (n > 0) {
            sent += (size_t)n;
            /* Legitimate debug traffic in flight — not starvation. */
            extern void starvation_watchdog_heartbeat(void);
            starvation_watchdog_heartbeat();
            continue;
        }
        ok = -1;
        break;
    }
#ifdef _WIN32
    mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    fcntl(sock, F_SETFL, flags);
#endif
    s_tcp_send_stall_ms += monotonic_ms() - t_start;
    return ok;
}

void debug_server_send_line(const char *json)
{
    if (s_client == SOCK_INVALID) return;
    size_t len = strlen(json);
    if (send_all_blocking(s_client, json, len) != 0 ||
        send_all_blocking(s_client, "\n", 1) != 0) {
        /* Client stopped draining — drop the connection, keep the
         * runtime. The next poll() accepts a fresh client. */
        sock_close(s_client);
        s_client = SOCK_INVALID;
        s_tcp_clients_dropped++;
    }
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

int debug_server_dirty_break_maybe_pause(uint32_t target, CPUState *cpu)
{
    if (!s_dirty_break_active) return 0;
    if (target < s_dirty_break_lo || target >= s_dirty_break_hi) return 0;

    /* Record the hit so dirty_break_state can be queried, but do NOT
     * pause the runtime. The hit recording is its own ring (effectively
     * size 1 — latest hit only). For broader history, use wtrace or
     * fn_entry over the same address window instead of relying on a
     * "stop and inspect" workflow. */
    s_dirty_break_active = 0;
    s_dirty_break_hits++;
    s_dirty_break_target = target;
    s_dirty_break_ra = cpu ? cpu->gpr[31] : 0;
    s_dirty_break_a0 = cpu ? cpu->gpr[4] : 0;
    s_dirty_break_a1 = cpu ? cpu->gpr[5] : 0;
    s_dirty_break_a2 = cpu ? cpu->gpr[6] : 0;
    s_dirty_break_a3 = cpu ? cpu->gpr[7] : 0;
    s_dirty_break_sp = cpu ? cpu->gpr[29] : 0;
    s_dirty_break_frame = (uint32_t)s_frame_count;
    return 1;
}

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
    /* Surface accumulated dispatch misses on every ping so they can't go
     * unnoticed across sessions (NES recomp template PRINCIPLES.md §13a:
     * "A dispatch miss is a SILENT GAME-BREAKING BUG"). 0 = healthy. */
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu,"
             "\"dispatch_miss_total\":%llu,"
             "\"dispatch_miss_unique\":%d}",
             id, (unsigned long long)s_frame_count,
             (unsigned long long)s_unknown_seq,
             s_unknown_unique_count);
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
    /* Nibble-table encode: snprintf per byte costs seconds for a 2 MB
     * read, which stalls the main-thread-pumped server (and the SDL
     * event loop) long enough to look like a wedge. */
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        uint8_t b = psx_read_byte(addr + (uint32_t)i);
        hex[(size_t)i * 2]     = H[b >> 4];
        hex[(size_t)i * 2 + 1] = H[b & 0xF];
    }
    char *tail = hex + (size_t)len * 2;
    memcpy(tail, "\"}", 3);
    debug_server_send_line(out);
    free(out);
}

/* "dump_ram" is an alias of "read_ram".  The old implementation answered a
 * single request with one response line per 256-byte chunk; any client that
 * follows the one-request/one-response protocol left the extra lines unread,
 * the socket send buffer filled, the main-thread-pumped server blocked, and
 * the freeze watchdog killed the process.  One request, one response. */

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
             "\"depth\":%d,\"depth24\":%d,"
             "\"disabled\":%d,"
             "\"gpustat\":\"0x%08X\","
             "\"gp0_writes\":%llu,"
             "\"gp0_nop\":%llu,\"gp0_fill\":%llu,\"gp0_draw\":%llu,\"gp0_env\":%llu,\"gp0_copy\":%llu,"
             "\"draw_area\":[%u,%u,%u,%u],"
             "\"draw_offset\":[%d,%d]}",
             id, di.display_x, di.display_y,
             di.width, di.height,
             di.depth24 ? 24 : 15, di.depth24,
             di.disabled,
             gpustat,
             (unsigned long long)gpu_get_gp0_count(),
             (unsigned long long)nop, (unsigned long long)fill,
             (unsigned long long)draw, (unsigned long long)env,
             (unsigned long long)copy,
             da.left, da.top, da.right, da.bottom,
             da.offset_x, da.offset_y);
}

static void handle_mem_words(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }

    uint32_t addr = hex_to_u32(addr_str);
    int count = json_get_int(json, "count", 16);
    if (count < 1) count = 1;
    if (count > 256) count = 256;

    size_t bufsz = 256u + (size_t)count * 32u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"words\":[",
                    id, addr);
    for (int i = 0; i < count && pos < bufsz - 32; i++) {
        uint32_t a = addr + (uint32_t)i * 4u;
        uint32_t v = psx_read_word(a);
        pos += snprintf(buf + pos, bufsz - pos, "%s\"0x%08X\"",
                        i ? "," : "", v);
    }
    pos += snprintf(buf + pos, bufsz - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
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

static void handle_timers_state(int id, const char *json)
{
    (void)json;
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    timers_get_snapshot(counter, mode, target, irq_line, frac);
    send_fmt("{\"id\":%d,\"ok\":true,\"timers\":["
             "{\"ch\":0,\"counter\":%u,\"mode\":\"0x%04X\",\"target\":%u,"
             "\"irq_line\":%d,\"frac\":%u},"
             "{\"ch\":1,\"counter\":%u,\"mode\":\"0x%04X\",\"target\":%u,"
             "\"irq_line\":%d,\"frac\":%u},"
             "{\"ch\":2,\"counter\":%u,\"mode\":\"0x%04X\",\"target\":%u,"
             "\"irq_line\":%d,\"frac\":%u}]}",
             id,
             counter[0], mode[0], target[0], irq_line[0], frac[0],
             counter[1], mode[1], target[1], irq_line[1], frac[1],
             counter[2], mode[2], target[2], irq_line[2], frac[2]);
}

/* GPU opcode counter — defined in gpu.c */
static const char *cdrom_trace_kind_name(uint8_t kind)
{
    switch (kind) {
    case 'N': return "init";
    case 'C': return "cmd";
    case 'I': return "set_irq";
    case 'F': return "fire_irq";
    case 'f': return "irq_masked";
    case 'S': return "sector";
    case 's': return "sector_skip";
    case 'A': return "xa_audio";
    case 'a': return "xa_skip";
    case 'X': return "xa_unsupported";
    case 'O': return "overwrite";
    case 'R': return "read";
    case 'W': return "write";
    case 'D': return "dma";
    default: return "unknown";
    }
}

static void handle_cdrom_state(int id, const char *json)
{
    (void)json;
    CDROMDebugState s;
    cdrom_debug_snapshot(&s);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"seq\":%llu,\"has_disc\":%d,"
             "\"index\":%u,\"stat\":\"0x%02X\","
             "\"request\":\"0x%02X\","
             "\"irq_enable\":\"0x%02X\",\"irq_flag\":\"0x%02X\","
             "\"mode\":\"0x%02X\","
             "\"param_count\":%d,\"response_read\":%d,\"response_count\":%d,"
             "\"sector_available\":%d,\"sector_read_pos\":%d,\"sector_size\":%d,"
             "\"reading\":%d,\"read_msf\":[%d,%d,%d],"
             "\"read_cmd\":\"0x%02X\",\"read_delay\":%d,"
             "\"filter_file\":%u,\"filter_channel\":%u,\"muted\":%u,"
             "\"seek_msf\":[%u,%u,%u],"
             "\"pending\":{\"cmd\":\"0x%02X\",\"active\":%d,\"delay\":%d,\"phase\":%d},"
             "\"last_sector\":{\"lba\":%d,\"size\":%d,\"frame\":%u,"
             "\"mode\":\"0x%02X\",\"have_raw\":%u},"
             "\"i_stat\":\"0x%08X\"}",
             id, (unsigned long long)s.seq, s.has_disc,
             s.index_reg, s.stat_reg, s.request_reg, s.irq_enable, s.irq_flag,
             s.mode_reg,
             s.param_count, s.response_read, s.response_count,
             s.sector_available, s.sector_read_pos, s.sector_size,
             s.reading, s.read_min, s.read_sec, s.read_sect,
             s.read_cmd, s.read_delay,
             s.filter_file, s.filter_channel, s.muted,
             s.seek_min, s.seek_sec, s.seek_sect,
             s.pending_cmd, s.pending_pending, s.pending_delay,
             s.pending_phase,
             s.last_sector_lba, s.last_sector_size, s.last_sector_frame,
             s.last_sector_mode, s.last_sector_have_raw,
             s.i_stat);
}

static void handle_cdrom_sector_dump(int id, const char *json)
{
    int offset = json_get_int(json, "offset", 0);
    int len = json_get_int(json, "len", 128);
    if (offset < 0) offset = 0;
    if (len < 1) len = 1;
    if (len > 2340) len = 2340;

    uint8_t *bytes = (uint8_t *)malloc((size_t)len);
    if (!bytes) { send_err(id, "oom"); return; }

    CDROMSectorDebugState s;
    uint32_t got = cdrom_debug_copy_last_sector((uint32_t)offset,
                                                (uint32_t)len,
                                                bytes, &s);

    size_t bufsz = 512u + (size_t)got * 2u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) {
        free(bytes);
        send_err(id, "oom");
        return;
    }

    size_t pos = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,"
                    "\"current\":{\"available\":%d,\"read_pos\":%d,\"size\":%d},"
                    "\"last\":{\"lba\":%d,\"size\":%d,\"frame\":%u,"
                    "\"mode\":\"0x%02X\",\"have_raw\":%u},"
                    "\"offset\":%d,\"len\":%u,\"hex\":\"",
                    id,
                    s.current_available, s.current_read_pos, s.current_size,
                    s.last_lba, s.last_size, s.last_frame,
                    s.last_mode, s.last_have_raw,
                    offset, got);
    for (uint32_t i = 0; i < got && pos + 3 < bufsz; i++) {
        pos += snprintf(buf + pos, bufsz - pos, "%02x", bytes[i]);
    }
    snprintf(buf + pos, bufsz - pos, "\"}");
    debug_server_send_line(buf);
    free(buf);
    free(bytes);
}

static void append_hex_bytes(char *buf, size_t bufsz, size_t *pos,
                             const uint8_t *bytes, uint32_t len)
{
    for (uint32_t i = 0; i < len && *pos + 3 < bufsz; i++) {
        *pos += snprintf(buf + *pos, bufsz - *pos, "%02x", bytes[i]);
    }
}

static void handle_cdrom_sector_history(int id, const char *json)
{
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > CDROM_SECTOR_HISTORY_CAP) count = CDROM_SECTOR_HISTORY_CAP;

    int filter_lba = -1;
    char lba_str[32];
    if (json_get_str(json, "lba", lba_str, sizeof(lba_str))) {
        filter_lba = (int)hex_to_u32(lba_str);
    }

    const CDROMSectorHistoryEntry *entries = NULL;
    uint64_t total = cdrom_debug_get_sector_history(&entries);
    uint64_t oldest = (total > CDROM_SECTOR_HISTORY_CAP)
        ? total - CDROM_SECTOR_HISTORY_CAP : 0;

    size_t bufsz = 256u + (size_t)count * 760u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,"
                    "\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total,
                    (unsigned long long)oldest);

    uint64_t seq = total;
    while (seq > oldest && emitted < count && pos < bufsz - 760) {
        seq--;
        const CDROMSectorHistoryEntry *e =
            &entries[seq % CDROM_SECTOR_HISTORY_CAP];
        if (e->seq != seq) continue;
        if (filter_lba >= 0 && e->lba != filter_lba) continue;

        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"lba\":%d,\"size\":%d,"
                        "\"frame\":%u,\"mode\":\"0x%02X\","
                        "\"have_raw\":%u,\"raw_mode\":\"0x%02X\","
                        "\"xa_file\":%u,\"xa_channel\":%u,"
                        "\"xa_submode\":\"0x%02X\",\"xa_coding\":\"0x%02X\","
                        "\"data_delivered\":%u,\"xa_audio_delivered\":%u,"
                        "\"skip_reason\":%u,\"bytes_len\":%u,\"hex\":\"",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, e->lba, e->size,
                        e->frame, e->mode, e->have_raw, e->raw_mode,
                        e->xa_file, e->xa_channel, e->xa_submode, e->xa_coding,
                        e->data_delivered, e->xa_audio_delivered,
                        e->skip_reason, e->bytes_len);
        append_hex_bytes(buf, bufsz, &pos, e->bytes, e->bytes_len);
        pos += snprintf(buf + pos, bufsz - pos, "\"}");
        emitted++;
    }

    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_cdrom_sector_history_clear(int id, const char *json)
{
    (void)json;
    cdrom_debug_clear_sector_history();
    send_ok(id);
}

static void handle_cdrom_trace_clear(int id, const char *json)
{
    (void)json;
    cdrom_debug_clear_trace();
    send_ok(id);
}

static void handle_cdrom_trace_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > CDROM_TRACE_CAP) count = CDROM_TRACE_CAP;

    const CDROMTraceEntry *entries = NULL;
    uint64_t total = cdrom_debug_get_trace(&entries);
    uint64_t oldest = (total > CDROM_TRACE_CAP) ? total - CDROM_TRACE_CAP : 0;
    uint64_t start = (total > (uint64_t)count) ? total - (uint64_t)count : 0;
    if (start < oldest) start = oldest;

    size_t bufsz = 256u + (size_t)count * 360u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest);
    for (uint64_t seq = start; seq < total && pos < bufsz - 400; seq++) {
        const CDROMTraceEntry *e = &entries[seq % CDROM_TRACE_CAP];
        if (e->seq != seq) continue;
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"kind\":\"%s\",\"addr\":\"0x%08X\","
                        "\"val\":\"0x%08X\",\"w\":%u,\"func\":\"0x%08X\","
                        "\"pc\":\"0x%08X\",\"frame\":%u,\"i_stat\":\"0x%08X\","
                        "\"index\":%u,\"stat\":\"0x%02X\","
                        "\"request\":\"0x%02X\","
                        "\"irq_enable\":\"0x%02X\",\"irq_flag\":\"0x%02X\","
                        "\"mode\":\"0x%02X\","
                        "\"param\":%u,\"resp_read\":%u,\"resp_count\":%u,"
                        "\"sector_avail\":%u,\"sector_pos\":%d,\"sector_size\":%d,"
                        "\"pending_cmd\":\"0x%02X\",\"pending\":%u,"
                        "\"pending_delay\":%d,\"reading\":%u,"
                        "\"read_cmd\":\"0x%02X\",\"read_delay\":%d}",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, cdrom_trace_kind_name(e->kind),
                        e->addr, e->val, (unsigned)e->width,
                        e->func, e->pc, e->frame, e->i_stat,
                        e->index_reg, e->stat_reg, e->request_reg, e->irq_enable, e->irq_flag,
                        e->mode_reg,
                        e->param_count, e->response_read, e->response_count,
                        e->sector_available, e->sector_read_pos, e->sector_size,
                        e->pending_cmd, e->pending_pending,
                        e->pending_delay, e->reading,
                        e->read_cmd, e->read_delay);
        emitted++;
    }
    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static const char *dma_trace_kind_name(uint32_t kind)
{
    switch (kind) {
    case 'S': return "start";
    case 'C': return "complete";
    default: return "unknown";
    }
}

static void handle_dma_state(int id, const char *json)
{
    (void)json;
    DMADebugState s;
    dma_debug_get_state(&s);

    char buf[2048];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"id\":%d,\"ok\":true,\"dpcr\":\"0x%08X\","
                    "\"dicr\":\"0x%08X\",\"channels\":[",
                    id, s.dpcr, s.dicr);
    for (int i = 0; i < 7 && pos < sizeof(buf) - 192; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"ch\":%d,\"madr\":\"0x%08X\","
                        "\"bcr\":\"0x%08X\",\"chcr\":\"0x%08X\","
                        "\"active\":%u,\"remaining_words\":%u,"
                        "\"cycles_accum\":%u}",
                        i ? "," : "",
                        i, s.channels[i].madr, s.channels[i].bcr,
                        s.channels[i].chcr, s.channels[i].active,
                        s.channels[i].remaining_words,
                        s.channels[i].cycles_accum);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

static void handle_dma_trace_clear(int id, const char *json)
{
    (void)json;
    dma_debug_clear_trace();
    send_ok(id);
}

static void handle_dma_trace_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > DMA_TRACE_CAP) count = DMA_TRACE_CAP;

    const DMATraceEntry *entries = NULL;
    uint64_t total = dma_debug_get_trace(&entries);
    uint64_t oldest = (total > DMA_TRACE_CAP) ? total - DMA_TRACE_CAP : 0;
    uint64_t start = (total > (uint64_t)count) ? total - (uint64_t)count : 0;
    if (start < oldest) start = oldest;

    size_t bufsz = 256u + (size_t)count * 512u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest);
    for (uint64_t seq = start; seq < total && pos < bufsz - 512; seq++) {
        const DMATraceEntry *e = &entries[seq % DMA_TRACE_CAP];
        if (e->seq != seq) continue;
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"frame\":%u,\"kind\":\"%s\",\"ch\":%u,"
                        "\"words\":%u,\"madr\":\"0x%08X\",\"bcr\":\"0x%08X\","
                        "\"chcr\":\"0x%08X\",\"dpcr\":\"0x%08X\","
                        "\"dicr_before\":\"0x%08X\",\"dicr_after\":\"0x%08X\","
                        "\"i_stat_before\":\"0x%08X\",\"i_stat_after\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"pc\":\"0x%08X\"}",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, e->frame,
                        dma_trace_kind_name(e->kind), e->channel,
                        e->total_words, e->madr, e->bcr, e->chcr, e->dpcr,
                        e->dicr_before, e->dicr_after,
                        e->i_stat_before, e->i_stat_after,
                        e->func, e->pc);
        emitted++;
    }
    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

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

static void handle_gpu_ring_stats(int id, const char *json)
{
    (void)json;
    uint32_t oldest = 0, newest = 0;
    gpu_gp0_ring_frame_span(&oldest, &newest);
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%u,"
             "\"max_words\":%u,\"oldest_frame\":%u,\"newest_frame\":%u}",
             id,
             (unsigned long long)gpu_gp0_ring_total(),
             gpu_gp0_ring_capacity(),
             gpu_gp0_ring_max_words(),
             oldest, newest);
}

static void handle_gpu_frame_dump(int id, const char *json)
{
    int target = json_get_int(json, "frame", -1);
    if (target < 0) { send_err(id, "missing frame"); return; }
    int max_entries = json_get_int(json, "count", 8192);
    if (max_entries < 1)    max_entries = 1;
    if (max_entries > 65536) max_entries = 65536;

    GpuGp0RingEntry *entries = (GpuGp0RingEntry *)malloc(
        (size_t)max_entries * sizeof(GpuGp0RingEntry));
    if (!entries) { send_err(id, "alloc failed"); return; }

    int n = gpu_gp0_ring_dump_frame((uint32_t)target, entries, max_entries);

    /* ~190 bytes per entry in JSON; budget conservatively. */
    size_t buf_sz = 256 + (size_t)n * 280u;
    char *buf = (char *)malloc(buf_sz);
    if (!buf) { free(entries); send_err(id, "alloc failed"); return; }

    size_t pos = (size_t)snprintf(buf, buf_sz,
        "{\"id\":%d,\"ok\":true,\"frame\":%u,\"count\":%d,\"max_words\":%u,\"entries\":[",
        id, (uint32_t)target, n, gpu_gp0_ring_max_words());

    for (int i = 0; i < n && pos < buf_sz - 256; i++) {
        const GpuGp0RingEntry *e = &entries[i];
        pos += (size_t)snprintf(buf + pos, buf_sz - pos,
            "%s{\"seq\":%u,\"op\":\"0x%02X\",\"n\":%u,"
            "\"src\":\"0x%08X\",\"pc\":\"0x%08X\",\"w\":[",
            i ? "," : "", e->seq, e->opcode, e->n_words,
            e->src_addr, e->pc);
        int show = e->n_words < GPU_GP0_RING_MAX_WORDS
                 ? e->n_words : GPU_GP0_RING_MAX_WORDS;
        for (int k = 0; k < show && pos < buf_sz - 32; k++) {
            pos += (size_t)snprintf(buf + pos, buf_sz - pos,
                "%s\"0x%08X\"", k ? "," : "", e->cmd[k]);
        }
        pos += (size_t)snprintf(buf + pos, buf_sz - pos, "]}");
    }
    snprintf(buf + pos, buf_sz - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
    free(entries);
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
static void json_escape_string(char *dst, size_t dst_size, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    if (!dst || dst_size == 0) return;
    if (!src) src = "";

    for (size_t in = 0; src[in] && out + 1 < dst_size; in++) {
        unsigned char c = (unsigned char)src[in];
        if (c == '"' || c == '\\') {
            if (out + 2 >= dst_size) break;
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c < 0x20) {
            if (out + 6 >= dst_size) break;
            dst[out++] = '\\';
            dst[out++] = 'u';
            dst[out++] = '0';
            dst[out++] = '0';
            dst[out++] = hex[c >> 4];
            dst[out++] = hex[c & 0x0F];
        } else {
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
}

static void handle_mc_status(int id, const char *json)
{
    (void)json;
    const char *p0 = "", *p1 = "";
    char p0_json[1024], p1_json[1024];
    uint8_t m0[2] = {0,0}, m1[2] = {0,0};
    int pres0 = 0, pres1 = 0, dirty0 = 0, dirty1 = 0;
    memcard_debug_info(0, &p0, m0, &pres0, &dirty0);
    memcard_debug_info(1, &p1, m1, &pres1, &dirty1);
    json_escape_string(p0_json, sizeof(p0_json), p0);
    json_escape_string(p1_json, sizeof(p1_json), p1);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"slot0\":{\"present\":%s,\"dirty\":%s,\"path\":\"%s\","
             "\"magic\":\"%c%c\",\"magic_hex\":\"%02X%02X\"},"
             "\"slot1\":{\"present\":%s,\"dirty\":%s,\"path\":\"%s\","
             "\"magic\":\"%c%c\",\"magic_hex\":\"%02X%02X\"}}",
             id,
             pres0 ? "true" : "false", dirty0 ? "true" : "false", p0_json,
             (m0[0] >= 0x20 && m0[0] < 0x7F) ? m0[0] : '?',
             (m0[1] >= 0x20 && m0[1] < 0x7F) ? m0[1] : '?',
             m0[0], m0[1],
             pres1 ? "true" : "false", dirty1 ? "true" : "false", p1_json,
             (m1[0] >= 0x20 && m1[0] < 0x7F) ? m1[0] : '?',
             (m1[1] >= 0x20 && m1[1] < 0x7F) ? m1[1] : '?',
             m1[0], m1[1]);
}

static void handle_spu_status(int id, const char *json)
{
    (void)json;
    SpuDebugInfo info;
    spu_debug_info(&info);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"ctrl\":\"0x%04X\",\"active_mask\":\"0x%06X\","
             "\"main_l\":%d,\"main_r\":%d,"
             "\"cd_l\":%d,\"cd_r\":%d,"
             "\"key_on_count\":%u,"
             "\"render_frames\":%llu,\"nonzero_frames\":%llu,"
             "\"last_peak\":%d,\"peak\":%d,"
             "\"cd_frames\":%u,\"cd_push_frames\":%llu,"
             "\"cd_overflow_frames\":%llu,\"cd_underflow_frames\":%llu}",
             id,
             info.ctrl & 0xFFFFu,
             info.active_mask & 0xFFFFFFu,
             info.main_l,
             info.main_r,
             info.cd_l,
             info.cd_r,
             info.key_on_count,
             (unsigned long long)info.render_frames,
             (unsigned long long)info.nonzero_frames,
             info.last_peak,
             info.peak,
             info.cd_frames,
             (unsigned long long)info.cd_push_frames,
             (unsigned long long)info.cd_overflow_frames,
             (unsigned long long)info.cd_underflow_frames);
}

/* ---- Per-voice SPU snapshot. Mirrors fields the Beetle oracle exposes
 * via PS_SPU::GetRegister(GSREG_V0_*) so cross-process diff tooling sees
 * the same JSON schema on both port 4370 and 4380.
 *
 * Single-shot emission: assemble the entire response into a heap buffer
 * and fire one send_fmt. debug_server_send_line appends '\n' on every
 * call, so multi-call patterns produce multi-line garbage on the wire. */
static void handle_spu_voices(int id, const char *json)
{
    (void)json;
    SpuGlobalState g;
    spu_get_global_state(&g);

    size_t cap = 8192;
    char *out = (char *)malloc(cap);
    if (!out) { send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    size_t off = 0;
    int n = snprintf(out + off, cap - off,
        "{\"id\":%d,\"ok\":true,"
        "\"ctrl\":\"0x%04X\",\"main_l\":\"0x%04X\",\"main_r\":\"0x%04X\","
        "\"kon\":\"0x%06X\",\"koff\":\"0x%06X\","
        "\"pmon\":\"0x%06X\",\"non\":\"0x%06X\",\"eon\":\"0x%06X\","
        "\"endx\":\"0x%06X\",\"active_mask\":\"0x%06X\","
        "\"voices\":[",
        id,
        g.ctrl, g.main_vol_l, g.main_vol_r,
        g.kon_latch, g.koff_latch,
        g.pmon, g.non, g.eon,
        g.endx, g.active_mask);
    if (n > 0) off += (size_t)n;

    for (int v = 0; v < 24; v++) {
        SpuVoiceState s;
        spu_get_voice_state(v, &s);
        n = snprintf(out + off, cap - off,
            "%s{\"v\":%d,\"active\":%d,"
            "\"vol_l\":\"0x%04X\",\"vol_r\":\"0x%04X\","
            "\"pitch\":\"0x%04X\","
            "\"start\":\"0x%05X\",\"loop\":\"0x%05X\","
            "\"adsr_lo\":\"0x%04X\",\"adsr_hi\":\"0x%04X\","
            "\"cur_addr\":\"0x%05X\",\"repeat_addr\":\"0x%05X\","
            "\"flags\":\"0x%02X\",\"sample_idx\":%d,\"phase\":\"0x%04X\"}",
            v == 0 ? "" : ",",
            v, s.active,
            s.vol_ctrl_l, s.vol_ctrl_r,
            s.pitch,
            (uint32_t)s.start_lo << 3,
            (uint32_t)s.loop_lo  << 3,
            s.adsr_lo, s.adsr_hi,
            s.cur_addr, s.repeat_addr,
            s.last_flags, s.sample_idx, s.phase);
        if (n > 0) off += (size_t)n;
    }
    n = snprintf(out + off, cap - off, "]}");
    if (n > 0) off += (size_t)n;
    send_fmt("%s", out);
    free(out);
}

/* ---- SPU event ring dump. Returns the most recent N events
 * (KEYON / KEYOFF / END_STOP / END_LOOP) with frame timestamps. */
static void handle_spu_events(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 4096) count = 4096;
    SpuEvent *evs = (SpuEvent *)malloc((size_t)count * sizeof(SpuEvent));
    if (!evs) { send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    uint32_t got = spu_event_get(evs, (uint32_t)count);
    uint64_t total = spu_event_total();
    static const char *kind_names[5] = { "?", "KEYON", "KEYOFF", "END_STOP", "END_LOOP" };

    /* Worst case ~200 chars per event; 64 KB is plenty for 4096 events. */
    size_t cap = 256u + (size_t)got * 256u;
    char *out = (char *)malloc(cap);
    if (!out) { free(evs); send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    size_t off = 0;
    int n = snprintf(out + off, cap - off,
        "{\"id\":%d,\"ok\":true,\"total\":%llu,\"count\":%u,\"events\":[",
        id, (unsigned long long)total, (unsigned)got);
    if (n > 0) off += (size_t)n;
    for (uint32_t i = 0; i < got; i++) {
        const SpuEvent *e = &evs[i];
        const char *kn = (e->kind <= 4) ? kind_names[e->kind] : "?";
        n = snprintf(out + off, cap - off,
            "%s{\"seq\":%llu,\"frame\":%u,\"kind\":\"%s\",\"v\":%d,"
            "\"pitch\":\"0x%04X\",\"addr\":\"0x%05X\","
            "\"adsr_lo\":\"0x%04X\",\"adsr_hi\":\"0x%04X\","
            "\"vol_l\":\"0x%04X\",\"vol_r\":\"0x%04X\"}",
            i == 0 ? "" : ",",
            (unsigned long long)e->seq, e->frame, kn, (int)e->voice,
            e->pitch, e->addr,
            e->adsr_lo, e->adsr_hi,
            e->vol_l, e->vol_r);
        if (n > 0) off += (size_t)n;
    }
    n = snprintf(out + off, cap - off, "]}");
    if (n > 0) off += (size_t)n;
    send_fmt("%s", out);
    free(out);
    free(evs);
}

static void handle_spu_events_reset(int id, const char *json)
{
    (void)json;
    spu_event_reset();
    send_fmt("{\"id\":%d,\"ok\":true}\n", id);
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

static void handle_sio_trace_window(int id, const char *json)
{
    int seq = json_get_int(json, "seq", -1);
    int before = json_get_int(json, "before", 8);
    int after = json_get_int(json, "after", 16);
    if (seq < 0) { send_err(id, "missing seq"); return; }
    if (before < 0) before = 0;
    if (after < 0) after = 0;

    const SioTraceEntry *buf;
    int write_idx;
    uint32_t total_seq = sio_get_trace(&buf, &write_idx);
    (void)write_idx;
    uint32_t oldest = (total_seq > (uint32_t)SIO_TRACE_CAP)
                    ? total_seq - (uint32_t)SIO_TRACE_CAP : 0;
    uint32_t lo = (uint32_t)((seq > before) ? (seq - before) : 0);
    uint32_t hi = (uint32_t)(seq + after);
    if (lo < oldest) lo = oldest;
    if (hi >= total_seq) hi = total_seq ? total_seq - 1 : 0;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"oldest\":%u,"
             "\"seq\":%d,\"entries\":[",
             id, (unsigned)total_seq, (unsigned)oldest, seq);

    int emitted = 0;
    if (total_seq > 0 && lo <= hi) {
        for (uint32_t s = lo; s <= hi; s++) {
            const SioTraceEntry *e = &buf[s % SIO_TRACE_CAP];
            if (e->seq != s) continue;
            if (emitted > 0) send_fmt(",");
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
            emitted++;
        }
    }
    send_fmt("],\"emitted\":%d}\n", emitted);
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

static void handle_sio_irq_window(int id, const char *json)
{
    int byte_seq = json_get_int(json, "byte_seq", -1);
    int before = json_get_int(json, "before", 8);
    int after = json_get_int(json, "after", 16);
    int src_filter = json_get_int(json, "src", -1);
    if (byte_seq < 0) { send_err(id, "missing byte_seq"); return; }
    if (before < 0) before = 0;
    if (after < 0) after = 0;

    uint32_t lo = (uint32_t)((byte_seq > before) ? (byte_seq - before) : 0);
    uint32_t hi = (uint32_t)(byte_seq + after);
    const SioIrqEntry *buf;
    int write_idx;
    uint32_t total_seq = sio_get_irq_ring(&buf, &write_idx);
    (void)write_idx;
    uint32_t avail = (total_seq < (uint32_t)SIO_IRQ_RING_CAP)
                   ? total_seq : (uint32_t)SIO_IRQ_RING_CAP;
    uint32_t start = total_seq - avail;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"available\":%u,"
             "\"byte_seq\":%d,\"src_filter\":%d,\"entries\":[",
             id, (unsigned)total_seq, (unsigned)avail, byte_seq, src_filter);
    int emitted = 0;
    for (uint32_t i = 0; i < avail; i++) {
        const SioIrqEntry *e = &buf[(start + i) % SIO_IRQ_RING_CAP];
        if (e->byte_seq < lo || e->byte_seq > hi) continue;
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

static void handle_card_read_summary(int id, const char *json)
{
    (void)json;
    const CardReadSummary *buf = NULL;
    uint32_t n = card_read_summary_get(&buf);

    char out[16 * 1024];
    int o = snprintf(out, sizeof(out),
                     "{\"id\":%d,\"ok\":true,\"count\":%u,\"cap\":%d,\"entries\":[",
                     id, (unsigned)n, CARD_READ_SUMMARY_CAP);
    for (uint32_t i = 0; i < n; i++) {
        const CardReadSummary *e = &buf[i];
        char peek[2 * CARD_READ_SUMMARY_PEEK + 1];
        int pi = 0;
        for (int b = 0; b < CARD_READ_SUMMARY_PEEK; b++) {
            pi += snprintf(peek + pi, sizeof(peek) - pi, "%02X", e->data_peek[b]);
        }
        peek[pi] = 0;
        o += snprintf(out + o, sizeof(out) - o,
                      "%s{\"seq\":%llu,\"cyc\":%llu,"
                      "\"slot\":%u,\"cmd\":\"0x%02X\",\"sector\":%u,"
                      "\"checksum\":\"0x%02X\",\"data_idx\":%u,"
                      "\"current_func\":\"0x%08X\",\"last_store_pc\":\"0x%08X\","
                      "\"dest_ram\":\"0x%08X\",\"data_peek\":\"%s\"}",
                      i == 0 ? "" : ",",
                      (unsigned long long)e->seq,
                      (unsigned long long)e->psx_cycle_count,
                      e->slot, e->cmd, e->sector,
                      e->checksum_card, e->data_idx_at_end,
                      e->current_func, e->last_store_pc,
                      e->dest_ram_addr, peek);
        if (o >= (int)sizeof(out) - 512) break;
    }
    snprintf(out + o, sizeof(out) - o, "]}");
    send_fmt("%s", out);
}

static void handle_card_read_summary_reset(int id, const char *json)
{
    (void)json;
    card_read_summary_reset();
    send_fmt("{\"id\":%d,\"ok\":true}\n", id);
}

static void handle_card_data_writes(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > CARD_DATA_WRITES_CAP) count = CARD_DATA_WRITES_CAP;

    const CardDataWriteEntry *buf = NULL;
    uint64_t total_seq = 0;
    uint32_t head = 0;
    uint32_t avail = card_data_writes_get(&buf, &total_seq, &head);
    if ((uint32_t)count > avail) count = (int)avail;

    char out[64 * 1024];
    int o = snprintf(out, sizeof(out),
                     "{\"id\":%d,\"ok\":true,\"total_seq\":%llu,"
                     "\"avail\":%u,\"count\":%d,\"entries\":[",
                     id, (unsigned long long)total_seq, avail, count);

    /* Iterate the OLDEST `count` entries first (reading from
     * (head - avail) forward, then taking the last `count`). */
    int start = ((int)head - count + (int)CARD_DATA_WRITES_CAP) % (int)CARD_DATA_WRITES_CAP;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % (int)CARD_DATA_WRITES_CAP;
        const CardDataWriteEntry *e = &buf[idx];
        o += snprintf(out + o, sizeof(out) - o,
                      "%s{\"seq\":%llu,\"cyc\":%llu,\"addr\":\"0x%08X\","
                      "\"value\":\"0x%08X\",\"width\":%u,"
                      "\"mc_state\":%u,\"mc_idx\":%u,\"slot\":%u,"
                      "\"store_pc\":\"0x%08X\",\"func\":\"0x%08X\"}",
                      i == 0 ? "" : ",",
                      (unsigned long long)e->seq,
                      (unsigned long long)e->psx_cycle_count,
                      e->addr, e->value, e->width,
                      e->mc_state_at_read, e->mc_data_idx_at_read,
                      e->slot, e->store_pc, e->func_addr);
        if (o >= (int)sizeof(out) - 512) break;
    }
    snprintf(out + o, sizeof(out) - o, "]}");
    send_fmt("%s", out);
}

static void handle_card_data_writes_reset(int id, const char *json)
{
    (void)json;
    card_data_writes_reset();
    send_fmt("{\"id\":%d,\"ok\":true}\n", id);
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

/* Reports the current pad word(s) and any active input override. `pad` is
 * slot 0 (kept for back-compat); slot0/slot1 report both ports' button word. */
extern uint16_t sio_get_pad_buttons(void);
extern uint16_t sio_get_pad_buttons_slot(int slot);
extern int sio_get_pad_connected(int slot);
extern int sio_get_pad_analog(int slot);
static void handle_pad_status(int id, const char *json)
{
    (void)json;
    uint16_t pad0 = sio_get_pad_buttons_slot(0);
    uint16_t pad1 = sio_get_pad_buttons_slot(1);
    send_fmt("{\"id\":%d,\"ok\":true,\"pad\":\"0x%04X\","
             "\"slot0\":{\"buttons\":\"0x%04X\",\"connected\":%s,\"analog\":%s},"
             "\"slot1\":{\"buttons\":\"0x%04X\",\"connected\":%s,\"analog\":%s},"
             "\"override\":%d,\"override_frames\":%d}\n",
             id, pad0,
             pad0, sio_get_pad_connected(0) ? "true" : "false", sio_get_pad_analog(0) ? "true" : "false",
             pad1, sio_get_pad_connected(1) ? "true" : "false", sio_get_pad_analog(1) ? "true" : "false",
             s_input_override, s_input_frames);
}

static void handle_clear_input(int id, const char *json)
{
    (void)json;
    s_input_override = -1;
    s_input_frames   = 0;
    send_ok(id);
}

static void handle_turbo(int id, const char *json)
{
    int enabled = json_get_int(json, "enabled", -1);
    if (enabled < 0) {
        enabled = json_get_int(json, "on", -1);
    }
    if (enabled < 0) {
        send_err(id, "missing enabled");
        return;
    }
    s_turbo_enabled = enabled ? 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"enabled\":%d}", id, s_turbo_enabled);
}

static void handle_turbo_state(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"enabled\":%d}", id, s_turbo_enabled);
}

/* pause / continue / step / run_to_frame: REMOVED.
 *
 * Per CLAUDE.md global rule #2 ("Never time/attach for observability —
 * always consume ring buffers"), pause/step is the wrong primitive for
 * observation. It produces synthesized snapshots ("what's state right
 * NOW") rather than reading the system's own continuously-recorded
 * history. Worse, it tempts the observer to think pause-step-read is
 * cheap; in this codebase it forced the runtime into a wait loop where
 * a dropped client connection looked like a freeze.
 *
 * Replacements (already in the runtime):
 *   - fn_entry_dump / fn_entry_tail   for what code ran
 *   - wtrace_dump                     for what memory was written
 *   - gpu_frame_dump frame=N          for what GP0 commands were issued
 *   - mdec_trace                      for MDEC events
 *   - sio_trace / sio_pc_trace        for SIO history
 *   - frame_range / get_frame         for per-frame state snapshots
 *
 * Handlers below return an error explaining the migration. The state
 * variables (s_paused, s_step_count, s_run_to) are kept as zero so
 * freeze_check still reports them and any stale callsite that reads
 * them gets a benign value. */
static void handle_pause(int id, const char *json) {
    (void)json;
    send_err(id, "pause is removed; query a ring buffer (fn_entry_tail, wtrace_dump, gpu_frame_dump, etc.) instead of synthesizing a snapshot");
}

static void handle_continue(int id, const char *json) {
    (void)json;
    send_err(id, "continue is removed (pause is removed; nothing to resume)");
}

static void handle_step(int id, const char *json) {
    (void)json;
    send_err(id, "step is removed; query a ring buffer over the window of interest instead of advancing N frames synchronously");
}

static void handle_run_to_frame(int id, const char *json) {
    (void)json;
    send_err(id, "run_to_frame is removed; use frame_range / read_frame_ram against the live frame ring buffer instead");
}

static void handle_dirty_break_range(int id, const char *json)
{
    char buf[32];
    if (!json_get_str(json, "lo", buf, sizeof(buf))) {
        send_err(id, "missing lo");
        return;
    }
    uint32_t lo = hex_to_u32(buf);
    if (!json_get_str(json, "hi", buf, sizeof(buf))) {
        send_err(id, "missing hi");
        return;
    }
    uint32_t hi = hex_to_u32(buf);
    if (hi <= lo) {
        send_err(id, "invalid range");
        return;
    }

    s_dirty_break_lo = lo;
    s_dirty_break_hi = hi;
    s_dirty_break_target = 0;
    s_dirty_break_ra = 0;
    s_dirty_break_a0 = 0;
    s_dirty_break_a1 = 0;
    s_dirty_break_a2 = 0;
    s_dirty_break_a3 = 0;
    s_dirty_break_sp = 0;
    s_dirty_break_frame = 0;
    s_dirty_break_active = 1;

    send_fmt("{\"id\":%d,\"ok\":true,\"active\":true,"
             "\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
             id, s_dirty_break_lo, s_dirty_break_hi);
}

static void handle_dirty_break_clear(int id, const char *json)
{
    (void)json;
    s_dirty_break_active = 0;
    send_ok(id);
}

static void handle_dirty_break_state(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"active\":%s,"
             "\"lo\":\"0x%08X\",\"hi\":\"0x%08X\",\"hits\":%llu,"
             "\"target\":\"0x%08X\",\"ra\":\"0x%08X\","
             "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
             "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
             "\"sp\":\"0x%08X\",\"frame\":%u,\"paused\":%s}",
             id, s_dirty_break_active ? "true" : "false",
             s_dirty_break_lo, s_dirty_break_hi,
             (unsigned long long)s_dirty_break_hits,
             s_dirty_break_target, s_dirty_break_ra,
             s_dirty_break_a0, s_dirty_break_a1,
             s_dirty_break_a2, s_dirty_break_a3,
             s_dirty_break_sp, s_dirty_break_frame,
             "false"  /* paused field kept for protocol stability; pause was removed */);
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

/* ---- Minimal self-contained PNG writer ------------------------------------
 * Emits a valid 8-bit truecolor (RGB) PNG with zero external dependencies: the
 * IDAT payload is a zlib stream whose DEFLATE body is "stored" (uncompressed)
 * blocks. Bigger on disk than a compressed PNG, but a real PNG that every
 * viewer (and the harness Read tool) accepts, and nothing new to link against
 * — which keeps the self-contained static runtime self-contained. */
static uint32_t s_png_crc_tbl[256];
static int      s_png_crc_init = 0;
static void png_crc_build(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_png_crc_tbl[n] = c;
    }
    s_png_crc_init = 1;
}
static uint32_t png_crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!s_png_crc_init) png_crc_build();
    for (size_t i = 0; i < n; i++) crc = s_png_crc_tbl[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static uint32_t png_adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    /* process in blocks so the 16-bit sums never overflow before the mod */
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        for (size_t i = 0; i < k; i++) { a += p[i]; b += a; }
        a %= 65521; b %= 65521; p += k; n -= k;
    }
    return (b << 16) | a;
}
static void png_put_be32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    png_put_be32(f, (uint32_t)len);
    uint32_t crc = 0xFFFFFFFFu;
    crc = png_crc_update(crc, (const uint8_t *)type, 4);
    crc = png_crc_update(crc, data, len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    png_put_be32(f, crc ^ 0xFFFFFFFFu);
}
/* Write an RGB (3 bytes/pixel, top-down) buffer as a PNG. Returns 1 on success. */
static int png_write_rgb(FILE *f, const uint8_t *rgb, uint32_t w, uint32_t h) {
    static const uint8_t sig[8] = { 137,80,78,71,13,10,26,10 };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    ihdr[0]=(uint8_t)(w>>24); ihdr[1]=(uint8_t)(w>>16); ihdr[2]=(uint8_t)(w>>8); ihdr[3]=(uint8_t)w;
    ihdr[4]=(uint8_t)(h>>24); ihdr[5]=(uint8_t)(h>>16); ihdr[6]=(uint8_t)(h>>8); ihdr[7]=(uint8_t)h;
    ihdr[8]=8;   /* bit depth   */
    ihdr[9]=2;   /* color type 2 = truecolor RGB */
    ihdr[10]=0;  /* compression */
    ihdr[11]=0;  /* filter      */
    ihdr[12]=0;  /* interlace   */
    png_chunk(f, "IHDR", ihdr, sizeof ihdr);

    /* Filtered raw scanlines: each row prefixed with filter byte 0 (None). */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (1 + (size_t)w * 3);
        row[0] = 0;
        memcpy(row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }

    /* zlib stream: 2-byte header + stored DEFLATE blocks + 4-byte Adler32. */
    size_t nblocks = (raw_len + 65534) / 65535; if (nblocks == 0) nblocks = 1;
    size_t z_len = 2 + nblocks * 5 + raw_len + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); return 0; }
    size_t zi = 0;
    z[zi++] = 0x78;  /* CMF: 32K window, deflate */
    z[zi++] = 0x01;  /* FLG: check bits make 0x7801 a multiple of 31 */
    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off; if (n > 65535) n = 65535;
        int final = (off + n >= raw_len);
        z[zi++] = (uint8_t)(final ? 1 : 0);          /* BFINAL | BTYPE=00 */
        z[zi++] = (uint8_t)(n & 0xFF); z[zi++] = (uint8_t)(n >> 8);
        uint16_t nl = (uint16_t)~n;
        z[zi++] = (uint8_t)(nl & 0xFF); z[zi++] = (uint8_t)(nl >> 8);
        memcpy(z + zi, raw + off, n); zi += n; off += n;
    }
    uint32_t ad = png_adler32(raw, raw_len);
    z[zi++] = (uint8_t)(ad >> 24); z[zi++] = (uint8_t)(ad >> 16);
    z[zi++] = (uint8_t)(ad >> 8);  z[zi++] = (uint8_t)ad;
    free(raw);

    png_chunk(f, "IDAT", z, zi);
    free(z);
    png_chunk(f, "IEND", NULL, 0);
    return 1;
}

/* Unified screenshot: writes an 8-bit RGB PNG of the current display to "path"
 * (default psx_screenshot.png in the runtime cwd) and answers with a single
 * metadata line.  Registered as both "screenshot" and "screenshot_file";
 * the old "screenshot" inline-hex-row variant streamed h+1 response lines
 * per request, which violated the one-request/one-response protocol and
 * poisoned every client connection that used it. */
static void handle_screenshot_file(int id, const char *json)
{
    /* Under the OpenGL FBO-present path, CPU VRAM can be stale (the FBO holds
     * the freshest frame and is presented without a readback). Sync it down so
     * the capture reflects what's on screen. No-op for the software backend or
     * when no GPU frame is pending. Safe here: the debug server is pumped on
     * the main (GL-context) thread. */
    extern void gl_renderer_sync_cpu(void);
    gl_renderer_sync_cpu();

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.width == 0 || di.height == 0) {
        send_err(id, "display disabled"); return;
    }

    uint32_t w = di.width;  if (w > 640) w = 640;
    uint32_t h = di.height; if (h > 512) h = 512;

    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        strncpy(path, "psx_screenshot.png", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    FILE *f = fopen(path, "wb");
    if (!f) { send_err(id, "cannot open file"); return; }

    /* Gather pixels top-down, RGB. */
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) { fclose(f); send_err(id, "alloc failed"); return; }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, b;
            gpu_display_pixel_rgb(&di, x, y, &r, &g, &b);
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = r; p[1] = g; p[2] = b;
        }
    }
    int ok = png_write_rgb(f, rgb, w, h);
    free(rgb);
    fclose(f);
    if (!ok) { send_err(id, "png encode failed"); return; }

    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%u,\"height\":%u}",
             id, path, w, h);
}

static void handle_vram_peek(int id, const char *json)
{
    int x = json_get_int(json, "x", 0);
    int y = json_get_int(json, "y", 0);
    int w = json_get_int(json, "w", 8);
    int h = json_get_int(json, "h", 1);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 128) w = 128;
    if (h > 128) h = 128;
    size_t hex_len = (size_t)w * h * 4 + 1;
    char *hex = (char *)malloc(hex_len);
    if (!hex) { send_err(id, "alloc failed"); return; }
    int pos = 0;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t p = gpu_vram_peek(x + col, y + row);
            pos += snprintf(hex + pos, hex_len - pos, "%04x", p);
        }
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"hex\":\"%s\"}",
             id, x, y, w, h, hex);
    free(hex);
}

/* GL-backend diagnostic: peek the GPU-side (FBO) VRAM for a rect, plus the
 * coherency flags/rects — lets probes diff FBO truth against CPU truth. */
extern int  gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out);
extern void gl_renderer_diag(int *gpu_dirty, int pending[5], int pack[5]);

static void handle_gl_fbo_peek(int id, const char *json)
{
    int x = json_get_int(json, "x", 0);
    int y = json_get_int(json, "y", 0);
    int w = json_get_int(json, "w", 8);
    int h = json_get_int(json, "h", 1);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 128) w = 128;
    if (h > 128) h = 128;
    uint16_t px[128 * 128];
    if (!gl_renderer_fbo_peek(x, y, w, h, px)) {
        send_err(id, "GL pipeline inactive or rect out of range"); return;
    }
    int gpu_dirty = 0, pend[5] = {0}, pack[5] = {0};
    gl_renderer_diag(&gpu_dirty, pend, pack);
    size_t hex_len = (size_t)w * h * 4 + 1;
    char *hex = (char *)malloc(hex_len);
    if (!hex) { send_err(id, "alloc failed"); return; }
    int pos = 0;
    for (int i = 0; i < w * h; i++)
        pos += snprintf(hex + pos, hex_len - pos, "%04x", px[i]);
    send_fmt("{\"id\":%d,\"ok\":true,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
             "\"gpu_dirty\":%d,"
             "\"pending\":[%d,%d,%d,%d,%d],\"pack\":[%d,%d,%d,%d,%d],"
             "\"hex\":\"%s\"}",
             id, x, y, w, h, gpu_dirty,
             pend[0], pend[1], pend[2], pend[3], pend[4],
             pack[0], pack[1], pack[2], pack[3], pack[4], hex);
    free(hex);
}

extern int gl_renderer_vram_diff(uint32_t *count, int bbox[4],
                                 int samples[8][2], uint16_t samples_px[8][2]);

static void handle_gl_vram_diff(int id, const char *json)
{
    (void)json;
    uint32_t n = 0;
    int bbox[4] = {0}, samples[8][2] = {{0}};
    uint16_t spx[8][2] = {{0}};
    int r = gl_renderer_vram_diff(&n, bbox, samples, spx);
    if (!r) { send_err(id, "GL pipeline inactive"); return; }
    int ns = r - 1;
    int gpu_dirty = 0, pend[5] = {0}, pack[5] = {0};
    gl_renderer_diag(&gpu_dirty, pend, pack);
    char smp[512]; int pos = 0; smp[0] = 0;
    for (int i = 0; i < ns; i++)
        pos += snprintf(smp + pos, sizeof(smp) - pos,
                        "%s[%d,%d,\"0x%04X\",\"0x%04X\"]", i ? "," : "",
                        samples[i][0], samples[i][1], spx[i][0], spx[i][1]);
    send_fmt("{\"id\":%d,\"ok\":true,\"mismatches\":%u,"
             "\"bbox\":[%d,%d,%d,%d],\"gpu_dirty\":%d,"
             "\"samples_xy_fbo_cpu\":[%s]}",
             id, n, bbox[0], bbox[1], bbox[2], bbox[3], gpu_dirty, smp);
}

/* GL-backend coherency event ring: dump the last n events (default 200),
 * optionally only events from frame >= frame_min. Always-on capture; this
 * just reads a window. */
static const char *gl_coh_kind_name(int k)
{
    switch (k) {
    case GL_COH_FLUSH:    return "flush";
    case GL_COH_FILL:     return "fill";
    case GL_COH_COPY_SRC: return "copy_src";
    case GL_COH_COPY:     return "copy";
    case GL_COH_DRAW:     return "draw";
    case GL_COH_PACK:     return "pack";
    case GL_COH_ENSURE:   return "ensure";
    case GL_COH_PRESENT:  return "present";
    case GL_COH_UPLOAD:   return "upload";
    case GL_COH_PEEK:     return "peek";
    case GL_COH_DIFF:     return "diff";
    default:              return "?";
    }
}

static void handle_gl_coh_ring(int id, const char *json)
{
    int n = json_get_int(json, "n", 200);
    long frame_min = json_get_int(json, "frame_min", -1);
    if (n < 1) n = 1;
    if (n > 8192) n = 8192;
    uint64_t total = gl_renderer_coh_total();
    uint64_t start = total > (uint64_t)n ? total - (uint64_t)n : 0;
    int bufsz = 64 + n * 64;
    char *buf = (char *)malloc((size_t)bufsz);
    if (!buf) { send_err(id, "alloc failed"); return; }
    int pos = snprintf(buf, bufsz,
                       "{\"id\":%d,\"ok\":true,\"total\":%llu,\"events\":[",
                       id, (unsigned long long)total);
    int first = 1;
    for (uint64_t s = start; s < total && pos < bufsz - 128; s++) {
        GlCohEvent e;
        if (!gl_renderer_coh_get(s, &e)) continue;
        if (frame_min >= 0 && e.frame < (uint32_t)frame_min) continue;
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s[%llu,%u,\"%s\",%d,%d,%d,%d]",
                        first ? "" : ",", (unsigned long long)s, e.frame,
                        gl_coh_kind_name(e.kind), e.x0, e.y0, e.x1, e.y1);
        first = 0;
    }
    pos += snprintf(buf + pos, bufsz - pos, "]}");
    send_fmt("%s", buf);
    free(buf);
}

/* ---- Write trace: hook + handlers (Tier 1 reverse debugger) ---- */
extern CPUState *debug_cpu_ptr;

static void wtrace_fill_entry(WriteTraceEntry *e, uint64_t seq,
                              uint32_t phys, uint32_t old_val,
                              uint32_t new_val, uint8_t width)
{
    uint32_t ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    e->seq       = seq;
    e->addr      = phys;
    e->old_val   = old_val;
    e->new_val   = new_val;
    e->ra        = ra;
    e->func_addr = g_debug_current_func_addr;
    e->pc        = g_debug_last_store_pc;
    e->cpu_pc    = debug_cpu_ptr ? debug_cpu_ptr->pc      : 0;
    e->sp        = debug_cpu_ptr ? debug_cpu_ptr->gpr[29] : 0;
    e->v0        = debug_cpu_ptr ? debug_cpu_ptr->gpr[2]  : 0;
    e->v1        = debug_cpu_ptr ? debug_cpu_ptr->gpr[3]  : 0;
    e->a0        = debug_cpu_ptr ? debug_cpu_ptr->gpr[4]  : 0;
    e->a1        = debug_cpu_ptr ? debug_cpu_ptr->gpr[5]  : 0;
    e->a2        = debug_cpu_ptr ? debug_cpu_ptr->gpr[6]  : 0;
    e->a3        = debug_cpu_ptr ? debug_cpu_ptr->gpr[7]  : 0;
    e->t0        = debug_cpu_ptr ? debug_cpu_ptr->gpr[8]  : 0;
    e->t1        = debug_cpu_ptr ? debug_cpu_ptr->gpr[9]  : 0;
    e->frame     = (uint32_t)s_frame_count;
    e->width     = width;
}

/* Record a single write into the RAM trace ring buffer. */
static void wtrace_record(uint32_t phys, uint32_t old_val, uint32_t new_val, uint8_t width)
{
    if (!s_wtrace) return;
    WriteTraceEntry *e = &s_wtrace[s_wtrace_head];
    wtrace_fill_entry(e, s_wtrace_seq++, phys, old_val, new_val, width);
    s_wtrace_head = (s_wtrace_head + 1) % WRITE_TRACE_CAP;
}

static void wtrace_boot_record(uint32_t phys, uint32_t old_val,
                               uint32_t new_val, uint8_t width)
{
    if (!s_wtrace_boot || s_wtrace_boot_range_count == 0) return;
    int match = 0;
    for (int i = 0; i < s_wtrace_boot_range_count; i++) {
        if (phys >= s_wtrace_boot_ranges[i].lo &&
            phys <  s_wtrace_boot_ranges[i].hi) {
            match = 1;
            break;
        }
    }
    if (!match) return;

    uint64_t seq = s_wtrace_boot_total++;
    if (s_wtrace_boot_count >= WRITE_TRACE_BOOT_CAP) return;
    WriteTraceEntry *e = &s_wtrace_boot[s_wtrace_boot_count++];
    wtrace_fill_entry(e, seq, phys, old_val, new_val, width);
}

/* Always-on catch-all recorder.  Lean record (no register window). */
static void wtrace_all_record(uint32_t phys, uint32_t new_val, uint8_t width)
{
    if (!s_wtrace_all) return;
    WriteTraceAllEntry *e = &s_wtrace_all[s_wtrace_all_head];
    e->seq     = s_wtrace_all_seq++;
    e->addr    = phys;
    e->new_val = new_val;
    e->pc      = g_debug_last_store_pc;
    e->ra      = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    e->frame   = (uint32_t)s_frame_count;
    e->w       = width;
    s_wtrace_all_head = (s_wtrace_all_head + 1) % WRITE_TRACE_ALL_CAP;
}

static void wtrace_transition_record(uint32_t phys, uint32_t old_val,
                                     uint32_t new_val, uint8_t width)
{
    if (!s_wtrace_trans || s_wtrace_trans_range_count == 0) return;
    if (old_val == new_val) return;

    int match = 0;
    for (int i = 0; i < s_wtrace_trans_range_count; i++) {
        if (phys >= s_wtrace_trans_ranges[i].lo &&
            phys <  s_wtrace_trans_ranges[i].hi) {
            match = 1;
            break;
        }
    }
    if (!match) return;

    WriteTraceTransEntry *e = &s_wtrace_trans[s_wtrace_trans_head];
    e->seq       = s_wtrace_trans_seq++;
    e->addr      = phys;
    e->old_val   = old_val;
    e->new_val   = new_val;
    e->pc        = g_debug_last_store_pc;
    e->func_addr = g_debug_current_func_addr;
    e->ra        = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    e->sp        = debug_cpu_ptr ? debug_cpu_ptr->gpr[29] : 0;
    e->s0        = debug_cpu_ptr ? debug_cpu_ptr->gpr[16] : 0;
    e->s1        = debug_cpu_ptr ? debug_cpu_ptr->gpr[17] : 0;
    e->s2        = debug_cpu_ptr ? debug_cpu_ptr->gpr[18] : 0;
    e->s3        = debug_cpu_ptr ? debug_cpu_ptr->gpr[19] : 0;
    e->stk20     = (debug_cpu_ptr && e->sp) ? trace_read_word(debug_cpu_ptr, e->sp + 20u) : 0;
    e->stk40     = (debug_cpu_ptr && e->sp) ? trace_read_word(debug_cpu_ptr, e->sp + 40u) : 0;
    e->frame     = (uint32_t)s_frame_count;
    e->width     = width;
    s_wtrace_trans_head = (s_wtrace_trans_head + 1) % WRITE_TRACE_TRANS_CAP;
}

/* Compat no-op: ape-flavored generated code emits debug_server_log_call_entry_cpu
 * at JAL sites (call-entry logging). Not needed for the card-driver comparison;
 * stub it so the good baseline links + runs. */
void debug_server_log_call_entry_cpu(uint32_t func_addr, CPUState *cpu) {
    (void)func_addr; (void)cpu;
}

/* Dedicated sparse card-driver-state ring (ported from ape-fw for good-vs-bad
 * comparison): card state table 0x9F20, result flags 0xB9D0, byte counter 0x72F0,
 * chain success 0x7520, chain ptrs 0x7528, and the EvCB card-event entries
 * 0xE044-0xE0D0 (status/spec/mode). Sparse => no eviction over a session. */
#define CARD_TRACE_CAP (1u << 16)
typedef struct {
    uint64_t seq; uint32_t phys; uint32_t old_val; uint32_t new_val;
    uint32_t pc; uint32_t cpu_pc; uint32_t ra; uint32_t func; uint32_t frame;
    uint8_t width; uint8_t in_exception;
} CardTraceEntry;
static CardTraceEntry s_card_trace[CARD_TRACE_CAP];
static uint64_t s_card_trace_seq = 0;
static inline int is_card_critical_addr(uint32_t phys) {
    return (phys >= 0x00009F20u && phys < 0x00009F40u) ||
           (phys >= 0x0000B9D0u && phys < 0x0000B9F0u) ||
           (phys >= 0x000072F0u && phys < 0x000072F4u) ||
           (phys >= 0x00007520u && phys < 0x00007524u) ||
           (phys >= 0x00007528u && phys < 0x00007530u) ||
           (phys >= 0x0000E044u && phys < 0x0000E0D0u);
}
static void card_trace_record(uint32_t phys, uint32_t old_val, uint32_t new_val, uint8_t width) {
    uint64_t seq = s_card_trace_seq++;
    CardTraceEntry *e = &s_card_trace[seq & (CARD_TRACE_CAP - 1u)];
    e->seq = seq; e->phys = phys; e->old_val = old_val; e->new_val = new_val;
    e->pc = g_debug_last_store_pc;
    e->cpu_pc = debug_cpu_ptr ? debug_cpu_ptr->pc : 0;
    e->ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    e->func = g_debug_current_func_addr;
    e->frame = (uint32_t)s_frame_count;
    e->width = width;
    e->in_exception = (uint8_t)(psx_get_in_exception() ? 1u : 0u);
}

static void handle_card_trace_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 200);
    if (count > (int)CARD_TRACE_CAP) count = CARD_TRACE_CAP;
    char alo[32] = {0}, ahi[32] = {0};
    uint32_t flo = 0, fhi = 0xFFFFFFFFu; int filt = 0;
    if (json_get_str(json, "addr_lo", alo, sizeof alo)) { flo = (uint32_t)strtoul(alo, NULL, 0); filt = 1; }
    if (json_get_str(json, "addr_hi", ahi, sizeof ahi)) { fhi = (uint32_t)strtoul(ahi, NULL, 0); filt = 1; }
    uint64_t total = s_card_trace_seq;
    uint64_t avail = (total < CARD_TRACE_CAP) ? total : CARD_TRACE_CAP;
    const size_t BUF_SZ = 2 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ);
    if (!out) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"count\":%d,\"filtered\":%d,\"entries\":[",
                    id, (unsigned long long)total, count, filt);
    uint64_t start = filt ? (total - avail) : (total - ((uint64_t)count < avail ? (uint64_t)count : avail));
    int emitted = 0; int first = 1;
    for (uint64_t s = start; s < total && emitted < count; s++) {
        CardTraceEntry *e = &s_card_trace[s & (CARD_TRACE_CAP - 1u)];
        if (filt && (e->phys < flo || e->phys >= fhi)) continue;
        if (pos > BUF_SZ - 512) break;
        pos += snprintf(out + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"phys\":\"0x%05X\",\"old\":\"0x%08X\",\"new\":\"0x%08X\","
                        "\"w\":%u,\"pc\":\"0x%08X\",\"cpu_pc\":\"0x%08X\",\"ra\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"in_exc\":%u,\"frame\":%u}",
                        first ? "" : ",", (unsigned long long)e->seq, e->phys,
                        e->old_val, e->new_val, e->width, e->pc, e->cpu_pc, e->ra,
                        e->func, e->in_exception, e->frame);
        first = 0; emitted++;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "]}\n");
    debug_server_send_line(out);
    free(out);
}

/* Multi-range check called from memory.c write paths.
 * Iterates up to 8 ranges; records if any match.
 * The always-on catch-all ring is recorded UNCONDITIONALLY first so
 * late-connecting probes can still see recent writes without arming. */
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width)
{
#ifdef PSX_NO_DEBUG_TOOLS
    (void)phys; (void)old_val; (void)new_val; (void)width;
    return;
#endif
    if (is_card_critical_addr(phys)) card_trace_record(phys, old_val, new_val, width);
    wtrace_all_record(phys, new_val, width);
    wtrace_transition_record(phys, old_val, new_val, width);
    wtrace_boot_record(phys, old_val, new_val, width);
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
#ifdef PSX_NO_DEBUG_TOOLS
    (void)addr; (void)val; (void)width;
    return;
#endif
    if (!s_mmio_trace) return;
    MmioTraceEntry *e = &s_mmio_trace[s_mmio_trace_head];
    e->seq       = s_mmio_trace_seq++;
    e->addr      = addr;
    e->val       = val;
    e->func_addr = g_debug_current_func_addr;
    e->pc        = g_debug_last_store_pc;
    e->cpu_pc    = debug_cpu_ptr ? debug_cpu_ptr->pc      : 0;
    e->ra        = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    e->sp        = debug_cpu_ptr ? debug_cpu_ptr->gpr[29] : 0;
    e->a0        = debug_cpu_ptr ? debug_cpu_ptr->gpr[4]  : 0;
    e->a1        = debug_cpu_ptr ? debug_cpu_ptr->gpr[5]  : 0;
    e->a2        = debug_cpu_ptr ? debug_cpu_ptr->gpr[6]  : 0;
    e->a3        = debug_cpu_ptr ? debug_cpu_ptr->gpr[7]  : 0;
    e->sr        = debug_cpu_ptr ? debug_cpu_ptr->cop0[12] : 0;
    e->epc       = debug_cpu_ptr ? debug_cpu_ptr->cop0[14] : 0;
    e->istat     = i_stat;
    e->imask     = i_mask;
    e->frame     = (uint32_t)s_frame_count;
    e->width     = width;
    s_mmio_trace_head = (s_mmio_trace_head + 1) % MMIO_TRACE_CAP;

    /* Mirror GP1 (0x1F801814) writes into the dedicated long-retention
     * display-control ring. */
    if (addr == 0x1F801814u && s_gp1_trace) {
        MmioTraceEntry *g = &s_gp1_trace[s_gp1_trace_head];
        *g = *e;
        g->seq = s_gp1_trace_seq++;
        s_gp1_trace_head = (s_gp1_trace_head + 1) % GP1_TRACE_CAP;
    }
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

/* Normalized verb: disarm every range in one shot. Parity with
 * psx-beetle's wtrace_disarm_all. */
static void handle_wtrace_disarm_all(int id, const char *json)
{
    (void)json;
    s_wtrace_range_count = 0;
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
                    "\"dirty_ram_guard_yields\":%llu,"
                    "\"fn_entry_total\":%llu,"
                    "\"sio_irq_total\":%u,"
                    "\"sio_byte_seq\":%u,"
                    "\"mc_max_state\":%d,"
                    "\"mc_aborts\":%d,"
                    "\"mc_read_done\":%d,"
                    "\"tx_writes\":%d,"
                    "\"psx_cycle_count\":%llu,"
                    "\"frame_count\":%llu,"
                    "\"paused\":%d,"
                    "\"step_count\":%d,"
                    "\"run_to\":%u,"
                    "\"client_connected\":%d,"
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
                    (unsigned long long)g_dirty_ram_guard_yields,
                    (unsigned long long)s_fn_entry_seq,
                    irq_total,
                    sio_get_seq(),
                    sio_get_mc_max_state(),
                    sio_get_mc_abort_count(),
                    sio_get_mc_read_done(),
                    sio_get_tx_writes(),
                    (unsigned long long)psx_get_cycle_count(),
                    (unsigned long long)s_frame_count,
                    s_paused,
                    s_step_count,
                    s_run_to,
                    (s_client != SOCK_INVALID),
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

static void handle_wtrace_boot_stats(int id, const char *json)
{
    (void)json;
    uint64_t newest = (s_wtrace_boot_total > 0) ? s_wtrace_boot_total - 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"stored\":%u,"
             "\"capacity\":%d,\"newest_seq\":%llu,\"ranges\":%d,"
             "\"full\":%s}",
             id, (unsigned long long)s_wtrace_boot_total,
             s_wtrace_boot_count, WRITE_TRACE_BOOT_CAP,
             (unsigned long long)newest, s_wtrace_boot_range_count,
             (s_wtrace_boot_count >= WRITE_TRACE_BOOT_CAP) ? "true" : "false");
}

static void handle_wtrace_boot_clear(int id, const char *json)
{
    (void)json;
    s_wtrace_boot_total = 0;
    s_wtrace_boot_count = 0;
    if (s_wtrace_boot)
        memset(s_wtrace_boot, 0,
               (size_t)WRITE_TRACE_BOOT_CAP * sizeof(WriteTraceEntry));
    send_ok(id);
}

static void handle_wtrace_boot_dump(int id, const char *json)
{
    if (!s_wtrace_boot) { send_err(id, "boot trace not initialized"); return; }

    char lo_str[32], hi_str[32];
    uint32_t filter_lo = 0, filter_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "addr_lo", lo_str, sizeof(lo_str)))
        filter_lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", hi_str, sizeof(hi_str)))
        filter_hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;

    int max_out = json_get_int(json, "count", 256);
    if (max_out < 1) max_out = 1;
    if (max_out > WRITE_TRACE_BOOT_CAP) max_out = WRITE_TRACE_BOOT_CAP;
    if (max_out > 1024) max_out = 1024;
    int newest_first = json_get_int(json, "newest", 0) != 0;

    const uint32_t MAX_OUT = (uint32_t)max_out;
    size_t BUF_SZ = 256u + (size_t)MAX_OUT * 512u;
    if (BUF_SZ > (size_t)128 * 1024 * 1024) BUF_SZ = (size_t)128 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"stored\":%u,"
                    "\"entries\":[",
                    id, (unsigned long long)s_wtrace_boot_total,
                    s_wtrace_boot_count);
    for (uint32_t i = 0; i < s_wtrace_boot_count &&
                         emitted < MAX_OUT &&
                         pos < BUF_SZ - 256; i++) {
        uint32_t idx = newest_first ? (s_wtrace_boot_count - 1u - i) : i;
        WriteTraceEntry *e = &s_wtrace_boot[idx];
        if (e->addr < filter_lo || e->addr >= filter_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"old\":\"0x%08X\","
                        "\"new\":\"0x%08X\",\"ra\":\"0x%08X\",\"func\":\"0x%08X\","
                        "\"pc\":\"0x%08X\",\"cpu_pc\":\"0x%08X\",\"sp\":\"0x%08X\","
                        "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\","
                        "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                        "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\","
                        "\"frame\":%u,\"w\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->old_val, e->new_val, e->ra, e->func_addr,
                        e->pc, e->cpu_pc, e->sp,
                        e->v0, e->v1, e->a0, e->a1, e->a2, e->a3,
                        e->t0, e->t1, e->frame, (unsigned)e->width);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%u}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

typedef struct {
    int used;
    uint32_t addr;
    uint64_t writes;
    uint64_t first_seq;
    uint64_t last_seq;
    uint32_t first_new;
    uint32_t last_new;
    uint32_t first_pc;
    uint32_t last_pc;
    uint32_t first_ra;
    uint32_t last_ra;
    uint32_t first_func;
    uint32_t last_func;
    uint32_t first_frame;
    uint32_t last_frame;
    uint32_t first_width;
    uint32_t last_width;
    uint32_t nonzero_writes;
    uint32_t min_new;
    uint32_t max_new;
    uint32_t or_new;
    uint32_t has_ff;
    uint32_t has_ffff;
    uint64_t first_nonzero_seq;
    uint32_t first_nonzero_new;
    uint32_t first_nonzero_pc;
    uint32_t first_nonzero_frame;
    uint32_t transition_count;
    uint32_t transitions[8];
} WTraceBootSummary;

static void handle_wtrace_boot_summary(int id, const char *json)
{
    if (!s_wtrace_boot) { send_err(id, "boot trace not initialized"); return; }

    char lo_str[32], hi_str[32];
    uint32_t filter_lo = 0, filter_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "addr_lo", lo_str, sizeof(lo_str)))
        filter_lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", hi_str, sizeof(hi_str)))
        filter_hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;

    int max_addrs = json_get_int(json, "max_addrs", 256);
    if (max_addrs < 1) max_addrs = 1;
    if (max_addrs > 1024) max_addrs = 1024;

    WTraceBootSummary *sum =
        (WTraceBootSummary *)calloc((size_t)max_addrs, sizeof(WTraceBootSummary));
    if (!sum) { send_err(id, "oom"); return; }

    int distinct = 0;
    int overflow = 0;
    for (uint32_t i = 0; i < s_wtrace_boot_count; i++) {
        WriteTraceEntry *e = &s_wtrace_boot[i];
        if (e->addr < filter_lo || e->addr >= filter_hi) continue;

        int slot = -1;
        for (int j = 0; j < distinct; j++) {
            if (sum[j].used && sum[j].addr == e->addr) {
                slot = j;
                break;
            }
        }
        if (slot < 0) {
            if (distinct >= max_addrs) {
                overflow = 1;
                continue;
            }
            slot = distinct++;
            sum[slot].used = 1;
            sum[slot].addr = e->addr;
            sum[slot].first_seq = e->seq;
            sum[slot].first_new = e->new_val;
            sum[slot].first_pc = e->pc;
            sum[slot].first_ra = e->ra;
            sum[slot].first_func = e->func_addr;
            sum[slot].first_frame = e->frame;
            sum[slot].first_width = e->width;
            sum[slot].min_new = e->new_val;
            sum[slot].max_new = e->new_val;
            sum[slot].or_new = e->new_val;
            if (e->new_val == 0xFFu) sum[slot].has_ff = 1;
            if (e->new_val == 0xFFFFu) sum[slot].has_ffff = 1;
            sum[slot].transitions[0] = e->new_val;
            sum[slot].transition_count = 1;
        } else if (sum[slot].last_new != e->new_val) {
            if (sum[slot].transition_count < 8)
                sum[slot].transitions[sum[slot].transition_count] = e->new_val;
            sum[slot].transition_count++;
        }

        sum[slot].writes++;
        sum[slot].last_seq = e->seq;
        sum[slot].last_new = e->new_val;
        sum[slot].last_pc = e->pc;
        sum[slot].last_ra = e->ra;
        sum[slot].last_func = e->func_addr;
        sum[slot].last_frame = e->frame;
        sum[slot].last_width = e->width;
        if (e->new_val < sum[slot].min_new) sum[slot].min_new = e->new_val;
        if (e->new_val > sum[slot].max_new) sum[slot].max_new = e->new_val;
        sum[slot].or_new |= e->new_val;
        if (e->new_val == 0xFFu) sum[slot].has_ff = 1;
        if (e->new_val == 0xFFFFu) sum[slot].has_ffff = 1;
        if (e->new_val != 0) {
            if (sum[slot].nonzero_writes == 0) {
                sum[slot].first_nonzero_seq = e->seq;
                sum[slot].first_nonzero_new = e->new_val;
                sum[slot].first_nonzero_pc = e->pc;
                sum[slot].first_nonzero_frame = e->frame;
            }
            sum[slot].nonzero_writes++;
        }
    }

    size_t BUF_SZ = 512u + (size_t)distinct * 768u;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { free(sum); send_err(id, "oom"); return; }

    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"stored\":%u,"
                    "\"distinct\":%d,\"overflow\":%s,\"entries\":[",
                    id, (unsigned long long)s_wtrace_boot_total,
                    s_wtrace_boot_count, distinct, overflow ? "true" : "false");
    for (int i = 0; i < distinct && pos < BUF_SZ - 512; i++) {
        WTraceBootSummary *s = &sum[i];
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"addr\":\"0x%08X\",\"writes\":%llu,"
                        "\"first_seq\":%llu,\"last_seq\":%llu,"
                        "\"first_new\":\"0x%08X\",\"last_new\":\"0x%08X\","
                        "\"first_pc\":\"0x%08X\",\"last_pc\":\"0x%08X\","
                        "\"first_ra\":\"0x%08X\",\"last_ra\":\"0x%08X\","
                        "\"first_func\":\"0x%08X\",\"last_func\":\"0x%08X\","
                        "\"first_frame\":%u,\"last_frame\":%u,"
                        "\"first_w\":%u,\"last_w\":%u,"
                        "\"nonzero_writes\":%u,"
                        "\"min_new\":\"0x%08X\",\"max_new\":\"0x%08X\","
                        "\"or_new\":\"0x%08X\",\"has_ff\":%s,\"has_ffff\":%s",
                        (i == 0) ? "" : ",",
                        s->addr, (unsigned long long)s->writes,
                        (unsigned long long)s->first_seq,
                        (unsigned long long)s->last_seq,
                        s->first_new, s->last_new, s->first_pc, s->last_pc,
                        s->first_ra, s->last_ra, s->first_func, s->last_func,
                        s->first_frame, s->last_frame,
                        s->first_width, s->last_width, s->nonzero_writes,
                        s->min_new, s->max_new, s->or_new,
                        s->has_ff ? "true" : "false",
                        s->has_ffff ? "true" : "false");
        if (s->nonzero_writes != 0) {
            pos += snprintf(buf + pos, BUF_SZ - pos,
                            ",\"first_nonzero_seq\":%llu,"
                            "\"first_nonzero_new\":\"0x%08X\","
                            "\"first_nonzero_pc\":\"0x%08X\","
                            "\"first_nonzero_frame\":%u",
                            (unsigned long long)s->first_nonzero_seq,
                            s->first_nonzero_new,
                            s->first_nonzero_pc,
                            s->first_nonzero_frame);
        }
        pos += snprintf(buf + pos, BUF_SZ - pos, ",\"transitions\":[");
        uint32_t transition_emit = s->transition_count;
        if (transition_emit > 8) transition_emit = 8;
        for (uint32_t t = 0; t < transition_emit && pos < BUF_SZ - 64; t++) {
            pos += snprintf(buf + pos, BUF_SZ - pos,
                            "%s\"0x%08X\"", (t == 0) ? "" : ",",
                            s->transitions[t]);
        }
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "],\"transition_count\":%u}", s->transition_count);
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
    free(sum);
}

/* ---- Always-on catch-all wtrace handlers (parity with psx-beetle) ---- */

static void handle_wtrace_all_stats(int id, const char *json)
{
    (void)json;
    uint64_t total = s_wtrace_all_seq;
    uint64_t oldest = (total <= WRITE_TRACE_ALL_CAP) ? 0 : total - WRITE_TRACE_ALL_CAP;
    uint64_t newest = (total > 0) ? total - 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%d,"
             "\"oldest_seq\":%llu,\"newest_seq\":%llu}",
             id, (unsigned long long)total, WRITE_TRACE_ALL_CAP,
             (unsigned long long)oldest, (unsigned long long)newest);
}

static void handle_wtrace_all_reset(int id, const char *json)
{
    (void)json;
    s_wtrace_all_seq = 0;
    s_wtrace_all_head = 0;
    if (s_wtrace_all)
        memset(s_wtrace_all, 0,
               (size_t)WRITE_TRACE_ALL_CAP * sizeof(WriteTraceAllEntry));
    send_ok(id);
}

static void handle_wtrace_all_dump(int id, const char *json)
{
    if (!s_wtrace_all) { send_err(id, "wtrace_all not initialized"); return; }

    /* Optional post-hoc address filter (matches wtrace_dump shape). */
    char lo_str[32], hi_str[32];
    uint32_t flo = 0, fhi = 0xFFFFFFFFu;
    if (json_get_str(json, "addr_lo", lo_str, sizeof(lo_str)))
        flo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", hi_str, sizeof(hi_str)))
        fhi = hex_to_u32(hi_str) & 0x1FFFFFFFu;

    uint64_t total = s_wtrace_all_seq;
    uint32_t avail = (total < WRITE_TRACE_ALL_CAP)
                     ? (uint32_t)total : WRITE_TRACE_ALL_CAP;
    uint32_t start = (total < WRITE_TRACE_ALL_CAP) ? 0 : s_wtrace_all_head;

    int max_out = json_get_int(json, "count", 256);
    if (max_out < 1) max_out = 1;
    if (max_out > 2048) max_out = 2048;
    int newest_first = json_get_int(json, "newest", 0) != 0;

    const uint32_t MAX_OUT = (uint32_t)max_out;
    size_t BUF_SZ = 256u + (size_t)MAX_OUT * 200u;
    if (BUF_SZ > (size_t)64 * 1024 * 1024) BUF_SZ = (size_t)64 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)total, avail);
    for (uint32_t i = 0; i < avail && emitted < MAX_OUT && pos < BUF_SZ - 256; i++) {
        uint32_t idx;
        if (newest_first) {
            uint32_t newest = (s_wtrace_all_head + WRITE_TRACE_ALL_CAP - 1u)
                              % WRITE_TRACE_ALL_CAP;
            idx = (newest + WRITE_TRACE_ALL_CAP - (i % WRITE_TRACE_ALL_CAP))
                  % WRITE_TRACE_ALL_CAP;
        } else {
            idx = (start + i) % WRITE_TRACE_ALL_CAP;
        }
        WriteTraceAllEntry *e = &s_wtrace_all[idx];
        if (e->addr < flo || e->addr >= fhi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\","
                        "\"new\":\"0x%08X\",\"pc\":\"0x%08X\","
                        "\"ra\":\"0x%08X\",\"frame\":%u,\"w\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->new_val, e->pc, e->ra,
                        e->frame, (unsigned)e->w);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%u}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_wtrace_trans_stats(int id, const char *json)
{
    (void)json;
    uint64_t total = s_wtrace_trans_seq;
    uint64_t oldest = (total <= WRITE_TRACE_TRANS_CAP) ? 0 : total - WRITE_TRACE_TRANS_CAP;
    uint64_t newest = (total > 0) ? total - 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%d,"
             "\"oldest_seq\":%llu,\"newest_seq\":%llu,\"ranges\":%d}",
             id, (unsigned long long)total, WRITE_TRACE_TRANS_CAP,
             (unsigned long long)oldest, (unsigned long long)newest,
             s_wtrace_trans_range_count);
}

static void handle_wtrace_trans_reset(int id, const char *json)
{
    (void)json;
    s_wtrace_trans_seq = 0;
    s_wtrace_trans_head = 0;
    if (s_wtrace_trans)
        memset(s_wtrace_trans, 0,
               (size_t)WRITE_TRACE_TRANS_CAP * sizeof(WriteTraceTransEntry));
    send_ok(id);
}

static void handle_wtrace_trans_dump(int id, const char *json)
{
    if (!s_wtrace_trans) { send_err(id, "wtrace_trans not initialized"); return; }

    char lo_str[32], hi_str[32], seq_str[32];
    uint32_t flo = 0, fhi = 0xFFFFFFFFu;
    if (json_get_str(json, "addr_lo", lo_str, sizeof(lo_str)))
        flo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", hi_str, sizeof(hi_str)))
        fhi = hex_to_u32(hi_str) & 0x1FFFFFFFu;

    uint64_t total = s_wtrace_trans_seq;
    uint64_t avail = (total < WRITE_TRACE_TRANS_CAP)
                     ? total : WRITE_TRACE_TRANS_CAP;
    uint64_t oldest = total - avail;

    int max_out = json_get_int(json, "count", 256);
    if (max_out < 1) max_out = 1;
    if (max_out > 2048) max_out = 2048;
    int newest_first = json_get_int(json, "newest", 0) != 0;

    uint64_t start = oldest;
    if (json_get_str(json, "seq_lo", seq_str, sizeof(seq_str))) {
        start = strtoull(seq_str, NULL, 0);
        if (start < oldest) start = oldest;
        if (start > total) start = total;
    }

    const size_t BUF_SZ = 256u + (size_t)max_out * 448u;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail);

    if (newest_first) {
        for (uint64_t s = total; s > oldest && emitted < max_out && pos < BUF_SZ - 448; ) {
            s--;
            const WriteTraceTransEntry *e = &s_wtrace_trans[s % WRITE_TRACE_TRANS_CAP];
            if (e->addr < flo || e->addr >= fhi) continue;
            pos += snprintf(buf + pos, BUF_SZ - pos,
                            "%s{\"seq\":%llu,\"addr\":\"0x%08X\","
                            "\"old\":\"0x%08X\",\"new\":\"0x%08X\","
                            "\"pc\":\"0x%08X\",\"func\":\"0x%08X\","
                            "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
                            "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\","
                            "\"s2\":\"0x%08X\",\"s3\":\"0x%08X\","
                            "\"stk20\":\"0x%08X\",\"stk40\":\"0x%08X\","
                            "\"frame\":%u,\"w\":%u}",
                            emitted == 0 ? "" : ",",
                            (unsigned long long)e->seq, e->addr,
                            e->old_val, e->new_val, e->pc, e->func_addr,
                            e->ra, e->sp, e->s0, e->s1, e->s2, e->s3,
                            e->stk20, e->stk40, e->frame, (unsigned)e->width);
            emitted++;
        }
    } else {
        for (uint64_t s = start; s < total && emitted < max_out && pos < BUF_SZ - 448; s++) {
            const WriteTraceTransEntry *e = &s_wtrace_trans[s % WRITE_TRACE_TRANS_CAP];
            if (e->addr < flo || e->addr >= fhi) continue;
            pos += snprintf(buf + pos, BUF_SZ - pos,
                            "%s{\"seq\":%llu,\"addr\":\"0x%08X\","
                            "\"old\":\"0x%08X\",\"new\":\"0x%08X\","
                            "\"pc\":\"0x%08X\",\"func\":\"0x%08X\","
                            "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
                            "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\","
                            "\"s2\":\"0x%08X\",\"s3\":\"0x%08X\","
                            "\"stk20\":\"0x%08X\",\"stk40\":\"0x%08X\","
                            "\"frame\":%u,\"w\":%u}",
                            emitted == 0 ? "" : ",",
                            (unsigned long long)e->seq, e->addr,
                            e->old_val, e->new_val, e->pc, e->func_addr,
                            e->ra, e->sp, e->s0, e->s1, e->s2, e->s3,
                            e->stk20, e->stk40, e->frame, (unsigned)e->width);
            emitted++;
        }
    }

    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_call_focus_stats(int id, const char *json)
{
    (void)json;
    uint64_t total = s_call_focus_seq;
    uint64_t oldest = (total <= CALL_FOCUS_CAP) ? 0 : total - CALL_FOCUS_CAP;
    uint64_t newest = (total > 0) ? total - 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%d,"
             "\"oldest_seq\":%llu,\"newest_seq\":%llu}",
             id, (unsigned long long)total, CALL_FOCUS_CAP,
             (unsigned long long)oldest, (unsigned long long)newest);
}

static void handle_call_focus_reset(int id, const char *json)
{
    (void)json;
    s_call_focus_seq = 0;
    if (s_call_focus)
        memset(s_call_focus, 0,
               (size_t)CALL_FOCUS_CAP * sizeof(CallFocusEntry));
    send_ok(id);
}

static void handle_call_focus_dump(int id, const char *json)
{
    if (!s_call_focus) { send_err(id, "call_focus not initialized"); return; }

    char val[32];
    uint64_t total = s_call_focus_seq;
    uint64_t avail = (total < CALL_FOCUS_CAP) ? total : CALL_FOCUS_CAP;
    uint64_t oldest = total - avail;
    uint64_t seq_lo = oldest;
    uint64_t seq_hi = total;
    uint32_t func_lo = 0;
    uint32_t func_hi = 0xFFFFFFFFu;
    uint32_t frame_lo = 0;
    uint32_t frame_hi = 0xFFFFFFFFu;

    if (json_get_str(json, "seq_lo", val, sizeof(val))) {
        seq_lo = strtoull(val, NULL, 0);
        if (seq_lo < oldest) seq_lo = oldest;
        if (seq_lo > total) seq_lo = total;
    }
    if (json_get_str(json, "seq_hi", val, sizeof(val))) {
        seq_hi = strtoull(val, NULL, 0);
        if (seq_hi < oldest) seq_hi = oldest;
        if (seq_hi > total) seq_hi = total;
    }
    if (json_get_str(json, "func", val, sizeof(val))) {
        func_lo = hex_to_u32(val);
        func_hi = func_lo + 1u;
    } else {
        if (json_get_str(json, "func_lo", val, sizeof(val)))
            func_lo = hex_to_u32(val);
        if (json_get_str(json, "func_hi", val, sizeof(val)))
            func_hi = hex_to_u32(val);
    }
    if (json_get_str(json, "frame_lo", val, sizeof(val)))
        frame_lo = (uint32_t)strtoul(val, NULL, 0);
    if (json_get_str(json, "frame_hi", val, sizeof(val)))
        frame_hi = (uint32_t)strtoul(val, NULL, 0);

    int max_out = json_get_int(json, "count", 128);
    if (max_out < 1) max_out = 1;
    if (max_out > 512) max_out = 512;
    int newest_first = json_get_int(json, "newest", 0) != 0;

    const size_t ENTRY_BUDGET = 1280u;
    const size_t BUF_SZ = 256u + (size_t)max_out * ENTRY_BUDGET;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%llu,"
                    "\"oldest\":%llu,\"seq_lo\":%llu,\"seq_hi\":%llu,"
                    "\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)avail,
                    (unsigned long long)oldest,
                    (unsigned long long)seq_lo, (unsigned long long)seq_hi);

#define CALL_FOCUS_EMIT_ENTRY(E) do { \
        const CallFocusEntry *ce__ = (E); \
        pos += snprintf(buf + pos, BUF_SZ - pos, \
            "%s{\"seq\":%llu,\"func\":\"0x%08X\",\"ra\":\"0x%08X\"," \
            "\"pc\":\"0x%08X\",\"frame\":%u,\"sp\":\"0x%08X\"," \
            "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\"," \
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\"," \
            "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\"," \
            "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\",\"s2\":\"0x%08X\",\"s3\":\"0x%08X\"," \
            "\"stk10\":\"0x%08X\",\"stk14\":\"0x%08X\",\"stk18\":\"0x%08X\"," \
            "\"stk20\":\"0x%08X\",\"stk40\":\"0x%08X\"," \
            "\"obj\":\"0x%08X\",\"obj10\":\"0x%08X\",\"obj14\":\"0x%08X\",\"obj18\":\"0x%08X\"," \
            "\"obj30\":\"0x%08X\",\"obj30_0\":%u,\"obj30_1\":%u," \
            "\"obj34\":%u,\"obj35\":%u,\"obj36\":%u,\"obj37\":%u," \
            "\"obj38\":%u,\"obj3c\":\"0x%08X\",\"obj40\":\"0x%08X\",\"obj44\":%u,\"obj45\":%u," \
            "\"obj46\":%u,\"obj49\":%u,\"obj4a\":%u,\"obj50\":%u," \
            "\"obje0\":%u,\"obje3\":%u,\"obje4\":%u,\"obje5\":%u," \
            "\"obje6\":%u,\"obje8\":%u,\"obje9\":%u,\"objea\":%u}", \
            emitted == 0 ? "" : ",", \
            (unsigned long long)ce__->seq, ce__->func_addr, ce__->ra, ce__->pc, \
            ce__->frame, ce__->sp, ce__->v0, ce__->v1, \
            ce__->a0, ce__->a1, ce__->a2, ce__->a3, ce__->t0, ce__->t1, \
            ce__->s0, ce__->s1, ce__->s2, ce__->s3, \
            ce__->stk10, ce__->stk14, ce__->stk18, ce__->stk20, ce__->stk40, \
            ce__->obj, ce__->obj_10, ce__->obj_14, ce__->obj_18, \
            ce__->obj_30, ce__->obj_30_0, ce__->obj_30_1, \
            ce__->obj_34, ce__->obj_35, ce__->obj_36, ce__->obj_37, \
            ce__->obj_38, ce__->obj_3c, ce__->obj_40, ce__->obj_44, ce__->obj_45, \
            ce__->obj_46, ce__->obj_49, ce__->obj_4a, ce__->obj_50, \
            ce__->obj_e0, ce__->obj_e3, ce__->obj_e4, ce__->obj_e5, \
            ce__->obj_e6, ce__->obj_e8, ce__->obj_e9, ce__->obj_ea); \
        emitted++; \
    } while (0)

    if (newest_first) {
        for (uint64_t s = seq_hi; s > seq_lo && emitted < max_out && pos < BUF_SZ - ENTRY_BUDGET; ) {
            s--;
            const CallFocusEntry *e = &s_call_focus[s % CALL_FOCUS_CAP];
            if (e->seq != s) continue;
            if (e->func_addr < func_lo || e->func_addr >= func_hi) continue;
            if (e->frame < frame_lo || e->frame >= frame_hi) continue;
            CALL_FOCUS_EMIT_ENTRY(e);
        }
    } else {
        for (uint64_t s = seq_lo; s < seq_hi && emitted < max_out && pos < BUF_SZ - ENTRY_BUDGET; s++) {
            const CallFocusEntry *e = &s_call_focus[s % CALL_FOCUS_CAP];
            if (e->seq != s) continue;
            if (e->func_addr < func_lo || e->func_addr >= func_hi) continue;
            if (e->frame < frame_lo || e->frame >= frame_hi) continue;
            CALL_FOCUS_EMIT_ENTRY(e);
        }
    }

#undef CALL_FOCUS_EMIT_ENTRY

    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
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

    int max_out = json_get_int(json, "count", 256);
    if (max_out < 1) max_out = 1;
    if (max_out > 2048) max_out = 2048;
    int newest_first = json_get_int(json, "newest", 0) != 0;

    const uint32_t MAX_OUT = (uint32_t)max_out;
    size_t BUF_SZ = 256u + (size_t)MAX_OUT * 512u;
    if (BUF_SZ > (size_t)128 * 1024 * 1024) BUF_SZ = (size_t)128 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)total, avail);
    for (uint32_t i = 0; i < avail && emitted < MAX_OUT && pos < BUF_SZ - 256; i++) {
        uint32_t idx;
        if (newest_first) {
            uint32_t newest = (s_wtrace_head + WRITE_TRACE_CAP - 1u) % WRITE_TRACE_CAP;
            idx = (newest + WRITE_TRACE_CAP - (i % WRITE_TRACE_CAP)) % WRITE_TRACE_CAP;
        } else {
            idx = (start + i) % WRITE_TRACE_CAP;
        }
        WriteTraceEntry *e = &s_wtrace[idx];
        if (e->addr < filter_lo || e->addr >= filter_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"old\":\"0x%08X\","
                        "\"new\":\"0x%08X\",\"ra\":\"0x%08X\",\"func\":\"0x%08X\","
                        "\"pc\":\"0x%08X\",\"cpu_pc\":\"0x%08X\",\"sp\":\"0x%08X\","
                        "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\","
                        "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                        "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\","
                        "\"frame\":%u,\"w\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->old_val, e->new_val, e->ra, e->func_addr,
                        e->pc, e->cpu_pc, e->sp,
                        e->v0, e->v1, e->a0, e->a1, e->a2, e->a3, e->t0, e->t1,
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

    int max_out = json_get_int(json, "count", 65536);
    if (max_out < 1) max_out = 1;
    if (max_out > (int)MMIO_TRACE_CAP) max_out = (int)MMIO_TRACE_CAP;
    int newest_first = json_get_int(json, "newest", 0) != 0;

    const uint32_t MAX_OUT = (uint32_t)max_out;
    size_t BUF_SZ = 256u + (size_t)MAX_OUT * 512u;
    if (BUF_SZ > (size_t)128 * 1024 * 1024) BUF_SZ = (size_t)128 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)total, avail);
    for (uint32_t i = 0; i < avail && emitted < MAX_OUT && pos < BUF_SZ - 256; i++) {
        uint32_t idx;
        if (newest_first) {
            uint32_t newest = (s_mmio_trace_head + MMIO_TRACE_CAP - 1u) % MMIO_TRACE_CAP;
            idx = (newest + MMIO_TRACE_CAP - (i % MMIO_TRACE_CAP)) % MMIO_TRACE_CAP;
        } else {
            idx = (start + i) % MMIO_TRACE_CAP;
        }
        MmioTraceEntry *e = &s_mmio_trace[idx];
        if (has_filter && e->addr != filter_addr) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"val\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"pc\":\"0x%08X\",\"cpu_pc\":\"0x%08X\","
                        "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
                        "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                        "\"sr\":\"0x%08X\",\"epc\":\"0x%08X\","
                        "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
                        "\"frame\":%u,\"w\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->val, e->func_addr, e->pc, e->cpu_pc,
                        e->ra, e->sp, e->a0, e->a1, e->a2, e->a3,
                        e->sr, e->epc, e->istat, e->imask,
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

/* gp1_dump — dump the dedicated GP1 display-control ring. Optional
 * `frame_lo`/`frame_hi` filter (applied server-side over the FULL ring,
 * like wtrace_dump's address filter), optional `count` (default 4096),
 * optional `newest` (1 = newest-first). */
static void handle_gp1_dump(int id, const char *json)
{
    if (!s_gp1_trace) { send_err(id, "gp1 trace not initialized"); return; }

    int frame_lo = json_get_int(json, "frame_lo", 0);
    int frame_hi = json_get_int(json, "frame_hi", 0x7FFFFFFF);

    uint64_t total = s_gp1_trace_seq;
    uint32_t avail = (total < GP1_TRACE_CAP) ? (uint32_t)total : GP1_TRACE_CAP;
    uint32_t start = (total < GP1_TRACE_CAP) ? 0 : s_gp1_trace_head;

    int max_out = json_get_int(json, "count", 4096);
    if (max_out < 1) max_out = 1;
    if (max_out > (int)GP1_TRACE_CAP) max_out = (int)GP1_TRACE_CAP;
    int newest_first = json_get_int(json, "newest", 0) != 0;

    const uint32_t MAX_OUT = (uint32_t)max_out;
    size_t BUF_SZ = 256u + (size_t)MAX_OUT * 512u;
    if (BUF_SZ > (size_t)128 * 1024 * 1024) BUF_SZ = (size_t)128 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)total, avail);
    for (uint32_t i = 0; i < avail && emitted < MAX_OUT && pos < BUF_SZ - 256; i++) {
        uint32_t idx;
        if (newest_first) {
            uint32_t newest = (s_gp1_trace_head + GP1_TRACE_CAP - 1u) % GP1_TRACE_CAP;
            idx = (newest + GP1_TRACE_CAP - (i % GP1_TRACE_CAP)) % GP1_TRACE_CAP;
        } else {
            idx = (start + i) % GP1_TRACE_CAP;
        }
        MmioTraceEntry *e = &s_gp1_trace[idx];
        if ((int)e->frame < frame_lo || (int)e->frame > frame_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"val\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"pc\":\"0x%08X\",\"cpu_pc\":\"0x%08X\","
                        "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
                        "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"sr\":\"0x%08X\",\"epc\":\"0x%08X\","
                        "\"frame\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->val, e->func_addr, e->pc, e->cpu_pc,
                        e->ra, e->sp, e->a0, e->a1,
                        e->sr, e->epc, e->frame);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%u}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static const char *mdec_event_kind_name(uint32_t kind)
{
    switch (kind) {
    case 1: return "reset";
    case 2: return "ctrl_write";
    case 3: return "cmd_begin";
    case 4: return "cmd_done";
    case 5: return "decode_done";
    case 6: return "dma_in_start";
    case 7: return "dma_in_end";
    case 8: return "dma_out_start";
    case 9: return "dma_out_end";
    case 10: return "output_drained";
    case 11: return "read_underflow";
    default: return "unknown";
    }
}

static void handle_mdec_state(int id, const char *json)
{
    (void)json;
    MDECDebugState s;
    mdec_debug_get_state(&s);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"command\":\"0x%08X\",\"expected_halfwords\":%u,"
             "\"input_count\":%u,\"output_size\":%u,\"output_pos\":%u,"
             "\"output_depth\":%u,\"output_signed\":%u,\"output_bit15\":%u,"
             "\"busy\":%u,\"input_full\":%u,\"enable_dma_in\":%u,\"enable_dma_out\":%u,"
             "\"last_status\":\"0x%08X\","
             "\"decode_macroblocks\":%u,\"decode_blocks\":%u,"
             "\"decode_stop_reason\":%u,\"decode_input_pos\":%u,\"decode_input_end\":%u,"
             "\"dma_in_words\":%u,\"dma_out_words\":%u,\"dma_read_underflows\":%u,"
             "\"trace_total\":%llu}",
             id, s.command, s.expected_halfwords, s.input_count,
             s.output_size, s.output_pos, s.output_depth, s.output_signed,
             s.output_bit15, s.busy, s.input_full, s.enable_dma_in,
             s.enable_dma_out, s.last_status, s.decode_macroblocks,
             s.decode_blocks, s.decode_stop_reason, s.decode_input_pos,
             s.decode_input_end, s.dma_in_words, s.dma_out_words,
             s.dma_read_underflows,
             (unsigned long long)mdec_debug_get_event_total());
}

static void handle_mdec_trace(int id, const char *json)
{
    uint64_t total = mdec_debug_get_event_total();
    uint64_t oldest = (total > 4096ull) ? total - 4096ull : 0;
    uint64_t seq_hi = total;
    uint64_t seq_lo = (total > 256ull) ? total - 256ull : oldest;
    char buf32[32];
    if (json_get_str(json, "seq_lo", buf32, sizeof(buf32))) seq_lo = strtoull(buf32, NULL, 0);
    if (json_get_str(json, "seq_hi", buf32, sizeof(buf32))) seq_hi = strtoull(buf32, NULL, 0);
    if (seq_lo < oldest) seq_lo = oldest;
    if (seq_hi > total) seq_hi = total;

    int max_out = json_get_int(json, "count", 256);
    if (max_out < 1) max_out = 1;
    if (max_out > 4096) max_out = 4096;

    MDECDebugEvent *events = (MDECDebugEvent *)malloc((size_t)max_out * sizeof(MDECDebugEvent));
    if (!events) { send_err(id, "oom"); return; }
    uint32_t n = mdec_debug_copy_events(seq_lo, seq_hi, events, (uint32_t)max_out);

    size_t buf_sz = 256u + (size_t)n * 384u;
    char *out = (char *)malloc(buf_sz);
    if (!out) { free(events); send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, buf_sz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,"
                    "\"seq_lo\":%llu,\"seq_hi\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest,
                    (unsigned long long)seq_lo, (unsigned long long)seq_hi);
    for (uint32_t i = 0; i < n && pos < buf_sz - 384u; i++) {
        const MDECDebugEvent *e = &events[i];
        pos += snprintf(out + pos, buf_sz - pos,
                        "%s{\"seq\":%llu,\"frame\":%u,\"kind\":\"%s\",\"value\":\"0x%08X\","
                        "\"command\":\"0x%08X\",\"input_count\":%u,\"expected_halfwords\":%u,"
                        "\"output_size\":%u,\"output_pos\":%u,\"macroblocks\":%u,"
                        "\"blocks\":%u,\"stop_reason\":%u,\"underruns\":%u}",
                        i ? "," : "",
                        (unsigned long long)e->seq, e->frame,
                        mdec_event_kind_name(e->kind), e->value, e->command,
                        e->input_count, e->expected_halfwords,
                        e->output_size, e->output_pos, e->macroblocks,
                        e->blocks, e->stop_reason, e->underruns);
    }
    pos += snprintf(out + pos, buf_sz - pos, "],\"emitted\":%u}", n);
    debug_server_send_line(out);
    free(out);
    free(events);
}

static void handle_mdec_trace_clear(int id, const char *json)
{
    (void)json;
    mdec_debug_clear();
    send_ok(id);
}

static void handle_quit(int id, const char *json)
{
    (void)json;
    send_ok(id);
    psx_crash_trace_set_exit_origin("tcp_quit");
    debug_server_shutdown();
    exit(0);
}

/* ---- Command dispatch table ---- */

/* dispatch_stats: static hit vs. miss coverage summary */
static void handle_dispatch_stats(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"static_hits\":%llu,"
             "\"miss_total\":%llu,"
             "\"miss_unique\":%d}",
             id,
             (unsigned long long)g_dispatch_static_hits,
             (unsigned long long)s_unknown_seq,
             s_unknown_unique_count);
}

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

static void handle_card_mgr_trace(int id, const char *json) {
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > CARD_MGR_TRACE_CAP) count = CARD_MGR_TRACE_CAP;
    int newest = json_get_int(json, "newest", 0) != 0;

    uint64_t total = s_card_mgr_trace_seq;
    uint64_t oldest = (total > CARD_MGR_TRACE_CAP) ? total - CARD_MGR_TRACE_CAP : 0;
    uint64_t available = total - oldest;
    if ((uint64_t)count > available) count = (int)available;
    uint64_t start = total - (uint64_t)count;
    if (start < oldest) start = oldest;

    size_t buf_sz = 256u + (size_t)count * 640u;
    char *buf = (char *)malloc(buf_sz);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, buf_sz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest);

    int first = 1;
    for (int i = 0; i < count && pos < buf_sz - 640u; i++) {
        uint64_t seq = newest ? (total - 1ull - (uint64_t)i) : (start + (uint64_t)i);
        if (seq < oldest || seq >= total) continue;
        const CardMgrTraceEntry *e = &s_card_mgr_trace[seq % CARD_MGR_TRACE_CAP];
        if (e->seq != seq) continue;
        pos += snprintf(buf + pos, buf_sz - pos,
            "%s{\"seq\":%llu,\"src\":%u,\"frame\":%u,\"func\":\"0x%08X\","
            "\"pc\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
            "\"v0\":\"0x%08X\",\"t0\":\"0x%08X\",\"t1\":\"0x%08X\","
            "\"s9f20\":\"0x%08X\",\"s9f24\":\"0x%08X\",\"s9f28\":\"0x%08X\","
            "\"s9f2c\":\"0x%08X\",\"s9f30\":\"0x%08X\",\"s9f34\":\"0x%08X\","
            "\"s7258\":\"0x%08X\",\"s725c\":\"0x%08X\",\"s7264\":\"0x%08X\","
            "\"s74bc\":\"0x%08X\",\"s7500\":\"0x%08X\",\"s7504\":\"0x%08X\","
            "\"s7508\":\"0x%08X\",\"s750c\":\"0x%08X\",\"s7510\":\"0x%08X\","
            "\"s7514\":\"0x%08X\",\"s7518\":\"0x%08X\",\"s751c\":\"0x%08X\","
            "\"s7520\":\"0x%08X\","
            "\"s7528\":\"0x%08X\",\"s752c\":\"0x%08X\",\"s7558\":\"0x%08X\","
            "\"s7568\":\"0x%08X\",\"s756c\":\"0x%08X\"}",
            first ? "" : ",",
            (unsigned long long)e->seq, e->source, e->frame, e->func_addr,
            e->pc, e->ra, e->a0, e->a1, e->a2, e->a3, e->v0, e->t0, e->t1,
            e->state_9f20, e->state_9f24, e->state_9f28,
            e->state_9f2c, e->state_9f30, e->state_9f34,
            e->state_7258, e->state_725c, e->state_7264,
            e->state_74bc, e->state_7500, e->state_7504,
            e->state_7508, e->state_750c, e->state_7510,
            e->state_7514, e->state_7518, e->state_751c,
            e->state_7520,
            e->state_7528, e->state_752c, e->state_7558,
            e->state_7568, e->state_756c);
        first = 0;
    }
    pos += snprintf(buf + pos, buf_sz - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

static void handle_card_mgr_clear(int id, const char *json) {
    (void)json;
    s_card_mgr_trace_seq = 0;
    memset(s_card_mgr_trace, 0, sizeof(s_card_mgr_trace));
    send_ok(id);
}

/* ---- Function entry/exit trace handlers ---- */

static void handle_fn_filter(int id, const char *json) {
    char buf[32];
    if (json_get_str(json, "lo", buf, sizeof(buf))) s_fn_trace_filter_lo = hex_to_u32(buf);
    if (json_get_str(json, "hi", buf, sizeof(buf))) s_fn_trace_filter_hi = hex_to_u32(buf);
    /* Enable per-dispatch shadow-stack tracking when a real range is set;
     * a "lo=0,hi=0xFFFFFFFF" call also flips it on (caller wants everything). */
    s_fn_trace_active = 1;
    send_fmt("{\"id\":%d,\"ok\":true,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\",\"active\":%d}\n",
             id, s_fn_trace_filter_lo, s_fn_trace_filter_hi, s_fn_trace_active);
}

static void handle_fn_disable(int id, const char *json) {
    (void)json;
    s_fn_trace_active = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"active\":0}\n", id);
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
    s_fn_direct_seen       = 0;
    s_fn_direct_no_cpu     = 0;
    s_fn_direct_filtered   = 0;
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
             "\"filter_lo\":\"0x%08X\",\"filter_hi\":\"0x%08X\","
             "\"active\":%d,\"direct_seen\":%llu,"
             "\"direct_no_cpu\":%llu,\"direct_filtered\":%llu}\n",
             id,
             (unsigned long long)s_fn_entry_seq,
             (unsigned long long)s_fn_exit_seq,
             s_fn_stack_top,
             (unsigned long long)s_fn_unmatched_returns,
             (unsigned long long)s_fn_stack_overflows,
             (unsigned long long)s_fn_tail_calls,
             (unsigned long long)FN_TRACE_CAP,
             (unsigned long long)FN_EXIT_TRACE_CAP,
             s_fn_trace_filter_lo, s_fn_trace_filter_hi,
             s_fn_trace_active,
             (unsigned long long)s_fn_direct_seen,
             (unsigned long long)s_fn_direct_no_cpu,
             (unsigned long long)s_fn_direct_filtered);
}

/* Helper: parse filter / range / count for fn_*_dump.
 *
 * Windowing defaults to the most recent 1M entries unless the caller
 * explicitly passes seq_lo. Keep emitted results bounded: the debug server
 * shares process health with the runtime, so giant JSON replies can look like
 * a game freeze. */
static void fn_dump_parse(const char *json, uint64_t total,
                         uint64_t *out_seq_lo, uint64_t *out_seq_hi,
                         uint32_t *out_addr_lo, uint32_t *out_addr_hi,
                         int *out_max) {
    char buf[32];
    static const uint64_t DEFAULT_WINDOW = 1ull << 20; /* 1M entries */
    static const int DEFAULT_COUNT = 256;
    static const int MAX_COUNT = 2048;
    *out_seq_hi  = total;
    *out_seq_lo  = (total > DEFAULT_WINDOW) ? total - DEFAULT_WINDOW : 0;
    *out_addr_lo = 0;
    *out_addr_hi = 0xFFFFFFFFu;
    *out_max     = DEFAULT_COUNT;
    if (json_get_str(json, "seq_lo", buf, sizeof(buf))) *out_seq_lo = strtoull(buf, NULL, 0);
    if (json_get_str(json, "seq_hi", buf, sizeof(buf))) *out_seq_hi = strtoull(buf, NULL, 0);
    if (json_get_str(json, "addr_lo", buf, sizeof(buf))) *out_addr_lo = hex_to_u32(buf);
    if (json_get_str(json, "addr_hi", buf, sizeof(buf))) *out_addr_hi = hex_to_u32(buf);
    *out_max = json_get_int(json, "count", *out_max);
    if (*out_max < 1) *out_max = 1;
    if (*out_max > MAX_COUNT) *out_max = MAX_COUNT;
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

    size_t BUF_SZ = 256u + (size_t)max_count * 640u;
    if (BUF_SZ > (size_t)128 * 1024 * 1024) BUF_SZ = (size_t)128 * 1024 * 1024;
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
    for (uint64_t s = seq_lo; s < seq_hi && emitted < max_count && pos < BUF_SZ - 640; s++) {
        FnEntryEntry *e = &s_fn_entry[s % FN_TRACE_CAP];
        if (e->func_addr < addr_lo || e->func_addr >= addr_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"func\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
            "\"t1\":\"0x%08X\",\"s0\":\"0x%08X\",\"s1\":\"0x%08X\","
            "\"s2\":\"0x%08X\",\"s3\":\"0x%08X\",\"depth\":%u,\"frame\":%u,\"exit_seq\":%llu}",
            first ? "" : ",",
            (unsigned long long)e->seq,
            e->func_addr, e->ra, e->a0, e->a1, e->a2, e->a3, e->t1,
            e->s0, e->s1, e->s2, e->s3, e->depth, e->frame,
            (unsigned long long)e->paired_exit_seq);
        first = 0;
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_fn_entry_tail(int id, const char *json) {
    if (!s_fn_entry) { send_err(id, "not initialized"); return; }
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > 256) count = 256;

    uint64_t total = s_fn_entry_seq;
    uint64_t oldest = (total > FN_TRACE_CAP) ? total - FN_TRACE_CAP : 0;
    uint64_t start = (total > (uint64_t)count) ? total - (uint64_t)count : 0;
    if (start < oldest) start = oldest;

    size_t BUF_SZ = 256u + (size_t)count * 512u;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"entries\":[",
                    id, (unsigned long long)total);
    int first = 1;
    int emitted = 0;
    for (uint64_t s = start; s < total && pos < BUF_SZ - 512; s++) {
        FnEntryEntry *e = &s_fn_entry[s % FN_TRACE_CAP];
        if (e->seq != s) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"func\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\","
            "\"a3\":\"0x%08X\",\"t1\":\"0x%08X\",\"s0\":\"0x%08X\","
            "\"s1\":\"0x%08X\",\"s2\":\"0x%08X\",\"s3\":\"0x%08X\",\"frame\":%u}",
            first ? "" : ",",
            (unsigned long long)e->seq, e->func_addr, e->ra,
            e->a0, e->a1, e->a2, e->a3, e->t1,
            e->s0, e->s1, e->s2, e->s3, e->frame);
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

    size_t BUF_SZ = 256u + (size_t)max_count * 512u;
    if (BUF_SZ > (size_t)128 * 1024 * 1024) BUF_SZ = (size_t)128 * 1024 * 1024;
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

/* cd_read_log: dump the CD DMA transfer ring.  Each entry records the
 * setloc LBA, destination RAM address, and byte count for one forward
 * channel-3 DMA transfer.  Use with overlay_dump to map overlay regions
 * back to their disc positions for the extract_overlays.py disc scanner.
 *
 * Parameters: "tail" (int, default 256) — how many recent entries to return.
 */
static void handle_cd_read_log(int id, const char *json)
{
    extern uint32_t cd_dma_log_get_total(void);
    extern void     cd_dma_log_get_entry(uint32_t idx, int *lba,
                                         uint32_t *dest, uint32_t *size);

    int tail = json_get_int(json, "tail", 256);
    uint32_t total = cd_dma_log_get_total();
    uint32_t cap   = 65536;
    uint32_t avail = total < cap ? total : cap;
    if ((uint32_t)tail > avail) tail = (int)avail;

    uint32_t start_idx = total > (uint32_t)tail ? total - (uint32_t)tail : 0;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"count\":%d,\"entries\":[",
             id, total, tail);
    int first = 1;
    for (uint32_t i = start_idx; i < total; i++) {
        int lba; uint32_t dest, size;
        cd_dma_log_get_entry(i, &lba, &dest, &size);
        if (lba < 0) continue;
        send_fmt("%s{\"lba\":%d,\"dest\":\"0x%08X\",\"size\":%u}",
                 first ? "" : ",", lba, dest, size);
        first = 0;
    }
    send_fmt("]}\n");
}

/* cdrom_instant_rate: get/set the 'instant' per-frame sector-IRQ budget
 * (step 3 tunable). Param "n" (optional int): new budget, clamped by
 * cdrom_set_instant_rate. Always returns the current value, so a no-arg
 * call is a pure query. */
static void handle_cdrom_instant_rate(int id, const char *json)
{
    int n = json_get_int(json, "n", 0);
    if (n > 0) cdrom_set_instant_rate(n);
    send_fmt("{\"id\":%d,\"ok\":true,\"instant_max_per_frame\":%d}\n",
             id, cdrom_get_instant_rate());
}

/* turbo_loads: get/set the turbo-through-loads enable (step 4). Param "n"
 * (optional: 0/1). Reports the enable, whether the load predicate holds RIGHT
 * NOW, and how many vblanks have run unpaced. */
static void handle_turbo_loads(int id, const char *json)
{
    extern int      g_turbo_loads_enabled;
    extern uint64_t g_turbo_loads_frames;
    extern int      fntrace_is_game_started(void);
    int n = json_get_int(json, "n", -1);
    if (n == 0 || n == 1) g_turbo_loads_enabled = n;
    send_fmt("{\"id\":%d,\"ok\":true,\"enabled\":%d,\"load_active\":%d,"
             "\"game_started\":%d,\"turbo_frames\":%llu}\n",
             id, g_turbo_loads_enabled, cdrom_load_in_progress(),
             fntrace_is_game_started(),
             (unsigned long long)g_turbo_loads_frames);
}

/* cdrom_bursts: dump the always-on CD load-burst ring, newest first. Each
 * record is one gap-separated run of delivered data sectors — i.e. one load.
 * Param "count" (optional, default 32, max 128). */
static void handle_cdrom_bursts(int id, const char *json)
{
    int count = json_get_int(json, "count", 32);
    if (count < 1)   count = 1;
    if (count > 128) count = 128;
    CdBurstRecord recs[128];
    int n = cdrom_get_bursts(recs, count);
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,"
             "\"instant_max_per_frame\":%d,\"bursts\":[",
             id, cdrom_get_burst_total(), cdrom_get_instant_rate());
    for (int i = 0; i < n; i++) {
        CdBurstRecord *b = &recs[i];
        send_fmt("%s{\"start_frame\":%u,\"end_frame\":%u,"
                 "\"frames\":%u,\"ms\":%llu,\"sectors\":%u,"
                 "\"rate\":%u,\"divisor\":%u}",
                 i ? "," : "", b->start_frame, b->end_frame,
                 b->end_frame - b->start_frame + 1u,
                 (unsigned long long)(b->end_ms - b->start_ms),
                 b->sectors, b->rate, b->divisor);
    }
    send_fmt("]}\n");
}

/* autocompile_status: variant-capture automation state — autocapture
 * enable/trigger counters + the background compile's state and output tail
 * (in-memory ring; no log files). */
static void handle_autocompile_status(int id, const char *json)
{
    extern int  autocompile_status_json(char *out, int cap);
    extern void overlay_autocapture_get_status(int *enabled,
                                               uint32_t *triggers,
                                               uint64_t *last_delta);
    (void)json;
    int      ac_en = 0;
    uint32_t trig = 0;
    uint64_t delta = 0;
    overlay_autocapture_get_status(&ac_en, &trig, &delta);
    char comp[4096];
    autocompile_status_json(comp, sizeof(comp));
    send_fmt("{\"id\":%d,\"ok\":true,\"autocapture_enabled\":%d,"
             "\"triggers\":%u,\"last_pressure\":%llu,\"compile\":%s}\n",
             id, ac_en, trig, (unsigned long long)delta, comp);
}

/* autocompile_run: manually kick the configured background compile. */
static void handle_autocompile_run(int id, const char *json)
{
    extern int autocompile_request(void);
    (void)json;
    int started = autocompile_request();
    send_fmt("{\"id\":%d,\"ok\":true,\"started\":%d}\n", id, started);
}

/* overlay_rescan: re-scan the DLL cache and clear the checked-regions memo
 * so newly compiled DLLs load on the next dispatch. */
static void handle_overlay_rescan(int id, const char *json)
{
    extern void overlay_loader_rescan(void);
    (void)json;
    overlay_loader_rescan();
    send_fmt("{\"id\":%d,\"ok\":true}\n", id);
}

/* overlay_capture_dump: flush overlay_captures.json on demand (does not require
 * a clean window-close). Use after roaming through areas so a freeze/kill can't
 * lose the captured overlays. Writes next to the runtime exe. */
static void handle_overlay_capture_dump(int id, const char *json)
{
    (void)json;
    overlay_capture_write_json();
    send_fmt("{\"id\":%d,\"ok\":true,\"capture_entries\":%d}\n",
             id, overlay_capture_count());
}

/* overlay_loader_status: report the dynamic overlay DLL loader state —
 * whether active, how many functions are registered, which regions have
 * been checked, and the most recent load event. Rule-3 inspection path for
 * the loader (it does no stderr logging). */
static void handle_overlay_loader_status(int id, const char *json)
{
    (void)json;
    int      active = 0, registered = 0, nchecked = 0, nwritten = 0;
    int      file_found = 0;
    uint32_t last_crc = 0;
    char     cache_dir[512] = {0}, game_id[64] = {0};
    uint32_t checked[8]     = {0};
    overlay_loader_get_status(&active, &registered, &nchecked,
                              cache_dir, sizeof(cache_dir),
                              game_id,   sizeof(game_id),
                              checked, 8, &nwritten,
                              &last_crc, &file_found);
    /* Escape backslashes in cache_dir for JSON */
    char esc_dir[768] = {0};
    for (int si = 0, di = 0; cache_dir[si] && di < (int)sizeof(esc_dir)-2; si++) {
        if (cache_dir[si] == '\\') esc_dir[di++] = '\\';
        esc_dir[di++] = cache_dir[si];
    }
    const char *msg = overlay_loader_last_msg();
    char esc_msg[512] = {0};
    for (int si = 0, di = 0; msg[si] && di < (int)sizeof(esc_msg)-2; si++) {
        if (msg[si] == '\\' || msg[si] == '"') esc_msg[di++] = '\\';
        esc_msg[di++] = msg[si];
    }
    char buf[1536];
    int n = snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"ok\":true,\"active\":%d,\"registered\":%d,"
        "\"regions_checked\":%d,\"last_crc\":\"0x%08X\",\"file_found\":%d,"
        "\"cache_dir\":\"%s\",\"game_id\":\"%s\",\"last_msg\":\"%s\"",
        id, active, registered, nchecked, last_crc, file_found,
        esc_dir, game_id, esc_msg);
    if (nwritten > 0) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"checked\":[");
        for (int i = 0; i < nwritten; i++)
            n += snprintf(buf + n, sizeof(buf) - n,
                          "%s\"0x%08X\"", i ? "," : "", checked[i]);
        n += snprintf(buf + n, sizeof(buf) - n, "]");
    }
    /* Inc1-D registration-lifetime counters. */
    {
        uint32_t loads=0, invs=0, unreg=0, lw_pc=0, lw_addr=0, lw_size=0, reval=0;
        uint64_t dnat=0, dint=0, stale=0; int regions=0;
        overlay_loader_get_counters(&loads, &invs, &unreg, &dnat, &dint,
                                    &stale, &lw_pc, &lw_addr, &lw_size, &regions,
                                    &reval);
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"regions\":%d,\"loads\":%u,\"invalidations\":%u,"
            "\"revalidations\":%u,\"unregistered_funcs\":%u,\"dispatch_native\":%llu,"
            "\"dispatch_interp_fallback\":%llu,\"stale_blocked\":%llu,"
            "\"last_write_pc\":\"0x%08X\",\"last_write_addr\":\"0x%08X\","
            "\"last_write_size\":%u",
            regions, loads, invs, reval, unreg,
            (unsigned long long)dnat, (unsigned long long)dint,
            (unsigned long long)stale, lw_pc, lw_addr, lw_size);
        int r0v=0; uint32_t r0w=0, r0lo=0, r0hi=0, r0crc=0, ratt=0, rmiss=0, rlast=0;
        overlay_loader_get_reload_debug(&r0v, &r0w, &r0lo, &r0hi, &r0crc,
                                        &ratt, &rmiss, &rlast);
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"r0_valid\":%d,\"r0_writes_since_invalid\":%u,"
            "\"r0_fn_lo\":\"0x%08X\",\"r0_fn_hi\":\"0x%08X\",\"r0_crc_live\":\"0x%08X\","
            "\"reval_attempts\":%u,\"reval_crc_miss\":%u,\"last_reval_crc\":\"0x%08X\"",
            r0v, r0w, r0lo, r0hi, r0crc, ratt, rmiss, rlast);
    }
    snprintf(buf + n, sizeof(buf) - n, "}\n");
    send_fmt("%s", buf);
}

/* overlay_candidates: dump the per-entry candidate table (addr, state, stored
 * vs live code hash, generation) so reload-on-return can be inspected directly. */
static void handle_overlay_candidates(int id, const char *json)
{
    (void)json;
    extern int overlay_loader_dump_candidates(char *out, int cap);
    static char cbuf[65536];
    int len = overlay_loader_dump_candidates(cbuf, (int)sizeof(cbuf));
    if (len < 0) len = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"candidates\":%s}\n", id, cbuf);
}

/* overlay_native_ring: dump the always-on ring of native overlay calls + the
 * in-progress entry (freeze-inside-native detector). Measurement surface for
 * the native↔interpreter parity investigation. */
static void handle_overlay_native_ring(int id, const char *json)
{
    (void)json;
    extern int overlay_loader_dump_native_ring(char *out, int cap);
    static char rbuf[16384];
    int len = overlay_loader_dump_native_ring(rbuf, (int)sizeof(rbuf));
    if (len < 0) len = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"ring\":%s}\n", id, rbuf);
}

/* overlay_diff_on/off: same-state native↔interp differential. With it on, each
 * matched overlay function runs both ways from identical CPU+RAM state; the
 * interp result is kept (game stays correct) and computation divergences are
 * logged. overlay_shadow_dump returns the divergence records. */
static void handle_overlay_diff_on(int id, const char *json)
{
    (void)json;
    extern void overlay_loader_set_diff_mode(int on);
    overlay_loader_set_diff_mode(1);
    send_fmt("{\"id\":%d,\"ok\":true,\"diff_mode\":1}\n", id);
}
static void handle_overlay_diff_off(int id, const char *json)
{
    (void)json;
    extern void overlay_loader_set_diff_mode(int on);
    overlay_loader_set_diff_mode(0);
    send_fmt("{\"id\":%d,\"ok\":true,\"diff_mode\":0}\n", id);
}
static void handle_overlay_shadow_dump(int id, const char *json)
{
    (void)json;
    extern int overlay_loader_dump_shadow(char *out, int cap);
    static char sbuf[1 << 18];
    int len = overlay_loader_dump_shadow(sbuf, (int)sizeof(sbuf));
    if (len < 0) len = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"shadow\":%s}\n", id, sbuf);
}
static void handle_overlay_shadow_detail(int id, const char *json)
{
    (void)json;
    extern int overlay_loader_dump_shadow_detail(char *out, int cap);
    static char dbuf[8192];
    int len = overlay_loader_dump_shadow_detail(dbuf, (int)sizeof(dbuf));
    if (len < 0) len = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"detail\":%s}\n", id, dbuf);
}

/* overlay_irq_suppress_on/off: drop native overlay code's per-block interrupt
 * checks (cadence ~ interpreter) to test whether the parity divergence is
 * interrupt-delivery timing rather than codegen. */
static void handle_overlay_irq_suppress_on(int id, const char *json)
{
    (void)json;
    extern void overlay_loader_set_irq_suppress(int mode, uint32_t ratelimit);
    overlay_loader_set_irq_suppress(1, 0);
    send_fmt("{\"id\":%d,\"ok\":true,\"irq_suppress\":1}\n", id);
}
static void handle_overlay_irq_suppress_off(int id, const char *json)
{
    (void)json;
    extern void overlay_loader_set_irq_suppress(int mode, uint32_t ratelimit);
    extern void overlay_loader_get_irq_suppress(int *mode, uint32_t *rl, uint64_t *supp);
    int m=0; uint32_t rl=0; uint64_t supp=0;
    overlay_loader_get_irq_suppress(&m,&rl,&supp);
    overlay_loader_set_irq_suppress(0, 0);
    send_fmt("{\"id\":%d,\"ok\":true,\"irq_suppress\":0,\"was_suppressed\":%llu}\n",
             id, (unsigned long long)supp);
}
/* overlay_irq_ratelimit <n>: arm native per-block interrupt checks at a
 * rate-limited cadence — the real check fires every Nth block, the rest are
 * dropped. N=1 == normal native cadence (every block); larger N approaches the
 * interpreter's coarse cadence. This is the decisive Priority-1 A/B knob; full
 * suppression (overlay_irq_suppress_on) is too blunt to be conclusive. */
static void handle_overlay_irq_ratelimit(int id, const char *json)
{
    extern void overlay_loader_set_irq_suppress(int mode, uint32_t ratelimit);
    int n = json_get_int(json, "n", 1);
    if (n < 1) n = 1;
    overlay_loader_set_irq_suppress(1, (uint32_t)n);
    send_fmt("{\"id\":%d,\"ok\":true,\"irq_suppress\":1,\"ratelimit\":%d}\n", id, n);
}
/* overlay_native_event_granularity <conservative|normal>: when conservative,
 * psx_advance_cycles splits a batched (N>1) cycle advance into N single-cycle
 * steps so device events fire at their true due-cycle in order — replicating
 * the interpreter's fine event timeline for native execution. Decisive test:
 * if the village->overworld blue screen clears with this on, the cause is
 * per-block event-ordering (root #2/#3), and the real fix is a due-cycle event
 * scheduler. */
static void handle_overlay_native_event_granularity(int id, const char *json)
{
    extern int g_event_step_conservative;
    char mode[32];
    if (!json_get_str(json, "mode", mode, sizeof(mode))) {
        send_err(id, "missing mode (conservative|normal)"); return;
    }
    int conservative = (strcmp(mode, "conservative") == 0);
    g_event_step_conservative = conservative;
    send_fmt("{\"id\":%d,\"ok\":true,\"event_granularity\":\"%s\"}\n",
             id, conservative ? "conservative" : "normal");
}

/* overlay_fp_dump: dump the native↔interp execution-fingerprint log (per
 * candidate function: in/out register CRC, native flag) for offline diffing. */
static void handle_overlay_fp_dump(int id, const char *json)
{
    extern int overlay_loader_write_fp_file(const char *path);
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        snprintf(path, sizeof(path), "overlay_fp.json");
    int count = overlay_loader_write_fp_file(path);
    send_fmt("{\"id\":%d,\"ok\":true,\"file\":\"%s\",\"entries\":%d}\n",
             id, path, count);
}

/* dirty_insn_gate <lo> <hi>: extra phys PC range the per-insn interp log
 * records (on top of the hardwired kernel ranges). hi=0 disables. */
static void handle_dirty_insn_gate(int id, const char *json)
{
    extern uint32_t g_insn_gate_lo, g_insn_gate_hi;
    char buf[32];
    if (json_get_str(json, "lo", buf, sizeof(buf))) g_insn_gate_lo = hex_to_u32(buf);
    if (json_get_str(json, "hi", buf, sizeof(buf))) g_insn_gate_hi = hex_to_u32(buf);
    send_fmt("{\"id\":%d,\"ok\":true,\"gate_lo\":\"0x%08X\",\"gate_hi\":\"0x%08X\"}\n",
             id, g_insn_gate_lo, g_insn_gate_hi);
}

/* insn_freeze <addr> <nth>: freeze the insn ring immediately BEFORE the Nth
 * candidate dispatch of phys <addr>, preserving the pre-divergence window.
 * addr=0 disarms and unfreezes. */
static void handle_insn_freeze(int id, const char *json)
{
    extern uint32_t g_insn_freeze_addr, g_insn_freeze_nth, g_insn_freeze_count;
    extern int g_insn_log_frozen;
    char buf[32];
    if (json_get_str(json, "addr", buf, sizeof(buf)))
        g_insn_freeze_addr = hex_to_u32(buf) & 0x1FFFFFFFu;
    g_insn_freeze_nth   = (uint32_t)json_get_int(json, "nth", 1);
    g_insn_freeze_count = 0;
    g_insn_log_frozen   = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"freeze_addr\":\"0x%08X\",\"nth\":%u}\n",
             id, g_insn_freeze_addr, g_insn_freeze_nth);
}

static void handle_insn_freeze_status(int id, const char *json)
{
    (void)json;
    extern uint32_t g_insn_freeze_addr, g_insn_freeze_nth, g_insn_freeze_count;
    extern int g_insn_log_frozen;
    extern uint64_t g_dirty_ram_insn_log_seq;
    send_fmt("{\"id\":%d,\"ok\":true,\"freeze_addr\":\"0x%08X\",\"nth\":%u,"
             "\"count\":%u,\"frozen\":%d,\"insn_log_seq\":%llu}\n",
             id, g_insn_freeze_addr, g_insn_freeze_nth, g_insn_freeze_count,
             g_insn_log_frozen, (unsigned long long)g_dirty_ram_insn_log_seq);
}

/* event_ring_dump: write the whole live event-timeline ring (IRQ deliver/gate,
 * I_STAT raises/changes, DMA kick/done, each tagged cycle/pc/func/mode/overlay)
 * to a JSON file for offline diffing of native-OFF vs native-ON runs. Optional
 * "path" param (default event_ring.json in CWD). */
static void handle_event_ring_dump(int id, const char *json)
{
    extern int event_ring_dump_file(const char *path);
    char path[256];
    if (!json_get_str(json, "path", path, sizeof(path)))
        snprintf(path, sizeof(path), "event_ring.json");
    int count = event_ring_dump_file(path);
    if (count < 0) { send_err(id, "event_ring file open failed"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"file\":\"%s\",\"entries\":%d}\n",
             id, path, count);
}
/* event_ring_tail: inline JSON tail of the most-recent N events for a quick
 * eyeball / first-output validation (default 64). */
static void handle_event_ring_tail(int id, const char *json)
{
    extern int event_ring_dump_json(char *out, int cap, int max_entries);
    int n = json_get_int(json, "n", 64);
    if (n < 1) n = 1;
    if (n > 4000) n = 4000;
    int cap = n * 256 + 512;
    char *out = (char *)malloc((size_t)cap);
    if (!out) { send_err(id, "alloc failed"); return; }
    int w = snprintf(out, cap, "{\"id\":%d,\"ok\":true,\"ring\":", id);
    w += event_ring_dump_json(out + w, cap - w, n);
    snprintf(out + w, cap - w, "}");
    debug_server_send_line(out);
    free(out);
}
/* event_ring_clear: reset the ring to isolate a fresh capture window. */
static void handle_event_ring_clear(int id, const char *json)
{
    (void)json;
    extern void event_ring_clear(void);
    event_ring_clear();
    send_fmt("{\"id\":%d,\"ok\":true,\"cleared\":true}\n", id);
}

/* overlay_native_on / overlay_native_off: toggle native overlay EXECUTION at
 * runtime (validity tracking still runs). A/B to prove whether native execution
 * is the cause without a rebuild. */
static void handle_overlay_native_on(int id, const char *json)
{
    (void)json;
    extern void overlay_loader_set_native_exec(int on);
    overlay_loader_set_native_exec(1);
    send_fmt("{\"id\":%d,\"ok\":true,\"native_exec\":1}\n", id);
}
static void handle_overlay_native_off(int id, const char *json)
{
    (void)json;
    extern void overlay_loader_set_native_exec(int on);
    overlay_loader_set_native_exec(0);
    send_fmt("{\"id\":%d,\"ok\":true,\"native_exec\":0}\n", id);
}

/* overlay_dump: extract RAM regions that dirty_ram has marked executable
 * above a threshold physical address. Writes <crc32>.bin files to a
 * caller-supplied directory and returns a JSON manifest.
 *
 * Parameters (JSON):
 *   "lo"  — low physical address threshold (default "0x98000")
 *   "dir" — output directory for .bin files (default "overlays")
 *
 * Returns:
 *   {"ok":true,"regions":[{"addr":"0x...","size":N,"crc32":"0x...","file":"<hex>.bin"},...]}
 */
static void handle_overlay_dump(int id, const char *json)
{
    extern uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);
    extern uint32_t dirty_ram_get_bitmap_word_count(void);
    extern uint8_t *memory_get_ram_ptr(void);
    extern uint32_t crc32_compute(const uint8_t *data, size_t len);

    char lo_buf[32]  = {0};
    char dir_buf[512] = {0};
    json_get_str(json, "lo",  lo_buf,  sizeof(lo_buf));
    json_get_str(json, "dir", dir_buf, sizeof(dir_buf));

    uint32_t lo_phys = lo_buf[0] ? (uint32_t)strtoul(lo_buf, NULL, 0) : 0x00098000u;
    if (!dir_buf[0]) strncpy(dir_buf, "overlays", sizeof(dir_buf) - 1);

    /* Create output directory (best-effort). */
#ifdef _WIN32
    { char cmd[600]; snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", dir_buf); system(cmd); }
#else
    { char cmd[600]; snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dir_buf); system(cmd); }
#endif

    uint8_t *ram      = memory_get_ram_ptr();
    uint32_t bw       = dirty_ram_get_bitmap_word_count();
    uint32_t page_sz  = 4096u;
    uint32_t lo_page  = lo_phys / page_sz;

    /* Scan bitmap for contiguous dirty-page runs above lo_phys. */
    char    out[65536];
    int     pos      = 0;
    int     first    = 1;
    int     in_run   = 0;
    uint32_t run_start = 0;

    pos += snprintf(out + pos, sizeof(out) - pos,
                    "{\"id\":%d,\"ok\":true,\"regions\":[", id);

    for (uint32_t page = lo_page; page < bw * 32u; page++) {
        uint32_t word = dirty_ram_get_bitmap_word(page >> 5);
        int dirty = (word >> (page & 31u)) & 1u;

        if (dirty && !in_run) {
            in_run = 1;
            run_start = page;
        } else if (!dirty && in_run) {
            in_run = 0;
            /* Emit region [run_start, page) */
            uint32_t phys = run_start * page_sz;
            uint32_t size = (page - run_start) * page_sz;
            uint32_t crc  = crc32_compute(ram + phys, size);

            char fname[32];
            snprintf(fname, sizeof(fname), "%08X.bin", crc);
            char fpath[600];
            snprintf(fpath, sizeof(fpath), "%s/%s", dir_buf, fname);
            FILE *bf = fopen(fpath, "wb");
            if (bf) { fwrite(ram + phys, 1, size, bf); fclose(bf); }

            pos += snprintf(out + pos, sizeof(out) - pos,
                            "%s{\"addr\":\"0x%08X\",\"size\":%u,"
                            "\"crc32\":\"0x%08X\",\"file\":\"%s\"}",
                            first ? "" : ",", phys, size, crc, fname);
            first = 0;
        }
    }
    /* Close any open run at end of RAM. */
    if (in_run) {
        uint32_t page = bw * 32u;
        uint32_t phys = run_start * page_sz;
        uint32_t size = (page - run_start) * page_sz;
        uint32_t crc  = crc32_compute(ram + phys, size);
        char fname[32];
        snprintf(fname, sizeof(fname), "%08X.bin", crc);
        char fpath[600];
        snprintf(fpath, sizeof(fpath), "%s/%s", dir_buf, fname);
        FILE *bf = fopen(fpath, "wb");
        if (bf) { fwrite(ram + phys, 1, size, bf); fclose(bf); }
        pos += snprintf(out + pos, sizeof(out) - pos,
                        "%s{\"addr\":\"0x%08X\",\"size\":%u,"
                        "\"crc32\":\"0x%08X\",\"file\":\"%s\"}",
                        first ? "" : ",", phys, size, crc, fname);
    }

    snprintf(out + pos, sizeof(out) - pos, "]}\n");
    send_fmt("%s", out);
}

/* ====================================================================
 * Freeze auto-dump accessors. Called from the freeze_heartbeat thread
 * after it detects a main-thread stall. Each function writes a JSON
 * array `[entry1,entry2,...]` of the newest entries directly to FILE*.
 *
 * Snapshot the ring's seq/head ONCE on entry so a concurrent writer
 * (very rare at dump time) cannot make us walk off the end.
 * ==================================================================== */

void debug_server_freeze_dump_wtrace_all_json(FILE *f, uint32_t max_count)
{
    if (!f) return;
    if (!s_wtrace_all) { fputs("[]", f); return; }

    uint64_t total = s_wtrace_all_seq;
    uint32_t head  = s_wtrace_all_head;
    uint32_t avail = (total < WRITE_TRACE_ALL_CAP)
                     ? (uint32_t)total : WRITE_TRACE_ALL_CAP;
    if (max_count > avail) max_count = avail;
    if (max_count == 0) { fputs("[]", f); return; }

    /* Walk oldest-first within the newest `max_count` window. */
    uint32_t start = (total < WRITE_TRACE_ALL_CAP)
                     ? (avail - max_count)
                     : (head + (WRITE_TRACE_ALL_CAP - max_count)) % WRITE_TRACE_ALL_CAP;

    fputc('[', f);
    for (uint32_t i = 0; i < max_count; i++) {
        uint32_t idx = (start + i) % WRITE_TRACE_ALL_CAP;
        const WriteTraceAllEntry *e = &s_wtrace_all[idx];
        fprintf(f,
            "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"new\":\"0x%08X\","
            "\"pc\":\"0x%08X\",\"ra\":\"0x%08X\",\"frame\":%u,\"w\":%u}",
            i ? "," : "",
            (unsigned long long)e->seq, e->addr, e->new_val,
            e->pc, e->ra, e->frame, (unsigned)e->w);
    }
    fputc(']', f);
}

void debug_server_freeze_dump_wtrace_json(FILE *f, uint32_t max_count)
{
    if (!f) return;
    if (!s_wtrace) { fputs("[]", f); return; }

    uint64_t total = s_wtrace_seq;
    uint32_t head  = s_wtrace_head;
    uint32_t avail = (total < WRITE_TRACE_CAP) ? (uint32_t)total : WRITE_TRACE_CAP;
    if (max_count > avail) max_count = avail;
    if (max_count == 0) { fputs("[]", f); return; }

    uint32_t start = (total < WRITE_TRACE_CAP)
                     ? (avail - max_count)
                     : (head + (WRITE_TRACE_CAP - max_count)) % WRITE_TRACE_CAP;

    fputc('[', f);
    for (uint32_t i = 0; i < max_count; i++) {
        uint32_t idx = (start + i) % WRITE_TRACE_CAP;
        const WriteTraceEntry *e = &s_wtrace[idx];
        fprintf(f,
            "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"old\":\"0x%08X\","
            "\"new\":\"0x%08X\",\"ra\":\"0x%08X\",\"func\":\"0x%08X\","
            "\"pc\":\"0x%08X\",\"cpu_pc\":\"0x%08X\",\"sp\":\"0x%08X\","
            "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
            "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
            "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\","
            "\"frame\":%u,\"w\":%u}",
            i ? "," : "",
            (unsigned long long)e->seq, e->addr, e->old_val, e->new_val,
            e->ra, e->func_addr, e->pc, e->cpu_pc, e->sp,
            e->v0, e->v1, e->a0, e->a1, e->a2, e->a3, e->t0, e->t1,
            e->frame, (unsigned)e->width);
    }
    fputc(']', f);
}

void debug_server_freeze_dump_frame_history_json(FILE *f, uint32_t max_count)
{
    if (!f) return;
    if (!s_frame_history) { fputs("[]", f); return; }

    uint64_t total = s_history_count;
    if (total == 0 || max_count == 0) { fputs("[]", f); return; }
    uint64_t oldest = (total > FRAME_HISTORY_CAP) ? total - FRAME_HISTORY_CAP : 0;
    uint64_t start  = (total > (uint64_t)max_count)
                      ? total - (uint64_t)max_count : 0;
    if (start < oldest) start = oldest;

    fputc('[', f);
    int first = 1;
    for (uint64_t fr = start; fr < total; fr++) {
        uint32_t idx = (uint32_t)(fr % FRAME_HISTORY_CAP);
        const PSXFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)fr) continue;
        fprintf(f,
            "%s{\"frame\":%u,\"verify\":%d,"
            "\"sr\":\"0x%08X\",\"cause\":\"0x%08X\",\"epc\":\"0x%08X\","
            "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
            "\"pad\":\"0x%04X\",\"sio_stat\":\"0x%04X\",\"sio_ctrl\":\"0x%04X\","
            "\"dispatch_count\":%u,\"total_dispatches\":%llu,"
            "\"disp\":{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"off\":%u},"
            "\"last_func\":\"%s\"}",
            first ? "" : ",",
            r->frame_number, r->verify_pass,
            r->cop0_sr, r->cop0_cause, r->cop0_epc,
            r->i_stat, r->i_mask,
            (unsigned)r->pad_buttons,
            (unsigned)r->sio_stat, (unsigned)r->sio_ctrl,
            r->dispatch_count,
            (unsigned long long)r->total_dispatches,
            (unsigned)r->display_area_x, (unsigned)r->display_area_y,
            (unsigned)r->display_w, (unsigned)r->display_h,
            (unsigned)r->display_disabled,
            r->last_func);
        first = 0;
    }
    fputc(']', f);
}

void debug_server_freeze_dump_sio_pc_json(FILE *f, uint32_t max_count)
{
    if (!f) return;
    uint64_t total = s_sio_pc_trace_seq;
    uint32_t avail = (total < SIO_PC_TRACE_CAP) ? (uint32_t)total : SIO_PC_TRACE_CAP;
    if (max_count > avail) max_count = avail;
    if (max_count == 0) { fputs("[]", f); return; }

    uint64_t start = total - (uint64_t)max_count;

    fputc('[', f);
    for (uint32_t i = 0; i < max_count; i++) {
        uint64_t s = start + i;
        const SioPcTraceEntry *e = &s_sio_pc_trace[s % SIO_PC_TRACE_CAP];
        fprintf(f,
            "%s{\"seq\":%llu,\"pc\":\"0x%08X\",\"func\":\"0x%08X\","
            "\"addr\":\"0x%08X\",\"value\":\"0x%08X\","
            "\"byte_seq\":%u,\"width\":%u}",
            i ? "," : "",
            (unsigned long long)e->seq, e->pc, e->func,
            e->addr, e->value, e->byte_seq, (unsigned)e->width);
    }
    fputc(']', f);
}

void debug_server_freeze_dump_thread_trace_json(FILE *f, uint32_t max_count)
{
    if (!f) return;
    uint64_t total = s_thread_trace_seq;
    uint64_t avail = (total < THREAD_TRACE_CAP) ? total : THREAD_TRACE_CAP;
    if ((uint64_t)max_count > avail) max_count = (uint32_t)avail;
    if (max_count == 0) { fputs("[]", f); return; }

    uint64_t start = total - (uint64_t)max_count;

    fputc('[', f);
    for (uint32_t i = 0; i < max_count; i++) {
        uint64_t s = start + i;
        const ThreadTraceEntry *e = &s_thread_trace[s % THREAD_TRACE_CAP];
        fprintf(f,
            "%s{\"seq\":%llu,\"kind\":%u,\"name\":\"%s\","
            "\"current_tcb\":\"0x%08X\",\"target_tcb\":\"0x%08X\","
            "\"current_state\":\"0x%08X\",\"target_state\":\"0x%08X\","
            "\"target_pc\":\"0x%08X\",\"func\":\"0x%08X\","
            "\"store_pc\":\"0x%08X\",\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
            "\"sr\":\"0x%08X\",\"epc\":\"0x%08X\","
            "\"istat\":\"0x%08X\",\"imask\":\"0x%08X\","
            "\"frame\":%u,\"in_exc\":%u}",
            i ? "," : "",
            (unsigned long long)e->seq, e->kind, thread_kind_name(e->kind),
            e->current_tcb, e->target_tcb,
            e->current_state, e->target_state,
            e->target_pc, e->func,
            e->last_store_pc, e->ra, e->sp,
            e->a0, e->a1,
            e->sr, e->epc, e->istat, e->imask,
            e->frame, (unsigned)e->in_exception);
    }
    fputc(']', f);
}

void debug_server_freeze_dump_restore_trace_json(FILE *f, uint32_t max_count)
{
    if (!f) return;
    uint64_t total = s_restore_trace_seq;
    uint64_t avail = (total < RESTORE_TRACE_CAP) ? total : RESTORE_TRACE_CAP;
    if ((uint64_t)max_count > avail) max_count = (uint32_t)avail;
    if (max_count == 0) { fputs("[]", f); return; }

    uint64_t start = total - (uint64_t)max_count;

    fputc('[', f);
    for (uint32_t i = 0; i < max_count; i++) {
        uint64_t s = start + i;
        const RestoreTraceEntry *e = &s_restore_trace[s % RESTORE_TRACE_CAP];
        fprintf(f,
            "%s{\"seq\":%llu,\"kind\":%u,\"name\":\"%s\",\"jmp\":%u,"
            "\"target\":\"0x%08X\",\"cpu_pc\":\"0x%08X\","
            "\"func\":\"0x%08X\",\"store_pc\":\"0x%08X\","
            "\"byte_seq\":%u,\"ra\":\"0x%08X\",\"sp\":\"0x%08X\","
            "\"sr\":\"0x%08X\",\"epc\":\"0x%08X\","
            "\"istat\":\"0x%08X\",\"imask\":\"0x%08X\","
            "\"frame\":%u,\"in_exc\":%u}",
            i ? "," : "",
            (unsigned long long)e->seq, e->kind, restore_kind_name(e->kind),
            e->jmp_val, e->target_pc, e->cpu_pc,
            e->func, e->last_store_pc, e->byte_seq, e->ra, e->sp,
            e->sr, e->epc, e->istat, e->imask,
            e->frame, (unsigned)e->in_exception);
    }
    fputc(']', f);
}

void debug_server_freeze_dump_fn_entry_json(FILE *f, uint32_t max_count)
{
    if (!f) return;
    if (!s_fn_entry) { fputs("[]", f); return; }

    uint64_t total  = s_fn_entry_seq;
    uint64_t oldest = (total > FN_TRACE_CAP) ? total - FN_TRACE_CAP : 0;
    uint64_t start  = (total > (uint64_t)max_count)
                      ? total - (uint64_t)max_count : 0;
    if (start < oldest) start = oldest;
    if (start >= total) { fputs("[]", f); return; }

    fputc('[', f);
    int first = 1;
    for (uint64_t s = start; s < total; s++) {
        const FnEntryEntry *e = &s_fn_entry[s % FN_TRACE_CAP];
        if (e->seq != s) continue;
        fprintf(f,
            "%s{\"seq\":%llu,\"func\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\","
            "\"a3\":\"0x%08X\",\"t1\":\"0x%08X\",\"frame\":%u}",
            first ? "" : ",",
            (unsigned long long)e->seq, e->func_addr, e->ra,
            e->a0, e->a1, e->a2, e->a3, e->t1, e->frame);
        first = 0;
    }
    fputc(']', f);
}

void debug_server_freeze_dump_dirty_block_json(FILE *f, uint32_t max_count)
{
    if (!f) { return; }

    uint64_t total = g_dirty_ram_block_log_seq;
    uint64_t avail = (total < (uint64_t)DIRTY_RAM_BLOCK_LOG_CAP)
                     ? total : (uint64_t)DIRTY_RAM_BLOCK_LOG_CAP;
    uint64_t want  = ((uint64_t)max_count < avail) ? (uint64_t)max_count : avail;
    uint64_t start = total - want;

    fputc('[', f);
    int first = 1;
    for (uint64_t s = start; s < total; s++) {
        const DirtyRamBlockLogEntry *e =
            &g_dirty_ram_block_log[s & (DIRTY_RAM_BLOCK_LOG_CAP - 1u)];
        if (e->seq != s) { continue; }
        fprintf(f,
            "%s{\"seq\":%llu,\"target\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\","
            "\"a3\":\"0x%08X\",\"t0\":\"0x%08X\",\"t1\":\"0x%08X\","
            "\"sp\":\"0x%08X\",\"frame\":%u}",
            first ? "" : ",",
            (unsigned long long)e->seq, e->target, e->ra,
            e->a0, e->a1, e->a2, e->a3, e->t0, e->t1, e->sp, e->frame);
        first = 0;
    }
    fputc(']', f);
}

typedef void (*CmdHandler)(int id, const char *json);
typedef struct { const char *name; CmdHandler handler; } CmdEntry;

static const CmdEntry s_commands[] = {
    { "ping",              handle_ping },
    { "frame",             handle_frame },
    { "get_registers",     handle_get_registers },
    { "read_ram",          handle_read_ram },
    { "dump_ram",          handle_read_ram },   /* alias: one request, one response */
    { "write_ram",         handle_write_ram },
    { "gpu_state",         handle_gpu_state },
    { "mem_words",         handle_mem_words },
    { "vram_peek",         handle_vram_peek },
    { "gl_coh_ring",       handle_gl_coh_ring },
    { "gl_fbo_peek",       handle_gl_fbo_peek },
    { "gl_vram_diff",      handle_gl_vram_diff },
    { "irq_state",         handle_irq_state },
    { "timers_state",      handle_timers_state },
    { "cdrom_state",       handle_cdrom_state },
    { "cdrom_sector_dump", handle_cdrom_sector_dump },
    { "cdrom_sector_history", handle_cdrom_sector_history },
    { "cdrom_sector_history_clear", handle_cdrom_sector_history_clear },
    { "cdrom_trace_dump",  handle_cdrom_trace_dump },
    { "cdrom_trace_clear", handle_cdrom_trace_clear },
    { "dma_state",         handle_dma_state },
    { "dma_trace_dump",    handle_dma_trace_dump },
    { "dma_trace_clear",   handle_dma_trace_clear },
    { "sio_state",         handle_sio_state },
    { "mc_status",         handle_mc_status },
    { "spu_status",        handle_spu_status },
    { "spu_voices",        handle_spu_voices },
    { "spu_events",        handle_spu_events },
    { "spu_events_reset",  handle_spu_events_reset },
    { "card_buffer_dump",  handle_card_buffer_dump },
    { "sio_arm_audit",     handle_sio_arm_audit },
    { "sio_burst_stats",   handle_sio_burst_stats },
    { "pace_state",        handle_pace_state },
    { "chain_trace",       handle_chain_trace },
    { "sio_trace",         handle_sio_trace },
    { "sio_trace_window",  handle_sio_trace_window },
    { "sio_pc_trace",      handle_sio_pc_trace },
    { "sio_pc_window",     handle_sio_pc_window },
    { "sio_ctrl_reg_trace", handle_sio_ctrl_reg_trace },
    { "sio_ctrl_reg_window", handle_sio_ctrl_reg_window },
    { "sio_ctrl_reg_clear", handle_sio_ctrl_reg_clear },
    { "restore_trace",     handle_restore_trace },
    { "restore_trace_window", handle_restore_trace_window },
    { "restore_trace_clear", handle_restore_trace_clear },
    { "thread_trace",      handle_thread_trace },
    { "thread_trace_clear", handle_thread_trace_clear },
    { "thread_ctx_ring",   handle_thread_ctx_ring },
    { "sreg_trace_dump",   handle_sreg_trace_dump },
    { "sreg_trace_find",   handle_sreg_trace_find },
    { "sreg_trace_stats",  handle_sreg_trace_stats },
    { "sreg_trace_clear",  handle_sreg_trace_clear },
    { "probe_trace",       handle_probe_trace },
    { "probe_clear",       handle_probe_clear },
    { "dirty_ram_stats",   handle_dirty_ram_stats },
    { "dirty_ram_unsupported", handle_dirty_ram_unsupported },
    { "dirty_block_log",   handle_dirty_block_log },
    { "dirty_flow_log",    handle_dirty_flow_log },
    { "dirty_insn_log",    handle_dirty_insn_log },
    { "dirty_insn_dump_file", handle_dirty_insn_dump_file },
    { "dirty_block_dump_file", handle_dirty_block_dump_file },
    { "fntrace_arm",       handle_fntrace_arm },
    { "fntrace_arm_clear", handle_fntrace_arm_clear },
    { "fntrace_armed",     handle_fntrace_armed },
    { "fntrace_clear",     handle_fntrace_clear },
    { "fntrace_dump",      handle_fntrace_dump },
    { "unknown_dispatch_log", handle_unknown_dispatch_log },
    { "bioscall_dump",     handle_bioscall_dump },
    { "card_trace_dump",   handle_card_trace_dump },
    { "card_txn_dump",     handle_card_txn_dump },
    { "card_read_summary", handle_card_read_summary },
    { "card_read_summary_reset", handle_card_read_summary_reset },
    { "card_data_writes",  handle_card_data_writes },
    { "card_data_writes_reset", handle_card_data_writes_reset },
    { "sio_irq_dump",      handle_sio_irq_dump },
    { "sio_irq_window",    handle_sio_irq_window },
    { "evcb_snapshot",     handle_evcb_snapshot },
    { "evcb_walk_dump",    handle_evcb_walk_dump },
    { "evcb_walk_stats",   handle_evcb_walk_stats },
    { "imask_trace",       handle_imask_trace },
    { "watch",             handle_watch },
    { "unwatch",           handle_unwatch },
    /* wtrace — normalized verb set (parity contract with psx-beetle).
     * One slot per "lo,hi" pair. arm/disarm/disarm_all/reset/ranges/
     * dump/stats. Both backends accept identical JSON shape. */
    { "wtrace_arm",          handle_wtrace_add },
    { "wtrace_disarm",       handle_wtrace_del },
    { "wtrace_disarm_all",   handle_wtrace_disarm_all },
    { "wtrace_reset",        handle_wtrace_clear },
    { "wtrace_ranges",       handle_wtrace_ranges },
    { "wtrace_dump",         handle_wtrace_dump },
    { "wtrace_stats",        handle_wtrace_stats },
    { "wtrace_boot_dump",    handle_wtrace_boot_dump },
    { "wtrace_boot_summary", handle_wtrace_boot_summary },
    { "wtrace_boot_stats",   handle_wtrace_boot_stats },
    { "wtrace_boot_reset",   handle_wtrace_boot_clear },
    /* Always-on catch-all wtrace ring (parity with psx-beetle). */
    { "wtrace_all_dump",     handle_wtrace_all_dump },
    { "wtrace_all_stats",    handle_wtrace_all_stats },
    { "wtrace_all_reset",    handle_wtrace_all_reset },
    { "wtrace_trans_dump",   handle_wtrace_trans_dump },
    { "wtrace_trans_stats",  handle_wtrace_trans_stats },
    { "wtrace_trans_reset",  handle_wtrace_trans_reset },
    { "call_focus_dump",     handle_call_focus_dump },
    { "call_focus_stats",    handle_call_focus_stats },
    { "call_focus_reset",    handle_call_focus_reset },
    /* Legacy verbs retained for existing tools/ scripts that haven't
     * been updated; they dispatch to the same handlers as the
     * normalized verbs.  Will retire once consumers are migrated. */
    { "wtrace_range",        handle_wtrace_range },
    { "wtrace_add",          handle_wtrace_add },
    { "wtrace_del",          handle_wtrace_del },
    { "wtrace_clear",        handle_wtrace_clear },
    { "freeze_check",      handle_freeze_check },
    { "mmio_dump",         handle_mmio_dump },
    { "mmio_clear",        handle_mmio_clear },
    { "gp1_dump",          handle_gp1_dump },
    { "mdec_state",        handle_mdec_state },
    { "mdec_trace",        handle_mdec_trace },
    { "mdec_trace_clear",  handle_mdec_trace_clear },
    { "set_input",         handle_set_input },
    { "press",             handle_press },
    { "pad_status",        handle_pad_status },
    { "clear_input",       handle_clear_input },
    { "turbo",             handle_turbo },
    { "turbo_state",       handle_turbo_state },
    { "pause",             handle_pause },
    { "continue",          handle_continue },
    { "step",              handle_step },
    { "run_to_frame",      handle_run_to_frame },
    { "dirty_break_range", handle_dirty_break_range },
    { "dirty_break_clear", handle_dirty_break_clear },
    { "dirty_break_state", handle_dirty_break_state },
    { "history",           handle_history },
    { "get_frame",         handle_get_frame },
    { "frame_range",       handle_frame_range },
    { "frame_timeseries",  handle_frame_timeseries },
    { "first_failure",     handle_first_failure },
    { "read_frame_ram",    handle_read_frame_ram },
    { "set_snapshot",      handle_set_snapshot },
    { "get_snapshots",     handle_get_snapshots },
    { "screenshot",        handle_screenshot_file },
    { "screenshot_file",   handle_screenshot_file },   /* alias */
    { "gpu_opcodes",       handle_gpu_opcodes },
    { "gpu_ring_stats",    handle_gpu_ring_stats },
    { "gpu_frame_dump",    handle_gpu_frame_dump },
    { "a0_history",        handle_a0_history },
    { "c0_history",        handle_c0_history },
    { "capture_quads",     handle_capture_quads },
    { "get_quads",         handle_get_quads },
    { "gte_state",         handle_gte_state },
    { "quit",              handle_quit },
    { "dispatch_stats",    handle_dispatch_stats },
    { "dispatch_check",    handle_dispatch_check },
    { "dispatch_tail",     handle_dispatch_tail },
    { "card_mgr_trace",    handle_card_mgr_trace },
    { "card_mgr_clear",    handle_card_mgr_clear },
    { "fn_filter",         handle_fn_filter },
    { "fn_disable",        handle_fn_disable },
    { "fn_clear",          handle_fn_clear },
    { "fn_stats",          handle_fn_stats },
    { "fn_entry_dump",     handle_fn_entry_dump },
    { "fn_entry_tail",     handle_fn_entry_tail },
    { "fn_exit_dump",      handle_fn_exit_dump },
    { "overlay_dump",      handle_overlay_dump },
    { "cd_read_log",       handle_cd_read_log },
    { "overlay_loader_status", handle_overlay_loader_status },
    { "overlay_candidates",   handle_overlay_candidates },
    { "overlay_native_ring",  handle_overlay_native_ring },
    { "overlay_irq_suppress_on",  handle_overlay_irq_suppress_on },
    { "overlay_irq_suppress_off", handle_overlay_irq_suppress_off },
    { "overlay_irq_ratelimit",    handle_overlay_irq_ratelimit },
    { "overlay_native_event_granularity", handle_overlay_native_event_granularity },
    { "event_ring_dump",          handle_event_ring_dump },
    { "event_ring_tail",          handle_event_ring_tail },
    { "event_ring_clear",         handle_event_ring_clear },
    { "overlay_diff_on",      handle_overlay_diff_on },
    { "overlay_diff_off",     handle_overlay_diff_off },
    { "overlay_shadow_dump",  handle_overlay_shadow_dump },
    { "overlay_shadow_detail", handle_overlay_shadow_detail },
    { "overlay_fp_dump",      handle_overlay_fp_dump },
    { "dirty_insn_gate",      handle_dirty_insn_gate },
    { "insn_freeze",          handle_insn_freeze },
    { "insn_freeze_status",   handle_insn_freeze_status },
    { "overlay_native_on",    handle_overlay_native_on },
    { "overlay_native_off",   handle_overlay_native_off },
    { "overlay_capture_dump", handle_overlay_capture_dump },
    { "cdrom_instant_rate",   handle_cdrom_instant_rate },
    { "cdrom_bursts",         handle_cdrom_bursts },
    { "turbo_loads",          handle_turbo_loads },
    { "autocompile_status",   handle_autocompile_status },
    { "autocompile_run",      handle_autocompile_run },
    { "overlay_rescan",       handle_overlay_rescan },
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

    /* Backlog 16, not 1: when main thread stalls (the very state we want
     * to probe during a freeze), backlog=1 means the first pending connect
     * fills the queue and every subsequent probe gets RST/ConnectionRefused.
     * 16 leaves room for a few probes to queue while we investigate. This
     * is observability infrastructure, not a freeze fix. */
    listen(s_listen, 16);
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

    if (!s_wtrace_boot) {
        s_wtrace_boot =
            (WriteTraceEntry *)calloc(WRITE_TRACE_BOOT_CAP, sizeof(WriteTraceEntry));
    }
    s_wtrace_boot_total = 0;
    s_wtrace_boot_count = 0;
    s_wtrace_boot_range_count = 0;

    /* Always-on catch-all wtrace ring (8 MB). Records EVERY RAM write
     * with lean fields (no register window). Sized for ~1 second of
     * coverage at typical Tomba write rates. */
    if (!s_wtrace_all) {
        s_wtrace_all = (WriteTraceAllEntry *)calloc(WRITE_TRACE_ALL_CAP,
                                                    sizeof(WriteTraceAllEntry));
    }
    s_wtrace_all_seq  = 0;
    s_wtrace_all_head = 0;

    if (!s_wtrace_trans) {
        s_wtrace_trans = (WriteTraceTransEntry *)calloc(WRITE_TRACE_TRANS_CAP,
                                                        sizeof(WriteTraceTransEntry));
    }
    s_wtrace_trans_seq = 0;
    s_wtrace_trans_head = 0;
    s_wtrace_trans_range_count = 0;

    fntrace_arm_from_env("PSX_FNTRACE_ARM");

    /* Function entry/exit ring buffers (32 MB each, 64 MB total). */
    if (!s_fn_entry) s_fn_entry = (FnEntryEntry *)calloc(FN_TRACE_CAP, sizeof(FnEntryEntry));
    if (!s_fn_exit)  s_fn_exit  = (FnExitEntry *)calloc(FN_EXIT_TRACE_CAP, sizeof(FnExitEntry));
    s_fn_entry_seq = 0;
    s_fn_exit_seq  = 0;
    s_fn_stack_top = 0;
    s_fn_unmatched_returns = 0;
    s_fn_stack_overflows   = 0;
    s_fn_tail_calls = 0;
    s_fn_prev_ra = 0;
    s_fn_direct_seen = 0;
    s_fn_direct_no_cpu = 0;
    s_fn_direct_filtered = 0;
    s_fn_trace_active = 0;
    s_fn_trace_filter_lo = 0u;
    s_fn_trace_filter_hi = 0xFFFFFFFFu;
    fn_trace_filter_from_env("PSX_FN_FILTER");

    if (!s_call_focus) {
        s_call_focus = (CallFocusEntry *)calloc(CALL_FOCUS_CAP,
                                                sizeof(CallFocusEntry));
    }
    s_call_focus_seq = 0;

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
    /* Card op result flags (kernel page 0xA000B9D0): B9D0 = success flag (set by
     * SIO IRQ chain handler on op success); B9D4..B9E0 = error flags (timeout,
     * checksum mismatch, etc.). FUN_bfc09144() in card_read returns the value
     * at B9D0 — sector-≥1 read bail at FUN_bfc08b3c gates on this. Capture
     * every writer to determine which writer fires for sector ≥1 (success vs
     * error path) and which PC sets the failure flag. */
    s_wtrace_ranges[13].lo = 0x0000B9D0u;
    s_wtrace_ranges[13].hi = 0x0000B9F0u;
    /* Shell COPY/DELETE/LOAD_DIR sub-state at 0x80066BC0. CIRCLE→7=LOAD_DIR,
     * CROSS→4=COPY-related, TRIANGLE→0=cleared, 6=DELETE. After clicking
     * COPY, BIOS got stuck at BC0=4 with NO further input transitions —
     * likely waiting for a card op completion that never fires. Track every
     * writer to identify the BIOS function driving this sub-state. */
    s_wtrace_ranges[14].lo = 0x00066BC0u;
    s_wtrace_ranges[14].hi = 0x00066BD0u;
    s_wtrace_ranges[15].lo = 0x00097420u; /* movie/frame handoff state */
    s_wtrace_ranges[15].hi = 0x00097430u;
    s_wtrace_range_count = 16;

    s_wtrace_boot_ranges[0].lo = 0x00097420u; /* movie/frame handoff state */
    s_wtrace_boot_ranges[0].hi = 0x00097430u;
    s_wtrace_boot_range_count = 1;

    s_wtrace_trans_ranges[0].lo = 0x00097420u; /* movie/frame handoff state */
    s_wtrace_trans_ranges[0].hi = 0x00097430u;
    s_wtrace_trans_range_count = 1;

#if DEFAULT_DEBUG_PORT == 4470
    /* Tomba STR/FMVs: movie state, CD sector descriptor ring, and CD globals.
     * These are passive traces for the game runtime only. */
    s_wtrace_ranges[15].lo = 0x000D7188u;
    s_wtrace_ranges[15].hi = 0x000D7588u;
    s_wtrace_ranges[16].lo = 0x0009B010u;
    s_wtrace_ranges[16].hi = 0x0009B050u;
    s_wtrace_ranges[17].lo = 0x000A15C8u;
    s_wtrace_ranges[17].hi = 0x000A3270u;
    /* Tomba task scheduler descriptors and scratchpad state. The current
     * black-screen blocker is a missing resume after the loader task closes,
     * so keep the task table and scheduler scratch bytes in the reverse trace
     * from process start. */
    s_wtrace_ranges[18].lo = 0x001FD800u;
    s_wtrace_ranges[18].hi = 0x001FD950u;
    s_wtrace_ranges[19].lo = 0x1F8001CCu;
    s_wtrace_ranges[19].hi = 0x1F800200u;
    s_wtrace_ranges[20].lo = 0x1F800150u;
    s_wtrace_ranges[20].hi = 0x1F800180u;
    /* Runtime-loaded Tomba overlay text/data and the high-stack descriptors
     * used by its primitive-list builders. This catches loader writes and any
     * later self-modification before the frame-2500 overlay loop floods the
     * trace. */
    s_wtrace_ranges[21].lo = 0x000E0000u;
    s_wtrace_ranges[21].hi = 0x000F0000u;
    s_wtrace_ranges[22].lo = 0x001FE000u;
    s_wtrace_ranges[22].hi = 0x001FE400u;
    /* BIOS card-operation public state arrays. The Tomba Load menu waits on
     * F4000001 events after _card_read; these cells reveal which BIOS path
     * leaves the operation armed or clears it and delivers the public event. */
    s_wtrace_ranges[23].lo = 0x00009F20u;
    s_wtrace_ranges[23].hi = 0x00009F38u;
    /* Tomba pad-poll buffer (game state +0x30 -> 0x8009EB58, second buffer at
     * 0x8009EB7A). Display thread state machine gates on (*0x1F8001FC & 0x4008)
     * which derives from `func_80028D70(0)` reading this buffer. Capture every
     * write so we can attribute who initialises the layout — BIOS PadInit,
     * runtime SIO, game-side memcpy, or none of the above. */
    s_wtrace_ranges[24].lo = 0x0009EB40u;
    s_wtrace_ranges[24].hi = 0x0009EB80u;
    /* The gflag word the display thread polls (newly-set bits). */
    s_wtrace_ranges[25].lo = 0x0009C9D0u;
    s_wtrace_ranges[25].hi = 0x0009C9E0u;
    /* OPTIONS/New Game render-state objects. The manager initializes two
     * 0xF0-byte objects at 0x8009B3A0 and 0x8009B490. Gate func 0x8006B494
     * reads object+0xE6/object+0x46, and func 0x8006A0C0 initializes the
     * dispatch callbacks at object+0x14/+0x18. Keep both objects always-on
     * from boot so the writers survive long menu/FMV runs. */
    s_wtrace_ranges[26].lo = 0x0009B300u;
    s_wtrace_ranges[26].hi = 0x0009B700u;
    /* Function pointer globals used by func_8006A0C0 before it decides whether
     * to initialize the object. */
    s_wtrace_ranges[27].lo = 0x00097520u;
    s_wtrace_ranges[27].hi = 0x00097538u;
    /* DRAWENV/GPU command queue globals. At the black-screen state,
     * 0x80090CAC continues to be copied every frame while queue indices
     * 0x80090DA0/0x80090DA4 stop advancing. Keep register context for the
     * copy/enqueue writers so we can identify the caller-supplied env. */
    s_wtrace_ranges[28].lo = 0x00090C80u;
    s_wtrace_ranges[28].hi = 0x00090DE0u;
    s_wtrace_ranges[29].lo = 0x000B3200u; /* title/load GPU packet buffer */
    s_wtrace_ranges[29].hi = 0x000B3800u;
    s_wtrace_ranges[30].lo = 0x000EA000u; /* BIOS licensed-screen GPU packet buffer */
    s_wtrace_ranges[30].hi = 0x000ED000u;
    s_wtrace_ranges[31].lo = 0x00097420u; /* movie/frame handoff state */
    s_wtrace_ranges[31].hi = 0x00097430u;
    s_wtrace_range_count = 32;

    s_wtrace_boot_ranges[0].lo = 0x0009B3B0u; /* slot0 callbacks */
    s_wtrace_boot_ranges[0].hi = 0x0009B3C4u;
    s_wtrace_boot_ranges[1].lo = 0x0009B3E0u; /* slot0 state bytes */
    s_wtrace_boot_ranges[1].hi = 0x0009B3F0u;
    s_wtrace_boot_ranges[2].lo = 0x0009B480u; /* slot0 gate/status */
    s_wtrace_boot_ranges[2].hi = 0x0009B490u;
    s_wtrace_boot_ranges[3].lo = 0x0009B4A0u; /* slot1 callbacks */
    s_wtrace_boot_ranges[3].hi = 0x0009B4B4u;
    s_wtrace_boot_ranges[4].lo = 0x0009B4D0u; /* slot1 state bytes */
    s_wtrace_boot_ranges[4].hi = 0x0009B4E0u;
    s_wtrace_boot_ranges[5].lo = 0x0009B570u; /* slot1 gate/status */
    s_wtrace_boot_ranges[5].hi = 0x0009B580u;
    s_wtrace_boot_ranges[6].lo = 0x00097520u; /* manager callbacks */
    s_wtrace_boot_ranges[6].hi = 0x00097538u;
    s_wtrace_boot_ranges[7].lo = 0x0000E1F4u; /* BIOS TCB save areas */
    s_wtrace_boot_ranges[7].hi = 0x0000E400u;
    s_wtrace_boot_ranges[8].lo = 0x0009C970u; /* title/menu state variables */
    s_wtrace_boot_ranges[8].hi = 0x0009C9A0u;
    s_wtrace_boot_ranges[9].lo = 0x000B3200u; /* title/load GPU packet buffer */
    s_wtrace_boot_ranges[9].hi = 0x000B3800u;
    s_wtrace_boot_ranges[10].lo = 0x000EA000u; /* BIOS licensed-screen GPU packet buffer */
    s_wtrace_boot_ranges[10].hi = 0x000ED000u;
    s_wtrace_boot_ranges[11].lo = 0x00097420u; /* movie/frame handoff state */
    s_wtrace_boot_ranges[11].hi = 0x00097430u;
    s_wtrace_boot_range_count = 12;

    s_wtrace_trans_ranges[0].lo = 0x0000E1F4u; /* BIOS TCB save areas */
    s_wtrace_trans_ranges[0].hi = 0x0000E400u;
    s_wtrace_trans_ranges[1].lo = 0x001FD800u; /* Tomba task table */
    s_wtrace_trans_ranges[1].hi = 0x001FD950u;
    s_wtrace_trans_ranges[2].lo = 0x1F8001CCu; /* scheduler scratch */
    s_wtrace_trans_ranges[2].hi = 0x1F800200u;
    s_wtrace_trans_ranges[3].lo = 0x0009B300u; /* render/menu state objects */
    s_wtrace_trans_ranges[3].hi = 0x0009B700u;
    s_wtrace_trans_ranges[4].lo = 0x00097520u; /* manager callbacks */
    s_wtrace_trans_ranges[4].hi = 0x00097538u;
    s_wtrace_trans_ranges[5].lo = 0x00090C80u; /* DRAWENV/GPU queue globals */
    s_wtrace_trans_ranges[5].hi = 0x00090DE0u;
    s_wtrace_trans_ranges[6].lo = 0x0009EB40u; /* pad buffers */
    s_wtrace_trans_ranges[6].hi = 0x0009EB80u;
    s_wtrace_trans_ranges[7].lo = 0x0009C9D0u; /* display gflag word */
    s_wtrace_trans_ranges[7].hi = 0x0009C9E0u;
    s_wtrace_trans_ranges[8].lo = 0x0009C970u; /* title/menu state variables */
    s_wtrace_trans_ranges[8].hi = 0x0009C9A0u;
    s_wtrace_trans_ranges[9].lo = 0x00097420u; /* movie/frame handoff state */
    s_wtrace_trans_ranges[9].hi = 0x00097430u;
    s_wtrace_trans_range_count = 10;
#endif

    /* Tier 1: heap-allocate MMIO trace ring buffer (2 MB). */
    if (!s_mmio_trace) {
        s_mmio_trace = (MmioTraceEntry *)calloc(MMIO_TRACE_CAP, sizeof(MmioTraceEntry));
    }
    s_mmio_trace_seq = 0;
    s_mmio_trace_head = 0;

    /* Dedicated GP1 display-control ring (long retention). */
    if (!s_gp1_trace) {
        s_gp1_trace = (MmioTraceEntry *)calloc(GP1_TRACE_CAP, sizeof(MmioTraceEntry));
    }
    s_gp1_trace_seq = 0;
    s_gp1_trace_head = 0;

    memset(s_watchpoints, 0, sizeof(s_watchpoints));
    memset(s_snapshot_addrs, 0, sizeof(s_snapshot_addrs));
    memset(s_snapshot_active, 0, sizeof(s_snapshot_active));
}

void debug_server_poll(void)
{
    /* Phase 1.0e-e2 starvation watchdog heartbeat. Refreshes the
     * "last poll wall-clock" timestamp; if too much time passes
     * between calls, the ring is dumped and the runtime aborts. */
    extern void starvation_watchdog_heartbeat(void);
    starvation_watchdog_heartbeat();

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
                /* Don't reset s_input_override here. A scheduled press
                 * (s_input_frames > 0) must continue to hold across the
                 * client disconnect — Python clients open a fresh socket
                 * per command, so clearing on every connection close
                 * would truncate every press to a 1-frame edge. */
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

    /* step / run_to_frame post-frame hooks: removed with the rest of
     * pause/step machinery. s_step_count and s_run_to stay at zero. */
    if (0) {
        send_fmt("{\"event\":\"unreachable\",\"frame\":%llu}",
                 (unsigned long long)(s_frame_count - 1));
    }
}

void debug_server_wait_if_paused(void)
{
    /* No-op: pause/step removed (see handle_pause). Kept exported so
     * main.cpp's vblank callback continues to compile without
     * conditional defines. s_paused is now permanently zero, so the
     * old `while (s_paused)` loop would have been a no-op anyway. */
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

int debug_server_turbo_enabled(void)
{
    return s_turbo_enabled != 0;
}
