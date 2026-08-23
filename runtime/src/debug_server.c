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
#include "psx_bss.h"
#include "nd_intro_ot.h"
#include "latency_ring.h"
#include "overlay_loader.h"
#include "overlay_capture.h"
#include "code_provider.h"
#include "overlay_backend.h"
#include "cpu_state.h"
#include "pgxp.h"
#include "dma.h"
#include "gpu.h"
#include "gpu_render.h"   /* gr_scale + gr_render_display_hires (screenshot_hires) */
#include "tex_pack.h"     /* HD texture replacement state (tex_pack command) */
#include "present_ring.h"
#include "load_transition_ring.h"
#include "cdrom.h"
#include "sio.h"
#include "sio1.h"
#include "dual_machine.h"
#include "memcard.h"
#include "spu.h"
#include "audio_trace.h"
#include "mdec.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "timers.h"
#include "dirty_ram_interp.h"
#include "card_read_summary.h"
#include "card_data_writes.h"
#include "crash_trace.h"
#include "gpu_gl_renderer.h"
#include "lockstep.h"
#include "psx_ram.h"

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
#  if defined(__x86_64__) || defined(_M_X64)
#    include <intrin.h>     /* __readgsqword — native stack-overflow guard */
#    define PSX_STACK_GUARD 1
#  endif
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
#  include <pthread.h>      /* phase_profile sampler thread */
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
   static int sock_error(void) { return errno; }
#endif

#include "psx_sdl.h"

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
static int    s_listen_err = 0;   /* platform socket error captured by init */

#define RECV_BUF_SIZE 8192
static PSX_BSS char s_recv_buf[RECV_BUF_SIZE];
static int  s_recv_len = 0;

/* ---- Dedicated TCP I/O thread (keeps the socket queryable under emu load) ----
 * The I/O thread owns accept/recv/send. A command still EXECUTES on the emu
 * thread at the debug_server_poll() safe point (no state races; the ~200 send
 * sites are unchanged — send_line buffers into s_resp_buf instead of touching
 * the socket). The client is one-command-per-connection, so a single in-flight
 * request slot suffices. A lock-free `ping` fast-path on the I/O thread answers
 * even while the emu thread is buried (freeze liveness). See debug_server_poll /
 * io_thread_main. */
enum { IO_IDLE = 0, IO_REQ = 1, IO_RESP = 2 };
static SDL_Thread *s_io_thread   = NULL;
static SDL_mutex  *s_io_mutex    = NULL;
static SDL_cond   *s_io_req_cv   = NULL;   /* emu waits on this for a request   */
static SDL_cond   *s_io_resp_cv  = NULL;   /* I/O waits on this for a response  */
static volatile int s_io_state   = IO_IDLE;
static volatile int s_io_running  = 0;
static char  s_io_req[RECV_BUF_SIZE];      /* request line (I/O -> emu)         */
static char *s_resp_buf = NULL;            /* response bytes (emu -> I/O)       */
static size_t s_resp_len = 0, s_resp_cap = 0;
static int    s_resp_overflow = 0;
static int    s_in_command = 0;            /* 1 while emu runs process_command  */
static int    io_thread_main(void *arg);   /* defined near debug_server_poll    */
static volatile int s_fmv_quiet = 0;

void debug_server_set_fmv_quiet(int quiet)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("PSX_DEBUG_FMV_QUIET");
        enabled = (!env || env[0] != '0') ? 1 : 0;
    }
    s_fmv_quiet = enabled && quiet;
}

int debug_server_fmv_quiet(void)
{
    return s_fmv_quiet ? 1 : 0;
}

/* ---- Frame counter (set by record_frame caller) ---- */
/* Non-static so other instrumentation (e.g. dirty_ram_interp.c) can stamp
 * ring-buffer entries with the current frame for cross-correlation. */
uint64_t s_frame_count = 0;

/* ---- Layer-1 first-divergence per-frame fingerprint ----------------------
 * Cumulative, ORDER-DEPENDENT rolling hashes over MAIN-RAM guest writes.
 * The point: native and dirty-interp are the SAME deterministic program run
 * two ways; they must produce an identical sequence of (addr,val,store_pc)
 * guest-RAM writes until a codegen/timing bug forks them. A guest RAM write
 * (and the PC issuing it) is pure guest semantics — granularity-invariant
 * across native vs interp (unlike host block/dispatch counts). We accumulate
 * two rolling hashes and snapshot them once per guest frame. Diff two runs'
 * per-frame columns: the FIRST frame whose hash differs is the first-divergence
 * frame, found in O(1) instead of O(n) function guesses. wr_hash vs pc_hash
 * classifies the fork: pc differs but wr matches => same writes via a different
 * control path; wr differs => actual state divergence. Reusable for any title.
 * Hashed over main RAM (phys < live size) only — game state lives there; MMIO/
 * scratchpad churn (device polling) would add benign cross-backend noise. */
uint64_t g_fp_wr_hash    = 1469598103934665603ULL;  /* FNV-1a-style seed (main RAM) */
uint64_t g_fp_pc_hash    = 1469598103934665603ULL;  /* store-PC path sig (main RAM)  */
uint64_t g_fp_write_count = 0;
uint64_t g_fp_mmio_hash  = 1469598103934665603ULL;  /* SEPARATE: device-register writes */
uint64_t g_fp_mmio_count = 0;
uint64_t g_fp_sp_hash    = 1469598103934665603ULL;  /* SEPARATE: scratchpad (0x1F8000xx) writes */
uint64_t g_fp_sp_count   = 0;
#define FP_RING_CAP 32768
typedef struct { uint32_t frame; uint64_t wr_hash; uint64_t pc_hash; uint64_t wcount;
                 uint64_t mmio_hash; uint64_t mmio_count;
                 uint64_t sp_hash; uint64_t sp_count; uint64_t cyc; } FpEntry;
static PSX_BSS FpEntry  s_fp_ring[FP_RING_CAP];
static uint32_t s_fp_head  = 0;
static uint64_t s_fp_total = 0;

/* Record a guest WRITE into the per-frame fingerprint. Main RAM (phys < live size)
 * feeds the proven wr/pc hashes. Scratchpad (0x1F800000..0x1F8003FF) feeds a
 * SEPARATE sp_hash — it was previously dropped entirely (the blind spot that
 * hid a possible pre-1823 scratchpad-state fork), but folding it into wr_hash
 * would pollute the main-RAM signal with benign device-poll churn, so it gets
 * its own classified column instead. */
static inline void fp_record_write(uint32_t phys, uint32_t val, uint32_t pc)
{
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {   /* scratchpad — separate hash */
        uint64_t s = g_fp_sp_hash;
        s = (s ^ (uint64_t)phys) * 1099511628211ULL;
        s = (s ^ (uint64_t)val)  * 1099511628211ULL;
        s = (s ^ (uint64_t)pc)   * 1099511628211ULL;
        g_fp_sp_hash = s;
        g_fp_sp_count++;
        return;
    }
    if (phys >= g_psx_ram_size) return;                  /* main RAM only */
    uint64_t h = g_fp_wr_hash;
    h = (h ^ (uint64_t)phys) * 1099511628211ULL;
    h = (h ^ (uint64_t)val)  * 1099511628211ULL;
    g_fp_wr_hash = h;
    g_fp_pc_hash = (g_fp_pc_hash ^ (uint64_t)pc) * 1099511628211ULL;
    g_fp_write_count++;
}

/* MMIO/device-register write signature — separate hash so the diff can tell a
 * RAM-state fork from a device-interaction fork (CD command, IRQ ack/mask, GPU).
 * This is the gap that hid the true first divergence: the main-RAM fingerprint
 * only sees the RAM AFTERMATH of a CD/IRQ-register divergence. */
static inline void fp_record_mmio(uint32_t addr, uint32_t val, uint32_t pc)
{
    uint64_t h = g_fp_mmio_hash;
    h = (h ^ (uint64_t)addr) * 1099511628211ULL;
    h = (h ^ (uint64_t)val)  * 1099511628211ULL;
    h = (h ^ (uint64_t)pc)   * 1099511628211ULL;
    g_fp_mmio_hash = h;
    g_fp_mmio_count++;
}

static void fp_snapshot(uint32_t frame)
{
    FpEntry *e = &s_fp_ring[s_fp_head];
    e->frame      = frame;
    e->wr_hash    = g_fp_wr_hash;
    e->pc_hash    = g_fp_pc_hash;
    e->wcount     = g_fp_write_count;
    e->mmio_hash  = g_fp_mmio_hash;
    e->mmio_count = g_fp_mmio_count;
    e->sp_hash    = g_fp_sp_hash;
    e->sp_count   = g_fp_sp_count;
    { extern uint64_t psx_get_cycle_count(void); e->cyc = psx_get_cycle_count(); }
    s_fp_head  = (s_fp_head + 1) % FP_RING_CAP;
    s_fp_total++;
}

/* ---- Layer-2 frame-gated ordered write recorder -------------------------
 * Once Layer-1 names the first-divergence frame N, arm `record_frame N` to
 * capture the full ORDERED list of main-RAM writes for that one frame in both
 * runs. Bounded (one frame), so it dumps in clean pages with no eviction race.
 * Diff the two ordered logs by index (longest common prefix) -> the first
 * differing (addr,val,pc) is the exact instruction where execution forks. */
/* UNIFIED ordered access recorder. One buffer, so the array index IS the true
 * execution order across writes AND device reads — no separate-indices problem.
 * Each entry carries `kind` so the diff can interleave and classify. Captures,
 * in execution order for the one gated frame: main-RAM writes, scratchpad
 * writes (previously the blind spot), MMIO writes, and MMIO reads. The literal
 * first divergent ACCESS (read or write) is the first index whose tuple differs
 * between two runs. */
#define REC_CAP 400000
#define REC_KIND_RAM_W   0   /* main-RAM write   */
#define REC_KIND_SP_W    1   /* scratchpad write */
#define REC_KIND_MMIO_W  2   /* device-register write */
#define REC_KIND_MMIO_R  3   /* device-register read  */
#define REC_KIND_RAM_R   4   /* main-RAM read (targeted watch range only) */
typedef struct { uint8_t kind; uint32_t addr; uint32_t val; uint32_t pc; uint32_t ra; uint64_t cyc; } RecEntry;
static PSX_BSS RecEntry  s_rec_buf[REC_CAP];
static uint32_t  s_rec_count = 0;
static int64_t   s_rec_frame = -1;       /* target guest frame, -1 = off */
static uint32_t  s_rec_overflow = 0;

/* Targeted main-RAM READ watch. Main-RAM data loads are far too hot to trace
 * wholesale, but a narrow watched range answers the decisive question for a
 * value-fork-with-no-write-divergence: what value does each backend actually
 * load from the suspect address, and does it even read that address (proving
 * pointer/register equality)? Reads in [lo,hi) land in the unified buffer as
 * REC_KIND_RAM_R, interleaved in execution order with the writes. The hot read
 * path checks only the int flag g_ram_read_watch_active (0 = no cost). */
int             g_ram_read_watch_active = 0;
static uint32_t s_rwatch_lo = 0, s_rwatch_hi = 0;

static inline void rec_event(uint8_t kind, uint32_t addr, uint32_t val,
                             uint32_t pc, uint32_t ra)
{
    if (s_rec_frame < 0 || (int64_t)s_frame_count != s_rec_frame) return;
    if (s_rec_count >= REC_CAP) { s_rec_overflow++; return; }
    RecEntry *e = &s_rec_buf[s_rec_count++];
    e->kind = kind; e->addr = addr; e->val = val; e->pc = pc; e->ra = ra;
    { extern uint64_t psx_get_cycle_count(void); e->cyc = psx_get_cycle_count(); }
}

void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val)
{
    if (phys >= s_rwatch_lo && phys < s_rwatch_hi)
        rec_event(REC_KIND_RAM_R, phys, val, 0, 0);
}

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
/* Optional analog-stick override (set_input lx/ly/rx/ry, 0..255, 0x80 =
 * centre). Lets injected input drive analog-mode movement; consumed by the
 * pad sampler alongside the button word. */
static int     s_axis_override = 0;
static uint8_t s_axis_st[4]    = { 0x80, 0x80, 0x80, 0x80 };

/*
 * Exact guest-VBlank input route. A client queues run-length encoded digital
 * pad segments while inactive, then starts the route atomically. Consuming the
 * queue in debug_server_get_input_override() avoids host/TCP timing gaps
 * between short presses and remains deterministic while turbo loads are active.
 */
#define INPUT_ROUTE_MAX_STEPS 4096
typedef struct {
    uint32_t frames;
    uint16_t buttons;
} InputRouteStep;
static PSX_BSS InputRouteStep s_input_route[INPUT_ROUTE_MAX_STEPS];
static uint32_t s_input_route_count = 0;
static uint32_t s_input_route_index = 0;
static uint32_t s_input_route_remaining = 0;
static int      s_input_route_active = 0;

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
static PSX_BSS Watchpoint s_watchpoints[MAX_WATCHPOINTS];

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
    /* Callee-saved regs: loop cursors/bounds live here (e.g. OpenBIOS
     * readPad's s2/s0 halfword loop — debugging it needed them and the ring
     * only had temps). Cheap: +24B/entry on a heap ring. */
    uint32_t s0;
    uint32_t s1;
    uint32_t s2;
    uint32_t s3;
    uint32_t s4;
    uint32_t s5;
    uint32_t frame;      /* VBlank frame number */
    uint8_t  width;      /* 1, 2, or 4 */
    int8_t   dma_ch;     /* DMA channel that produced this write (0-6), or -1 = CPU store.
                          * When >=0, `pc` is the DMA kick PC, not g_debug_last_store_pc. */
    uint8_t  pad[2];     /* align to 8 bytes */
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
static PSX_BSS SioPcTraceEntry s_sio_pc_trace[SIO_PC_TRACE_CAP];
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
static PSX_BSS SioCtrlRegTraceEntry s_sio_ctrl_reg_trace[SIO_CTRL_REG_TRACE_CAP];
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
static PSX_BSS RestoreTraceEntry s_restore_trace[RESTORE_TRACE_CAP];
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
static PSX_BSS ThreadTraceEntry s_thread_trace[THREAD_TRACE_CAP];
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

static PSX_BSS SregTraceEntry s_sreg_trace[SREG_TRACE_CAP];
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
static PSX_BSS ProbeTraceEntry s_probe_trace[PROBE_TRACE_CAP];
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

extern uint16_t psx_read_half(uint32_t addr);
static uint32_t trace_read_word(CPUState *cpu, uint32_t addr)
{
    (void)cpu;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys > 0x001FFFFCu &&
        (phys < 0x1F800000u || phys > 0x1F8003FCu)) {
        return 0;
    }
    /* Use psx_read_word directly (NOT cpu->read_word): debug instrumentation
     * must not charge guest main-RAM read wait states, and runs on the IO
     * thread where psx_advance_cycles would race the emulation thread. */
    ls_suppress_begin();
    uint32_t v = psx_read_word(addr);
    ls_suppress_end();
    return v;
}

static uint32_t trace_read_half(CPUState *cpu, uint32_t addr)
{
    (void)cpu;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys > 0x001FFFFEu &&
        (phys < 0x1F800000u || phys > 0x1F8003FEu)) {
        return 0;
    }
    ls_suppress_begin();
    uint32_t v = psx_read_half(addr);
    ls_suppress_end();
    return v;
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
static PSX_BSS uint32_t s_dispatch_ring[DISPATCH_TRACE_CAP];
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
static PSX_BSS UnknownDispatchEntry s_unknown_ring[UNKNOWN_DISPATCH_CAP];
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
static PSX_BSS UnknownUniqueEntry s_unknown_unique[UNKNOWN_UNIQUE_CAP];
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
static PSX_BSS uint32_t s_dispatch_unique[DISPATCH_UNIQUE_CAP];
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
static PSX_BSS ChainTraceEntry s_chain_trace[CHAIN_TRACE_CAP];
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
#define CARD_MGR_TRACE_CAP 65536
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
    uint32_t repeat; /* spin-collapse: extra consecutive hits folded in */
    uint8_t  source; /* 0 = direct entry hook, 1 = dispatch hook */
    uint8_t  pad[3];
} CardMgrTraceEntry;
static PSX_BSS CardMgrTraceEntry s_card_mgr_trace[CARD_MGR_TRACE_CAP];
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
    /* ROM card driver + bu-device layer: 0xBFC08600 (validators 0x884C/0x89C0,
     * sync waits 0x895C) through dev_bu_open 0x996C / dirsearch 0xA754 /
     * card ops 0xD970-0xDBC0 (was 0xB600..0xC240 — too narrow: the open()
     * failure path lives in 0x8600..0xB600 and 0xC240..0xE000). */
    if (phys >= 0x1FC08600u && phys < 0x1FC0E000u) return 1;
    if (phys >= 0x00004900u && phys < 0x00006C80u) return 1;
    /* Kernel FCB/file layer: open 0x2958, FCB alloc 0x3060, devparse 0x31E8,
     * firstfile 0x39A4, event funcs 0x1EC8+. Covers the B-call file API the
     * game uses to reach the bu device. */
    if (phys >= 0x00001000u && phys < 0x00004900u) return 1;
    if (phys == 0x00001B44u) return 1;
    /* MMX6 game-side card manager (BU-op issuer 0x8001C1AC, event poll
     * 0x8001C824, status poll caller 0x8001C64C). Covers the game half of
     * the card conversation so game->BIOS decision points are visible. */
    if (phys >= 0x0001C000u && phys < 0x0001D000u) return 1;
    return 0;
}

static void card_mgr_trace_record(uint32_t func_addr, uint8_t source) {
    if (!debug_cpu_ptr) return;
    uint32_t phys = func_addr & 0x1FFFFFFFu;
    if (!card_mgr_trace_target(phys)) return;

    /* Spin-collapse: a poll loop (e.g. the game's TestEvent spin at
     * 0x8001C824) re-enters the same function millions of times and would
     * wipe the ring. Fold a hit identical in (func, source, ra) to the
     * previous entry into that entry's repeat count; refresh v0 so the
     * final iteration's return value is the one preserved.
     *
     * TestEvent's body (B(0Bh) entry 0x1EC8, exit block 0x1F04, EvCB
     * helpers 0x641C/0x659C) alternates funcs AND src0/src1 hooks every
     * entry, so the identical-key fold never engages and one spinning
     * frame floods the ring with ~12K entries (k-run measured), evicting
     * the window under investigation. Fold ANY consecutive run of these
     * spin-body funcs into one entry: rep then equals the total hook-hit
     * count of the run (divide by 2 hooks/call for calls; /8 for full
     * 4-descriptor poll iterations when 641C fires per call). Sequence
     * boundaries stay visible because any non-body func (per-VBlank card
     * machinery, delivers) cuts the run. */
    if (s_card_mgr_trace_seq > 0) {
        CardMgrTraceEntry *p =
            &s_card_mgr_trace[(s_card_mgr_trace_seq - 1) % CARD_MGR_TRACE_CAP];
        int body  = (phys == 0x1EC8u || phys == 0x1F04u ||
                     phys == 0x641Cu || phys == 0x659Cu);
        int pbody = (p->func_addr == 0x1EC8u || p->func_addr == 0x1F04u ||
                     p->func_addr == 0x641Cu || p->func_addr == 0x659Cu);
        if ((body && pbody) ||
            (p->func_addr == phys && p->source == source &&
             p->ra == debug_cpu_ptr->gpr[31])) {
            p->repeat++;
            p->v0    = debug_cpu_ptr->gpr[2];
            p->frame = (uint32_t)s_frame_count;
            return;
        }
    }

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
    e->repeat    = 0;
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
    /* EvCB walk ring: ALWAYS-ON (ring-buffer model — never arm-gated).
     * Capture on DeliverEvent entry; record the matching EXIT snapshot when
     * execution next dispatches to the stored return RA. Runs before the
     * fntrace arm gate below so the ring covers every DeliverEvent from
     * boot, whether or not the shadow-stack tracer is armed. */
    if (debug_cpu_ptr) {
        uint32_t ev_ra = debug_cpu_ptr->gpr[31];
        if (target == EVCB_DELIVER_EVENT_ADDR) {
            if (s_evcb_pending_active) {
                s_evcb_unwound_count++;
            }
            s_evcb_pending_exit_ra   = ev_ra & 0x1FFFFFFFu;
            s_evcb_pending_entry_seq = s_fn_entry_seq;
            s_evcb_pending_active    = 1;
            evcb_snapshot_capture(EVCB_TAG_ENTRY, s_fn_entry_seq);
        } else if (s_evcb_pending_active && target == s_evcb_pending_exit_ra) {
            evcb_snapshot_capture(EVCB_TAG_EXIT, s_evcb_pending_entry_seq);
            s_evcb_pending_active = 0;
        }
    }

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

    /* EvCB ring capture moved to the top of this function (always-on,
     * ahead of the fntrace arm gate) — see the block above. */

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
/* Native stack-overflow guard. The host C stack mirrors the guest call graph,
 * and an in-range guest `jal` emits a DIRECT func_X(cpu) call — so a runaway
 * guest recursion (a corrupt jump-table that loops a handler back on itself,
 * the seesaw/Bug-D wild-call family) grows the native stack WITHOUT going
 * through psx_dispatch_impl, leaving g_psx_dispatch_depth (and the depth-counter
 * guard in dirty_ram_dispatch) blind to it — crash reports show a STACK_OVERFLOW
 * with dispatch_depth=0. This reads the running fiber's actual stack bounds from
 * the x64 TEB (StackBase = GS:[0x08], DeallocationStack/reserve-low = GS:[0x1478])
 * and, when the remaining headroom runs low, halts gracefully via psx_fatal_halt
 * (captured crash report + halt-and-serve in debug / clean exit + report in
 * release) BEFORE the host stack overflows — on EVERY dispatch path, in both
 * builds. Guest fibers are 1 MB (traps.c); the headroom (¼ of the stack) only
 * trips on runaway recursion (thousands of frames), never on legitimate
 * gameplay call depth (~hundreds of KB at most). TENTATIVE containment + better
 * diagnostics, NOT a root fix: the upstream data corruption that produces the
 * wild recursion is unaddressed (needs an oracle repro — see crash report's
 * dirty_block cycle for the recursing PCs). */
/* Always-on recent guest function-entry ring (GUEST addresses — build-independent,
 * unlike the SEH native stack_scan whose host offsets only map against the exact
 * crashing binary). Fed at EVERY recompiled function entry in BOTH debug and
 * release. For a runaway recursion the tail of this ring is dominated by the
 * recursing func(s); the crash report dumps it so the next ORGANIC crash names
 * the culprit directly — closing the gap where the report otherwise can't
 * identify a direct-call (dispatch_depth=0) runaway. g_psx_recursion_func is the
 * function being entered at the instant the guard trips (the recursing one).
 * Defined unconditionally so crash_trace.c can always dump them; only the FEED
 * (and the guard) is gated on PSX_STACK_GUARD. */
#define PSX_RECENT_FN_CAP 64u
PSX_BSS uint32_t g_psx_recent_fn[PSX_RECENT_FN_CAP];
uint32_t g_psx_recent_fn_i   = 0;
uint32_t g_psx_recursion_func = 0;

/* ── §19 compiled-entry stack-depth profile ───────────────────────────────────
 * Sampled at EVERY compiled function entry (the deepest hot path), so the
 * per-frame MAX host-stack-used here is the TRUE intra-frame depth — the §17
 * vblank sample is shallow and the §18 boundary recorder proved the boundary is
 * flat. Resolves the open contradiction (native_stack walker says 23k-deep vs
 * live counters say flat): if the FROZEN frame's max_kb climbs to ~the guard
 * threshold while normal frames stay low => real compiled recursion; if it stays
 * low => the native-guard tripped on a BAD TEB read (raw base/dealloc/sp at the
 * trip are dumped to verify the math). Defined always; fed only under the guard. */
typedef struct { uint32_t frame; uint32_t entries; uint32_t max_kb; uint32_t max_func; } CeSum;
#define CE_CAP 512u
static PSX_BSS CeSum    s_ce[CE_CAP];
static uint64_t s_ce_seq = 0;
static uint32_t s_ce_frame = 0xFFFFFFFFu, s_ce_entries = 0, s_ce_max_kb = 0, s_ce_max_func = 0;
/* raw TEB values captured AT the guard trip (sanity-check garbage-read hypothesis) */
static uint64_t g_ce_trip_base = 0, g_ce_trip_dealloc = 0, g_ce_trip_sp = 0;
static uint32_t g_ce_trip_kb = 0, g_ce_trip_frame = 0;

static void ce_sample(uint32_t func_addr, uintptr_t base, uintptr_t sp) {
    extern uint64_t s_frame_count;
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s_ce_frame) {
        if (s_ce_frame != 0xFFFFFFFFu) {
            CeSum *e = &s_ce[s_ce_seq++ & (CE_CAP - 1u)];
            e->frame = s_ce_frame; e->entries = s_ce_entries;
            e->max_kb = s_ce_max_kb; e->max_func = s_ce_max_func;
        }
        s_ce_frame = f; s_ce_entries = 0; s_ce_max_kb = 0; s_ce_max_func = 0;
    }
    s_ce_entries++;
    uint32_t kb = (base > sp) ? (uint32_t)((base - sp) >> 10) : 0;
    if (kb > s_ce_max_kb) { s_ce_max_kb = kb; s_ce_max_func = func_addr; }
}

/* Dump the compiled-entry profile (called by crash_trace + the ce_profile cmd). */
int ce_profile_json(char *out, int cap) {
    /* fold the in-progress frame into the ring */
    if (s_ce_frame != 0xFFFFFFFFu) {
        CeSum *e = &s_ce[s_ce_seq & (CE_CAP - 1u)];   /* peek slot (do not advance) */
        e->frame = s_ce_frame; e->entries = s_ce_entries;
        e->max_kb = s_ce_max_kb; e->max_func = s_ce_max_func;
    }
    int n = snprintf(out, cap,
        "{\"cur_frame\":%u,\"cur_entries\":%u,\"cur_max_kb\":%u,"
        "\"trip\":{\"frame\":%u,\"kb\":%u,\"base\":\"0x%llX\",\"dealloc\":\"0x%llX\",\"sp\":\"0x%llX\"},"
        "\"frames\":[",
        s_ce_frame, s_ce_entries, s_ce_max_kb,
        g_ce_trip_frame, g_ce_trip_kb, (unsigned long long)g_ce_trip_base,
        (unsigned long long)g_ce_trip_dealloc, (unsigned long long)g_ce_trip_sp);
    uint64_t total = s_ce_seq + (s_ce_frame != 0xFFFFFFFFu ? 1 : 0);
    uint32_t avail = total < CE_CAP ? (uint32_t)total : CE_CAP;
    uint64_t start = total - avail;
    for (uint32_t i = 0; i < avail && n < cap - 96; i++) {
        const CeSum *e = &s_ce[(start + i) & (CE_CAP - 1u)];
        n += snprintf(out + n, cap - n, "%s{\"f\":%u,\"e\":%u,\"mkb\":%u,\"mf\":\"0x%08X\"}",
                      i ? "," : "", e->frame, e->entries, e->max_kb, e->max_func);
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

#ifdef PSX_STACK_GUARD
static void psx_native_stack_guard(uint32_t func_addr) {
    char probe;
    uintptr_t sp      = (uintptr_t)&probe;
    uintptr_t base    = (uintptr_t)__readgsqword(0x08);    /* TEB StackBase (high) */
    uintptr_t dealloc = (uintptr_t)__readgsqword(0x1478);  /* TEB DeallocationStack (reserve low) */
    ce_sample(func_addr, base, sp);                        /* §19: true intra-frame depth */
    /* §20 diag: optional EARLY trip at a custom used-KB so the leak structure can
     * be captured after only a few thousand leak-frames instead of waiting for the
     * full 48 MB overflow. PSX_STACK_GUARD_KB=N trips when (base-sp) > N KB. */
    {
        static long s_guard_kb = -2;
        if (s_guard_kb == -2) { const char *e = getenv("PSX_STACK_GUARD_KB");
                                s_guard_kb = (e && *e) ? atol(e) : -1; }
        if (s_guard_kb > 0 && base > sp && (base - sp) > (uintptr_t)s_guard_kb * 1024u) {
            g_psx_recursion_func = func_addr;
            extern uint64_t s_frame_count;
            g_ce_trip_base = base; g_ce_trip_dealloc = dealloc; g_ce_trip_sp = sp;
            g_ce_trip_kb = (uint32_t)((base - sp) >> 10);
            g_ce_trip_frame = (uint32_t)s_frame_count;
            extern void psx_fatal_halt(const char *reason);
            psx_fatal_halt("PSX_STACK_GUARD_KB early trip — capture the leak structure "
                           "(see native_stack run-length cycle + ce_profile + xprobe)");
        }
    }
    if (base <= dealloc) return;                            /* implausible TEB read — skip */
    uintptr_t headroom = (base - dealloc) / 4;             /* ¼ stack (256 KB on a 1 MB fiber) */
    if (headroom < (128u << 10)) headroom = 128u << 10;
    if (sp > dealloc && (sp - dealloc) < headroom) {
        g_psx_recursion_func = func_addr;                  /* the func recursing at the trip */
        extern uint64_t s_frame_count;
        g_ce_trip_base = base; g_ce_trip_dealloc = dealloc; g_ce_trip_sp = sp;
        g_ce_trip_kb = (base > sp) ? (uint32_t)((base - sp) >> 10) : 0;
        g_ce_trip_frame = (uint32_t)s_frame_count;
        extern void psx_fatal_halt(const char *reason);
        psx_fatal_halt("native stack guard tripped — runaway guest recursion "
                       "(direct func_X dispatch; g_psx_dispatch_depth is blind to it "
                       "— see recursion_func + recent_fn ring + the dirty_block cycle)");
    }
}
#endif

/* Self-test for the runaway-recursion crash capture: the `synth_recurse` TCP
 * command forces a deep recursion on the next guest function entry so you can
 * confirm the stack guard halts gracefully AND the report names the culprit
 * (recursion_func + recent_fn). Debug-only (the command lives behind the debug
 * server); intentionally HALTS the emulation. */
#ifndef PSX_NO_DEBUG_TOOLS
static volatile int s_synth_recurse_armed = 0;
static int psx_synth_recurse(volatile int n) {
    volatile char pad[2048]; pad[0] = (char)n;
    debug_server_log_call_entry(0x8000DEADu);   /* feeds the ring + runs the guard each level */
    int r = psx_synth_recurse(n + 1);
    return r + pad[0];
}
void debug_server_synth_recurse_arm(void) { s_synth_recurse_armed = 1; }
#endif

static inline void cyc_watch_observe(uint32_t block_leader_phys);  /* defined below; used at fn-entry */

/* Last guest function ENTERED, fed unconditionally at every compiled entry
 * (one u32 store). The wall-time sampler's static histogram keys on THIS, not
 * g_debug_current_func_addr: the dispatch stamp survives across post-exception
 * resumption, so under any IRQ cadence it pools most static samples on the
 * kernel exception-exit body (measured 55% on 0xF40) instead of the code that
 * actually ran. Entry-stamp attribution is leaf-biased but honest. */
volatile uint32_t g_psx_last_fn_entry = 0;

/* Addressable entry for overlay CPS callbacks (header inlines the Release path). */
void debug_server_log_call_entry_fn(uint32_t func_addr) {
#ifdef PSX_NO_DEBUG_TOOLS
    g_psx_last_fn_entry = func_addr;
#else
    debug_server_log_call_entry(func_addr);
#endif
}

#ifdef PSX_NO_DEBUG_TOOLS
/* Release: inlined in cpu_state.h for every generated TU. */
#else
void debug_server_log_call_entry(uint32_t func_addr) {
    /* Whole-call native replay is diagnostic and must not double-consume or
     * overwrite live trace/stack-watch state. Architectural CPU state is
     * compared by the caller; these observers are intentionally inert. */
    { extern int g_ls_replay_active; if (g_ls_replay_active) return; }
    g_psx_last_fn_entry = func_addr;
#ifdef PSX_STACK_GUARD
    g_psx_recent_fn[g_psx_recent_fn_i++ & (PSX_RECENT_FN_CAP - 1u)] = func_addr;
    psx_native_stack_guard(func_addr);   /* runs in debug AND release (before the early-return) */
#endif
    if (s_fmv_quiet) return;
    ls_suppress_begin();
    if (s_synth_recurse_armed) { s_synth_recurse_armed = 0; psx_synth_recurse(0); }
    /* cyc_watch: universal compiled-function-entry hook (game AND BIOS, incl.
     * relocated BIOS-shell funcs which dispatch oddly but still log their entry
     * here). Sampled at function entry, before the body runs — matches the
     * Beetle side (PC==anchor, before execute). Covers the game-dispatch path
     * that debug_server_trace_dispatch misses. */
    cyc_watch_observe(func_addr & 0x1FFFFFFFu);
    s_fn_direct_seen++;
    if (!debug_cpu_ptr) {
        s_fn_direct_no_cpu++;
        ls_suppress_end();
        return;
    }
    card_mgr_trace_record(func_addr, 0);
    sreg_trace_record(func_addr);
    call_focus_record(func_addr);
    if (!s_fn_entry) { ls_suppress_end(); return; }
    if (!fn_trace_in_filter(func_addr)) { ls_suppress_end(); return; }
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
    ls_suppress_end();
}
#endif /* !PSX_NO_DEBUG_TOOLS */

/* Always-on A0/B0/C0 BIOS-call ring (ported from ape-fw for good-vs-bad
 * event-delivery comparison). Recorded at the central dispatch chokepoint. */
#define BIOSCALL_RING_CAP (1 << 16)
typedef struct {
    uint64_t seq; uint32_t table_base; uint32_t index; uint32_t func_ptr;
    uint32_t a0, a1, a2, a3; uint32_t ra; uint32_t current_func; uint32_t frame;
    uint8_t in_exception;
} BiosCallEntry;
static PSX_BSS BiosCallEntry s_bioscall_ring[BIOSCALL_RING_CAP];
static uint64_t s_bioscall_seq = 0;
/* Arm flag for the B0 event/thread-op capture in debug_server_trace_dispatch.
 * OFF by default: recording every IRQ-context DeliverEvent per dispatch floods the
 * ring and starves the poll-based command path at a freeze. Arm via `event_hook`
 * only for the window of interest. */
int g_event_hook_armed = 0;
#define BIOSCALL_UNIQUE_CAP 2048
typedef struct { uint32_t table_base; uint32_t index; uint64_t count; } BiosCallUnique;
static PSX_BSS BiosCallUnique s_bioscall_unique[BIOSCALL_UNIQUE_CAP];
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

/* ---- cyc_watch: native↔Beetle per-anchor cycle comparator ----
 *
 * Arms a single guest-PC anchor and records, into an always-on ring, the
 * tuple (hit_index, psx_cycle_count, pc) the first N times the guest's
 * executing block leader equals that anchor.  The companion tool
 * tools/cycle_compare.py arms the SAME anchor on psx-beetle (matching
 * command, added parent-side) and diffs elapsed cycles per hit_index to
 * localize fine per-instruction cycle drift (e.g. the -8 class) that the
 * gross per-frame rate already hides.
 *
 * CAPTURE SEMANTICS (the Beetle side must match EXACTLY):
 *   - The anchor is sampled at BLOCK ENTRY, BEFORE the anchor instruction
 *     (the block leader at the anchor PC) executes.
 *   - psx_cycle_count at that instant = absolute guest cycles charged for
 *     ALL prior blocks, NOT including any cycle of the anchor block.
 *   - The anchor matches a basic-block *leader* PC (the address handed to
 *     the dispatcher). A PC that is only ever reached mid-block (never a
 *     dispatch/interp block leader) will not fire — anchor on a function
 *     entry or branch target.
 *
 * ANCHOR NORMALIZATION: the armed PC is masked to a physical address
 * (pc & 0x1FFFFFFF) and compared against the physical block leader on both
 * the compiled-dispatch path (already-physical `phys`) and the dirty-RAM
 * interpreter path (`target & 0x1FFFFFFF`). Caveat: BIOS-shell functions
 * relocated into RAM 0x30000-0x5AFFF are dispatched at physical 0x1FC18xxx;
 * to anchor one of those, arm its relocated physical PC. Game/BIOS-ROM
 * anchors are a plain mask and need no special handling.
 *
 * Default-off, additive: when g_cyc_watch_armed == 0 the observe hook is a
 * single load + compare and records nothing — zero behavior change. */
#define CYC_WATCH_RING_CAP 1024
typedef struct {
    uint32_t hit_index;       /* 0-based ordinal of this anchor hit */
    uint32_t pc;              /* matched physical block-leader PC */
    uint64_t psx_cycle_count; /* absolute guest cycles at block entry */
} CycWatchEntry;
static PSX_BSS CycWatchEntry s_cyc_watch_ring[CYC_WATCH_RING_CAP];
static volatile int s_cyc_watch_armed = 0; /* 1 = recording active */
static uint32_t s_cyc_watch_anchor_phys = 0; /* armed anchor (A / start), masked to phys */
static uint32_t s_cyc_watch_anchor_raw = 0;  /* armed anchor as supplied (for echo) */
static uint32_t s_cyc_watch_max_hits = 16;   /* stop after this many hits */
static uint32_t s_cyc_watch_hits = 0;        /* hits recorded so far */
/* Two-anchor REGION mode (FAITHFUL_TIMING_PLAN.md §3c): when end_phys != 0, each
 * recorded entry is the Δcycles of one A->B pass (cycles at B minus cycles at A),
 * i.e. the cost of the KNOWN code path between two dispatch points (e.g. a function
 * entry -> its exit-transfer target). end_phys == 0 keeps the single-anchor mode
 * (entry stores absolute cycles at A). Both anchors must be dispatch points (the
 * native observer only fires at function entries / compiled-dispatch / dirty
 * blocks). */
static uint32_t s_cyc_watch_end_phys = 0;    /* B / end anchor (0 = single-anchor) */
static uint32_t s_cyc_watch_end_raw  = 0;
static int      s_cyc_watch_in_region = 0;   /* 1 = saw A, awaiting B */
static uint64_t s_cyc_watch_region_start = 0;

/* Hot-path block-entry observer. Called from the compiled-dispatch path
 * (debug_server_trace_dispatch), the universal function-entry hook
 * (debug_server_log_call_entry), and the dirty-RAM path
 * (debug_server_dirty_break_maybe_pause). `block_leader_phys` is physical. */
/* Dedupe state for the dispatch+prologue double-fire (see below). */
static uint32_t s_cyc_watch_last_phys  = 0xFFFFFFFFu;
static uint64_t s_cyc_watch_last_cycle = 0xFFFFFFFFFFFFFFFFull;

static inline void cyc_watch_observe(uint32_t block_leader_phys)
{
    if (!s_cyc_watch_armed) return;                 /* disarmed: no cost */

    /* DEDUPE the double-fire: a block reached via the dispatcher is observed
     * BOTH by debug_server_trace_dispatch (routing) AND by the function's own
     * debug_server_log_call_entry (prologue) — same phys, same cycle, 0 cycles
     * apart. That double-records every dispatched entry (a function reached by a
     * direct jal fires only the prologue, so it looked like alternating pairs in
     * the ring). A genuine re-entry of the same PC is always ≥1 cycle later, so
     * keying on (phys,cycle) drops ONLY the spurious double, never a real hit. */
    uint64_t cyc_now = psx_get_cycle_count();
    if (block_leader_phys == s_cyc_watch_last_phys &&
        cyc_now           == s_cyc_watch_last_cycle) {
        return;
    }
    s_cyc_watch_last_phys  = block_leader_phys;
    s_cyc_watch_last_cycle = cyc_now;

    if (s_cyc_watch_end_phys != 0u) {               /* ── REGION mode (A..B Δ) ── */
        if (!s_cyc_watch_in_region) {
            if (block_leader_phys == s_cyc_watch_anchor_phys) {
                s_cyc_watch_region_start = cyc_now;
                s_cyc_watch_in_region = 1;
            }
        } else if (block_leader_phys == s_cyc_watch_end_phys) {
            CycWatchEntry *e = &s_cyc_watch_ring[s_cyc_watch_hits];
            e->hit_index       = s_cyc_watch_hits;
            e->pc              = block_leader_phys;
            e->psx_cycle_count = cyc_now - s_cyc_watch_region_start;  /* Δ(B-A) */
            s_cyc_watch_hits++;
            s_cyc_watch_in_region = 0;
            if (s_cyc_watch_hits >= s_cyc_watch_max_hits) s_cyc_watch_armed = 0;
        }
        return;
    }

    if (block_leader_phys != s_cyc_watch_anchor_phys) return;  /* ── single-anchor ── */
    if (s_cyc_watch_hits >= s_cyc_watch_max_hits) {
        s_cyc_watch_armed = 0;                      /* full: stop sampling */
        return;
    }
    CycWatchEntry *e = &s_cyc_watch_ring[s_cyc_watch_hits];
    e->hit_index       = s_cyc_watch_hits;
    e->pc              = block_leader_phys;
    e->psx_cycle_count = cyc_now;
    s_cyc_watch_hits++;
    if (s_cyc_watch_hits >= s_cyc_watch_max_hits) s_cyc_watch_armed = 0;
}

/* ---- pc_probe: multi-PC block-leader counters + reg samples (default-off) ----
 * Arm via TCP `pc_probe_arm` or env PSX_PC_PROBE / PSX_ND_INTRO_PROBE.
 * Samples $t0/$fp/$v0/frame at matched leaders. Disarmed = one branch.
 * nd_intro=2 also fills mode/depth/ot_base/ot_index from GP + game struct. */
#define PC_PROBE_MAX_PCS   16
#define PC_PROBE_SAMPLE_CAP 64
typedef struct {
    uint32_t pc;
    uint64_t count;
    uint32_t last_t0;
    uint32_t last_fp;
    uint32_t last_v0;
    uint32_t last_mode;
    uint32_t last_depth;
    uint32_t last_ot_base;
    uint32_t last_ot_index;
    uint32_t last_frame;
    uint32_t t0_zero;
    uint32_t t0_nonzero;
} PcProbeSlot;
typedef struct {
    uint32_t pc;
    uint32_t frame;
    uint32_t t0;
    uint32_t fp;
    uint32_t v0;
    uint32_t mode;
    uint32_t depth;
    uint32_t ot_base;
    uint32_t ot_index;
} PcProbeSample;
static volatile int s_pc_probe_armed = 0;
static int          s_pc_probe_n = 0;
static PcProbeSlot  s_pc_probe[PC_PROBE_MAX_PCS];
static PcProbeSample s_pc_probe_samples[PC_PROBE_SAMPLE_CAP];
static uint32_t     s_pc_probe_sample_n = 0;
static uint32_t     s_pc_probe_sample_max = 32;

static void pc_probe_clear_state(void)
{
    s_pc_probe_armed = 0;
    s_pc_probe_n = 0;
    s_pc_probe_sample_n = 0;
    memset(s_pc_probe, 0, sizeof(s_pc_probe));
    memset(s_pc_probe_samples, 0, sizeof(s_pc_probe_samples));
}

static int pc_probe_add_pc(uint32_t raw)
{
    uint32_t phys = raw & 0x1FFFFFFFu;
    if (phys == 0) return 0;
    for (int i = 0; i < s_pc_probe_n; i++) {
        if (s_pc_probe[i].pc == phys) return 1;
    }
    if (s_pc_probe_n >= PC_PROBE_MAX_PCS) return 0;
    s_pc_probe[s_pc_probe_n].pc = phys;
    s_pc_probe_n++;
    return 1;
}

/* Comma/space-separated hex list, e.g. "0x80044D10,0x80044E58". */
static int pc_probe_parse_list(const char *list)
{
    if (!list || !*list) return 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", list);
    int added = 0;
    for (char *p = tmp, *tok; (tok = strtok(p, ", \t\n")) != NULL; p = NULL) {
        uint32_t raw = (uint32_t)strtoul(tok, NULL, 0);
        if (pc_probe_add_pc(raw)) added++;
    }
    return added;
}

static void pc_probe_arm_nd_intro_defaults(void)
{
    /* NdIntroMeshDraw post-RTPT funnel (block leaders with cyc_observe). */
    static const uint32_t k[] = {
        0x80044580u, /* after dispatch: $fp = v0 OT ptr */
        0x80044C80u, /* face clip start */
        0x80044CB0u, /* passed neg outcode */
        0x80044CDCu, /* passed hi outcode → alloc */
        0x80044D10u, /* $t0 == 0 ? skip : emit */
        0x80044D18u, /* emit continue */
        0x80044DA4u, /* PolyG4 build */
        0x80044E58u, /* clip skip */
        0x80044EC8u, /* $t0==0 / OOM skip */
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        pc_probe_add_pc(k[i]);
}

/* OT/depth leafs of NdIntroDrawDispatch — which path returns the OT slot. */
static void pc_probe_arm_nd_intro_ot_defaults(void)
{
    static const uint32_t k[] = {
        0x80044580u, /* after jal: v0 → $fp OT slot */
        0x800440A0u, /* dispatch entry (mode half @ gp+0x4DE) */
        0x80044120u, /* mode0 depth scale using *(game+0x1D04) */
        0x80044154u, /* return OT base via *(game+buf*4+0x18C8) */
        0x800441A8u, /* return *(game+0x25C)+0xFFC (near-ish) */
        0x800441C0u, /* mode2 entry */
        0x80044268u, /* mode1/ shared OT-base return */
        0x80044DA4u, /* PolyG4 emit: confirm $fp sticky */
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        pc_probe_add_pc(k[i]);
}

/* Textured wood funnel — live path is func_8006A52C (not dead twin 0x8006A6B8). */
static void pc_probe_arm_nd_intro_wood_defaults(void)
{
    static const uint32_t k[] = {
        0x8006A52Cu, /* NdIntroWoodEmit entry */
        0x8006A564u, /* RTPT GTE caller_ra (sticky) */
        0x8006A57Cu, /* post-RTPT face loop */
        0x8006A590u, /* FLAG read / face setup */
        0x8006A5B4u, /* NCLIP */
        0x8006A610u, /* next-face hub (skip + post-emit) */
        0x8006A69Cu, /* RTPS single-vert return (bgez $0 → loop) */
        0x8006A6B8u, /* dead twin entry — expect count=0 */
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        pc_probe_add_pc(k[i]);
}

/* Depth compare: wood OTZ vs digit/glow OT link ptrs vs MeshDraw OtFar.
 * Only block-leader PCs (cyc_observe); mid-block AddPrim sites never fire. */
static void pc_probe_arm_nd_intro_depth_defaults(void)
{
    static const uint32_t k[] = {
        0x8006A608u, /* wood jalr emit — GTE OTZ */
        0x80023094u, /* DigitGt4 entry — $t7 already OT slot */
        0x80053E10u, /* glow color ori — $a2=prim; OT via game+0x147C later */
        0x800440A0u, /* DrawDispatch entry — GP mode/depth */
        0x80044580u, /* MeshDraw after dispatch */
        0x800441A8u, /* OtFar leaf */
        0x80052F98u, /* DigitFx entry */
        0x800444ECu, /* MeshDraw entry — sample $ra */
        0x80044DA4u, /* PolyG4 emit hub — lighting bytes on stack */
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        pc_probe_add_pc(k[i]);
}

/* Wood DL / helper selection: batch setup + which emit helper is bound. */
static void pc_probe_arm_nd_intro_wood_dl_defaults(void)
{
    static const uint32_t k[] = {
        0x8006AAF0u, /* wood batch setup: a0=stream, ra=material desc */
        0x8006AB58u, /* fallthrough past flag gates (accepted batch) */
        0x8006ACE0u, /* after lw s5,96(ra) — helper bound */
        0x8006A52Cu, /* wood emit entry */
        0x8006A608u, /* wood jalr emit — also GTE OTZ/SXY via wood_ot */
        0x8006AE90u, /* alt helper — expect 0 in ND */
        0x8006AD20u, /* wood batch epilogue / next-opcode hub */
        0x80044580u, /* MeshDraw post-RTPT — compare SX band */
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        pc_probe_add_pc(k[i]);
}

/* Resolve double-buffered OT base: *(*(0x8008D2AC) + 0x18C8 + (*(+0xC)<<2)). */
static void pc_probe_ot_context(CPUState *cpu, uint32_t v0_ot,
                                uint32_t *mode_out, uint32_t *depth_out,
                                uint32_t *ot_base_out, uint32_t *ot_index_out)
{
    uint32_t mode = 0, depth = 0, ot_base = 0, ot_index = 0xFFFFFFFFu;
    if (cpu) {
        uint32_t gp = cpu->gpr[28];
        mode = psx_read_word(gp + 0x4D4u);
        /* Depth countdown half lives at gp+0x4D8 (signed). */
        depth = (uint32_t)(int32_t)(int16_t)(psx_read_word(gp + 0x4D8u) & 0xFFFFu);
        uint32_t game = psx_read_word(0x8008D2ACu);
        if (game) {
            uint32_t buf = psx_read_word(game + 0xCu);
            ot_base = psx_read_word(game + (buf << 2) + 0x18C8u);
            if (ot_base && v0_ot) {
                int32_t delta = (int32_t)(v0_ot - ot_base);
                if ((delta & 3) == 0 && delta >= 0 && delta < (1 << 20))
                    ot_index = (uint32_t)(delta >> 2);
            }
        }
    }
    if (mode_out) *mode_out = mode;
    if (depth_out) *depth_out = depth;
    if (ot_base_out) *ot_base_out = ot_base;
    if (ot_index_out) *ot_index_out = ot_index;
}

static inline void pc_probe_observe(uint32_t block_leader_phys)
{
    if (!s_pc_probe_armed || s_pc_probe_n <= 0) return;
    extern CPUState *debug_cpu_ptr;
    CPUState *cpu = debug_cpu_ptr;
    for (int i = 0; i < s_pc_probe_n; i++) {
        if (s_pc_probe[i].pc != block_leader_phys) continue;
        PcProbeSlot *s = &s_pc_probe[i];
        s->count++;
        if (cpu) {
            uint32_t t0 = cpu->gpr[8];
            uint32_t fp = cpu->gpr[30];
            uint32_t v0 = cpu->gpr[2];
            uint32_t a1 = cpu->gpr[5];
            uint32_t a2 = cpu->gpr[6];
            uint32_t t7 = cpu->gpr[15];
            uint32_t otz = cpu->gte_data[7] & 0xFFFFu;
            uint32_t mode = 0, depth = 0, ot_base = 0, ot_index = 0xFFFFFFFFu;
            /* Wood: a2 is stack-table value (often prim-ish); OT slot ≈ ot_base+OTZ.
             * Digit GT4 AddPrim @23180: $t7 = OT slot. Glow @53EB8: $a1 = OT head. */
            int wood_ot = (block_leader_phys == 0x0006A608u ||
                           block_leader_phys == 0x0006A600u);
            int digit_ot = (block_leader_phys == 0x00023094u);
            int glow_ot = (block_leader_phys == 0x00053E10u);
            int digit_rain_ot = (block_leader_phys == 0x0006AE34u);
            int dispatch = (block_leader_phys == 0x000440A0u);
            uint32_t ot_ptr = 0;
            if (digit_ot)
                ot_ptr = t7; /* caller-supplied OT slot */
            else if (digit_rain_ot)
                ot_ptr = cpu->gpr[11]; /* $t3 OT slot from MAC0>>17 helper */
            else if (glow_ot) {
                /* OT head at game+0x147C (same as AddPrim a few insns later). */
                uint32_t game = psx_read_word(0x8008D2ACu);
                ot_ptr = game ? psx_read_word(game + 0x147Cu) : 0;
            } else if (block_leader_phys == 0x00044580u ||
                       block_leader_phys == 0x000441A8u)
                ot_ptr = v0; /* dispatch result / OtFar v0 */
            else if (wood_ot)
                ot_ptr = 0; /* resolve via OTZ after ot_base known */
            else
                ot_ptr = fp ? fp : v0;
            pc_probe_ot_context(cpu, ot_ptr, &mode, &depth, &ot_base, &ot_index);
            if (wood_ot) {
                /* WoodEmit jalr $s5 @0x8006A608: MAC0 already swc2'd to 44($at);
                 * OT base at 56($at) from model+228. Index = MAC0>>17 (== OTZ>>5). */
                uint32_t at = cpu->gpr[1];
                uint32_t mac0 = at ? psx_read_word(at + 44u) : 0;
                uint32_t wood_base = at ? psx_read_word(at + 56u) : 0;
                uint32_t widx = mac0 >> 17;
                if (!widx && otz)
                    widx = otz >> 5;
                ot_base = wood_base;
                ot_index = widx;
                ot_ptr = wood_base ? (wood_base + (widx << 2)) : 0;
                t0 = ot_ptr;           /* actual AddPrim OT slot */
                depth = otz;
                v0 = cpu->gpr[21];     /* $s5 helper */
                mode = wood_base;
                if (wood_base)
                    psx_nd_note_wood_batch_ot(wood_base);
            } else if (digit_ot) {
                t0 = t7;
                v0 = ot_index;
            } else if (glow_ot) {
                t0 = ot_ptr;
                v0 = ot_index;
                depth = a2; /* prim ptr */
            } else if (dispatch) {
                /* Fresh GP mode/depth before leaf runs; v0 not OT yet. */
                t0 = mode;
                v0 = depth;
            } else if (block_leader_phys == 0x000444ECu) {
                /* MeshDraw entry: expose caller $ra (no jal-site in EXE). */
                t0 = cpu->gpr[31];
                v0 = cpu->gpr[2]; /* entry gate */
            } else if (block_leader_phys == 0x00044DA4u) {
                /* PolyG4 emit: sp+0x40/0x41 hold computed shade bytes. */
                uint32_t sp = cpu->gpr[29];
                t0 = psx_read_word(sp + 0x40u) & 0xFFFFu;
                v0 = fp; /* OT link ptr */
            } else if (block_leader_phys == 0x0006A52Cu) {
                /* Wood emit entry: $t9=face list, $at=ctx, $s5=emit helper. */
                t0 = cpu->gpr[25]; /* t9 */
                v0 = cpu->gpr[1];  /* at */
                depth = cpu->gpr[21]; /* s5 */
            } else if (block_leader_phys == 0x0006ACE0u) {
                /* Post helper-select: $s5 set, $a2=model.
                 * ra may be clobbered by pre-emit jalr; trust s5 + model. */
                t0 = cpu->gpr[21]; /* s5 helper */
                v0 = cpu->gpr[6];  /* a2 model */
                if (v0) {
                    mode = psx_read_word(v0 + 184u); /* flags */
                    /* face-list ptr @a2+200; depth = first face word */
                    uint32_t faces = psx_read_word(v0 + 200u);
                    depth = faces ? psx_read_word(faces) : 0;
                    ot_base = faces;
                    ot_index = faces ? psx_read_word(faces + 4u) : 0;
                }
            } else if (block_leader_phys == 0x0006AAF0u) {
                /* Wood batch setup: $a0 stream, $ra = model (helpers at +92).
                 * mode=flags@+184, depth=helper@+96, ot_base=name@+8,
                 * ot_index=batch OT @+228 (live; model RAM is reused later). */
                t0 = cpu->gpr[4];  /* a0 */
                v0 = cpu->gpr[31]; /* model */
                if (v0) {
                    mode = psx_read_word(v0 + 184u);  /* draw flags */
                    depth = psx_read_word(v0 + 96u);  /* s5 helper */
                    ot_base = psx_read_word(v0 + 8u); /* name/tag */
                    ot_index = psx_read_word(v0 + 228u); /* batch OT */
                    if (ot_index)
                        psx_nd_note_wood_batch_ot_tagged(ot_index, ot_base);
                }
            } else if (block_leader_phys == 0x0006AB58u) {
                /* Fallthrough past flag bne — batch accepted. */
                t0 = cpu->gpr[6]; /* a2 */
                v0 = t0 ? psx_read_word(t0 + 184u) : 0;
                depth = cpu->gpr[31]; /* desc */
            } else if (block_leader_phys == 0x00044580u) {
                /* MeshDraw post-dispatch: also pack GTE max SX into mode. */
                {
                    int32_t sx_max = -0x8000;
                    for (int si = 12; si <= 14; si++) {
                        int32_t sx = (int32_t)(int16_t)(cpu->gte_data[si] & 0xFFFFu);
                        if (sx > sx_max) sx_max = sx;
                    }
                    mode = (uint32_t)sx_max;
                }
            } else if (block_leader_phys == 0x000444E8u) {
                /* True MeshDraw entry: lh gp+0x4DC → v0 gate (observe pre-lh). */
                t0 = cpu->gpr[28]; /* gp */
                v0 = cpu->gpr[2];
            } else if (block_leader_phys == 0x00069BB0u) {
                /* Sibling matrix/OT setup entry: a1=ctx (s4+360), a0=mesh.
                 * ot_index = preferred wood batch OT cache.
                 * mode = CODE model+228, depth = GLOW model+228 when those
                 * ND intro models are resident (stable addrs from batch stream). */
                t0 = cpu->gpr[5]; /* a1 */
                v0 = cpu->gpr[4]; /* a0 */
                if (t0) {
                    uint32_t main_ot = psx_read_word(t0 + 244u);
                    ot_base = main_ot;
                    /* s4+0xb4 = scene OT bump near WoodEmit batch pools. */
                    if (t0 > 0xB4u)
                        psx_nd_note_sibling_ot_hint(psx_read_word(t0 - 0xB4u));
                }
                ot_index = psx_nd_wood_batch_ot();
                {
                    /* ND intro digit/glow models — confirmed stable during rain. */
                    const uint32_t code_m = 0x800FF390u;
                    const uint32_t glow_m = 0x800FF294u;
                    uint32_t code_tag = psx_read_word(code_m + 8u);
                    uint32_t glow_tag = psx_read_word(glow_m + 8u);
                    mode = (code_tag == 0x45444F43u) ? psx_read_word(code_m + 228u) : 0;
                    depth = (glow_tag == 0x574F4C47u) ? psx_read_word(glow_m + 228u) : 0;
                    if (mode)
                        psx_nd_note_wood_batch_ot_tagged(mode, 0x45444F43u);
                    else if (depth)
                        psx_nd_note_wood_batch_ot_tagged(depth, 0x574F4C47u);
                }
            } else if (block_leader_phys == 0x00069C34u ||
                       block_leader_phys == 0x00069CC4u) {
                /* Face-loop jal / entry: a3 = OT origin (last slot). */
                t0 = cpu->gpr[7]; /* a3 */
                v0 = cpu->gpr[5]; /* a1 */
                mode = cpu->gpr[4]; /* a0 mesh */
                depth = t0;
                ot_base = (t0 >= 4092u) ? (t0 - 4092u) : 0;
            } else if (digit_rain_ot) {
                /* NdIntroDigitRainCode36: $t3 = OT slot; expose vs game ot_base. */
                t0 = ot_ptr;
                v0 = ot_index;
                depth = otz;
            }
            s->last_t0 = t0;
            s->last_fp = fp;
            s->last_v0 = v0;
            s->last_mode = mode;
            s->last_depth = depth;
            s->last_ot_base = ot_base;
            s->last_ot_index = ot_index;
            s->last_frame = (uint32_t)s_frame_count;
            if (t0 == 0) s->t0_zero++;
            else s->t0_nonzero++;
            if (s_pc_probe_sample_n < s_pc_probe_sample_max &&
                s_pc_probe_sample_n < PC_PROBE_SAMPLE_CAP) {
                PcProbeSample *sm = &s_pc_probe_samples[s_pc_probe_sample_n++];
                sm->pc = block_leader_phys;
                sm->frame = (uint32_t)s_frame_count;
                sm->t0 = t0;
                sm->fp = fp;
                sm->v0 = v0;
                sm->mode = mode;
                sm->depth = depth;
                sm->ot_base = ot_base;
                sm->ot_index = ot_index;
            }
        }
        return;
    }
}

/* Exported per-basic-block-leader cycle observer. Emitted by the recompiler at
 * EVERY compiled block leader (under #ifndef PSX_NO_DEBUG_TOOLS, so prod builds
 * emit nothing — zero overhead) so native's cycle observation matches Beetle's
 * (which samples before every instruction). This lets cyc_watch anchor ANY
 * block-leader PC — interior loop tops, prologue exits — not just function
 * entries, which is required for a clean, KNOWN-instruction-sequence ruler.
 * Disarmed cost = one volatile load + return (see cyc_watch_observe). */
void debug_server_cyc_observe(uint32_t block_leader_phys) {
#ifdef PSX_NO_DEBUG_TOOLS
    (void)block_leader_phys;
    return;
#else
    if (s_fmv_quiet) return;
    uint32_t phys = block_leader_phys & 0x1FFFFFFFu;
    /* ND debug: PSX_ND_WOOD_CLEAR80=1 clears model+184 bit0x80 at the flag-load
     * leader so 0x5CF/'cras' models enter NdIntroWoodBatchSetup's textured path.
     * Fires before lw v1,184(a2) @ AB3C. Default-off. */
    if (phys == 0x0006AB3Cu) {
        static int s_clear80 = -1;
        if (s_clear80 < 0) {
            const char *e = getenv("PSX_ND_WOOD_CLEAR80");
            s_clear80 = (e && *e && *e != '0') ? 1 : 0;
            if (s_clear80)
                fprintf(stdout, "psxrecomp: PSX_ND_WOOD_CLEAR80 enabled\n");
        }
        if (s_clear80 && debug_cpu_ptr) {
            uint32_t a2 = debug_cpu_ptr->gpr[6];
            if (a2) {
                uint32_t fl = psx_read_word(a2 + 184u);
                if (fl & 0x80u)
                    psx_write_word(a2 + 184u, fl & ~0x80u);
            }
        }
    }
    /* Cache WoodEmit batch OT at WoodBatchSetup (runs BEFORE sibling on ND
     * rain frames) and at scratch load. */
    if (phys == 0x0006AAF0u && debug_cpu_ptr) {
        uint32_t model = debug_cpu_ptr->gpr[31]; /* $ra = model */
        if (model) {
            uint32_t tag = psx_read_word(model + 8u);
            uint32_t base = psx_read_word(model + 228u);
            if (base)
                psx_nd_note_wood_batch_ot_tagged(base, tag);
        }
    }
    if ((phys == 0x0006ACFCu || phys == 0x0006AD08u) && debug_cpu_ptr) {
        uint32_t base = 0;
        if (phys == 0x0006ACFCu) {
            uint32_t a2 = debug_cpu_ptr->gpr[6];
            if (a2)
                base = psx_read_word(a2 + 228u);
        } else {
            base = debug_cpu_ptr->gpr[5]; /* a1 after lw model+228 */
        }
        if (base)
            psx_nd_note_wood_batch_ot(base);
    }
    /* ND debug: PSX_ND_SIB_OT_LIFT=<n> adds n*4 to $a3 at sibling face-loop
     * entry 0x80069CC4 (a3 = *(a1+244)+4092 = last OT slot). Sibling PolyFT3
     * right-flap faces use face_hi≈0 → farthest bucket (drawn before additive
     * 0x36 glow). Negative n (e.g. -800) moves them nearer in the 1024-entry OT.
     * Applied once per a3 value. */
    if (phys == 0x00069CC4u && debug_cpu_ptr) {
        static int s_sib_lift = -2; /* -2 unset, 0 disabled, else delta */
        static uint32_t s_sib_lifted_a3 = 0;
        if (s_sib_lift == -2) {
            const char *e = getenv("PSX_ND_SIB_OT_LIFT");
            if (e && *e) {
                s_sib_lift = atoi(e);
                fprintf(stdout, "psxrecomp: PSX_ND_SIB_OT_LIFT=%d\n", s_sib_lift);
            } else {
                s_sib_lift = 0;
            }
        }
        if (s_sib_lift != 0) {
            uint32_t a3 = debug_cpu_ptr->gpr[7];
            if (a3 && a3 != s_sib_lifted_a3) {
                debug_cpu_ptr->gpr[7] = a3 + (uint32_t)(int32_t)(s_sib_lift * 4);
                s_sib_lifted_a3 = debug_cpu_ptr->gpr[7];
            }
        }
    }
    cyc_watch_observe(phys);
    pc_probe_observe(phys);
    /* #2 lockstep comparator: per-basic-block compiled-vs-interp check. Self-gates
     * on the armed frame window; ~free (one branch) when disarmed. */
    { extern void ls_at_leader(uint32_t, CPUState*); extern CPUState *debug_cpu_ptr;
      ls_at_leader(phys, debug_cpu_ptr); }
#endif
}

void debug_server_trace_dispatch(uint32_t func_addr) {
#ifdef PSX_NO_DEBUG_TOOLS
    (void)func_addr;
    return;
#endif
    if (s_fmv_quiet) return;
    ls_suppress_begin();
    /* cyc_watch: compiled-dispatch path. func_addr is already the physical
     * (normalized) block leader. Sampled before the block runs. */
    cyc_watch_observe(func_addr & 0x1FFFFFFFu);

    card_mgr_trace_record(func_addr, 1);

    {
        uint32_t vphys = func_addr & 0x1FFFFFFFu;
        if ((vphys == 0xA0u || vphys == 0xB0u || vphys == 0xC0u) && debug_cpu_ptr) {
            psx_bioscall_record(vphys, debug_cpu_ptr->gpr[9], 0,
                                debug_cpu_ptr->gpr[4], debug_cpu_ptr->gpr[5],
                                debug_cpu_ptr->gpr[6], debug_cpu_ptr->gpr[7],
                                debug_cpu_ptr->gpr[31]);
        }
        /* Event/thread-op stream (ChatGPT-conferred blocked-main-thread hunt,
         * MMX6 cutscene->gameplay freeze). The recompiler resolves B0 event calls
         * to DIRECT compiled-function calls, so they bypass the 0xB0 vector above
         * (the vector ring stays empty). But the event functions ARE dispatched as
         * compiled funcs (they appear in dispatch_tail), so capture them HERE keyed
         * on their SCPH1001 RAM entry addresses (from B0_table @ 0x874), recorded
         * with a synthetic table_base=0xB0 + the real B0 index so bioscall_dump
         * surfaces the full OpenEvent/WaitEvent/DeliverEvent/EnableEvent/... stream.
         * in_exception distinguishes IRQ-context DeliverEvent from main-thread calls. */
        else if (debug_cpu_ptr && g_event_hook_armed) {
            int evi = -1;
            switch (vphys) {
                case 0x1B44u: evi = 0x07; break; /* DeliverEvent(class,spec) */
                case 0x1D8Cu: evi = 0x08; break; /* OpenEvent(class,spec,mode,func) */
                case 0x1E1Cu: evi = 0x09; break; /* CloseEvent(event) */
                case 0x1E44u: evi = 0x0A; break; /* WaitEvent(event) */
                case 0x1EC8u: evi = 0x0B; break; /* TestEvent(event) */
                case 0x1F10u: evi = 0x0C; break; /* EnableEvent(event) */
                case 0x1F4Cu: evi = 0x0D; break; /* DisableEvent(event) */
                case 0x20D4u: evi = 0x10; break; /* ChangeTh(pcb,tcb) */
                default: break;
            }
            if (evi >= 0) {
                psx_bioscall_record(0xB0u, (uint32_t)evi, vphys,
                                    debug_cpu_ptr->gpr[4], debug_cpu_ptr->gpr[5],
                                    debug_cpu_ptr->gpr[6], debug_cpu_ptr->gpr[7],
                                    debug_cpu_ptr->gpr[31]);
            }
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
    ls_suppress_end();
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
    extern uint64_t g_dirty_ram_native_handoffs;
    extern uint32_t dirty_ram_get_bitmap(void);
    extern uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);
    extern uint32_t dirty_ram_get_bitmap_word_count(void);
    extern uint32_t dirty_ram_text_modified_bitmap_word(uint32_t word_index);
    extern uint32_t dirty_ram_text_diverged_bitmap_word(uint32_t word_index);
    extern void dirty_ram_text_exact_mismatch_stats(uint64_t *count,
                                                     uint32_t out[5]);
    (void)json;

    char buf[32 * 1024];
    int n = snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"ok\":true,\"blocks_run\":%llu,"
             "\"insns_run\":%llu,\"aborts\":%llu,"
             "\"guard_yields\":%llu,\"native_handoffs\":%llu,"
             "\"dirty_bitmap\":\"0x%08X\",\"per_pc\":[",
             id,
             (unsigned long long)g_dirty_ram_blocks_run,
             (unsigned long long)g_dirty_ram_insns_run,
             (unsigned long long)g_dirty_ram_aborts,
             (unsigned long long)g_dirty_ram_guard_yields,
             (unsigned long long)g_dirty_ram_native_handoffs,
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
        /* Reserve enough tail room for all bitmap/guard diagnostics below. */
        if (n >= (int)sizeof(buf) - 2048) break;
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
    uint64_t exact_mismatches = 0;
    uint32_t exact_last[5] = {0};
    dirty_ram_text_exact_mismatch_stats(&exact_mismatches, exact_last);
    n += snprintf(buf + n, sizeof(buf) - n,
                  "],\"text_native_blocked\":%llu,"
                  "\"text_diverged_pages\":%u,"
                  "\"text_exact_mismatches\":%llu,"
                  "\"text_exact_last_range\":\"0x%08X\","
                  "\"text_exact_last_len\":%u,"
                  "\"text_exact_last_mismatch\":\"0x%08X\","
                  "\"text_exact_last_live\":%u,"
                  "\"text_exact_last_ref\":%u,"
                  "\"text_modified_bitmap_words\":[",
                  (unsigned long long)dirty_ram_text_native_blocked(),
                  (unsigned)dirty_ram_text_diverged_pages(),
                  (unsigned long long)exact_mismatches,
                  (unsigned)exact_last[0], (unsigned)exact_last[1],
                  (unsigned)exact_last[2], (unsigned)exact_last[3],
                  (unsigned)exact_last[4]);
    for (uint32_t i = 0; i < word_count; i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s\"0x%08X\"",
                      i == 0 ? "" : ",",
                      (unsigned)dirty_ram_text_modified_bitmap_word(i));
        if (n >= (int)sizeof(buf) - 64) break;
    }
    n += snprintf(buf + n, sizeof(buf) - n,
                  "],\"text_diverged_bitmap_words\":[");
    for (uint32_t i = 0; i < word_count; i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s\"0x%08X\"",
                      i == 0 ? "" : ",",
                      (unsigned)dirty_ram_text_diverged_bitmap_word(i));
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
                        "\"at\":\"0x%08X\",\"k0\":\"0x%08X\",\"k1\":\"0x%08X\","
                        "\"current_tcb\":\"0x%08X\",\"task_ptr\":\"0x%08X\","
                        "\"task_mode\":\"0x%08X\",\"task_submode\":\"0x%08X\","
                        "\"frame\":%u,\"transferred\":%u}",
                        emitted == 0 ? "" : ",",
                        (unsigned long long)e->seq,
                        e->pc, e->insn, e->next_pc, e->target,
                        e->before_s0, e->after_s0, e->sp, e->ra,
                        e->v0, e->v1, e->a0, e->a1, e->a2, e->a3,
                        e->t0, e->t1, e->t2,
                        e->at, e->k0, e->k1,
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
#include "starvation_ring.h"
#include "bios_hle.h"
#include "psx_bios_image.h"
#include "parity_trace.h"
#include "device_trace.h"

/* ---- parity_dump / parity_ctl: general two-process control-flow parity ring.
 * Mirrors the IDENTICAL command on psx-beetle so tools/parity_diff.py can pull
 * both timelines and align by logical sequence (PRINCIPLES.md first-divergence). */
/* Two rows have the same watched-STATE iff their watch words + epc + tcb_state
 * match (pc/ra/sp ignored). Used by the `transitions` dump filter to collapse
 * runs of identical-state dispatch rows into one (with a `reps` count), so a
 * 130k-row cutscene dump trims to the handful of rows where state changed. */
static int parity_same_state(const ParityEntry *a, const ParityEntry *b)
{
    if (a->kind != b->kind) return 0;
    if (a->epc != b->epc || a->tcb_state != b->tcb_state) return 0;
    for (int k = 0; k < PARITY_WATCH_MAX; k++)
        if (a->watch[k] != b->watch[k]) return 0;
    return 1;
}

static void handle_parity_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 131072);
    /* transitions=1: emit only state-change rows (collapse identical-state dispatch
     * runs), each with reps=run length. The decisive trim for cross-process diff. */
    int trans = json_get_int(json, "transitions", 0);
    if (count < 1) count = 1;
    if (count > 131072) count = 131072;
    ParityEntry *e = (ParityEntry *)malloc(sizeof(ParityEntry) * (size_t)count);
    if (!e) { send_err(id, "oom"); return; }
    uint32_t got = parity_trace_get(e, (uint32_t)count);
    const size_t BUF_SZ = 12 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ);
    if (!out) { free(e); send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
        "{\"id\":%d,\"ok\":true,\"total\":%llu,\"armed\":%d,\"frozen\":%d,\"count\":%u,\"entries\":[",
        id, (unsigned long long)parity_trace_total(), parity_trace_is_armed(),
        parity_trace_is_frozen(), got);
    uint32_t run = 1, emitted = 0;
    for (uint32_t i = 0; i < got; i++) {
        if (pos > BUF_SZ - 2048) break;
        /* In transitions mode, advance through identical-state dispatch rows until
         * a boundary (next row differs, is a control event, or end of buffer). */
        if (trans) {
            int boundary = (i + 1 >= got) || !parity_same_state(&e[i], &e[i + 1])
                           || e[i].kind != PARITY_KIND_DISPATCH;
            if (!boundary) { run++; continue; }
        }
        ParityEntry *r = &e[i];
        pos += snprintf(out + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"frame\":%u,\"cycle\":%llu,\"reps\":%u,\"kind\":\"%s\",\"cur_tcb\":\"0x%08X\","
            "\"pc\":\"0x%08X\",\"ra\":\"0x%08X\",\"sp\":\"0x%08X\",\"epc\":\"0x%08X\","
            "\"state\":\"0x%08X\",\"target\":\"0x%08X\","
            "\"w\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"],"
            "\"wwpc\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"],"
            "\"wwcy\":[%llu,%llu,%llu,%llu,%llu,%llu],"
            "\"wwf\":[%u,%u,%u,%u,%u,%u],"
            "\"wwt\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
            emitted ? "," : "", (unsigned long long)r->seq, r->frame,
            (unsigned long long)r->cycle, trans ? run : 1u, parity_kind_str(r->kind),
            r->current_tcb, r->pc, r->ra, r->sp, r->epc, r->tcb_state, r->target,
            r->watch[0], r->watch[1], r->watch[2], r->watch[3], r->watch[4], r->watch[5],
            r->watch_wpc[0], r->watch_wpc[1], r->watch_wpc[2], r->watch_wpc[3], r->watch_wpc[4], r->watch_wpc[5],
            (unsigned long long)r->watch_wcycle[0], (unsigned long long)r->watch_wcycle[1],
            (unsigned long long)r->watch_wcycle[2], (unsigned long long)r->watch_wcycle[3],
            (unsigned long long)r->watch_wcycle[4], (unsigned long long)r->watch_wcycle[5],
            r->watch_wframe[0], r->watch_wframe[1], r->watch_wframe[2], r->watch_wframe[3], r->watch_wframe[4], r->watch_wframe[5],
            r->watch_wtcb[0], r->watch_wtcb[1], r->watch_wtcb[2], r->watch_wtcb[3], r->watch_wtcb[4], r->watch_wtcb[5]);
        emitted++; run = 1;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "],\"emitted\":%u}\n", emitted);
    debug_server_send_line(out); free(out); free(e);
}

static void handle_parity_ctl(int id, const char *json)
{
    if (json_get_int(json, "reset", 0)) parity_trace_reset();
    long armv = json_get_int(json, "arm", -1);
    if (armv == 0 || armv == 1) parity_trace_arm((int)armv);
    char buf[160];
    snprintf(buf, sizeof buf,
        "{\"id\":%d,\"ok\":true,\"armed\":%d,\"frozen\":%d,\"total\":%llu}\n",
        id, parity_trace_is_armed(), parity_trace_is_frozen(),
        (unsigned long long)parity_trace_total());
    debug_server_send_line(buf);
}

/* ---- devtrace_dump / devtrace_ctl: general two-process device-event ring.
 * Identical command + JSON on psx-beetle so tools/devtrace_diff.py pulls both
 * device-IRQ timelines and aligns them by guest cycle. Optional filters:
 *   count           max newest events to return (default 65536, cap = ring)
 *   cyc_lo / cyc_hi half-open guest-cycle window (decimal) — slice the load
 *   src             only this I_STAT source bit (0..10); omit/negative = all */
static void handle_devtrace_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 65536);
    if (count < 1) count = 1;
    if (count > (1 << 20)) count = (1 << 20);
    char buf[32];
    uint64_t cyc_lo = 0, cyc_hi = ~0ull;
    if (json_get_str(json, "cyc_lo", buf, sizeof buf)) cyc_lo = strtoull(buf, NULL, 0);
    if (json_get_str(json, "cyc_hi", buf, sizeof buf)) cyc_hi = strtoull(buf, NULL, 0);
    int src = json_get_int(json, "src", -1);

    DevEvent *e = (DevEvent *)malloc(sizeof(DevEvent) * (size_t)count);
    if (!e) { send_err(id, "oom"); return; }
    uint32_t got = device_trace_get(e, (uint32_t)count);
    const size_t BUF_SZ = 12 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ);
    if (!out) { free(e); send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
        "{\"id\":%d,\"ok\":true,\"total\":%llu,\"armed\":%d,\"count\":%u,\"events\":[",
        id, (unsigned long long)device_trace_total(), device_trace_is_armed(), got);
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < got; i++) {
        if (pos > BUF_SZ - 256) break;
        DevEvent *r = &e[i];
        if (r->cycle < cyc_lo || r->cycle >= cyc_hi) continue;
        if (src >= 0 && (int)r->source != src) continue;
        pos += snprintf(out + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"cycle\":%llu,\"frame\":%u,\"srcn\":%u,\"src\":\"%s\",\"detail\":%u}",
            emitted ? "," : "", (unsigned long long)r->seq, (unsigned long long)r->cycle,
            r->frame, r->source, device_source_str(r->source), r->detail);
        emitted++;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "],\"emitted\":%u}\n", emitted);
    debug_server_send_line(out); free(out); free(e);
}

static void handle_devtrace_ctl(int id, const char *json)
{
    if (json_get_int(json, "reset", 0)) device_trace_reset();
    long armv = json_get_int(json, "arm", -1);
    if (armv == 0 || armv == 1) device_trace_arm((int)armv);
    char buf[128];
    snprintf(buf, sizeof buf,
        "{\"id\":%d,\"ok\":true,\"armed\":%d,\"total\":%llu}\n",
        id, device_trace_is_armed(), (unsigned long long)device_trace_total());
    debug_server_send_line(buf);
}

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
                        "\"s3\":\"0x%08X\",\"sp\":\"0x%08X\",\"frame\":%u}",
                        emitted == 0 ? "" : ",",
                        (unsigned long long)seq,
                        e->target, e->ra, e->a0, e->a1, e->a2, e->a3,
                        e->s3, e->sp, e->frame);
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

/* bios_info — which recompiled BIOS this build links, and whether the
 * loaded ROM matches it. Everything static comes from psx_bios_image (the
 * generated dispatch's self-description, couriered from the BIOS profile);
 * `loaded_wordsum` is memory.c's checksum of the ROM actually loaded, and
 * `match` compares the two — with the launch identity gate in place a
 * running process should always report match:1.
 *   {"cmd":"bios_info"} */
static void handle_bios_info(int id, const char *json)
{
    (void)json;
    extern uint32_t memory_get_bios_checksum(void);
    char out[1024];   /* image_id + full sha256 + ~15 numeric fields */
    snprintf(out, sizeof out,
             "{\"id\":%d,\"ok\":true,\"image_id\":\"%s\",\"sha256\":\"%s\","
             "\"crc32\":\"%08X\",\"size\":%u,\"bundled\":%d,"
             "\"kbless_ram_lo\":\"0x%X\",\"kbless_ram_hi\":\"0x%X\","
             "\"kbless_rom_off\":\"0x%X\",\"shell_entry_phys\":\"0x%X\","
             "\"deliver_event_ret\":\"0x%X\","
             "\"image_wordsum\":\"%08X\",\"loaded_wordsum\":\"%08X\","
             "\"match\":%d}\n",
             id, psx_bios_image.image_id, psx_bios_image.image_sha256,
             psx_bios_image.image_crc32, psx_bios_image.image_size,
             psx_bios_image.image_bundled,
             psx_bios_image.kbless_ram_lo, psx_bios_image.kbless_ram_hi,
             psx_bios_image.kbless_rom_off, psx_bios_image.shell_entry_phys,
             psx_bios_image.deliver_event_ret,
             psx_bios_image.image_wordsum, memory_get_bios_checksum(),
             psx_bios_image.image_wordsum == memory_get_bios_checksum());
    debug_server_send_line(out);
}

/* hle_dump — query the BIOS-HLE tier's always-on call ring (bios_hle.c).
 * Records every A0/B0/C0 vector dispatch seen by the HLE hook with its
 * routing decision (0 = fell through to LLE, 1 = serviced in HLE, 2 = boot
 * shell-skip), args, result, and guest cycle. Empty when the tier is off
 * (LLE mode installs no hook; the bioscall ring covers that case).
 *   {"cmd":"hle_dump","tail":N}          last N ring entries
 *   {"cmd":"hle_dump"}                   status summary (mode + totals)
 * Optional filters: "fn":N (t1 index), "route":0|1|2. */
static void handle_hle_dump(int id, const char *json)
{
    int  tail       = json_get_int(json, "tail", 0);
    long want_fn    = json_get_int(json, "fn", -1);
    long want_route = json_get_int(json, "route", -1);
    uint64_t total = psx_hle_ring_seq();
    if (tail <= 0) {
        char out[256];
        snprintf(out, sizeof out,
                 "{\"id\":%d,\"ok\":true,\"backend\":\"%s\",\"boot_skip\":%d,"
                 "\"boot_turbo_active\":%d,\"total\":%llu}\n",
                 id, psx_bios_hle_backend_name(),
                 psx_bios_hle_boot_skip_enabled(),
                 psx_bios_hle_boot_turbo_active(),
                 (unsigned long long)total);
        debug_server_send_line(out);
        return;
    }
    if (tail > (int)PSX_HLE_RING_CAP) tail = PSX_HLE_RING_CAP;
    uint64_t avail = (total < PSX_HLE_RING_CAP) ? total : PSX_HLE_RING_CAP;
    if ((uint64_t)tail > avail) tail = (int)avail;
    /* "since": absolute start seq (paging anchor). Overrides tail's implicit
     * start so a fast-moving ring can be read from a known point instead of
     * hoping the tail hasn't slid past it. Clamped to the retained window. */
    long long since = json_get_int(json, "since", -1);
    uint64_t start = total - (uint64_t)tail;
    if (since >= 0) {
        start = (uint64_t)since;
        if (start + PSX_HLE_RING_CAP < total) start = total - PSX_HLE_RING_CAP;
    }
    const size_t BUF_SZ = 8 * 1024 * 1024;
    char *out = (char *)malloc(BUF_SZ); if (!out) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"start\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)start);
    int first = 1; int truncated = 0;
    for (uint64_t s = start; s < total; s++) {
        const PsxHleCallEntry *e = psx_hle_ring_entry(s);
        if (!e) continue;
        if (want_fn >= 0 && (long)e->fn != want_fn) continue;
        if (want_route >= 0 && (long)e->route != want_route) continue;
        if (pos > BUF_SZ - 512) { truncated = 1; break; }
        pos += snprintf(out + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"cycle\":%llu,\"cycle_last\":%llu,"
                        "\"vec\":\"0x%X\",\"fn\":\"0x%02X\","
                        "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                        "\"ra\":\"0x%08X\",\"v0\":\"0x%08X\",\"repeat\":%u,"
                        "\"tcb\":\"0x%08X\",\"exc\":%u,\"route\":%u}",
                        first ? "" : ",", (unsigned long long)e->seq,
                        (unsigned long long)e->cycle, (unsigned long long)e->cycle_last,
                        e->vector, e->fn,
                        e->a0, e->a1, e->a2, e->a3, e->ra, e->v0, e->repeat,
                        e->tcb, e->in_exc, e->route);
        first = 0;
    }
    pos += snprintf(out + pos, BUF_SZ - pos, "],\"truncated\":%d}\n", truncated);
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
        case 13: return "fiber_dispatch_exit";        /* fiber's psx_dispatch returned, in_exc==0 */
        case 20: return "syscall3_enter";             /* ChangeThread/RFE syscall, in_exc==0 (switch-eligible) */
        case 24: return "syscall3_enter_in_exc";      /* ChangeThread/RFE syscall, in_exc==1 (forced manual-RFE) */
        case 26: return "fiber_dispatch_exit_in_exc"; /* fiber's psx_dispatch returned, in_exc==1 */
        case 30: return "inexc_switch_escape";        /* guest moved PCB[0] inside handler -> scheduler escape */
        case 31: return "inexc_switch_defer";
        case 32: return "deferred_switch_escape";
        case 33: return "deferred_switch_stale";
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

/* ---- MMIO-READ trace (rtrace) ----
 * The read counterpart to the MMIO write ring. CPU loads are far more
 * voluminous than stores, so this ring is RANGE-FILTERED (default-armed to the
 * CD regs + I_STAT/I_MASK — the device regs the IRQ handler reads to decide
 * whether to ack). This is THE decisive signal for the verifier-vs-lowering
 * classification: what value did 0x2458 actually read from 0x1F801803 (CD
 * IRQ-flag) / I_STAT at exception time. Reuses MmioTraceEntry so each read also
 * snapshots func/pc/ra + i_stat/i_mask at read time. Always-on for the armed
 * ranges from init (Rule: ring buffers capture continuously, probes query). */
static MmioTraceEntry *s_mmio_rtrace = NULL;
static uint64_t s_mmio_rtrace_seq  = 0;
static uint32_t s_mmio_rtrace_head = 0;
#define MMIO_RTRACE_MAX_RANGES 8
static struct { uint32_t lo, hi; } s_mmio_rtrace_ranges[MMIO_RTRACE_MAX_RANGES];
static int s_mmio_rtrace_range_count = 0;

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
        /* A BARE json number is DECIMAL by JSON semantics, but nearly every
         * string consumer here feeds hex_to_u32 / strtoul(base 0|16) under the
         * quoted-hex wire convention. A client that forgets to hex-format an
         * address (python json.dumps of an int emits bare decimal) used to get
         * it parsed AS HEX — 10-digit decimals saturate strtoul to 0xFFFFFFFF
         * and the server silently read junk (burned an evidence chain
         * 2026-07-02). Normalize bare pure-decimal values to "0x%llX" so both
         * hex_to_u32 and base-0 consumers recover the intended value. Bare
         * booleans / 0x-prefixed / hex-lettered values pass through unchanged.
         * (json_get_int does NOT share this path — it scans raw JSON itself.) */
        if (i > 0) {
            int all_dec = 1;
            for (int k = 0; k < i; k++)
                if (out[k] < '0' || out[k] > '9') { all_dec = 0; break; }
            if (all_dec) {
                unsigned long long v = strtoull(out, NULL, 10);
                snprintf(out, (size_t)out_sz, "0x%llX", v);
            }
        }
        return out;
    }
}

static int json_get_int(const char *json, const char *key, int def)
{
    /* Raw scan (NOT via json_get_str): integer fields keep bare-decimal
     * semantics and must not go through the hex normalization above.
     * Quoted numbers are accepted too (base 0 handles "123" and "0x7B"). */
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return def;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') p++;
    if (*p == '-' || (*p >= '0' && *p <= '9'))
        return (int)strtol(p, NULL, 0);
    return def;
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

/* Append a response line to s_resp_buf (the I/O thread sends it). Only active
 * while the emu thread is processing a command; async pushes (e.g. watchpoint
 * events) have no persistent client under the one-command-per-connection
 * protocol and are dropped — matching the prior near-no-op behaviour. */
void debug_server_send_line(const char *json)
{
    if (!s_in_command) return;
    size_t len = strlen(json);
    size_t need = s_resp_len + len + 2;          /* + '\n' + NUL */
    if (need > s_resp_cap) {
        size_t cap = s_resp_cap ? s_resp_cap : 65536;
        while (cap < need) cap *= 2;
        if (cap > (64u * 1024u * 1024u)) {        /* hard cap: drop the overflow */
            s_resp_overflow = 1;
            return;
        }
        char *nb = (char *)realloc(s_resp_buf, cap);
        if (!nb) { s_resp_overflow = 1; return; }
        s_resp_buf = nb; s_resp_cap = cap;
    }
    memcpy(s_resp_buf + s_resp_len, json, len);
    s_resp_len += len;
    s_resp_buf[s_resp_len++] = '\n';
    s_resp_buf[s_resp_len] = '\0';
}

void debug_server_send_fmt(const char *fmt, ...)
{
    /* No fixed-size truncation: lines larger than the stack buffer (e.g. a
     * full gl_coh_ring dump) are heap-formatted at their exact size. A 64KB
     * vsnprintf cap here used to silently truncate big ring responses into
     * unparseable JSON. */
    char buf[65536];
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (need >= 0 && (size_t)need < sizeof(buf)) {
        debug_server_send_line(buf);
        return;
    }
    if (need < 0) return;
    char *big = (char *)malloc((size_t)need + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)need + 1, fmt, ap);
    va_end(ap);
    debug_server_send_line(big);
    free(big);
}

#define send_line  debug_server_send_line
#define send_fmt   debug_server_send_fmt

int debug_server_dirty_break_maybe_pause(uint32_t target, CPUState *cpu)
{
    /* cyc_watch: dirty-RAM interpreter path. `target` is the virtual block
     * leader about to be interpreted; mask to physical and sample before
     * the block runs (same instant as the compiled path). This hook is
     * invoked unconditionally at every interp block entry, so the anchor
     * is observed even when no dirty-break range is set. */
    cyc_watch_observe(target & 0x1FFFFFFFu);

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

/* Layer-1 first-divergence: dump the per-frame write fingerprint ring.
 * Params: count (default 1024), frame_lo / frame_hi (optional inclusive
 * filter). Entries are oldest-first within the window. Diff the wr/pc columns
 * of two runs (native vs interp/oracle): the first frame whose wr or pc differs
 * is the first-divergence frame. Small integer fields only — no large/ragged
 * payload, so it never trips the trace-dump JSON/eviction problems. */
static void handle_frame_fingerprint(int id, const char *json)
{
    int count = json_get_int(json, "count", 1024);
    if (count < 1) count = 1;
    if (count > FP_RING_CAP) count = FP_RING_CAP;
    int flo = json_get_int(json, "frame_lo", -1);
    int fhi = json_get_int(json, "frame_hi", -1);

    uint32_t avail = (s_fp_total < FP_RING_CAP) ? (uint32_t)s_fp_total : FP_RING_CAP;
    uint32_t start = (s_fp_total < FP_RING_CAP) ? 0 : s_fp_head;

    size_t BUF = 512 + (size_t)count * 224;
    char *out = (char *)malloc(BUF);
    if (!out) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)s_fp_total, avail);
    int emitted = 0;
    for (uint32_t i = 0; i < avail && emitted < count; i++) {
        FpEntry *e = &s_fp_ring[(start + i) % FP_RING_CAP];
        if (flo >= 0 && (int)e->frame < flo) continue;
        if (fhi >= 0 && (int)e->frame > fhi) continue;
        pos += snprintf(out + pos, BUF - pos,
                        "%s{\"frame\":%u,\"wr\":\"0x%016llx\",\"pc\":\"0x%016llx\",\"wc\":%llu,"
                        "\"mmio\":\"0x%016llx\",\"mc\":%llu,"
                        "\"sp\":\"0x%016llx\",\"sc\":%llu,\"cyc\":%llu}",
                        emitted ? "," : "", e->frame,
                        (unsigned long long)e->wr_hash,
                        (unsigned long long)e->pc_hash,
                        (unsigned long long)e->wcount,
                        (unsigned long long)e->mmio_hash,
                        (unsigned long long)e->mmio_count,
                        (unsigned long long)e->sp_hash,
                        (unsigned long long)e->sp_count,
                        (unsigned long long)e->cyc);
        emitted++;
    }
    pos += snprintf(out + pos, BUF - pos, "]}\n");
    debug_server_send_line(out);
    free(out);
}

/* Layer-2: arm the frame-gated write recorder for a guest frame. */
static void handle_record_frame(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    s_rec_count = 0; s_rec_overflow = 0;
    s_rec_frame = (f < 0) ? -1 : (int64_t)f;
    send_fmt("{\"id\":%d,\"ok\":true,\"armed_frame\":%lld,\"cur_frame\":%llu}",
             id, (long long)s_rec_frame, (unsigned long long)s_frame_count);
}

static const char *rec_kind_str(uint8_t k)
{
    switch (k) {
        case REC_KIND_RAM_W:  return "ramw";
        case REC_KIND_SP_W:   return "spw";
        case REC_KIND_MMIO_W: return "mmiow";
        case REC_KIND_MMIO_R: return "mmior";
        case REC_KIND_RAM_R:  return "ramr";
        default:              return "?";
    }
}

/* Layer-2: dump the recorded frame's UNIFIED ordered access log (paged). The
 * array index `i` is the true execution order across writes and reads; `kind`
 * classifies each. Optional "kind" param (0..3) filters to one class; default
 * (omitted/-1) emits everything. Diff two runs by `i` — the first differing
 * (kind,addr,val,pc) tuple is the literal first divergent access. */
static void handle_record_frame_dump(int id, const char *json)
{
    int offset = json_get_int(json, "offset", 0);
    int count  = json_get_int(json, "count", 1500);
    int kfilt  = json_get_int(json, "kind", -1);
    if (offset < 0) offset = 0;
    if (count < 1) count = 1;
    if (count > 4000) count = 4000;
    size_t BUF = 512 + (size_t)count * 160;
    char *out = (char *)malloc(BUF);
    if (!out) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF - pos,
        "{\"id\":%d,\"ok\":true,\"frame\":%lld,\"total\":%u,\"overflow\":%u,\"offset\":%d,\"entries\":[",
        id, (long long)s_rec_frame, s_rec_count, s_rec_overflow, offset);
    int emitted = 0;
    for (int i = offset; i < (int)s_rec_count && emitted < count; i++) {
        RecEntry *e = &s_rec_buf[i];
        if (kfilt >= 0 && (int)e->kind != kfilt) continue;
        pos += snprintf(out + pos, BUF - pos,
            "%s{\"i\":%d,\"kind\":\"%s\",\"addr\":\"0x%08X\",\"val\":\"0x%08X\",\"pc\":\"0x%08X\",\"ra\":\"0x%08X\",\"cyc\":%llu}",
            emitted ? "," : "", i, rec_kind_str(e->kind), e->addr, e->val, e->pc, e->ra,
            (unsigned long long)e->cyc);
        emitted++;
    }
    pos += snprintf(out + pos, BUF - pos, "]}\n");
    debug_server_send_line(out);
    free(out);
}

/* Layer-2 companion: dump only the device-register READS from the unified log,
 * preserving their execution-order index `i`. (Thin wrapper over the unified
 * buffer filtered to MMIO reads — retained for the existing read-diff tools.) */
static void handle_record_reads_dump(int id, const char *json)
{
    int offset = json_get_int(json, "offset", 0);
    int count  = json_get_int(json, "count", 1500);
    if (offset < 0) offset = 0;
    if (count < 1) count = 1;
    if (count > 4000) count = 4000;
    size_t BUF = 512 + (size_t)count * 112;
    char *out = (char *)malloc(BUF);
    if (!out) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(out + pos, BUF - pos,
        "{\"id\":%d,\"ok\":true,\"frame\":%lld,\"total\":%u,\"offset\":%d,\"entries\":[",
        id, (long long)s_rec_frame, s_rec_count, offset);
    int emitted = 0;
    int seen = 0;
    for (int i = 0; i < (int)s_rec_count && emitted < count; i++) {
        RecEntry *e = &s_rec_buf[i];
        if (e->kind != REC_KIND_MMIO_R) continue;
        if (seen++ < offset) continue;          /* page over reads only */
        pos += snprintf(out + pos, BUF - pos,
            "%s{\"i\":%d,\"addr\":\"0x%08X\",\"val\":\"0x%08X\",\"pc\":\"0x%08X\"}",
            emitted ? "," : "", i, e->addr, e->val, e->pc);
        emitted++;
    }
    pos += snprintf(out + pos, BUF - pos, "]}\n");
    debug_server_send_line(out);
    free(out);
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
    /* Effectively the entire live main RAM in one shot.  Response uses a heap-
     * sized envelope so we don't truncate. */
    if ((uint32_t)len > g_psx_ram_size) len = (int)g_psx_ram_size;

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

/* geom_correction — is [video] geometry_correction / perspective_texturing
 * actually doing anything on THIS title?
 *
 * Both enhancements are silent no-ops on content they cannot prove is
 * projected geometry: a vertex whose sub-pixel fraction was never cached, or a
 * packet whose position words lack full GTE projection provenance, simply draws
 * the faithful way. So "enabled" alone tells you nothing — these counters are
 * how you tell an engaged correction from an inert one. Both are free-running
 * totals; sample twice and diff for a per-window rate. */
static void handle_geom_correction(int id, const char *json)
{
    (void)json;
    /* The miss split is the diagnostic that matters. The position table is
     * exact (one slot per reachable SXY, no hashing), so "unrecorded" means no
     * projection was EVER cached at that screen position — a real coverage gap
     * in the tracking, not a cache artifact. A high unrecorded share means the
     * game's vertex path never reaches us in a matchable form, which only
     * full value propagation can fix; a high ambiguous share instead means
     * distinct vertices are landing on the same pixel. */
    uint32_t lookups = 0, hits = 0, unrec = 0, ambig = 0;
    gte_geometry_correction_stats(&lookups, &hits, &unrec, &ambig);
    /* PGXP dataflow census (per-vertex): the primary provenance source.
     * dataflow_hit is the number that had to move — the G1.9 gate is a
     * dataflow-hit share dramatically above the 5.2% the position table
     * measured on its own. value_mismatch counts shadows that were present
     * but described a different word (stale = provenance hole to hunt). */
    PGXPStats ps;
    pgxp_get_stats(&ps);
    /* Perspective arming, with its real denominator. perspective_triangles on
     * its own could only be compared against gp0_draw, which counts untextured
     * primitives that are correctly never armed — so it read as a coverage
     * figure without being one. texcorr.attempts counts exactly the textured
     * triangles that reach the predicate. */
    uint64_t tc_att = 0, tc_arm = 0, tc_off = 0, tc_nosrc = 0, tc_noz = 0;
    gpu_texture_correction_stats(&tc_att, &tc_arm, &tc_off, &tc_nosrc, &tc_noz);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"geometry_correction\":%d,"
             "\"geometry_vertex_hits\":%u,"
             "\"perspective_triangles\":%u,"
             "\"texcorr\":{\"attempts\":%llu,\"armed\":%llu,"
             "\"no_correction\":%llu,\"no_source\":%llu,\"no_depth\":%llu},"
             "\"lookups\":%u,\"miss_unrecorded\":%u,\"miss_ambiguous\":%u,"
             "\"pgxp\":{\"enabled\":%d,\"cpu_mode\":%d,\"tolerance\":%.3f,"
             "\"lookups\":%llu,\"dataflow_hit\":%llu,\"fallback_hit\":%llu,"
             "\"native\":%llu,\"value_mismatch\":%llu,\"trunc_reject\":%llu,"
             "\"tolerance_reject\":%llu,\"w_valid\":%llu,"
             "\"produced\":%llu,\"swc2_stores\":%llu}}",
             id,
             gte_geometry_correction_enabled(),
             (unsigned)hits,
             (unsigned)gpu_texture_correction_hits(),
             (unsigned long long)tc_att, (unsigned long long)tc_arm,
             (unsigned long long)tc_off, (unsigned long long)tc_nosrc,
             (unsigned long long)tc_noz,
             (unsigned)lookups, (unsigned)unrec, (unsigned)ambig,
             pgxp_enabled(), pgxp_cpu_mode(), (double)pgxp_tolerance(),
             (unsigned long long)ps.lookups,
             (unsigned long long)ps.dataflow_hit,
             (unsigned long long)ps.fallback_hit,
             (unsigned long long)ps.native,
             (unsigned long long)ps.value_mismatch,
             (unsigned long long)ps.trunc_reject,
             (unsigned long long)ps.tolerance_reject,
             (unsigned long long)ps.w_valid,
             (unsigned long long)ps.produced,
             (unsigned long long)ps.swc2_stores);
}

/* pgxp — live-tune the value-propagation engine for one-toggle isolation runs
 * without a rebuild: {"cmd":"pgxp","cpu_mode":0|1,"tolerance":F}. Fields are
 * optional; the reply echoes the resulting state (same shape as
 * geom_correction's "pgxp" object, flattened). */
static void handle_pgxp(int id, const char *json)
{
    /* Live toggles for the one-toggle-at-a-time A/B protocol (ENHANCEMENTS.md
     * G1.6 method rule): same scene, flip one knob, screenshot_hires. */
    int geom = json_get_int(json, "geometry", -1);
    if (geom >= 0)
        gte_geometry_correction_set(geom != 0);
    int tex = json_get_int(json, "texture", -1);
    if (tex >= 0)
        gpu_texture_correction_set(tex != 0);
    if (geom >= 0 && tex < 0) {
        /* keep the engine armed consistently with both flags */
        gpu_texture_correction_set(gpu_texture_correction_enabled());
    }
    int cm = json_get_int(json, "cpu_mode", -1);
    if (cm >= 0)
        pgxp_set_cpu_mode(cm != 0);
    /* tolerance is fractional (sub-pixel), so scan it directly — json_get_int
     * would truncate 0.5 to 0. */
    const char *p = strstr(json, "\"tolerance\"");
    if (p) {
        p += 11;
        while (*p == ' ' || *p == ':' || *p == '"') p++;
        if (*p == '-' || (*p >= '0' && *p <= '9') || *p == '.')
            pgxp_set_tolerance((float)strtod(p, NULL));
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"enabled\":%d,\"cpu_mode\":%d,"
             "\"tolerance\":%.3f,\"suppress\":%u,\"active\":%d}",
             id, pgxp_enabled(), pgxp_cpu_mode(), (double)pgxp_tolerance(),
             (unsigned)pgxp_test_suppress_depth(), pgxp_test_active());
}

static void handle_gpu_state(int id, const char *json)
{
    (void)json;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    uint32_t gpustat = gpu_read_gpustat();
    uint32_t hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0, hr1 = 0, hr2 = 0;
    gpu_get_crtc_debug(&hx1, &hx2, &hy1, &hy2, &hr1, &hr2);

    GpuDrawArea da;
    gpu_get_draw_area(&da);
    uint64_t nop, fill, draw, env, copy;
    gpu_get_gp0_stats(&nop, &fill, &draw, &env, &copy);
    int split_active = 0, split_left_age = 0, split_right_age = 0;
    gpu_vertical_split_debug(&split_active, &split_left_age, &split_right_age);
    GpuWsDebug ws;
    gpu_ws_get_debug(&ws);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"display_x\":%d,\"display_y\":%d,"
             "\"width\":%d,\"height\":%d,"
             "\"depth\":%d,\"depth24\":%d,"
             "\"disabled\":%d,"
             "\"h_display\":[%u,%u],\"v_display\":[%u,%u],"
             "\"hres1\":%u,\"hres2\":%u,"
             "\"gpustat\":\"0x%08X\","
             "\"gp0_writes\":%llu,"
             "\"gp0_nop\":%llu,\"gp0_fill\":%llu,\"gp0_draw\":%llu,\"gp0_env\":%llu,\"gp0_copy\":%llu,"
             "\"draw_area\":[%u,%u,%u,%u],"
             "\"draw_offset\":[%d,%d],"
             "\"vertical_split\":{\"active\":%d,\"left_age\":%d,\"right_age\":%d},"
             "\"ws\":{\"configured\":%d,\"active\":%d,\"game_mode\":%d,"
             "\"present_native_43\":%d,\"x_margin\":%d,"
             "\"activation_margin\":%d,\"squash\":[%d,%d],"
             "\"mode\":%d,\"nw_extra\":%d,"
             "\"cur_frame\":%llu,\"last_tag_frame\":%u,\"last_3d_frame\":%u,"
             "\"gte_verts\":%u,\"last_world3d_frame\":%u,"
             "\"ovh_prims\":%u,\"last_ovh_frame\":%u,"
             "\"auto_ui\":{\"configured\":%d,\"dense\":%d,\"ot_rank\":%u,"
             "\"candidates\":%llu,\"transforms\":%llu},"
             "\"aspect_cone\":{\"calls\":%llu,\"identity_43\":%llu,"
             "\"vanilla_keep\":%llu,\"visible_keep\":%llu,"
             "\"guard_keep\":%llu,\"hysteresis_keep\":%llu,"
             "\"outside_reject\":%llu,\"queue_reject\":%llu,"
             "\"queue_highwater\":[%u,%u,%u]},"
             "\"terrain_angle\":{\"calls\":%llu,\"identity_43\":%llu,"
             "\"max_vanilla\":%u,\"max_widened\":%u}}}",
             id, di.display_x, di.display_y,
             di.width, di.height,
             di.depth24 ? 24 : 15, di.depth24,
             di.disabled,
             hx1, hx2, hy1, hy2, hr1, hr2,
             gpustat,
             (unsigned long long)gpu_get_gp0_count(),
             (unsigned long long)nop, (unsigned long long)fill,
             (unsigned long long)draw, (unsigned long long)env,
             (unsigned long long)copy,
             da.left, da.top, da.right, da.bottom,
             da.offset_x, da.offset_y,
             split_active, split_left_age, split_right_age,
             ws.configured, ws.active, ws.game_mode,
             ws.present_native_43, ws.x_margin, ws.activation_margin,
             ws.xnum, ws.xden,
             ws.mode, ws.nw_extra,
             (unsigned long long)ws.cur_frame, ws.last_tag_frame,
              ws.last_3d_frame, ws.gte_verts, ws.last_world3d_frame,
              ws.ovh_prims, ws.last_ovh_frame,
              ws.auto_ui_squash, ws.auto_ui_dense, ws.auto_ui_ot_rank,
              (unsigned long long)ws.auto_ui_candidates,
              (unsigned long long)ws.auto_ui_transforms,
              (unsigned long long)ws.aspect_cone_calls,
             (unsigned long long)ws.aspect_cone_43_identity,
             (unsigned long long)ws.aspect_cone_vanilla_keep,
             (unsigned long long)ws.aspect_cone_visible_keep,
             (unsigned long long)ws.aspect_cone_guard_keep,
             (unsigned long long)ws.aspect_cone_hysteresis_keep,
             (unsigned long long)ws.aspect_cone_outside_reject,
             (unsigned long long)ws.aspect_cone_queue_reject,
             ws.aspect_cone_queue_highwater[0],
             ws.aspect_cone_queue_highwater[1],
             ws.aspect_cone_queue_highwater[2],
             (unsigned long long)ws.angle_calls,
             (unsigned long long)ws.angle_43_identity,
             ws.angle_max_vanilla, ws.angle_max_widened);
}

static void handle_ws_aspect_cone_site(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "address", addr_str, sizeof(addr_str))) {
        send_err(id, "missing address");
        return;
    }
    GpuWsAspectConeSiteDebug site;
    if (!gpu_ws_get_aspect_cone_site_debug(hex_to_u32(addr_str), &site)) {
        send_err(id, "aspect-cone site not configured");
        return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"address\":\"0x%08X\","
             "\"calls\":%llu,\"identity_43\":%llu,"
             "\"vanilla_keep\":%llu,\"visible_keep\":%llu,"
             "\"guard_keep\":%llu,\"hysteresis_keep\":%llu,"
             "\"outside_reject\":%llu,\"queue_reject\":%llu}",
             id, site.address,
             (unsigned long long)site.calls,
             (unsigned long long)site.identity_43,
             (unsigned long long)site.vanilla_keep,
             (unsigned long long)site.visible_keep,
             (unsigned long long)site.guard_keep,
             (unsigned long long)site.hysteresis_keep,
             (unsigned long long)site.outside_reject,
             (unsigned long long)site.queue_reject);
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

/* Precise-event-slicing validation: report the cycle distance to the next
 * deliverable interrupt, broken down per source. Compare against the live timer
 * counters / VBLANK pacing (timers_state, freeze_check) to validate
 * cycles_to_next_event before wiring it into the two-tier executor. UINT32_MAX
 * (4294967295) for a source means "no deliverable IRQ scheduled". */
static void handle_cycles_to_next_event(int id, const char *json)
{
    (void)json;
    uint32_t agg = cycles_to_next_event();
    uint32_t t = timers_cycles_to_irq(i_mask);
    uint32_t c = cdrom_cycles_to_irq(i_mask);
    uint32_t d = dma_cycles_to_irq(i_mask);
    uint32_t s = sio_cycles_to_irq(i_mask);
    uint32_t s1 = sio1_cycles_to_irq(i_mask);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
             "\"cycles_to_next_event\":%u,"
             "\"timers\":%u,\"cdrom\":%u,\"dma\":%u,\"sio\":%u,"
             "\"sio1\":%u}",
             id, i_stat, i_mask, agg, t, c, d, s, s1);
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

/* vblank_rate: report the ONE cycle-paced VBlank authority's raise/deliver
 * counts, the (normally-off) GPUSTAT-poll fallback raise count, and the
 * per-frame GP0(E5) draw-offset-Y range/count. Used to confirm the guest is
 * receiving exactly 60 VBlanks/s (not the ~96/s the stale poll fallback caused)
 * and to probe double-buffer draw-offset alternation. */
static void handle_vblank_rate(int id, const char *json)
{
    (void)json;
    extern uint64_t g_vblank_raise_count, g_vblank_deliver_count;
    extern uint64_t g_pollhack_vblank_count;
    extern int32_t  g_doff_min_last, g_doff_max_last;
    extern uint32_t g_doff_cnt_last;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"cycle_paced_raise\":%llu,"
             "\"delivered\":%llu,"
             "\"pollhack_raise\":%llu,"
             "\"doff_min\":%d,\"doff_max\":%d,\"doff_cnt\":%u}",
             id,
             (unsigned long long)g_vblank_raise_count,
             (unsigned long long)g_vblank_deliver_count,
             (unsigned long long)g_pollhack_vblank_count,
             g_doff_min_last, g_doff_max_last, g_doff_cnt_last);
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
             "\"read_hold_cycles\":%llu,\"read_hold_events\":%llu,"
             "\"int1_pended\":%llu,\"int1_lost\":%llu,\"int1_pending_now\":%u,"
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
             (unsigned long long)s.read_hold_cycles,
             (unsigned long long)s.read_hold_events,
             (unsigned long long)s.int1_pended,
             (unsigned long long)s.int1_lost,
             s.int1_pending_now,
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

static const char *cdrom_command_kind_name(uint8_t kind)
{
    switch (kind) {
    case 'C': return "exec";
    case 'Q': return "queued";
    default: return "unknown";
    }
}

static void handle_cdrom_command_history(int id, const char *json)
{
    int count = json_get_int(json, "count", 128);
    if (count < 1) count = 1;
    if (count > CDROM_COMMAND_HISTORY_CAP) count = CDROM_COMMAND_HISTORY_CAP;

    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);

    const CDROMCommandHistoryEntry *entries = NULL;
    uint64_t total = cdrom_debug_get_command_history(&entries);
    uint64_t oldest = (total > CDROM_COMMAND_HISTORY_CAP)
        ? total - CDROM_COMMAND_HISTORY_CAP : 0;

    size_t bufsz = 256u + (size_t)count * 640u;
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
    while (seq > oldest && emitted < count && pos < bufsz - 640) {
        seq--;
        const CDROMCommandHistoryEntry *e =
            &entries[seq % CDROM_COMMAND_HISTORY_CAP];
        if (e->seq != seq) continue;
        if (frame_lo >= 0 && (int)e->frame < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame > frame_hi) continue;

        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"frame\":%u,\"kind\":\"%s\","
                        "\"cmd\":\"0x%02X\",\"param_count\":%u,\"params\":[",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, e->frame,
                        cdrom_command_kind_name(e->kind),
                        e->cmd, e->param_count);
        for (uint8_t i = 0; i < e->param_count && i < 16 && pos < bufsz - 96; i++) {
            pos += snprintf(buf + pos, bufsz - pos,
                            "%s\"0x%02X\"", i ? "," : "", e->params[i]);
        }
        pos += snprintf(buf + pos, bufsz - pos,
                        "],\"stat\":\"0x%02X\",\"request\":\"0x%02X\","
                        "\"irq_enable\":\"0x%02X\",\"irq_flag\":\"0x%02X\","
                        "\"mode\":\"0x%02X\",\"seek_msf\":[%u,%u,%u],"
                        "\"read_msf\":[%u,%u,%u],\"read_cmd\":\"0x%02X\","
                        "\"reading\":%u,\"pending_cmd\":\"0x%02X\","
                        "\"pending\":%u,\"queued_cmd\":\"0x%02X\","
                        "\"queued\":%u,\"func\":\"0x%08X\",\"pc\":\"0x%08X\","
                        "\"i_stat\":\"0x%08X\"}",
                        e->stat, e->request, e->irq_enable, e->irq_flag,
                        e->mode, e->seek_min, e->seek_sec, e->seek_sect,
                        e->read_min, e->read_sec, e->read_sect, e->read_cmd,
                        e->reading, e->pending_cmd, e->pending_pending,
                        e->queued_cmd, e->queued_pending, e->func, e->pc,
                        e->i_stat);
        emitted++;
    }

    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_cdrom_command_history_clear(int id, const char *json)
{
    (void)json;
    cdrom_debug_clear_command_history();
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

    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);

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
        if (frame_lo >= 0 && (int)e->frame < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame > frame_hi) continue;
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
    case 'W': return "write";
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

static void append_word_array(char *buf, size_t bufsz, size_t *pos,
                              const uint32_t *words, int count)
{
    *pos += snprintf(buf + *pos, bufsz - *pos, "[");
    for (int i = 0; i < count && *pos < bufsz - 16; i++) {
        *pos += snprintf(buf + *pos, bufsz - *pos,
                         "%s\"0x%08X\"", i ? "," : "", words[i]);
    }
    *pos += snprintf(buf + *pos, bufsz - *pos, "]");
}

static void handle_dma_cdrom_history(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > DMA_CDROM_HISTORY_CAP) count = DMA_CDROM_HISTORY_CAP;

    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);
    int newest = json_get_int(json, "newest", 0) != 0;

    const DMACDROMHistoryEntry *entries = NULL;
    uint64_t total = dma_debug_get_cdrom_history(&entries);
    uint64_t oldest = (total > DMA_CDROM_HISTORY_CAP) ? total - DMA_CDROM_HISTORY_CAP : 0;

    size_t bufsz = 256u + (size_t)count * 1152u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest);

    uint64_t start = newest && total > (uint64_t)count ? total - (uint64_t)count : oldest;
    if (start < oldest) start = oldest;
    for (uint64_t seq = start; seq < total && emitted < count && pos < bufsz - 1152; seq++) {
        const DMACDROMHistoryEntry *e = &entries[seq % DMA_CDROM_HISTORY_CAP];
        if (e->seq != seq) continue;
        if (frame_lo >= 0 && (int)e->frame_start < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame_start > frame_hi) continue;

        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"frame_start\":%u,\"frame_end\":%u,"
                        "\"start_addr\":\"0x%08X\",\"final_addr\":\"0x%08X\","
                        "\"requested_words\":%u,\"moved_words\":%u,"
                        "\"bcr\":\"0x%08X\",\"chcr\":\"0x%08X\",\"dpcr\":\"0x%08X\","
                        "\"dicr_start\":\"0x%08X\",\"dicr_end\":\"0x%08X\","
                        "\"i_stat_start\":\"0x%08X\",\"i_stat_end\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"pc\":\"0x%08X\","
                        "\"lba\":%d,\"sector_size\":%d,"
                        "\"sector_read_pos_start\":%d,\"sector_read_pos_end\":%d,"
                        "\"mode\":\"0x%02X\",\"sector_available_start\":%u,"
                        "\"sector_available_end\":%u,\"completed\":%u,"
                        "\"first_words\":",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, e->frame_start, e->frame_end,
                        e->start_addr, e->final_addr,
                        e->requested_words, e->moved_words,
                        e->bcr, e->chcr, e->dpcr,
                        e->dicr_start, e->dicr_end,
                        e->i_stat_start, e->i_stat_end,
                        e->func, e->pc,
                        e->lba, e->sector_size,
                        e->sector_read_pos_start, e->sector_read_pos_end,
                        e->mode, e->sector_available_start,
                        e->sector_available_end, e->completed);
        append_word_array(buf, bufsz, &pos, e->first_words, e->first_count);
        pos += snprintf(buf + pos, bufsz - pos, ",\"last_words\":");
        append_word_array(buf, bufsz, &pos, e->last_words, e->last_count);
        pos += snprintf(buf + pos, bufsz - pos, "}");
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
extern int gpu_get_a0_src(int index, uint32_t *s2, uint32_t *a0, uint32_t *a1, uint32_t *frame);

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
        uint32_t s2 = 0, a0r = 0, a1r = 0, aframe = 0;
        gpu_get_a0_history(i, &x, &y, &w, &h, &fw0, &fw1, &wcount);
        gpu_get_a0_extra(i, &func, &sp, &ra, &s1, stk);
        gpu_get_a0_src(i, &s2, &a0r, &a1r, &aframe);
        pos += snprintf(buf + pos, bufsz - pos,
            "%s{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
            "\"fw0\":\"0x%08X\",\"fw1\":\"0x%08X\",\"words\":%d,"
            "\"func\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"s1\":\"0x%08X\",\"s2\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"frame\":%u,"
            "\"stk\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\"]}",
            i ? "," : "", i, x, y, w, h, fw0, fw1, wcount,
            func, sp, ra, s1, s2, a0r, a1r, aframe,
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

    /* ~190 bytes base + up to ~90 for the copy builder chain; budget conservatively. */
    size_t buf_sz = 256 + (size_t)n * 400u;
    char *buf = (char *)malloc(buf_sz);
    if (!buf) { free(entries); send_err(id, "alloc failed"); return; }

    size_t pos = (size_t)snprintf(buf, buf_sz,
        "{\"id\":%d,\"ok\":true,\"frame\":%u,\"count\":%d,\"max_words\":%u,\"entries\":[",
        id, (uint32_t)target, n, gpu_gp0_ring_max_words());

    for (int i = 0; i < n && pos < buf_sz - 256; i++) {
        const GpuGp0RingEntry *e = &entries[i];
        pos += (size_t)snprintf(buf + pos, buf_sz - pos,
            "%s{\"seq\":%u,\"op\":\"0x%02X\",\"n\":%u,"
            "\"src\":\"0x%08X\",\"ot\":%u,\"pc\":\"0x%08X\","
            "\"func\":\"0x%08X\",\"ra\":\"0x%08X\",\"w\":[",
            i ? "," : "", e->seq, e->opcode, e->n_words,
            e->src_addr, (unsigned)e->ot_rank, e->pc, e->func, e->ra);
        int show = e->n_words < GPU_GP0_RING_MAX_WORDS
                 ? e->n_words : GPU_GP0_RING_MAX_WORDS;
        for (int k = 0; k < show && pos < buf_sz - 32; k++) {
            pos += (size_t)snprintf(buf + pos, buf_sz - pos,
                "%s\"0x%08X\"", k ? "," : "", e->cmd[k]);
        }
        pos += (size_t)snprintf(buf + pos, buf_sz - pos, "]");
        if (e->opcode == 0x80) {
            pos += (size_t)snprintf(buf + pos, buf_sz - pos,
                ",\"csp\":\"0x%08X\",\"bld\":[", e->csp);
            for (int k = 0; k < 6 && e->bld[k] && pos < buf_sz - 32; k++)
                pos += (size_t)snprintf(buf + pos, buf_sz - pos,
                    "%s\"0x%08X\"", k ? "," : "", e->bld[k]);
            pos += (size_t)snprintf(buf + pos, buf_sz - pos, "]");
        }
        pos += (size_t)snprintf(buf + pos, buf_sz - pos, "}");
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

/* Dump recent GTE RTPS/RTPT projections (inputs + outputs) from the always-on
 * GTE ring. {"cmd":"gte_ring_dump","count":N,"newest":1,"frame":F} — frame
 * optional (omit or -1 for all). Used to find flattened/degenerate character
 * projections and split game-code input bugs from GTE-math bugs. */
static void handle_gte_ring_dump(int id, const char *json)
{
    extern unsigned long long gte_rtp_ring_total(void);
    extern int gte_rtp_ring_dump_json(char *out, int outsz, int max_count,
                                      int newest_first, long frame_filter);
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > 512) count = 512;
    int newest = json_get_int(json, "newest", 1) != 0;
    long frame = (long)json_get_int(json, "frame", -1);

    size_t BUF_SZ = 256u + (size_t)count * 720u;
    char *entries = (char *)malloc(BUF_SZ);
    char *reply   = (char *)malloc(BUF_SZ + 256u);
    if (!entries || !reply) { free(entries); free(reply); send_err(id, "oom"); return; }
    int n = gte_rtp_ring_dump_json(entries, (int)BUF_SZ, count, newest, frame);
    snprintf(reply, BUF_SZ + 256u,
             "{\"id\":%d,\"ok\":true,\"total\":%llu,\"emitted\":%d,\"entries\":[%s]}",
             id, gte_rtp_ring_total(), n, entries);
    debug_server_send_line(reply);
    free(entries); free(reply);
}

/* INTPL (vertex-lerp) ring: inputs (ir0 blend, in=IR1-3 pose A, fc=pose B)
 * and outputs (mac / out=IR1-3 / flag) per op. offset pages through the
 * matching entries after the frame filter, so a whole frame is reachable. */
static void handle_gte_intpl_dump(int id, const char *json)
{
    extern unsigned long long gte_intpl_ring_total(void);
    extern int gte_intpl_ring_dump_json(char *out, int outsz, int max_count,
                                        int newest_first, long frame_filter,
                                        int offset);
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > 512) count = 512;
    int newest = json_get_int(json, "newest", 1) != 0;
    long frame = (long)json_get_int(json, "frame", -1);
    int offset = json_get_int(json, "offset", 0);
    if (offset < 0) offset = 0;

    size_t BUF_SZ = 256u + (size_t)count * 420u;
    char *entries = (char *)malloc(BUF_SZ);
    char *reply   = (char *)malloc(BUF_SZ + 256u);
    if (!entries || !reply) { free(entries); free(reply); send_err(id, "oom"); return; }
    int n = gte_intpl_ring_dump_json(entries, (int)BUF_SZ, count, newest, frame, offset);
    snprintf(reply, BUF_SZ + 256u,
             "{\"id\":%d,\"ok\":true,\"total\":%llu,\"emitted\":%d,\"offset\":%d,\"entries\":[%s]}",
             id, gte_intpl_ring_total(), n, offset, entries);
    debug_server_send_line(reply);
    free(entries); free(reply);
}

/* Per-frame GTE projection stats (nproj / nsat / nflat) over recent frames —
 * shows the alternating flat/normal render pattern. */
static void handle_gte_frame_stats(int id, const char *json)
{
    extern int gte_fstat_dump_json(char *out, int outsz, int max_frames);
    int n = json_get_int(json, "frames", 120);
    if (n < 1) n = 1; if (n > 512) n = 512;
    size_t BUF = 256u + (size_t)n * 96u;
    char *body = (char *)malloc(BUF), *reply = (char *)malloc(BUF + 128u);
    if (!body || !reply) { free(body); free(reply); send_err(id, "oom"); return; }
    int emitted = gte_fstat_dump_json(body, (int)BUF, n);
    snprintf(reply, BUF + 128u, "{\"id\":%d,\"ok\":true,\"emitted\":%d,\"frames\":[%s]}",
             id, emitted, body);
    debug_server_send_line(reply); free(body); free(reply);
}

/* Latched degenerate (saturated-output) GTE projections with full inputs. */
static void handle_gte_latch_dump(int id, const char *json)
{
    extern unsigned long long gte_latch_total(void);
    extern int gte_latch_dump_json(char *out, int outsz, int max_count);
    int n = json_get_int(json, "count", 64);
    if (n < 1) n = 1; if (n > 256) n = 256;
    size_t BUF = 256u + (size_t)n * 720u;
    char *body = (char *)malloc(BUF), *reply = (char *)malloc(BUF + 128u);
    if (!body || !reply) { free(body); free(reply); send_err(id, "oom"); return; }
    int emitted = gte_latch_dump_json(body, (int)BUF, n);
    snprintf(reply, BUF + 128u, "{\"id\":%d,\"ok\":true,\"latch_total\":%llu,\"emitted\":%d,\"entries\":[%s]}",
             id, gte_latch_total(), emitted, body);
    debug_server_send_line(reply); free(body); free(reply);
}

static void handle_dual_input(int id, const char *json)
{
    char route[16];
    int r = -1;
    if (json_get_str(json, "route", route, sizeof route)) {
        if      (!strcmp(route, "both")) r = 0;
        else if (!strcmp(route, "a"))    r = 1;
        else if (!strcmp(route, "b"))    r = 2;
    }
    if (r < 0) { send_err(id, "route must be 'both'|'a'|'b'"); return; }
    psx_dual_set_input_route(r);
    send_fmt("{\"id\":%d,\"ok\":true,\"route\":%d}", id, r);
}

static void handle_dual_state(int id, const char *json)
{
    (void)json;
    uint64_t cyc[2] = { 0, 0 };
    int live = psx_dual_machine_live();
    psx_dual_machine_cycles(cyc);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"active\":%d,"
             "\"live\":%d,"
             "\"input_route\":%d,"
             "\"swaps\":%llu,"
             "\"cycles_a\":%llu,"
             "\"cycles_b\":%llu,"
             "\"skew\":%lld}",
             id,
             live >= 0,
             live,
             psx_dual_get_input_route(),
             (unsigned long long)psx_dual_machine_swaps(),
             (unsigned long long)cyc[0],
             (unsigned long long)cyc[1],
             (long long)(cyc[0] - cyc[1]));
}

static void handle_sio1_state(int id, const char *json)
{
    (void)json;
    Sio1Device *d = sio1_get_device();
    uint32_t txc = 0, rxc = 0, ovr = 0, irqs = 0;
    if (!d) { send_err(id, "sio1 not initialized"); return; }
    sio1_device_get_counters(d, &txc, &rxc, &ovr, &irqs);
    /* Side-effect-free peeks only (a real STAT/RXDATA read pops/perturbs). */
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"regs_enabled\":%d,"
             "\"backend\":\"%s\","
             "\"stat\":\"0x%08X\","
             "\"mode\":\"0x%04X\","
             "\"ctrl\":\"0x%04X\","
             "\"baud\":\"0x%04X\","
             "\"bit_cycles\":%u,"
             "\"char_cycles\":%u,"
             "\"active\":%d,"
             "\"tx_chars\":%u,"
             "\"rx_chars\":%u,"
             "\"overruns\":%u,"
             "\"irqs\":%u}",
             id,
             g_sio1_regs_enabled,
             sio1_get_backend(),
             sio1_device_peek_stat(d, psx_get_cycle_count()),
             sio1_device_peek_mode(d),
             sio1_device_peek_ctrl(d),
             sio1_device_peek_baud(d),
             sio1_device_bit_cycles(d),
             sio1_device_char_cycles(d),
             sio1_device_active(d),
             txc, rxc, ovr, irqs);
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
             /* Side-effect-free peeks (sio_read pops the RX FIFO / clears ACK). */
             sio_peek_stat(),
             sio_peek_ctrl(),
             sio_peek_rx_data(),
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
    /* The DSP-fidelity state (issue #103: SPU IRQ, reverb, noise, sweeps) lives
     * in SpuGlobalState. Surfaced here so the whole SPU can be judged from one
     * always-on query — without it there is no way to tell whether the reverb
     * engine is actually stepping, whether the IRQ is armed, or which volume
     * registers are sweeping. */
    SpuGlobalState g;
    spu_get_global_state(&g);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"ctrl\":\"0x%04X\",\"active_mask\":\"0x%06X\","
             "\"main_l\":%d,\"main_r\":%d,"
             "\"cd_l\":%d,\"cd_r\":%d,"
             "\"key_on_count\":%u,"
             "\"render_frames\":%llu,\"nonzero_frames\":%llu,"
             "\"last_peak\":%d,\"peak\":%d,"
             "\"cd_frames\":%u,\"cd_push_frames\":%llu,"
             "\"cd_overflow_frames\":%llu,\"cd_underflow_frames\":%llu,"
             "\"pmon\":\"0x%06X\",\"non\":\"0x%06X\",\"eon\":\"0x%06X\","
             "\"endx\":\"0x%06X\","
             "\"irq_flag\":%u,\"irq_addr\":\"0x%05X\","
             "\"reverb_on\":%u,\"reverb_mbase\":\"0x%05X\","
             "\"reverb_cur\":\"0x%05X\",\"capture_pos\":\"0x%03X\","
             "\"noise_lfsr\":\"0x%04X\","
             "\"sweep_l_mask\":\"0x%06X\",\"sweep_r_mask\":\"0x%06X\","
             "\"sweep_main\":%u}",
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
             (unsigned long long)info.cd_underflow_frames,
             g.pmon & 0xFFFFFFu,
             g.non  & 0xFFFFFFu,
             g.eon  & 0xFFFFFFu,
             g.endx & 0xFFFFFFu,
             (unsigned)g.irq_flag,
             g.irq_addr & 0xFFFFFu,
             (unsigned)g.reverb_on,
             g.reverb_mbase & 0xFFFFFu,
             g.reverb_cur & 0xFFFFFu,
             g.capture_pos & 0xFFFu,
             (unsigned)g.noise_lfsr,
             g.sweep_l_mask & 0xFFFFFFu,
             g.sweep_r_mask & 0xFFFFFFu,
             (unsigned)g.sweep_main);
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

    /* 24 voices x ~280 chars + header. Headroom matters: snprintf would silently
     * truncate mid-object and hand the caller unparseable JSON. */
    size_t cap = 16384;
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
            "\"flags\":\"0x%02X\",\"sample_idx\":%d,\"phase\":\"0x%04X\","
            "\"env\":\"0x%04X\",\"env_phase\":%d,"
            /* Live effective volumes. For a sweeping register (bit 15 set) the
             * vol_l/vol_r control words above say nothing about the current
             * level, so these are the only way to see a sweep actually glide. */
            "\"vol_cur_l\":%d,\"vol_cur_r\":%d}",
            v == 0 ? "" : ",",
            v, s.active,
            s.vol_ctrl_l, s.vol_ctrl_r,
            s.pitch,
            (uint32_t)s.start_lo << 3,
            (uint32_t)s.loop_lo  << 3,
            s.adsr_lo, s.adsr_hi,
            s.cur_addr, s.repeat_addr,
            s.last_flags, s.sample_idx, s.phase,
            s.env_level, s.adsr_phase,
            s.vol_cur_l, s.vol_cur_r);
        if (n > 0) off += (size_t)n;
    }
    n = snprintf(out + off, cap - off, "]}");
    if (n > 0) off += (size_t)n;
    send_fmt("%s", out);
    free(out);
}

/* ---- SPU RAM peek: {"addr":N,"len":M} -> hex bytes. Sample data is the
 * ground truth for voice-rail triage (what does a parked loop block hold?). */
static void handle_spu_ram(int id, const char *json)
{
    uint32_t addr = (uint32_t)json_get_int(json, "addr", 0);
    int len = json_get_int(json, "len", 16);
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;
    uint8_t bytes[4096];
    uint32_t got = spu_ram_peek(addr, bytes, (uint32_t)len);
    char *hex = (char *)malloc((size_t)got * 2u + 1u);
    if (!hex) { send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    for (uint32_t i = 0; i < got; i++)
        snprintf(hex + i * 2u, 3u, "%02X", bytes[i]);
    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%05X\",\"len\":%u,\"hex\":\"%s\"}",
             id, addr, (unsigned)got, hex);
    free(hex);
}

/* ---- SPU event ring dump. Returns the most recent N events
 * (KEYON / KEYOFF / END_STOP / END_LOOP / IRQ) with frame timestamps. */
static void handle_spu_events(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 4096) count = 4096;
    SpuEvent *evs = (SpuEvent *)malloc((size_t)count * sizeof(SpuEvent));
    if (!evs) { send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    uint32_t got = spu_event_get(evs, (uint32_t)count);
    uint64_t total = spu_event_total();
    /* Index by SpuEventKind (spu.h). IRQ (=5) is not voice-attributable; the
     * ring stores voice=0xFF for it and `addr` is the byte address that matched
     * the programmed IRQ address. Keep this table in step with SpuEventKind or
     * a new kind renders as "?". */
    static const char *kind_names[6] = { "?", "KEYON", "KEYOFF", "END_STOP",
                                         "END_LOOP", "IRQ" };

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
        const char *kn = (e->kind < sizeof(kind_names) / sizeof(kind_names[0]))
                         ? kind_names[e->kind] : "?";
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

/* ---- Always-on audio tap rings (audio_trace.c) -------------------------
 * audio_stats  — counters: per-tap produced/nonzero/audible/peak + pump
 *                behavior (skips, underruns, queue watermarks).
 * audio_wav    — dump a tap's PCM ring slice to a 44100 Hz s16 WAV on the
 *                server side: {"tap":0,"path":"...","start":-1,"count":0}.
 * audio_events — most recent N pipeline events (REG_WRITE/RENDER/SKIP/
 *                UNDERRUN/MUTE/UNMUTE/CD_PUSH/DMA), sample-clock stamped.
 * Protocol mirrored on psx-beetle's port 4380 (beetle_debug_server.c). */
/* Bridge/legacy output health (main.cpp; C linkage). Returns 0 pre-device. */
extern int psx_audio_out_stats(double *fill_ms, double *target_ms,
                               uint64_t *underruns,
                               uint64_t *overflow_drops, double *correction,
                               int *legacy, int *host_rate);

static void handle_audio_stats(int id, const char *json)
{
    (void)json;
    AudioTraceStats st;
    audio_trace_get_stats(&st);
    double fill_ms = 0.0, target_ms = 0.0, correction = 0.0;
    uint64_t out_underruns = 0, overflow_drops = 0;
    int legacy = 1, host_rate = 44100;
    int out_ok = psx_audio_out_stats(&fill_ms, &target_ms, &out_underruns,
                                     &overflow_drops, &correction, &legacy,
                                     &host_rate);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"taps\":["
             "{\"name\":\"spu_out\",\"frames\":%llu,\"nonzero\":%llu,"
             "\"audible\":%llu,\"peak\":%d,\"rate\":%u},"
             "{\"name\":\"cd_in\",\"frames\":%llu,\"nonzero\":%llu,"
             "\"audible\":%llu,\"peak\":%d,\"rate\":%u},"
             "{\"name\":\"host\",\"frames\":%llu,\"nonzero\":%llu,"
             "\"audible\":%llu,\"peak\":%d,\"rate\":%u}],"
             "\"pump_calls\":%llu,\"pump_skips\":%llu,\"underruns\":%llu,"
             "\"queue_hiwater\":%u,\"queue_lowater\":%u,"
             "\"mutes\":%llu,\"unmutes\":%llu,\"events_total\":%llu,"
             "\"out\":{\"active\":%d,\"mode\":\"%s\",\"host_rate\":%d,"
             "\"fill_ms\":%.1f,\"target_ms\":%.1f,\"underruns\":%llu,"
             "\"overflow_drops\":%llu,"
             "\"correction\":%.5f}}",
             id,
             (unsigned long long)st.tap_frames[0],
             (unsigned long long)st.tap_nonzero[0],
             (unsigned long long)st.tap_audible[0], st.tap_peak[0],
             audio_trace_tap_rate(0),
             (unsigned long long)st.tap_frames[1],
             (unsigned long long)st.tap_nonzero[1],
             (unsigned long long)st.tap_audible[1], st.tap_peak[1],
             audio_trace_tap_rate(1),
             (unsigned long long)st.tap_frames[2],
             (unsigned long long)st.tap_nonzero[2],
             (unsigned long long)st.tap_audible[2], st.tap_peak[2],
             audio_trace_tap_rate(2),
             (unsigned long long)st.pump_calls,
             (unsigned long long)st.pump_skips,
             (unsigned long long)st.underruns,
             st.queue_hiwater, st.queue_lowater,
             (unsigned long long)st.mute_events,
             (unsigned long long)st.unmute_events,
             (unsigned long long)st.events_total,
             out_ok, legacy ? "legacy-push" : "bridge-pull", host_rate,
             fill_ms, target_ms,
             (unsigned long long)out_underruns,
             (unsigned long long)overflow_drops,
             correction);
}

static void handle_audio_wav(int id, const char *json)
{
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path))) {
        send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"missing path\"}", id);
        return;
    }
    int tap = json_get_int(json, "tap", AUDIO_TAP_SPU_OUT);
    /* start/count as strings so 64-bit sample indices survive. */
    char buf[32];
    int64_t start = -1;
    uint64_t count = 0;
    if (json_get_str(json, "start", buf, sizeof(buf)))
        start = strtoll(buf, NULL, 0);
    else
        start = (int64_t)json_get_int(json, "start", -1);
    if (json_get_str(json, "count", buf, sizeof(buf)))
        count = strtoull(buf, NULL, 0);
    else
        count = (uint64_t)json_get_int(json, "count", 0);

    int64_t wrote = audio_trace_dump_wav(tap, path, start, count);
    if (wrote < 0) {
        send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"dump failed (bad tap/path or empty ring)\"}", id);
        return;
    }
    uint64_t total = audio_trace_tap_total(tap);
    send_fmt("{\"id\":%d,\"ok\":true,\"tap\":%d,\"frames\":%lld,"
             "\"rate\":%u,\"tap_total\":%llu}",
             id, tap, (long long)wrote, audio_trace_tap_rate(tap),
             (unsigned long long)total);
}

static void handle_audio_events(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 8192) count = 8192;
    AudioTraceEvent *evs =
        (AudioTraceEvent *)malloc((size_t)count * sizeof(AudioTraceEvent));
    if (!evs) { send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    uint32_t got = audio_trace_events_get(evs, (uint32_t)count);
    uint64_t total = audio_trace_events_total();
    static const char *kind_names[] = {
        "?", "REG", "RENDER", "SKIP", "UNDERRUN",
        "MUTE", "UNMUTE", "CD_PUSH", "DMA", "XA_ZERO", "SINK_DROP"
    };

    size_t cap = 256u + (size_t)got * 192u;
    char *out = (char *)malloc(cap);
    if (!out) { free(evs); send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    size_t off = 0;
    int n = snprintf(out + off, cap - off,
        "{\"id\":%d,\"ok\":true,\"total\":%llu,\"count\":%u,\"events\":[",
        id, (unsigned long long)total, (unsigned)got);
    if (n > 0) off += (size_t)n;
    for (uint32_t i = 0; i < got; i++) {
        const AudioTraceEvent *e = &evs[i];
        const char *kn = (e->kind < sizeof(kind_names) / sizeof(kind_names[0]))
                         ? kind_names[e->kind] : "?";
        n = snprintf(out + off, cap - off,
            "%s{\"seq\":%llu,\"smp\":%llu,\"frame\":%u,"
            "\"kind\":\"%s\",\"a\":\"0x%X\",\"b\":\"0x%X\"}",
            i == 0 ? "" : ",",
            (unsigned long long)e->seq,
            (unsigned long long)e->sample_idx,
            e->frame, kn, e->a, e->b);
        if (n > 0) off += (size_t)n;
    }
    n = snprintf(out + off, cap - off, "]}");
    if (n > 0) off += (size_t)n;
    send_fmt("%s", out);
    free(out);
    free(evs);
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

/* Post-probe bit7 → TX 0x57 handoff (Ape Escape LOAD). */
static void handle_card_handoff(int id, const char *json)
{
    int count = json_get_int(json, "count", 64);
    int idx = 0, total = 0;
    const SioCardHandoffEntry *buf = sio_get_card_handoff(&idx, &total);
    int cap = sio_card_handoff_cap();
    int avail = total < cap ? total : cap;
    if (count > avail) count = avail;
    if (count < 0) count = 0;

    int start = count ? (idx - count + cap) % cap : 0;
    static const char *kinds[] = {
        "?", "probe_abort", "b7_set", "b7_clear", "tx", "card_ack", "unstick",
        "select_flush_ack", "ack_deferred_istat7", "nest_irq_pulse", "b7_hold"
    };
    send_fmt("{\"id\":%d,\"ok\":true,\"armed\":%d,\"total\":%d,\"count\":%d,\"entries\":[",
             id, sio_card_handoff_armed(), total, count);
    for (int i = 0; i < count; i++) {
        const SioCardHandoffEntry *e = &buf[(start + i) % cap];
        const char *k = (e->kind < (uint8_t)(sizeof(kinds) / sizeof(kinds[0])))
                            ? kinds[e->kind] : "?";
        if (i) send_fmt(",");
        send_fmt("{\"kind\":\"%s\",\"byte\":\"0x%02X\",\"imask\":\"0x%03X\","
                 "\"pc\":\"0x%08X\",\"func\":\"0x%08X\","
                 "\"a6c10\":\"0x%08X\",\"b4e30\":\"0x%08X\",\"b4e38\":\"0x%08X\","
                 "\"cyc\":%llu}",
                 k, e->byte, e->imask,
                 (unsigned)e->pc, (unsigned)e->func,
                 (unsigned)e->a6c10, (unsigned)e->b4e30, (unsigned)e->b4e38,
                 (unsigned long long)e->cyc);
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
    /* Optional stick override: any of lx/ly/rx/ry (0..255) arms it; omitted
     * axes centre. Absent entirely -> released (buttons-only injection). */
    int ax[4] = { json_get_int(json, "lx", -1), json_get_int(json, "ly", -1),
                  json_get_int(json, "rx", -1), json_get_int(json, "ry", -1) };
    s_axis_override = (ax[0] >= 0 || ax[1] >= 0 || ax[2] >= 0 || ax[3] >= 0);
    for (int i = 0; i < 4; i++) {
        int v = ax[i] < 0 ? 0x80 : (ax[i] > 255 ? 255 : ax[i]);
        s_axis_st[i] = (uint8_t)v;
    }
    send_ok(id);
}

static void handle_press(int id, const char *json)
{
    int buttons = json_get_int(json, "buttons", -1);
    int frames  = json_get_int(json, "frames", 2);
    if (buttons < 0) { send_err(id, "missing buttons"); return; }
    s_input_override = buttons;
    s_input_frames   = frames;
    int ax[4] = { json_get_int(json, "lx", -1), json_get_int(json, "ly", -1),
                  json_get_int(json, "rx", -1), json_get_int(json, "ry", -1) };
    s_axis_override = (ax[0] >= 0 || ax[1] >= 0 || ax[2] >= 0 || ax[3] >= 0);
    for (int i = 0; i < 4; i++) {
        int v = ax[i] < 0 ? 0x80 : (ax[i] > 255 ? 255 : ax[i]);
        s_axis_st[i] = (uint8_t)v;
    }
    send_ok(id);
}

/* Reports the current pad word(s) and any active input override. `pad` is
 * slot 0 (kept for back-compat); slot0/slot1 report both ports' button word. */
extern uint16_t sio_get_pad_buttons(void);
extern uint16_t sio_get_pad_buttons_slot(int slot);
extern int sio_get_pad_connected(int slot);
extern int sio_get_pad_analog(int slot);
extern void sio_get_pad_sticks(int slot, uint8_t out[4]);
static void handle_pad_status(int id, const char *json)
{
    (void)json;
    uint16_t pad0 = sio_get_pad_buttons_slot(0);
    uint16_t pad1 = sio_get_pad_buttons_slot(1);
    uint8_t sticks0[4], sticks1[4];
    sio_get_pad_sticks(0, sticks0);
    sio_get_pad_sticks(1, sticks1);
    send_fmt("{\"id\":%d,\"ok\":true,\"pad\":\"0x%04X\","
             "\"slot0\":{\"buttons\":\"0x%04X\",\"connected\":%s,\"analog\":%s,\"sticks\":[%u,%u,%u,%u]},"
             "\"slot1\":{\"buttons\":\"0x%04X\",\"connected\":%s,\"analog\":%s,\"sticks\":[%u,%u,%u,%u]},"
             "\"override\":%d,\"override_frames\":%d,"
             "\"override_axes\":[%u,%u,%u,%u],\"override_axes_valid\":%s}\n",
             id, pad0,
             pad0, sio_get_pad_connected(0) ? "true" : "false", sio_get_pad_analog(0) ? "true" : "false",
             sticks0[0], sticks0[1], sticks0[2], sticks0[3],
             pad1, sio_get_pad_connected(1) ? "true" : "false", sio_get_pad_analog(1) ? "true" : "false",
             sticks1[0], sticks1[1], sticks1[2], sticks1[3],
             s_input_override, s_input_frames,
             s_axis_st[0], s_axis_st[1], s_axis_st[2], s_axis_st[3],
             s_axis_override ? "true" : "false");
}

static void handle_clear_input(int id, const char *json)
{
    (void)json;
    s_input_route_active = 0;
    s_input_route_index = 0;
    s_input_route_remaining = 0;
    s_input_override = -1;
    s_input_frames   = 0;
    s_axis_override  = 0;
    s_axis_st[0] = s_axis_st[1] = s_axis_st[2] = s_axis_st[3] = 0x80;
    send_ok(id);
}

static void handle_input_route_clear(int id, const char *json)
{
    (void)json;
    s_input_route_active = 0;
    s_input_route_count = 0;
    s_input_route_index = 0;
    s_input_route_remaining = 0;
    send_ok(id);
}

static void handle_input_route_append(int id, const char *json)
{
    int frames = json_get_int(json, "frames", -1);
    int buttons = json_get_int(json, "buttons", -1);
    if (s_input_route_active) {
        send_err(id, "input route is active"); return;
    }
    if (frames <= 0) {
        send_err(id, "frames must be positive"); return;
    }
    if (buttons < 0 || buttons > 0xFFFF) {
        send_err(id, "buttons must be a 16-bit pad word"); return;
    }
    if (s_input_route_count >= INPUT_ROUTE_MAX_STEPS) {
        send_err(id, "input route is full"); return;
    }
    InputRouteStep *step = &s_input_route[s_input_route_count++];
    step->frames = (uint32_t)frames;
    step->buttons = (uint16_t)buttons;
    send_fmt("{\"id\":%d,\"ok\":true,\"steps\":%u}\n",
             id, (unsigned)s_input_route_count);
}

static void handle_input_route_start(int id, const char *json)
{
    (void)json;
    if (s_input_route_count == 0) {
        send_err(id, "input route is empty"); return;
    }
    s_input_override = -1;
    s_input_frames = 0;
    s_axis_override = 0;
    s_input_route_index = 0;
    s_input_route_remaining = s_input_route[0].frames;
    s_input_route_active = 1;
    send_fmt("{\"id\":%d,\"ok\":true,\"steps\":%u,\"start_frame\":%llu}\n",
             id, (unsigned)s_input_route_count,
             (unsigned long long)s_frame_count);
}

static void handle_input_route_stop(int id, const char *json)
{
    (void)json;
    s_input_route_active = 0;
    s_input_route_remaining = 0;
    send_ok(id);
}

static void handle_input_route_status(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"active\":%s,\"steps\":%u,"
             "\"index\":%u,\"remaining\":%u}\n",
             id, s_input_route_active ? "true" : "false",
             (unsigned)s_input_route_count, (unsigned)s_input_route_index,
             (unsigned)s_input_route_remaining);
}

/* Live A/B for the native-wide HUD corner gate:
 *   {"cmd":"ws_hud_mode","tag_rects":0|1}
 * tag_rects=1 lets TAGGED rect-family prims re-anchor too (Tomba's AP
 * counter renders through the tagged sprite funnel). */
static void handle_ws_hud_mode(int id, const char *json)
{
    int v = json_get_int(json, "tag_rects", -1);
    if (v < 0) { send_err(id, "missing tag_rects (0|1)"); return; }
    gpu_ws_set_nw_hud_tag_rects(v);
    send_fmt("{\"id\":%d,\"ok\":true,\"tag_rects\":%d}", id, v ? 1 : 0);
}

/* Kernel-image bless state: {"cmd":"kernel_bless"} ->
 * entries/clean/mismatch/native_hits/verifies/invalidations.
 * (memory.c psx_kernel_bless_*; PSX_KERNEL_BLESS=0 disables the mechanism.) */
static void handle_kernel_bless(int id, const char *json)
{
    extern void psx_kernel_bless_stats(uint64_t out[6]);
    (void)json;
    uint64_t s[6];
    psx_kernel_bless_stats(s);
    send_fmt("{\"id\":%d,\"ok\":true,\"entries\":%llu,\"clean\":%llu,"
             "\"mismatch\":%llu,\"native_hits\":%llu,\"verifies\":%llu,"
             "\"invalidations\":%llu}",
             id, (unsigned long long)s[0], (unsigned long long)s[1],
             (unsigned long long)s[2], (unsigned long long)s[3],
             (unsigned long long)s[4], (unsigned long long)s[5]);
}

static void handle_ws_margin(int id, const char *json)
{
    int v = json_get_int(json, "value", -2);
    if (v < -1) { send_err(id, "missing value (>=0 to force, -1 to clear)"); return; }
    gpu_ws_set_margin_override(v);
    GpuWsDebug ws;
    gpu_ws_get_debug(&ws);
    send_fmt("{\"id\":%d,\"ok\":true,\"override\":%d,\"x_margin\":%d,"
             "\"activation_margin\":%d,\"active\":%d}",
             id, v, ws.x_margin, ws.activation_margin, ws.active);
}

/* frame_perf: per-frame GPU/CPU phase timing (gpu_gl_renderer.c frame_perf ring).
 * Reports avg/max over the recent ring for ALL frames plus the native-wide (16:9)
 * and 4:3 subsets, so a 16:9-vs-4:3 A/B reads straight off one query. The GPU
 * phases are true GL_TIME_ELAPSED times — unaffected by the debug build's CPU
 * overhead — so scene_gpu (fill) vs present_gpu (wide composite) vs emu_cpu
 * (guest emulation, frame minus present) pinpoints where the frame goes. */
/* Self-test the runaway-recursion crash capture: forces a runaway on the next
 * guest function entry; the stack guard halts gracefully and the report names the
 * recursing func (0x8000DEAD here) + the recent_fn ring. Validates that an
 * organic crash will be root-causable. Intentionally halts the emulation. */
static void handle_synth_recurse(int id, const char *json)
{
    (void)json;
    /* The self-test (debug_server_synth_recurse_arm + its machinery) is compiled
     * only with the debug tools; guard the call so the Release build — where this
     * handler is dead code (the TCP server never starts) — still links. */
#ifndef PSX_NO_DEBUG_TOOLS
    extern void debug_server_synth_recurse_arm(void);
    debug_server_synth_recurse_arm();
    send_fmt("{\"id\":%d,\"ok\":true,\"armed\":true}", id);
#else
    (void)id;
#endif
}

static void handle_frame_perf(int id, const char *json)
{
    (void)json;
    double all[18], wide[18], n43[18];
    int na = gl_renderer_perf_aggregate(-1, all);
    gl_renderer_perf_aggregate(1, wide);
    gl_renderer_perf_aggregate(0, n43);
    if (na <= 0) { send_err(id, "no frame_perf samples (GL timer queries unavailable, or no GL frames yet)"); return; }
    /* per-prim GPU microseconds (scene_gpu / prims): high => draw-call/state-change
     * bound (batching wins); low => per-triangle/fill bound. */
    double wpp = wide[9] > 0 ? wide[5] * 1000.0 / wide[9] : 0.0;
    double npp = n43[9]  > 0 ? n43[5]  * 1000.0 / n43[9]  : 0.0;
    /* mirror split (wide frames only): canon = scene_gpu minus the mirror
     * passes; mirror_pass_us = mirror GPU time per mirror pass. */
    double wcanon = wide[5] - wide[10]; if (wcanon < 0) wcanon = 0;
    double wmpp   = wide[12] > 0 ? wide[10] * 1000.0 / wide[12] : 0.0;
    double tex_frac = 0.0; gl_renderer_perf_prim_split(&tex_frac);
    uint64_t br[8]; extern void gl_renderer_batch_diag(uint64_t out[8]);
    gl_renderer_batch_diag(br);
    send_fmt("{\"id\":%d,\"ok\":true,\"samples\":%d,\"wide_frames\":%d,\"frames_4_3\":%d,"
             "\"tex_frac\":%.3f,\"ws_ablate\":%d,"
             "\"batch_diag\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu],"
             "\"all\":{\"total_ms_avg\":%.3f,\"total_ms_max\":%.3f,\"emu_cpu_ms_avg\":%.3f,"
             "\"present_wall_ms_avg\":%.3f,\"scene_gpu_ms_avg\":%.3f,\"scene_gpu_ms_max\":%.3f,"
             "\"present_gpu_ms_avg\":%.3f,\"present_gpu_ms_max\":%.3f,\"prims_avg\":%.0f},"
             "\"wide_16_9\":{\"n\":%d,\"total_ms_avg\":%.3f,\"emu_cpu_ms_avg\":%.3f,"
             "\"scene_gpu_ms_avg\":%.3f,\"scene_gpu_ms_max\":%.3f,\"present_gpu_ms_avg\":%.3f,"
             "\"prims_avg\":%.0f,\"per_prim_us\":%.3f,"
             "\"mirror_gpu_ms_avg\":%.3f,\"mirror_gpu_ms_max\":%.3f,\"canon_gpu_ms_avg\":%.3f,"
             "\"mirror_passes_avg\":%.1f,\"mirror_pass_us\":%.3f,"
             "\"cpu_flush_ms_avg\":%.3f,\"cpu_wide_ms_avg\":%.3f,\"batches_avg\":%.1f,"
             "\"wide_sets_avg\":%.1f,\"fbo_creates_avg\":%.2f},"
             "\"a4_3\":{\"n\":%d,\"total_ms_avg\":%.3f,\"emu_cpu_ms_avg\":%.3f,"
             "\"scene_gpu_ms_avg\":%.3f,\"scene_gpu_ms_max\":%.3f,\"present_gpu_ms_avg\":%.3f,"
             "\"prims_avg\":%.0f,\"per_prim_us\":%.3f,"
             "\"cpu_flush_ms_avg\":%.3f,\"batches_avg\":%.1f}}",
             id, na, (int)wide[0], (int)n43[0], tex_frac, gl_renderer_get_ws_ablate(),
             (unsigned long long)br[0], (unsigned long long)br[1],
             (unsigned long long)br[2], (unsigned long long)br[3],
             (unsigned long long)br[4], (unsigned long long)br[5],
             (unsigned long long)br[6], (unsigned long long)br[7],
             all[1], all[2], all[3], all[4], all[5], all[6], all[7], all[8], all[9],
             (int)wide[0], wide[1], wide[3], wide[5], wide[6], wide[7], wide[9], wpp,
             wide[10], wide[11], wcanon, wide[12], wmpp,
             wide[13], wide[14], wide[15], wide[16], wide[17],
             (int)n43[0], n43[1], n43[3], n43[5], n43[6], n43[7], n43[9], npp,
             n43[13], n43[15]);
}

/* gl_ws_ablate mode=<0..3>: native-wide mirror ablation for perf attribution.
 * 0 normal | 1 skip mirror passes (incl. wide_clear) | 2 mirror state churn, no
 * draws | 3 mirror draws stay on the hr FBO (no per-pass FBO rebind; corrupts
 * content — probe only). No mode= just reports. */
static void handle_gl_ws_ablate(int id, const char *json)
{
    int mode = json_get_int(json, "mode", -1);
    if (mode >= 0) gl_renderer_set_ws_ablate(mode);
    send_fmt("{\"id\":%d,\"ok\":true,\"mode\":%d}", id, gl_renderer_get_ws_ablate());
}

static void handle_gl_interp(int id, const char *json)
{
    (void)json;
    int enabled = 0, suspended = 0, history = 0;
    double host_hz = 0.0, target_hz = 0.0;
    uint64_t swaps = 0;
    gl_renderer_interpolation_diag(&enabled, &suspended, &history,
                                   &host_hz, &target_hz, &swaps);
    send_fmt("{\"id\":%d,\"ok\":true,\"enabled\":%d,\"suspended\":%d,\"history\":%d,"
             "\"host_hz\":%.3f,\"target_hz\":%.3f,\"swaps\":%llu}",
             id, enabled, suspended, history, host_hz, target_hz,
             (unsigned long long)swaps);
}

/* gl_wide_fast on=<0|1>: native-wide centre-blit fast path. 1 (default) = skip
 * the redundant centre mirror and copy the canonical 4:3 frame into the wide
 * surface centre at present (fast). 0 = re-rasterize the whole wide surface
 * every prim (the original full-mirror path). For A/B perf + parity checks. */
extern void gl_renderer_set_wide_fast(int on);
extern int  gl_renderer_get_wide_fast(void);
static void handle_gl_wide_fast(int id, const char *json)
{
    int on = json_get_int(json, "on", -1);
    if (on >= 0) gl_renderer_set_wide_fast(on);
    send_fmt("{\"id\":%d,\"ok\":true,\"on\":%d}", id, gl_renderer_get_wide_fast());
}

/* Live GTE widescreen-squash toggle (diagnostic for 8C far-backdrop void):
 * ws_aspect num=<n> den=<d> calls gte_set_display_aspect at runtime so we can
 * compare squash ON (e.g. 16/9) vs OFF (1/1) in-place without a relaunch. */
extern void gte_set_display_aspect(int num, int den);
static void handle_ws_aspect(int id, const char *json)
{
    int num = json_get_int(json, "num", -1);
    int den = json_get_int(json, "den", -1);
    if (num <= 0 || den <= 0) { send_err(id, "need num>0 den>0 (4 3 = squash off)"); return; }
    gte_set_display_aspect(num, den);
    send_fmt("{\"id\":%d,\"ok\":true,\"num\":%d,\"den\":%d}", id, num, den);
}

/* Live native-wide vs squash toggle (A/B): ws_nw on=<0|1> re-engages the wide
 * path in the chosen mode without a relaunch. 2 = native-wide, 1 = squash. */
extern void psx_ws_set_native_wide(int on);
extern int  psx_ws_get_native_wide(void);
static void handle_ws_nw(int id, const char *json)
{
    int on = json_get_int(json, "on", -1);
    if (on >= 0) psx_ws_set_native_wide(on);
    GpuWsDebug ws;
    gpu_ws_get_debug(&ws);
    send_fmt("{\"id\":%d,\"ok\":true,\"native_wide\":%d,\"mode\":%d,\"nw_extra\":%d}",
             id, psx_ws_get_native_wide(), ws.mode, ws.nw_extra);
}

/* ws_backdrop_ring: dump the always-on auto_backdrop rewrite ring (which windows
 * fire, live extent/camera/DL-count, orig vs final bound). Read-only; small
 * heap envelope so the per-byte stall of a giant read is never in play. */
static void handle_ws_backdrop_ring(int id, const char *json)
{
    (void)json;
    size_t cap = 1u << 16;                    /* 64 KB: 512 entries * ~90 chars */
    char *buf = (char *)malloc(cap);
    if (!buf) { send_err(id, "alloc failed"); return; }
    int hdr  = snprintf(buf, cap, "{\"id\":%d,\"ok\":true,", id);
    int body = psx_ws_backdrop_ring_json(buf + hdr, (int)cap - hdr - 4);
    snprintf(buf + hdr + body, cap - (size_t)(hdr + body), "}");
    debug_server_send_line(buf);
    free(buf);
}

/* ws_ui_groups: dump the auto_ui_squash partition for the last UI prepass —
 * per primitive its op / key / raw key inputs (y, h, derived band and family) /
 * union-find root / final anchor.
 *
 * auto_ui_squash squashes each spatial run about its own anchor, so a HUD
 * element split across two runs gets two anchors and comes apart as the frame
 * widens (elements drifting to opposite edges, glyphs sliding off their
 * background box). Diagnosing that needed to know which run each primitive
 * landed in, and nothing exposed it: `key` is a hash, so unequal keys do not
 * say WHICH of CLUT/texpage/band/family differed, and `anchor` takes only three
 * values, so equal anchors do not prove two prims actually co-grouped.
 * Read-only; sized for the 2048-entry prepass cap. */
static void handle_ws_ui_groups(int id, const char *json)
{
    (void)json;
    size_t cap = 1u << 19;                 /* 512 KB: 2048 items * ~180 chars */
    char *buf = (char *)malloc(cap);
    if (!buf) { send_err(id, "alloc failed"); return; }
    int hdr  = snprintf(buf, cap, "{\"id\":%d,\"ok\":true,", id);
    int body = psx_ws_ui_groups_json(buf + hdr, (int)cap - hdr - 4);
    snprintf(buf + hdr + body, cap - (size_t)(hdr + body), "}");
    debug_server_send_line(buf);
    free(buf);
}

/* pgxp_depth [on=0/1]: depth-test using PGXP's recovered per-vertex W.
 *
 * Live, because this one changes WHICH PIXELS SURVIVE and the only honest test
 * is flipping it on the same scene. No args = report. */
/* zbuf <path>: dump the real depth buffer over the display rect as a 16-bit
 * greyscale PNG. Distinct from pgxp_depth zdebug, which paints every fragment
 * its own depth including semi-transparent ones that never write. */
static void handle_zbuf(int id, const char *json)
{
    extern int gl_renderer_read_depth(float *, int, int, int, int);
    extern int gr_scale(void);
    GpuDisplayInfo di; gpu_get_display_info(&di);
    if (di.disabled || !di.width || !di.height) { send_err(id, "display disabled"); return; }
    if (di.depth24) { send_err(id, "24bpp scanout"); return; }
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path))) { send_err(id, "missing path"); return; }
    int S = gr_scale(); if (S < 1) S = 1;
    uint32_t w = di.width > 640 ? 640 : di.width;
    uint32_t h = di.height > 512 ? 512 : di.height;
    uint32_t ow = w * (uint32_t)S, oh = h * (uint32_t)S;
    float *f = (float *)malloc((size_t)ow * oh * sizeof(float));
    if (!f) { send_err(id, "alloc failed"); return; }
    int got = gl_renderer_read_depth(f, (int)di.display_x, (int)di.display_y, (int)w, (int)h);
    if (got <= 0) { free(f); send_err(id, "no depth surface"); return; }
    /* Raw float32, row-major, bottom-up as GL returns it. No PNG: the consumer
     * is a histogram, and quantising to 8 or 16 bits would throw away exactly
     * the resolution this is being read to measure. */
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(f); send_err(id, "open failed"); return; }
    const size_t wrote = fwrite(f, sizeof(float), (size_t)ow * oh, fp);
    fclose(fp); free(f);
    if (wrote != (size_t)ow * oh) { send_err(id, "short write"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"w\":%u,\"h\":%u,\"scale\":%d}",
             id, path, ow, oh, S);
}

static void handle_pgxp_depth(int id, const char *json)
{
    const int on = json_get_int(json, "on", -1);
    extern void  gl_renderer_set_pgxp_zscale(float s);
    extern float gl_renderer_pgxp_zscale(void);
    const int zs = json_get_int(json, "zscale", -1);
    if (zs > 0) gl_renderer_set_pgxp_zscale((float)zs);
    extern void gl_renderer_set_zdebug(int on);
    const int zdbg = json_get_int(json, "zdebug", -1);
    if (zdbg >= 0) gl_renderer_set_zdebug(zdbg);
    extern void gl_renderer_set_depth_always(int on);
    const int always = json_get_int(json, "always", -1);
    if (always >= 0) gl_renderer_set_depth_always(always);
    if (on >= 0) gl_renderer_set_pgxp_depth(on);
    /* Report the Z distribution alongside the flag: choosing znear without it
     * is guesswork, and guessing it wrong is what broke this twice. */
    extern void gl_renderer_pgxp_zstats(float *, float *, double *,
                                        unsigned long long *,
                                        unsigned long long *, int);
    float zmn = 0.0f, zmx = 0.0f; double zmean = 0.0;
    unsigned long long zn = 0, hist[20] = {0};
    gl_renderer_pgxp_zstats(&zmn, &zmx, &zmean, &zn, hist, 20);
    char hb[256]; int ho = 0;
    for (int i = 0; i < 20 && ho < (int)sizeof(hb) - 24; i++)
        ho += snprintf(hb + ho, sizeof(hb) - (size_t)ho, "%s%llu",
                       i ? "," : "", hist[i]);
    send_fmt("{\"id\":%d,\"ok\":true,\"on\":%d,\"zmin\":%.1f,\"zmax\":%.1f,"
             "\"zmean\":%.1f,\"zn\":%llu,\"zhist\":[%s]}",
             id, gl_renderer_pgxp_depth(), zmn, zmx, zmean, zn, hb);
}

/* ws_menu_edge_fill [on=0/1]: pillarbox edge fill for 4:3-pinned presents.
 *
 * The 3D-gated widescreen layer keeps 2D screens (title, menus, loading) at
 * 4:3, which on a very wide display leaves them as an island in a black
 * window. With this on, the frame's own edge columns are extended across the
 * margins instead. It is a look, not a correctness fix — it smears where the
 * edge pixels are busy — so it needs a live A/B rather than a rebuild.
 * No args = report. */
static void handle_ws_menu_edge_fill(int id, const char *json)
{
    extern int g_ws_menu_edge_fill_flag;
    int on = json_get_int(json, "on", -1);
    if (on >= 0) g_ws_menu_edge_fill_flag = on ? 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"on\":%d}", id, g_ws_menu_edge_fill_flag);
}

/* tex_pack [sub=stats|uploads|dumped|missing]: HD texture replacement state.
 *
 * tex_pack.cpp has carried tex_pack_debug_json since it was written, but it was
 * never reachable from the wire -- so "is the pack actually matching?" could
 * only be answered by looking at the screen and guessing. That is precisely the
 * question that decides whether a Beetle-authored pack is compatible at all,
 * and "missing" (pack entries no draw has claimed yet) is the authoring aid for
 * building a new one. Defaults to stats. */
static void handle_tex_pack(int id, const char *json)
{
    /* repl=0/1 toggles substitution live. An A/B that needs a rebuild is an
     * A/B that does not get run. */
    const int dbg = json_get_int(json, "repl_debug", -1);
    if (dbg >= 0) {
        extern int g_repl_debug;
        g_repl_debug = dbg ? 1 : 0;
        send_fmt("{\"id\":%d,\"ok\":true,\"repl_debug\":%d}", id, g_repl_debug);
        return;
    }
    const int repl = json_get_int(json, "repl", -1);
    if (repl >= 0) {
        send_fmt("{\"id\":%d,\"ok\":true,\"repl\":%d}",
                 id, tex_pack_replace_enabled(repl));
        return;
    }

    char sub[32];
    if (!json_get_str(json, "sub", sub, sizeof(sub)))
        strncpy(sub, "stats", sizeof(sub) - 1);
    sub[sizeof(sub) - 1] = '\0';

    const int cap = 1 << 16;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) { send_err(id, "alloc failed"); return; }
    /* tex_pack_debug_json emits a complete JSON VALUE — an object for "stats",
     * an array for the others — so it has to be given a key, not spliced in as
     * bare fields. */
    int hdr = snprintf(buf, (size_t)cap,
                       "{\"id\":%d,\"ok\":true,\"active\":%d,\"%s\":",
                       id, tex_pack_active(), sub);
    int body = tex_pack_debug_json(sub, buf + hdr, cap - hdr - 4);
    if (body <= 0) { free(buf); send_err(id, "unknown sub"); return; }
    snprintf(buf + hdr + body, 4, "}");
    debug_server_send_line(buf);
    free(buf);
}

/* ws_backdrop_margin [m=<N>]: live-tune the far-backdrop widen strategy without
 * a rebuild. m<0 = whole-row preload, m=0 = off, m>0 = widen N columns each side.
 * No m= just reports the current value. */
static void handle_ws_backdrop_margin(int id, const char *json)
{
    int m = json_get_int(json, "m", -123456789);   /* sentinel = no change */
    if (m != -123456789) g_ws_bd_margin = m;
    send_fmt("{\"id\":%d,\"ok\":true,\"margin\":%d,\"mode\":\"%s\"}",
             id, g_ws_bd_margin,
             g_ws_bd_margin < 0 ? "whole-row" : (g_ws_bd_margin == 0 ? "off" : "widen-cols"));
}

/* ws_backdrop_stretch [on=0/1] [pct=N] [thresh=N]: live-tune the native-wide
 * 2D-backdrop x-stretch (GL renderer). on toggles the feature; pct=0 auto-fits
 * (g_wide_w/native_w), else pct/100 is the scale; thresh = px past 4:3 that ends
 * the per-frame backdrop phase. No args = report. */
static void handle_ws_backdrop_stretch(int id, const char *json)
{
    extern int g_ws_bd_stretch_on, g_ws_bd_stretch_pct, g_ws_bd_phase_thresh, g_ws_bd_phase_mode;
    extern int g_bdg_applied, g_bdg_prims, g_bdg_clearx, g_bdg_cur, g_bdg_base, g_bdg_w, g_bdg_off;
    extern uint32_t g_ws_backdrop_lo, g_ws_backdrop_hi, g_bdg_src_lo, g_bdg_src_hi;
    int on  = json_get_int(json, "on", -1);
    int pct = json_get_int(json, "pct", -1);
    int th  = json_get_int(json, "thresh", -1);
    int md  = json_get_int(json, "mode", -1);
    if (on  >= 0) g_ws_bd_stretch_on   = on;
    if (pct >= 0) g_ws_bd_stretch_pct  = pct;
    if (th  >= 0) g_ws_bd_phase_thresh = th;
    if (md  >= 0) g_ws_bd_phase_mode   = md;
    send_fmt("{\"id\":%d,\"ok\":true,\"on\":%d,\"pct\":%d,\"thresh\":%d,\"mode\":%d,"
             "\"dbg\":{\"applied\":%d,\"prims\":%d,\"wide_cur\":%d,\"base\":%d,\"wide_w\":%d,\"off\":%d,"
             "\"bd_lo\":\"%08x\",\"bd_hi\":\"%08x\",\"src_lo\":\"%08x\",\"src_hi\":\"%08x\"}}",
             id, g_ws_bd_stretch_on, g_ws_bd_stretch_pct, g_ws_bd_phase_thresh, g_ws_bd_phase_mode,
             g_bdg_applied, g_bdg_prims, g_bdg_cur, g_bdg_base, g_bdg_w, g_bdg_off,
             g_ws_backdrop_lo, g_ws_backdrop_hi, g_bdg_src_lo, g_bdg_src_hi);
}

/* prim<->pixel correlation gate (ws_dbg_stretch). Forces g_ws_bd_stretch_on=1 and
 * sets the selectable match mode used by the GL native-wide 2D-stretch gate so a
 * probe can stretch an exact prim set and screenshot which one fills the void.
 * Args: mode (0..8), lo, hi (OT addrs, hex via _hex fields or decimal), clut.
 * Reports matched/tagged counts (last frame) + applied. */
static void handle_ws_dbg_stretch(int id, const char *json)
{
    extern int g_ws_bd_stretch_on, g_ws_bd_stretch_pct, g_ws_bd_phase_mode;
    extern int g_bdg_applied, g_dbg_mode, g_dbg_match_n, g_dbg_match_tagged;
    extern uint32_t g_dbg_lo, g_dbg_hi;
    extern unsigned short g_dbg_clut;
    int mode = json_get_int(json, "mode", -1);
    int pct  = json_get_int(json, "pct", -1);
    char s[40];
    if (mode >= 0) {
        g_dbg_mode = mode;
        g_ws_bd_stretch_on = 1;            /* ensure the stretch is enabled */
        g_ws_bd_phase_mode = 1;            /* route bd_prim_gate -> psx_ws_prim_in_backdrop */
    }
    if (pct >= 0) g_ws_bd_stretch_pct = pct;
    if (json_get_str(json, "lo", s, sizeof(s)))   g_dbg_lo   = hex_to_u32(s) & 0x1FFFFFFFu;
    if (json_get_str(json, "hi", s, sizeof(s)))   g_dbg_hi   = hex_to_u32(s) & 0x1FFFFFFFu;
    if (json_get_str(json, "clut", s, sizeof(s))) g_dbg_clut = (unsigned short)hex_to_u32(s);
    send_fmt("{\"id\":%d,\"ok\":true,\"mode\":%d,\"on\":%d,\"pct\":%d,"
             "\"lo\":\"%08x\",\"hi\":\"%08x\",\"clut\":\"%04x\","
             "\"matched\":%d,\"matched_tagged\":%d,\"applied\":%d}",
             id, g_dbg_mode, g_ws_bd_stretch_on, g_ws_bd_stretch_pct,
             g_dbg_lo, g_dbg_hi, g_dbg_clut, g_dbg_match_n, g_dbg_match_tagged, g_bdg_applied);
}

/* 8C far-backdrop depth split. ws_far_threshold [t=<SZ>] sets the SZ cutoff
 * above which backdrop-driver geometry is un-squashed (near props stay
 * squashed). With no t=, just reports the observed SZ stats since last read so
 * the threshold can be set from data. */
extern void gte_ws_set_far_threshold(int t);
extern int  gte_ws_get_far_threshold(void);
extern void gte_ws_get_sz_stats(int* mn, int* mx, unsigned* n, unsigned* far_n);
static void handle_ws_far_threshold(int id, const char *json)
{
    int t = json_get_int(json, "t", -123456789);  /* sentinel = no change */
    if (t != -123456789) gte_ws_set_far_threshold(t);
    int mn = 0, mx = 0; unsigned n = 0, far_n = 0;
    gte_ws_get_sz_stats(&mn, &mx, &n, &far_n);
    send_fmt("{\"id\":%d,\"ok\":true,\"threshold\":%d,\"sz_min\":%d,\"sz_max\":%d,\"sz_n\":%u,\"sz_far\":%u}",
             id, gte_ws_get_far_threshold(), mn, mx, n, far_n);
}

/* ws_dome on=<0|1> [num=<W> den=<H>]: native-wide sky-DOME expand. Scales far-
 * depth (SZ >= ws_far_threshold) GTE X outward from the projection centre by
 * (3*num)/(4*den) so a finite sky dome grows to fill the wider FOV. Tune the
 * depth split with ws_far_threshold (its sz_far count = verts being expanded). */
extern void gte_ws_set_dome_expand(int on, int aspect_num, int aspect_den);
static void handle_ws_dome(int id, const char *json)
{
    int on  = json_get_int(json, "on", 1);
    int num = json_get_int(json, "num", 16);
    int den = json_get_int(json, "den", 9);
    gte_ws_set_dome_expand(on, num, den);
    send_fmt("{\"id\":%d,\"ok\":true,\"on\":%d,\"num\":%d,\"den\":%d}", id, on ? 1 : 0, num, den);
}

/* ws_dome_probe on=<0|1> [thr=<SZ>] | dump: tally which guest function projects
 * far (SZ>=thr) vertices -> identifies the sky-dome draw fn (top far emitter,
 * highest max_sz) for the per-function dome-expand bracket. Arm with on=1, let a
 * dome frame render, then call with no args to dump the tally. */
extern void gte_dome_probe(int on, int thr);
extern int  gte_dome_probe_dump(uint32_t* funcs, uint32_t* counts, int32_t* maxsz, int cap);
static void handle_ws_dome_probe(int id, const char *json)
{
    int on = json_get_int(json, "on", -1);
    int thr = json_get_int(json, "thr", -1);
    if (on >= 0) gte_dome_probe(on, thr);
    uint32_t funcs[48], counts[48]; int32_t maxsz[48];
    int n = gte_dome_probe_dump(funcs, counts, maxsz, 48);
    /* simple insertion-sort by count desc for readability */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && counts[j] > counts[j-1]; j--) {
            uint32_t tf=funcs[j];funcs[j]=funcs[j-1];funcs[j-1]=tf;
            uint32_t tc=counts[j];counts[j]=counts[j-1];counts[j-1]=tc;
            int32_t tm=maxsz[j];maxsz[j]=maxsz[j-1];maxsz[j-1]=tm;
        }
    char buf[4096]; int p = snprintf(buf, sizeof buf, "{\"id\":%d,\"ok\":true,\"n\":%d,\"funcs\":[", id, n);
    for (int i = 0; i < n && p < (int)sizeof(buf)-80; i++)
        p += snprintf(buf+p, sizeof(buf)-p, "%s{\"ra\":\"0x%08X\",\"n\":%u,\"max_sz\":%d}",
                      i?",":"", funcs[i], counts[i], maxsz[i]);
    snprintf(buf+p, sizeof(buf)-p, "]}");
    debug_server_send_line(buf);
}

static void handle_ws_census(int id, const char *json)
{
    char act[16] = {0};
    json_get_str(json, "action", act, sizeof(act));
    if (strcmp(act, "on") == 0)  { gpu_ws_census_set(1); send_fmt("{\"id\":%d,\"ok\":true,\"on\":1,\"seq\":%llu}", id, (unsigned long long)gpu_ws_census_seq()); return; }
    if (strcmp(act, "off") == 0) { gpu_ws_census_set(0); send_fmt("{\"id\":%d,\"ok\":true,\"on\":0,\"seq\":%llu}", id, (unsigned long long)gpu_ws_census_seq()); return; }
    /* default action = dump */
    int f0 = json_get_int(json, "start", -1);
    int f1 = json_get_int(json, "end", -1);
    if (f0 < 0 || f1 < 0) { send_err(id, "missing start/end (or action on|off)"); return; }
    char path[256];
    if (!json_get_str(json, "out", path, sizeof(path)))
        snprintf(path, sizeof(path), "psx_census.csv");
    int n = gpu_ws_census_dump((uint32_t)f0, (uint32_t)f1, path);
    if (n < 0) { send_err(id, "census dump: cannot open file"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"rows\":%d,\"path\":\"%s\",\"seq\":%llu}",
             id, n, path, (unsigned long long)gpu_ws_census_seq());
}

static void handle_mmx6_freshfix(int id, const char *json)
{
    extern void gpu_ws_mmx6_set_freshfix(int on);
    extern int  gpu_ws_mmx6_freshfix_get(void);
    extern long gpu_ws_mmx6_refill_cols(void);
    extern int  gpu_ws_mmx6_validate(int *bad_out);
    int on = json_get_int(json, "on", -1);
    if (on >= 0) gpu_ws_mmx6_set_freshfix(on);
    int total = 0, bad = -1;
    if (json_get_int(json, "validate", 0) > 0) total = gpu_ws_mmx6_validate(&bad);
    send_fmt("{\"id\":%d,\"ok\":true,\"freshfix\":%d,\"last_refill_cols\":%ld,"
             "\"validate_total\":%d,\"validate_bad\":%d}",
             id, gpu_ws_mmx6_freshfix_get(), gpu_ws_mmx6_refill_cols(), total, bad);
}

/* Save-state save/load via the debug server. Player-facing hotkeys route
 * through the F7 save-state menu; this command keeps the flow headless.
 * {"cmd":"savestate","op":"save"|
 * "load","slot":N}. The request is staged and runs at the next block boundary
 * (savestate_poll); a load unwinds the guest, so the ack is sent before it. */
static void handle_savestate(int id, const char *json)
{
    extern int savestate_request_save(int slot);
    extern int savestate_request_load(int slot);
    extern int psx_netplay_active(void);
    extern int psx_netplay_is_host(void);
    extern int psx_netplay_request_save(int slot);
    extern int psx_netplay_request_load(int slot);
    int slot = json_get_int(json, "slot", -1);
    if (slot < 0) { send_err(id, "missing slot"); return; }
    char op[16];
    if (!json_get_str(json, "op", op, sizeof(op))) { send_err(id, "missing op"); return; }
    int staged;
    if (strcmp(op, "save") && strcmp(op, "load")) {
        send_err(id, "op must be save|load");
        return;
    }
    if (psx_netplay_active()) {
        if (!psx_netplay_is_host()) {
            send_err(id, "savestate refused (netplay guest; host-only)");
            return;
        }
        staged = !strcmp(op, "save") ? psx_netplay_request_save(slot)
                                     : psx_netplay_request_load(slot);
    } else if (!strcmp(op, "save")) {
        staged = savestate_request_save(slot);
    } else {
        staged = savestate_request_load(slot);
    }
    if (!staged) {
        /* Refused: bad slot, not configured, load on LLE, or netplay busy. */
        send_err(id, "savestate request refused (LLE run cannot load states; "
                     "check slot / configuration / netplay host)");
        return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"op\":\"%s\",\"slot\":%d}", id, op, slot);
}

/* Live CPU overclock for anchored A/B: animation-rate artifacts cannot be
 * attributed to the overclock without flipping it on the exact same scene. */
/* Census of imperfect PGXP triangles for the last complete frame: which
 * screen prims are NOT fully dataflow-corrected, and why, per vertex. */
static void handle_pgxp_census(int id, const char *json)
{
    (void)json;
    static PgxpCensusEnt ents[8192];
    uint32_t frame = 0, tris = 0, clean = 0;
    int n = gpu_pgxp_census_dump(&frame, &tris, &clean, ents, 8192);
    size_t buf_sz = 256 + (size_t)n * 96u;
    char *buf = (char *)malloc(buf_sz);
    if (!buf) { send_err(id, "alloc failed"); return; }
    size_t pos = (size_t)snprintf(buf, buf_sz,
        "{\"id\":%d,\"ok\":true,\"frame\":%u,\"tris\":%u,\"clean\":%u,"
        "\"imperfect\":%d,\"entries\":[", id, frame, tris, clean, n);
    for (int i = 0; i < n && pos < buf_sz - 128; i++) {
        PgxpCensusEnt *e = &ents[i];
        pos += (size_t)snprintf(buf + pos, buf_sz - pos,
            "%s{\"bb\":[%d,%d,%d,%d],\"cls\":[%u,%u,%u],\"src\":\"0x%08X\"}",
            i ? "," : "", e->x0, e->y0, e->x1, e->y1,
            e->cls[0], e->cls[1], e->cls[2], e->src);
    }
    snprintf(buf + pos, buf_sz - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

static void handle_pgxp_texcensus(int id, const char *json)
{
    (void)json;
    static PgxpCensusEnt ents[8192];
    uint32_t frame = 0;
    int n = gpu_pgxp_texcensus_dump(&frame, ents, 8192);
    size_t buf_sz = 256 + (size_t)n * 96u;
    char *buf = (char *)malloc(buf_sz);
    if (!buf) { send_err(id, "alloc failed"); return; }
    size_t pos = (size_t)snprintf(buf, buf_sz,
        "{\"id\":%d,\"ok\":true,\"frame\":%u,\"misses\":%d,\"entries\":[",
        id, frame, n);
    for (int i = 0; i < n && pos < buf_sz - 128; i++) {
        PgxpCensusEnt *e = &ents[i];
        pos += (size_t)snprintf(buf + pos, buf_sz - pos,
            "%s{\"bb\":[%d,%d,%d,%d],\"reason\":%u,\"vtx\":%u,\"why\":%u,\"src\":\"0x%08X\"}",
            i ? "," : "", e->x0, e->y0, e->x1, e->y1,
            e->cls[0], e->cls[1], e->cls[2], e->src);
    }
    snprintf(buf + pos, buf_sz - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

static void handle_overclock(int id, const char *json)
{
    int pct = json_get_int(json, "percent", -1);
    if (pct > 0)
        psx_set_cpu_overclock((uint32_t)pct);
    send_fmt("{\"id\":%d,\"ok\":true,\"percent\":%u}",
             id, psx_get_cpu_overclock());
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
#include "png_write.h"   /* png_write_rgb + zlib/CRC helpers (shared with Beetle) */

/* Canonical screenshot: writes an 8-bit RGB PNG of the current PSX display to
 * "path" (default psx_screenshot.png in the runtime cwd) and answers with a
 * single metadata line. Registered as "screenshot_file"; the user-facing
 * "screenshot" command selects the presented native-wide surface when one is
 * active and falls back to this canonical capture at 4:3.
 * the old "screenshot" inline-hex-row variant streamed h+1 response lines
 * per request, which violated the one-request/one-response protocol and
 * poisoned every client connection that used it. */
/* ---- Always-on display ring ----------------------------------------------
 * The last DISP_RING_CAP vblanks' display areas, raw 15-bit VRAM halfwords,
 * captured in debug_server_record_frame (ring-buffer rule: continuous
 * capture, observers query a window after the fact). Purpose: FRAME-EXACT
 * screenshots. `screenshot_file` returns whatever is displayed when the
 * command lands — 1-3 frames after the `frame` query that named it — which
 * makes cross-renderer same-frame diffing impossible. With this ring, two
 * deterministic runs (GL vs software) can each serve the display for the
 * SAME frame number, giving a pixel-exact renderer-divergence census.
 * GL reads GPU-side truth via the fbo peek; software reads CPU VRAM (its
 * authoritative surface). depth24 scanout frames are tagged and refused
 * (15-bit only). */
#define DISP_RING_CAP   64    /* full-VRAM entries are 1MB each; 32 frames of
                               * lookback (~0.3-0.5s) for exact-frame forensics */
#define DISP_RING_MAX_W 640
#define DISP_RING_MAX_H 256
/* Full-VRAM aux capture alongside the display: the entire 1024x512 raw VRAM
 * as the renderer sees it at this frame (GL: FBO truth; SW: CPU truth), so an
 * offline client can micro-cosim ANY prim of the frame — rasterize it from
 * the GP0 ring against the exact same-frame texels/CLUTs — and attribute a
 * bad pixel to sampling vs content. Streamed texture pages and palette
 * cycling make later peeks worthless; only a same-frame capture is evidence. */
typedef struct {
    uint32_t frame;
    uint16_t w, h;
    uint8_t  depth24, valid;
    uint16_t *px;                 /* DISP_RING_MAX_W*DISP_RING_MAX_H halfwords */
    uint16_t *vram;               /* full 1024x512 */
} DispRingEntry;
static PSX_BSS DispRingEntry s_disp_ring[DISP_RING_CAP];
static uint16_t     *s_disp_ring_px = NULL;   /* one block for all entries */

static void disp_ring_capture(void)
{
    /* Full-VRAM GL readback is intentionally expensive and can perturb the
     * performance issue being measured. Keep the forensic ring default-on, but
     * allow a controlled acceptance run to disable continuous capture while
     * retaining the TCP server and one-shot screenshot command. */
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PSX_DISPLAY_RING");
        enabled = (!e || !*e || *e != '0') ? 1 : 0;
    }
    if (!enabled) return;
    if (!s_disp_ring_px) {
        size_t per  = (size_t)DISP_RING_MAX_W * DISP_RING_MAX_H;
        size_t vram = (size_t)1024 * 512;
        s_disp_ring_px = (uint16_t *)malloc(DISP_RING_CAP * (per + vram) * sizeof(uint16_t));
        if (!s_disp_ring_px) return;
        for (int i = 0; i < DISP_RING_CAP; i++) {
            uint16_t *base = s_disp_ring_px + (size_t)i * (per + vram);
            s_disp_ring[i].px   = base;
            s_disp_ring[i].vram = base + per;
        }
    }
    DispRingEntry *e = &s_disp_ring[(uint32_t)(s_frame_count % DISP_RING_CAP)];
    e->frame = (uint32_t)s_frame_count;
    e->valid = 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.width == 0 || di.height == 0) return;
    uint32_t w = di.width, h = di.height;
    if (w > DISP_RING_MAX_W) w = DISP_RING_MAX_W;
    if (h > DISP_RING_MAX_H) h = DISP_RING_MAX_H;
    e->w = (uint16_t)w; e->h = (uint16_t)h;
    e->depth24 = (uint8_t)(di.depth24 ? 1 : 0);
    /* GPU-side truth when the GL raster pipeline is live (scissored pack of
     * just the display rect + one small readback); CPU VRAM otherwise (the
     * software backend's authoritative surface). Runs on the present thread,
     * where the GL context is current. */
    extern int gl_renderer_fbo_peek(int x, int y, int w_, int h_, uint16_t *out);
    int got = 0;
    if (di.display_x + w <= 1024 && di.display_y + h <= 512)
        got = gl_renderer_fbo_peek((int)di.display_x, (int)di.display_y,
                                   (int)w, (int)h, e->px);
    if (!got) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                e->px[y * w + x] =
                    gpu_vram_peek((int)(di.display_x + x), (int)(di.display_y + y));
    }
    /* Full-VRAM aux capture (same GL-truth/CPU-truth split as the display). */
    if (!gl_renderer_fbo_peek(0, 0, 1024, 512, e->vram)) {
        const uint16_t *v = gpu_get_vram();
        memcpy(e->vram, v, (size_t)1024 * 512 * sizeof(uint16_t));
    }
    e->valid = 1;
}

/* Dump an entry's full-VRAM capture as a raw little-endian u16 blob
 * (1024x512 row-major). */
static void handle_display_ring_aux(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path))) {
        send_err(id, "missing path"); return;
    }
    if (!s_disp_ring_px) { send_err(id, "display ring not started"); return; }
    DispRingEntry *e = &s_disp_ring[(uint32_t)((uint64_t)f % DISP_RING_CAP)];
    if (!e->valid || e->frame != (uint32_t)f) {
        send_err(id, "frame not in display ring"); return;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) { send_err(id, "cannot open file"); return; }
    size_t n1 = fwrite(e->vram, sizeof(uint16_t), (size_t)1024 * 512, fp);
    fclose(fp);
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%d,\"path\":\"%s\","
             "\"vram_words\":%u}", id, f, path, (unsigned)n1);
}

static void handle_display_ring_stats(int id, const char *json)
{
    (void)json;
    uint32_t oldest = 0, newest = 0;
    int n = 0;
    for (int i = 0; i < DISP_RING_CAP; i++) {
        if (!s_disp_ring_px || !s_disp_ring[i].valid) continue;
        uint32_t f = s_disp_ring[i].frame;
        if (n == 0 || f < oldest) oldest = f;
        if (n == 0 || f > newest) newest = f;
        n++;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"capacity\":%d,\"valid\":%d,"
             "\"oldest_frame\":%u,\"newest_frame\":%u}",
             id, DISP_RING_CAP, n, oldest, newest);
}

static void handle_display_ring_get(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path))) {
        send_err(id, "missing path"); return;
    }
    if (!s_disp_ring_px) { send_err(id, "display ring not started"); return; }
    DispRingEntry *e = &s_disp_ring[(uint32_t)((uint64_t)f % DISP_RING_CAP)];
    if (!e->valid || e->frame != (uint32_t)f) {
        send_err(id, "frame not in display ring"); return;
    }
    if (e->depth24) { send_err(id, "frame is 24bpp scanout (unsupported)"); return; }
    uint32_t w = e->w, h = e->h;
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) { send_err(id, "alloc failed"); return; }
    for (uint32_t i = 0; i < w * h; i++) {
        uint16_t p = e->px[i];
        rgb[i * 3 + 0] = (uint8_t)((p & 0x1F) << 3);
        rgb[i * 3 + 1] = (uint8_t)(((p >> 5) & 0x1F) << 3);
        rgb[i * 3 + 2] = (uint8_t)(((p >> 10) & 0x1F) << 3);
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(rgb); send_err(id, "cannot open file"); return; }
    int ok = png_write_rgb(fp, rgb, w, h);
    free(rgb);
    fclose(fp);
    if (!ok) { send_err(id, "png encode failed"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%d,\"path\":\"%s\","
             "\"width\":%u,\"height\":%u}", id, f, path, w, h);
}

static void handle_screenshot_file(int id, const char *json)
{
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.width == 0 || di.height == 0) {
        send_err(id, "display disabled"); return;
    }
    /* Under the OpenGL FBO-present path, CPU VRAM can be stale (the FBO holds
     * the freshest frame and is presented without a readback). Sync it down so
     * the capture reflects what's on screen. NEVER sync on depth24: FBO
     * readback clobbers packed RGB888 MDEC bytes in the CPU mirror. */
    if (!di.depth24) {
        extern void gl_renderer_sync_cpu(void);
        gl_renderer_sync_cpu();
        extern void vk_renderer_sync_cpu(void);
        vk_renderer_sync_cpu();
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

/* screenshot_hires — capture the SUPERSAMPLED surface, not native VRAM.
 *
 * screenshot_file above reads gpu_display_pixel_rgb, i.e. the native 15-bit
 * VRAM the PS1 would have produced. That is the right capture for faithfulness
 * work, but it is BLIND to any enhancement that lives only in the high-
 * resolution mirror: at [video] supersampling >= 2 the geometry-correction
 * sub-pixel vertices, the SSAA edges and the perspective UVs are all resolved
 * in the hi-res surface and are gone by the time pixels are packed back to
 * native. Verifying those against a native screenshot silently "confirms" a
 * clean frame while the player is looking at a broken one — which is exactly
 * how the geometry-correction cracking got mis-reported as fixed.
 *
 * This routes the same present path the window uses (gr_render_display_hires),
 * so what lands in the PNG is what the player sees. Falls back to the native
 * resolve when no hi-res surface exists (supersampling 1 / software), so the
 * command always answers with something truthful about the actual output. */
static void handle_screenshot_hires(int id, const char *json)
{
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.width == 0 || di.height == 0) {
        send_err(id, "display disabled"); return;
    }
    if (di.depth24) { send_err(id, "24bpp scanout (unsupported)"); return; }

    int scale = gr_scale();
    if (scale < 1) scale = 1;
    uint32_t w = di.width, h = di.height;
    if (w > 640) w = 640;
    if (h > 512) h = 512;
    uint32_t ow = w * (uint32_t)scale, oh = h * (uint32_t)scale;

    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        strncpy(path, "psx_screenshot_hires.png", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    /* calloc, not malloc: any pixel a resolve declines to touch must be a
     * deterministic black, never whatever the allocator handed back. */
    uint32_t *argb = (uint32_t *)calloc((size_t)ow * oh, sizeof(uint32_t));
    if (!argb) { send_err(id, "alloc failed"); return; }
    /* Pitch is BYTES, not pixels: the resolves step rows with
     * (uint8_t *)out + row * out_pitch (gpu_sw_renderer.c sw_render_display /
     * sw_render_display_hires), and the present path passes it that way
     * (main.cpp gr_render_display_hires(sdl_pixel_buf, sw * sizeof(uint32_t))).
     * Passing the pixel width here stepped 1/4 of a row per row, which tiled
     * the frame 4x across and left the bottom three quarters black. */
    int got = gr_render_display_hires(argb, (int)(ow * sizeof(uint32_t)),
                                      (int)di.display_x,
                                      (int)di.display_y, (int)w, (int)h);
    /* The resolves return the pixel COUNT they wrote, and a partial cover is a
     * real case: gr_scale() reports the GL backend's internal scale, but
     * sw_render_display_hires falls back to the native resolve when the CPU
     * hi-res mirror does not exist (gpu_sw_renderer.c: !g_hr || g_scale <= 1).
     * That fills w*h of an ow*oh buffer and still returns non-zero, so testing
     * `!got` alone would emit a PNG that is mostly untouched allocation. Demand
     * full cover, else redo it honestly at native size. */
    if (got < (int)((size_t)ow * oh)) {
        /* No hi-res surface (scale 1, a backend without one, or a partial
         * cover): resolve the native display instead and say so, rather than
         * emitting a blank. */
        scale = 1; ow = w; oh = h;
        got = gr_render_display(argb, (int)(ow * sizeof(uint32_t)),
                                (int)di.display_x,
                                (int)di.display_y, (int)w, (int)h);
        if (got < (int)((size_t)ow * oh)) {
            free(argb); send_err(id, "no display surface"); return;
        }
    }

    uint8_t *rgb = (uint8_t *)malloc((size_t)ow * oh * 3);
    if (!rgb) { free(argb); send_err(id, "alloc failed"); return; }
    for (size_t i = 0; i < (size_t)ow * oh; i++) {
        uint32_t p = argb[i];
        rgb[i * 3 + 0] = (uint8_t)((p >> 16) & 0xFF);
        rgb[i * 3 + 1] = (uint8_t)((p >> 8) & 0xFF);
        rgb[i * 3 + 2] = (uint8_t)(p & 0xFF);
    }
    free(argb);

    FILE *f = fopen(path, "wb");
    if (!f) { free(rgb); send_err(id, "cannot open file"); return; }
    int ok = png_write_rgb(f, rgb, ow, oh);
    free(rgb);
    fclose(f);
    if (!ok) { send_err(id, "png encode failed"); return; }

    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%u,"
             "\"height\":%u,\"scale\":%d}", id, path, ow, oh, scale);
}

/* present_shot — PNG of the COMPOSED renderer output: the frame after SDL fits
 * the display buffer into the logical surface, i.e. at the aspect the player is
 * actually looking at.
 *
 * The three buffer-level captures above (screenshot / screenshot_file /
 * screenshot_hires) all resolve the display buffer BEFORE that fit. On a 508x256
 * display in a 4:3 window they answer 508x256 while the window shows 640x480 —
 * the same pixels at a different shape. That is correct for faithfulness work
 * and WRONG for anything aspect-shaped: a widescreen change alters the GTE
 * squash and the present fit, so validating it against a pre-fit buffer measures
 * the one stage the change does not touch.
 *
 * Staged and fulfilled in the present path (see present_shot_request in
 * main.cpp), so the ack means "queued", not "written" — the PNG lands on the
 * next present. Sample `present_shot_seq` before staging and poll it until the
 * counter moves; `wrote` in that reply says whether a file actually landed.
 *
 * Refused up front on headless (no present surface) and on the Vulkan backend,
 * which presents through its own swapchain and has no readback hook — accepting
 * there would leave a request nothing can ever fulfil. */
static void handle_present_shot(int id, const char *json)
{
    extern int present_shot_request(const char *path);
    extern int present_shot_seq(void);
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        strncpy(path, "psx_present_shot.png", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    if (!present_shot_request(path)) {
        send_err(id, "present_shot unavailable (headless, or the Vulkan backend "
                     "which has no present readback)");
        return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"staged\":true,\"seq\":%d}",
             id, path, present_shot_seq());
}

/* present_shot_seq — completion counter for the staged capture above. Sample it
 * before present_shot and poll until it changes; the counter advances on every
 * completion, success or not, so the poll always terminates. `wrote` reports
 * whether that completion actually produced a PNG. */
static void handle_present_shot_seq(int id, const char *json)
{
    extern int present_shot_seq(void);
    extern int present_shot_ok(void);
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"seq\":%d,\"wrote\":%d}",
             id, present_shot_seq(), present_shot_ok());
}

/* dump_buffer: dump a raw 512x240 VRAM region starting at display Y = `y` to a
 * PNG, regardless of what the game currently displays. Used to inspect BOTH
 * double-buffer halves (y=0 and y=256) coherently in one call to see whether a
 * strobing character is present in one buffer only. */
static void handle_dump_buffer(int id, const char *json)
{
    extern void gl_renderer_sync_cpu(void);
    gl_renderer_sync_cpu();
    extern void vk_renderer_sync_cpu(void);
    vk_renderer_sync_cpu();

    int y0 = json_get_int(json, "y", 0);
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        strncpy(path, "psx_buffer.png", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    GpuDisplayInfo di;
    di.display_x = 0; di.display_y = (uint32_t)y0; di.depth24 = 0;
    di.disabled = 0; di.width = 512; di.height = 240;

    uint32_t w = 512, h = 240;
    FILE *f = fopen(path, "wb");
    if (!f) { send_err(id, "cannot open file"); return; }
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) { fclose(f); send_err(id, "alloc failed"); return; }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, b;
            gpu_display_pixel_rgb(&di, x, y, &r, &g, &b);
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = r; p[1] = g; p[2] = b;
        }
    int ok = png_write_rgb(f, rgb, w, h);
    free(rgb); fclose(f);
    if (!ok) { send_err(id, "png encode failed"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"y\":%d}", id, path, y0);
}

/* wide_shot: capture the NATIVE-WIDE present surface (post-compositor, what the
 * window actually shows in 16:9) to a PNG — unlike screenshot_file, which dumps
 * the canonical 320 VRAM region (pre-compositor). Pulls the exact buffer the
 * present path uses (gr_render_wide_display into a scratch buffer), so the dump
 * reflects the composited wide frame 1:1, orientation included. Errors if the
 * active backend has no wide compositor or native-wide isn't engaged. */
static void handle_wide_shot(int id, const char *json)
{
    extern int gr_wide_supported(void);
    extern int gr_scale(void);
    extern int gr_render_wide_display(uint32_t *out, int pitch, int base_x,
                                      int disp_y, int disp_h);
    extern void gl_renderer_sync_cpu(void);
    gl_renderer_sync_cpu();   /* no-op on SW / when no GL frame pending */

    if (!gr_wide_supported()) { send_err(id, "active backend has no wide compositor"); return; }
    int extra = ws_nw_extra();
    if (extra <= 0) { send_err(id, "native-wide not engaged (extra=0; need a wide game frame)"); return; }

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.width == 0 || di.height == 0) { send_err(id, "display disabled"); return; }

    int scale = gr_scale(); if (scale < 1) scale = 1;
    int present_w = (int)di.width + extra;
    int W = present_w * scale, H = (int)di.height * scale;

    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        strncpy(path, "psx_wide.png", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    uint32_t *buf = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    if (!buf) { send_err(id, "alloc failed"); return; }
    int n = gr_render_wide_display(buf, W * (int)sizeof(uint32_t),
                                   (int)di.display_x, (int)di.display_y, (int)di.height);
    if (n <= 0) { free(buf); send_err(id, "no wide surface for displayed buffer"); return; }

    /* ARGB8888 (0xAARRGGBB) -> RGB, written in the buffer's row order (so the
     * PNG shows exactly the present orientation — the point of this probe). */
    uint8_t *rgb = (uint8_t *)malloc((size_t)W * H * 3);
    if (!rgb) { free(buf); send_err(id, "alloc failed"); return; }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t px = buf[(size_t)y * W + x];
            uint8_t *p = rgb + ((size_t)y * W + x) * 3;
            p[0] = (uint8_t)((px >> 16) & 0xFF);
            p[1] = (uint8_t)((px >> 8) & 0xFF);
            p[2] = (uint8_t)(px & 0xFF);
        }
    }
    free(buf);
    FILE *f = fopen(path, "wb");
    if (!f) { free(rgb); send_err(id, "cannot open file"); return; }
    int ok = png_write_rgb(f, rgb, (uint32_t)W, (uint32_t)H);
    free(rgb); fclose(f);
    if (!ok) { send_err(id, "png encode failed"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d}",
             id, path, W, H);
}

/* screenshot: capture what the player is actually being shown. Native-wide
 * presentation is wider than the canonical PSX display rectangle, so routing
 * this command unconditionally through handle_screenshot_file silently omits
 * precisely the reveal margins that widescreen diagnostics need to inspect.
 * Keep screenshot_file as the explicit canonical-VRAM probe. */
static void handle_present_screenshot(int id, const char *json)
{
    extern int gr_wide_supported(void);
    if (gr_wide_supported() && ws_nw_extra() > 0) {
        handle_wide_shot(id, json);
        return;
    }
    handle_screenshot_file(id, json);
}

/* wide_full: dump the ENTIRE active wide compositor surface (both double-buffer
 * y-bands, full width incl. both reveal margins) to a PNG. Unlike wide_shot
 * (which crops to the displayed band), this shows where the over-draw actually
 * lands across the whole surface. SW backend only (diagnostic). base_x defaults
 * to 0 (the common vertical-double-buffer origin). */
static void handle_wide_full(int id, const char *json)
{
    /* Backend-dispatched: gr_wide_dump_full routes to the active renderer's full
     * wide-surface dump (SW: sw_wide_dump_full; GL: glb_wide_dump_full), so the
     * whole compositor surface can be inspected on either backend over TCP. */
    extern int gr_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh, int base_x);
    extern void gl_renderer_sync_cpu(void);
    gl_renderer_sync_cpu();   /* no-op on SW; ensures pending GL frame is flushed */
    int base_x = json_get_int(json, "base_x", 0);
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)))
        strncpy(path, "psx_wide_full.png", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    int cap = 1024 * 1024 * 4;  /* up to 4 Mpix (e.g. 426*512*S^2) */
    uint32_t *buf = (uint32_t *)malloc((size_t)cap * sizeof(uint32_t));
    if (!buf) { send_err(id, "alloc failed"); return; }
    int W = 0, H = 0;
    int n = gr_wide_dump_full(buf, cap, &W, &H, base_x);
    if (n <= 0) { free(buf); send_err(id, "no wide surface (SW backend? native-wide engaged?)"); return; }

    uint8_t *rgb = (uint8_t *)malloc((size_t)W * H * 3);
    if (!rgb) { free(buf); send_err(id, "alloc failed"); return; }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t px = buf[(size_t)y * W + x];
            uint8_t *p = rgb + ((size_t)y * W + x) * 3;
            p[0] = (uint8_t)((px >> 16) & 0xFF);
            p[1] = (uint8_t)((px >> 8) & 0xFF);
            p[2] = (uint8_t)(px & 0xFF);
        }
    free(buf);
    FILE *f = fopen(path, "wb");
    if (!f) { free(rgb); send_err(id, "cannot open file"); return; }
    int ok = png_write_rgb(f, rgb, (uint32_t)W, (uint32_t)H);
    free(rgb); fclose(f);
    if (!ok) { send_err(id, "png encode failed"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d}",
             id, path, W, H);
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

/* GL present ring: every SwapWindow with path, source rect, letterbox rect,
 * glGetError, wall-clock ms and the backbuffer centre pixel sampled right
 * before the swap. Always-on capture; this just reads a window.
 *   {"cmd":"gl_present_ring","n":600}
 * -> events: [seq, frame, path, t_ms, [dx,dy,w,h], [lx,ly,lw,lh],
 *             [r,g,b], glerr] */
extern uint64_t gl_renderer_pres_total(void);

static void handle_gl_present_ring(int id, const char *json)
{
    static const char *path_name[5] = { "vram", "wide", "cpu", "blank", "interp" };
    int n = json_get_int(json, "n", 300);
    if (n < 1) n = 1;
    if (n > 4096) n = 4096;
    uint64_t total = gl_renderer_pres_total();
    uint64_t start = total > (uint64_t)n ? total - (uint64_t)n : 0;
    int bufsz = 96 + n * 112;
    char *buf = (char *)malloc((size_t)bufsz);
    if (!buf) { send_err(id, "alloc failed"); return; }
    int pos = snprintf(buf, bufsz,
                       "{\"id\":%d,\"ok\":true,\"total\":%llu,\"now_ms\":%u,\"events\":[",
                       id, (unsigned long long)total, (unsigned)SDL_GetTicks());
    int first = 1;
    for (uint64_t s = start; s < total && pos < bufsz - 160; s++) {
        GlPresEvent e;
        if (!gl_renderer_pres_get(s, &e)) continue;
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s[%llu,%u,\"%s\",%u,[%d,%d,%d,%d],[%d,%d,%d,%d],[%u,%u,%u],%u,[%u,%u,%u,%u]]",
                        first ? "" : ",", (unsigned long long)s, e.frame,
                        e.path < 5 ? path_name[e.path] : "?", e.t_ms,
                        e.dx, e.dy, e.w, e.h, e.lx, e.ly, e.lw, e.lh,
                        e.px_r, e.px_g, e.px_b, e.glerr,
                        e.src_r, e.src_g, e.src_b, e.src_valid);
        first = 0;
    }
    pos += snprintf(buf + pos, bufsz - pos, "]}");
    send_fmt("%s", buf);
    free(buf);
}

/* Present-classification ring (all backends; see present_ring.h): how each
 * present was classified — 4:3 pillarbox / native-wide / canonical — and
 * whether a native-wide present fell back to the canonical width (the
 * "everything stretched for a while" signature). Always-on; this reads a
 * window.
 *   {"cmd":"present_ring","n":600}
 * -> events: [seq, frame, path, present_w, disp_w, disp_h, game_mode,
 *             native_43, fellback, tag_delta, nw_extra, gte_verts,
 *             ovh_prims] */
static void handle_present_ring(int id, const char *json)
{
    static const char *pres_path_name[4] =
        { "blank", "native43", "wide", "canonical" };
    int n = json_get_int(json, "n", 300);
    if (n < 1) n = 1;
    if (n > 4096) n = 4096;
    uint64_t total = present_ring_total();
    uint64_t start = total > (uint64_t)n ? total - (uint64_t)n : 0;
    int bufsz = 96 + n * 112;
    char *buf = (char *)malloc((size_t)bufsz);
    if (!buf) { send_err(id, "alloc failed"); return; }
    int pos = snprintf(buf, bufsz,
                       "{\"id\":%d,\"ok\":true,\"total\":%llu,\"events\":[",
                       id, (unsigned long long)total);
    int first = 1;
    for (uint64_t s = start; s < total && pos < bufsz - 128; s++) {
        PresRingEntry e;
        if (!present_ring_get(s, &e)) continue;
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s[%llu,%u,\"%s\",%u,%u,%u,%u,%u,%u,%ld,%u,%u,%u]",
                        first ? "" : ",", (unsigned long long)s, e.frame,
                        e.path < 4 ? pres_path_name[e.path] : "?",
                        e.present_w, e.disp_w, e.disp_h,
                        e.game_mode, e.native_43, e.wide_fellback,
                        (long)e.tag_delta, e.nw_extra,
                        e.gte_verts, e.ovh_prims);
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
    extern int      g_dma_exec_depth;   /* >0 while a DMA moves data (dma.c) */
    extern int      g_dma_cur_ch;       /* in-flight DMA channel, or -1 */
    extern uint32_t g_dma_initiator_pc; /* guest PC that kicked the DMA */
    uint32_t ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    e->seq       = seq;
    e->addr      = phys;
    e->old_val   = old_val;
    e->new_val   = new_val;
    e->ra        = ra;
    e->func_addr = g_debug_current_func_addr;
    e->pc        = g_debug_last_store_pc;
    /* DMA-sourced write: tag the channel and attribute to the DMA kick PC
     * instead of the stale last-CPU-store PC (which is meaningless mid-DMA). */
    if (g_dma_exec_depth > 0) {
        e->dma_ch = (int8_t)g_dma_cur_ch;
        if (g_dma_initiator_pc) e->pc = g_dma_initiator_pc;
    } else {
        e->dma_ch = -1;
    }
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
    e->s0        = debug_cpu_ptr ? debug_cpu_ptr->gpr[16] : 0;
    e->s1        = debug_cpu_ptr ? debug_cpu_ptr->gpr[17] : 0;
    e->s2        = debug_cpu_ptr ? debug_cpu_ptr->gpr[18] : 0;
    e->s3        = debug_cpu_ptr ? debug_cpu_ptr->gpr[19] : 0;
    e->s4        = debug_cpu_ptr ? debug_cpu_ptr->gpr[20] : 0;
    e->s5        = debug_cpu_ptr ? debug_cpu_ptr->gpr[21] : 0;
    e->frame     = (uint32_t)s_frame_count;
    e->width     = width;
}

/* Record a single write into the RAM trace ring buffer. */
/* Capture-freeze frame: when non-zero, the deep high-traffic rings (wtrace RAM
 * writes, MMIO read/write traces) stop appending once s_frame_count reaches it.
 * This preserves a causal window whose tail would otherwise be evicted by a
 * post-window event storm (e.g. a CD re-load livelock) faster than a probe can
 * read it — the "keep the ring covering the window" fix, not arm-and-hope. 0=off. */
static uint32_t g_capture_freeze_frame = 0;
static inline int capture_frozen(void) {
    return g_capture_freeze_frame != 0 &&
           (uint32_t)s_frame_count >= g_capture_freeze_frame;
}

static void wtrace_record(uint32_t phys, uint32_t old_val, uint32_t new_val, uint8_t width)
{
    if (!s_wtrace) return;
    if (capture_frozen()) return;
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
static PSX_BSS CardTraceEntry s_card_trace[CARD_TRACE_CAP];
static uint64_t s_card_trace_seq = 0;
static inline int is_card_critical_addr(uint32_t phys) {
    return (phys >= 0x00009F20u && phys < 0x00009F40u) ||
           (phys >= 0x0000B9D0u && phys < 0x0000B9F0u) ||
           (phys >= 0x000072F0u && phys < 0x000072F4u) ||
           (phys >= 0x00007520u && phys < 0x00007524u) ||
           (phys >= 0x00007528u && phys < 0x00007530u) ||
           /* All 7 card EvCB slots (0xE028 + i*0x1C, i=0..6). The old range
            * started at slot 1 and stopped before slot 6, leaving SwCARD-IOE
            * (slot 0 — the success event) and HwCARD-NEW invisible. */
           (phys >= 0x0000E028u && phys < 0x0000E0ECu);
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
    if (s_fmv_quiet) return;
    if (is_card_critical_addr(phys)) card_trace_record(phys, old_val, new_val, width);
    fp_record_write(phys, new_val, g_debug_last_store_pc);
    {
        uint32_t ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
        if (phys < g_psx_ram_size)
            rec_event(REC_KIND_RAM_W, phys, new_val, g_debug_last_store_pc, ra);
        else if (phys >= 0x1F800000u && phys <= 0x1F8003FFu)
            rec_event(REC_KIND_SP_W, phys, new_val, g_debug_last_store_pc, ra);
    }
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
    if (s_fmv_quiet) return;
    /* First-divergence fingerprint + frame recorder also see device writes. */
    fp_record_mmio(addr, val, g_debug_last_store_pc);
    rec_event(REC_KIND_MMIO_W, addr, val, g_debug_last_store_pc,
              debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0);
    if (!s_mmio_trace) return;
    if (capture_frozen()) return;
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

/* MMIO read trace — called from memory.c mmio_read32/16/8 wrappers AFTER the
 * value is computed. Range-filtered (default CD + I_STAT) so the ring retains a
 * long window of just the device-register reads of interest, even across a
 * livelock that polls them. `val` is the value the CPU actually loaded. */
void debug_server_trace_mmio_read(uint32_t addr, uint32_t val, uint8_t width)
{
#ifdef PSX_NO_DEBUG_TOOLS
    (void)addr; (void)val; (void)width;
    return;
#endif
    if (s_fmv_quiet) return;
    rec_event(REC_KIND_MMIO_R, addr, val, g_debug_last_store_pc,
              debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0);
    if (!s_mmio_rtrace || s_mmio_rtrace_range_count == 0) return;
    if (capture_frozen()) return;
    uint32_t phys = addr & 0x1FFFFFFFu;
    int hit = 0;
    for (int i = 0; i < s_mmio_rtrace_range_count; i++) {
        if (phys + width > s_mmio_rtrace_ranges[i].lo &&
            phys < s_mmio_rtrace_ranges[i].hi) { hit = 1; break; }
    }
    if (!hit) return;

    MmioTraceEntry *e = &s_mmio_rtrace[s_mmio_rtrace_head];
    e->seq       = s_mmio_rtrace_seq++;
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
    s_mmio_rtrace_head = (s_mmio_rtrace_head + 1) % MMIO_TRACE_CAP;
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

/* ---- cyc_watch command handlers (see cyc_watch_observe above) ---- */

/* cyc_watch — arm an anchor PC. {"pc":"0x...","n":16}. Clears the ring,
 * masks the anchor to physical, and starts recording. The Beetle side
 * (psx-beetle, added parent-side) implements the SAME command/spec. */
static void handle_cyc_watch(int id, const char *json)
{
    char pcbuf[64];
    if (!json_get_str(json, "pc", pcbuf, sizeof(pcbuf))) {
        send_err(id, "cyc_watch requires pc");
        return;
    }
    uint32_t raw = hex_to_u32(pcbuf);
    int n = json_get_int(json, "n", 16);
    if (n < 1) n = 1;
    if (n > CYC_WATCH_RING_CAP) n = CYC_WATCH_RING_CAP;
    /* Optional second anchor -> REGION mode: each entry = Δcycles of one A->B pass. */
    char endbuf[64];
    uint32_t end_raw = json_get_str(json, "end", endbuf, sizeof(endbuf)) ? hex_to_u32(endbuf) : 0u;

    /* Disarm first so the hot path can't sample mid-reset. */
    s_cyc_watch_armed = 0;
    s_cyc_watch_anchor_raw  = raw;
    s_cyc_watch_anchor_phys = raw & 0x1FFFFFFFu;
    s_cyc_watch_end_raw     = end_raw;
    s_cyc_watch_end_phys    = end_raw & 0x1FFFFFFFu;
    s_cyc_watch_in_region   = 0;
    s_cyc_watch_region_start= 0;
    s_cyc_watch_max_hits    = (uint32_t)n;
    s_cyc_watch_hits        = 0;
    s_cyc_watch_last_phys   = 0xFFFFFFFFu;   /* reset dedupe state per arm */
    s_cyc_watch_last_cycle  = 0xFFFFFFFFFFFFFFFFull;
    memset(s_cyc_watch_ring, 0, sizeof(s_cyc_watch_ring));
    s_cyc_watch_armed = 1;

    send_fmt("{\"id\":%d,\"ok\":true,\"anchor\":\"0x%08X\","
             "\"anchor_phys\":\"0x%08X\",\"end\":\"0x%08X\",\"end_phys\":\"0x%08X\","
             "\"region\":%d,\"max_hits\":%u}",
             id, s_cyc_watch_anchor_raw, s_cyc_watch_anchor_phys,
             s_cyc_watch_end_raw, s_cyc_watch_end_phys,
             (s_cyc_watch_end_phys != 0u) ? 1 : 0, s_cyc_watch_max_hits);
}

/* cyc_watch_dump — return the recorded ring as JSON. */
static void handle_cyc_watch_dump(int id, const char *json)
{
    (void)json;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"ok\":true,\"anchor\":\"0x%08X\","
             "\"anchor_phys\":\"0x%08X\",\"end\":\"0x%08X\",\"end_phys\":\"0x%08X\","
             "\"region\":%d,\"armed\":%d,\"max_hits\":%u,"
             "\"hits\":%u,\"entries\":[",
             id, s_cyc_watch_anchor_raw, s_cyc_watch_anchor_phys,
             s_cyc_watch_end_raw, s_cyc_watch_end_phys,
             (s_cyc_watch_end_phys != 0u) ? 1 : 0,
             s_cyc_watch_armed ? 1 : 0, s_cyc_watch_max_hits,
             s_cyc_watch_hits);
    send_line(buf);
    for (uint32_t i = 0; i < s_cyc_watch_hits; i++) {
        CycWatchEntry *e = &s_cyc_watch_ring[i];
        snprintf(buf, sizeof(buf),
                 "%s{\"hit_index\":%u,\"pc\":\"0x%08X\",\"cycles\":%llu}",
                 (i == 0) ? "" : ",",
                 e->hit_index, e->pc,
                 (unsigned long long)e->psx_cycle_count);
        send_line(buf);
    }
    send_line("]}");
}

/* cyc_watch_clear — disarm and zero the ring. */
static void handle_cyc_watch_clear(int id, const char *json)
{
    (void)json;
    s_cyc_watch_armed = 0;
    s_cyc_watch_hits  = 0;
    s_cyc_watch_anchor_phys = 0;
    s_cyc_watch_anchor_raw  = 0;
    s_cyc_watch_end_phys = 0;
    s_cyc_watch_end_raw  = 0;
    s_cyc_watch_in_region = 0;
    s_cyc_watch_region_start = 0;
    memset(s_cyc_watch_ring, 0, sizeof(s_cyc_watch_ring));
    send_ok(id);
}

/* pc_probe_arm — {"pcs":"0xA,0xB", "n":32, "nd_intro":1..5} or empty pcs+nd_intro.
 * 1=PolyG4 clip 2=OT leaves 3=wood 4=depth 5=wood DL/helper bind. */
static void handle_pc_probe_arm(int id, const char *json)
{
    pc_probe_clear_state();
    int n = json_get_int(json, "n", 32);
    if (n < 1) n = 1;
    if (n > PC_PROBE_SAMPLE_CAP) n = PC_PROBE_SAMPLE_CAP;
    s_pc_probe_sample_max = (uint32_t)n;

    int nd = json_get_int(json, "nd_intro", 0);
    char pcs[512];
    const char *have_pcs = json_get_str(json, "pcs", pcs, sizeof(pcs));
    if (nd == 5) pc_probe_arm_nd_intro_wood_dl_defaults();
    else if (nd == 4) pc_probe_arm_nd_intro_depth_defaults();
    else if (nd == 3) pc_probe_arm_nd_intro_wood_defaults();
    else if (nd == 2) pc_probe_arm_nd_intro_ot_defaults();
    else if (nd) pc_probe_arm_nd_intro_defaults();
    if (have_pcs) pc_probe_parse_list(pcs);
    if (s_pc_probe_n <= 0) {
        send_err(id, "pc_probe_arm needs pcs=... and/or nd_intro=1..5");
        return;
    }
    s_pc_probe_armed = 1;
    send_fmt("{\"id\":%d,\"ok\":true,\"armed\":1,\"n_pcs\":%d,\"sample_max\":%u,\"nd_intro\":%d}",
             id, s_pc_probe_n, s_pc_probe_sample_max, nd);
}

static void handle_pc_probe_dump(int id, const char *json)
{
    (void)json;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"ok\":true,\"armed\":%d,\"n_pcs\":%d,\"sample_max\":%u,"
             "\"samples_n\":%u,\"slots\":[",
             id, s_pc_probe_armed ? 1 : 0, s_pc_probe_n, s_pc_probe_sample_max,
             s_pc_probe_sample_n);
    send_line(buf);
    for (int i = 0; i < s_pc_probe_n; i++) {
        const PcProbeSlot *s = &s_pc_probe[i];
        snprintf(buf, sizeof(buf),
                 "%s{\"pc\":\"0x%08X\",\"count\":%llu,\"t0_zero\":%u,\"t0_nonzero\":%u,"
                 "\"last_t0\":\"0x%08X\",\"last_fp\":\"0x%08X\",\"last_v0\":\"0x%08X\","
                 "\"last_mode\":\"0x%08X\",\"last_depth\":\"0x%08X\","
                 "\"last_ot_base\":\"0x%08X\",\"last_ot_index\":\"0x%08X\","
                 "\"last_frame\":%u}",
                 (i == 0) ? "" : ",",
                 s->pc, (unsigned long long)s->count, s->t0_zero, s->t0_nonzero,
                 s->last_t0, s->last_fp, s->last_v0,
                 s->last_mode, s->last_depth, s->last_ot_base, s->last_ot_index,
                 s->last_frame);
        send_line(buf);
    }
    send_line("],\"samples\":[");
    for (uint32_t i = 0; i < s_pc_probe_sample_n; i++) {
        const PcProbeSample *sm = &s_pc_probe_samples[i];
        snprintf(buf, sizeof(buf),
                 "%s{\"pc\":\"0x%08X\",\"frame\":%u,\"t0\":\"0x%08X\","
                 "\"fp\":\"0x%08X\",\"v0\":\"0x%08X\",\"mode\":\"0x%08X\","
                 "\"depth\":\"0x%08X\",\"ot_base\":\"0x%08X\",\"ot_index\":\"0x%08X\"}",
                 (i == 0) ? "" : ",",
                 sm->pc, sm->frame, sm->t0, sm->fp, sm->v0,
                 sm->mode, sm->depth, sm->ot_base, sm->ot_index);
        send_line(buf);
    }
    send_line("]}");
}

static void handle_pc_probe_clear(int id, const char *json)
{
    (void)json;
    pc_probe_clear_state();
    send_ok(id);
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
/* Dump the VSync callback-pointer (0x80079D44) write-provenance ring (memory.c).
 * Each entry = one write to that word with its real source: store_pc (accurate for
 * CPU/dirty-interp stores; stale for DMA), and dma_depth/ch/madr/bcr (set in dma.c
 * while a DMA moves data). Lets the corrupting 0x016F0110 write be attributed to the
 * exact channel + destination instead of a stale per-instruction store PC. */
static void handle_d44_ring(int id, const char *json)
{
    (void)json;
    typedef struct { uint64_t seq; uint32_t val, old, store_pc;
                     int32_t dma_depth, dma_ch; uint32_t dma_madr, dma_bcr, frame; } D44E;
    extern D44E g_d44_ring[]; extern uint64_t g_d44_seq;
    uint64_t total = g_d44_seq;
    uint32_t cap = 32u;
    uint32_t n = total < cap ? (uint32_t)total : cap;
    char buf[8192]; size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"entries\":[",
                    id, (unsigned long long)total);
    for (uint32_t i = 0; i < n && pos < sizeof(buf) - 256; i++) {
        uint64_t idx = total - n + i;
        D44E *e = &g_d44_ring[idx & (cap - 1u)];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"seq\":%llu,\"frame\":%u,\"old\":\"0x%08X\",\"new\":\"0x%08X\","
            "\"store_pc\":\"0x%08X\",\"dma_depth\":%d,\"dma_ch\":%d,"
            "\"dma_madr\":\"0x%08X\",\"dma_bcr\":\"0x%08X\"}",
            i ? "," : "", (unsigned long long)e->seq, e->frame, e->old, e->val,
            e->store_pc, e->dma_depth, e->dma_ch, e->dma_madr, e->dma_bcr);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

/* Dump the IRQ-delivery context ring (interrupts.c): per IRQ delivery, the VSync
 * callback word [0x80079D44], CD-DMA-active + dma-depth, and COP0/IRQ state. Shows
 * whether VBlank was delivered while 0x80079D44 was the clobbered 0x016F0110 AND a
 * CD DMA was mid-transfer (the VSync-in-DMA-window bug). */
static void handle_irqctx_ring(int id, const char *json)
{
    typedef struct { uint64_t seq, cycle; uint32_t frame, istat, imask, sr, d44,
                     cdrom_active, is_vblank; int dma_depth;
                     uint32_t take_pc, real_epc, exit_pc, exit_reason, same_thread,
                     restored, v0_exit, v0_saved, v1_exit, v1_saved, ra_exit,
                     ra_saved, redirects, entry_sp, pump_site; } E;
    extern E g_irqctx_ring[]; extern uint64_t g_irqctx_seq;
    /* Ring cap must track IRQCTX_RING_CAP in interrupts.c. */
    uint32_t cap = 4096u;
    /* Optional frame-window filter + newest-N count so a deep ring can be
     * windowed to the delivery window of interest (ring-buffer discipline). */
    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);
    int count    = json_get_int(json, "count", 128);
    if (count < 1) count = 1;
    if (count > (int)cap) count = (int)cap;
    uint64_t total = g_irqctx_seq;
    uint32_t avail = total < cap ? (uint32_t)total : cap;
    uint32_t n = (uint32_t)count < avail ? (uint32_t)count : avail;
    size_t BUF_SZ = 512u + (size_t)n * 410u;
    char *buf = (char *)malloc(BUF_SZ); if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"entries\":[",
                    id, (unsigned long long)total);
    int emitted = 0;
    for (uint32_t i = 0; i < n && pos < BUF_SZ - 512; i++) {
        uint64_t idx = total - n + i;
        E *e = &g_irqctx_ring[idx & (cap - 1u)];
        if (frame_lo >= 0 && (int)e->frame < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame > frame_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"cycle\":%llu,\"frame\":%u,\"vblank\":%u,"
            "\"d44\":\"0x%08X\",\"cdrom_active\":%u,\"dma_depth\":%d,"
            "\"sr\":\"0x%08X\",\"istat\":\"0x%08X\",\"imask\":\"0x%08X\","
            "\"take_pc\":\"0x%08X\",\"real_epc\":\"0x%08X\",\"exit_pc\":\"0x%08X\","
            "\"exit_reason\":%u,\"same_thread\":%u,\"restored\":%u,"
            "\"v0_exit\":\"0x%08X\",\"v0_saved\":\"0x%08X\","
            "\"v1_exit\":\"0x%08X\",\"v1_saved\":\"0x%08X\","
            "\"ra_exit\":\"0x%08X\",\"ra_saved\":\"0x%08X\",\"redirects\":%u,"
            "\"entry_sp\":\"0x%08X\",\"pump_site\":%u}",
            emitted ? "," : "", (unsigned long long)e->seq, (unsigned long long)e->cycle,
            e->frame, e->is_vblank, e->d44, e->cdrom_active, e->dma_depth,
            e->sr, e->istat, e->imask,
            e->take_pc, e->real_epc, e->exit_pc, e->exit_reason, e->same_thread,
            e->restored, e->v0_exit, e->v0_saved, e->v1_exit, e->v1_saved,
            e->ra_exit, e->ra_saved, e->redirects, e->entry_sp, e->pump_site);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

/* sp_ring — dump the always-on stack-domain transition ring (fntrace.c).
 * One entry per dispatch whose guest SP crossed a 64 KB domain: the
 * provenance record for "who installed this stack pointer". */
static void handle_sp_ring(int id, const char *json)
{
    extern SpDomainEntry g_spdom_ring[]; extern uint64_t g_spdom_seq;
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > (int)SPDOM_RING_CAP) count = (int)SPDOM_RING_CAP;
    uint64_t total = g_spdom_seq;
    uint32_t avail = total < SPDOM_RING_CAP ? (uint32_t)total : SPDOM_RING_CAP;
    uint32_t n = (uint32_t)count < avail ? (uint32_t)count : avail;
    size_t BUF_SZ = 256u + (size_t)n * 200u;
    char *buf = (char *)malloc(BUF_SZ); if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"entries\":[",
                    id, (unsigned long long)total);
    for (uint32_t i = 0; i < n && pos < BUF_SZ - 256; i++) {
        uint64_t idx = total - n + i;
        SpDomainEntry *e = &g_spdom_ring[idx % SPDOM_RING_CAP];
        pos += snprintf(buf + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"cycle\":%llu,\"frame\":%u,"
            "\"prev_sp\":\"0x%08X\",\"new_sp\":\"0x%08X\","
            "\"target\":\"0x%08X\",\"ra\":\"0x%08X\",\"tcb\":\"0x%08X\"}",
            i ? "," : "", (unsigned long long)e->seq, (unsigned long long)e->cycle,
            e->frame, e->prev_sp, e->new_sp, e->target, e->ra, e->tcb);
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

/* disp_ring — dump the always-on dispatch tail ring (fntrace.c). */
static void handle_disp_ring(int id, const char *json)
{
    extern DispTailEntry g_disp_tail[]; extern uint64_t g_disp_tail_seq;
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > (int)DISP_TAIL_CAP) count = (int)DISP_TAIL_CAP;
    uint64_t total = g_disp_tail_seq;
    uint32_t avail = total < DISP_TAIL_CAP ? (uint32_t)total : DISP_TAIL_CAP;
    uint32_t n = (uint32_t)count < avail ? (uint32_t)count : avail;
    size_t BUF_SZ = 256u + (size_t)n * 130u;
    char *buf = (char *)malloc(BUF_SZ); if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"entries\":[",
                    id, (unsigned long long)total);
    for (uint32_t i = 0; i < n && pos < BUF_SZ - 200; i++) {
        uint64_t idx = total - n + i;
        DispTailEntry *e = &g_disp_tail[idx % DISP_TAIL_CAP];
        pos += snprintf(buf + pos, BUF_SZ - pos,
            "%s{\"seq\":%llu,\"cycle\":%llu,\"target\":\"0x%08X\","
            "\"ra\":\"0x%08X\",\"sp\":\"0x%08X\"}",
            i ? "," : "", (unsigned long long)idx, (unsigned long long)e->cycle,
            e->target, e->ra, e->sp);
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

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
    extern uint32_t g_async_rfe_resume_pc;
    extern uint32_t g_dirty_safe_resume_pc;
    extern uint64_t g_async_rfe_set_count;
    extern uint64_t g_async_rfe_fire_count;
    extern uint64_t g_slice_fired, g_slice_irq_taken;
    extern uint64_t g_pczero_count;
    extern uint32_t g_pczero_addr, g_pczero_ra, g_pczero_in_exc,
                    g_pczero_async_rfe, g_pczero_dirty_safe;
    extern uint32_t g_slice_last_block, g_slice_last_first_pc, g_slice_last_first_insn;
    extern uint32_t g_slice_last_committed, g_slice_last_istat, g_slice_last_imask, g_slice_last_sr;
    extern uint32_t g_slice_entry_deliverable;
    extern uint64_t g_sentinel_reach_dirty;
    extern uint64_t g_sentinel_reach_traps;
    extern uint32_t g_sentinel_reach_async;
    extern uint64_t g_nestgate_depth, g_nestgate_rfepend,
                    g_nestgate_escreason, g_nestgate_iec;

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
                    "\"async_rfe_resume_pc\":\"0x%08X\","
                    "\"dirty_safe_resume_pc\":\"0x%08X\","
                    "\"async_rfe_set\":%llu,"
                    "\"async_rfe_fire\":%llu,"
                    "\"reach_dirty\":%llu,"
                    "\"reach_traps\":%llu,"
                    "\"reach_async\":\"0x%08X\","
                    "\"dirty_ram_blocks\":%llu,"
                    "\"dirty_ram_insns\":%llu,"
                    "\"dirty_ram_aborts\":%llu,"
                    "\"dirty_ram_guard_yields\":%llu,"
                    "\"slice_fired\":%llu,"
                    "\"slice_irq_taken\":%llu,"
                    "\"slice_block\":\"0x%08X\","
                    "\"slice_first_pc\":\"0x%08X\","
                    "\"slice_first_insn\":\"0x%08X\","
                    "\"slice_committed\":\"0x%08X\","
                    "\"slice_istat\":\"0x%08X\","
                    "\"slice_imask\":\"0x%08X\","
                    "\"slice_sr\":\"0x%08X\","
                    "\"slice_entry_deliverable\":%u,"
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
                    "\"pczero_count\":%llu,"
                    "\"pczero_addr\":\"0x%08X\","
                    "\"pczero_ra\":\"0x%08X\","
                    "\"pczero_in_exc\":%u,"
                    "\"pczero_async_rfe\":\"0x%08X\","
                    "\"pczero_dirty_safe\":\"0x%08X\","
                    "\"nestgate_depth\":%llu,"
                    "\"nestgate_rfepend\":%llu,"
                    "\"nestgate_escreason\":%llu,"
                    "\"nestgate_iec\":%llu,"
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
                    g_async_rfe_resume_pc, g_dirty_safe_resume_pc,
                    (unsigned long long)g_async_rfe_set_count,
                    (unsigned long long)g_async_rfe_fire_count,
                    (unsigned long long)g_sentinel_reach_dirty,
                    (unsigned long long)g_sentinel_reach_traps,
                    g_sentinel_reach_async,
                    (unsigned long long)g_dirty_ram_blocks_run,
                    (unsigned long long)g_dirty_ram_insns_run,
                    (unsigned long long)g_dirty_ram_aborts,
                    (unsigned long long)g_dirty_ram_guard_yields,
                    (unsigned long long)g_slice_fired,
                    (unsigned long long)g_slice_irq_taken,
                    g_slice_last_block,
                    g_slice_last_first_pc,
                    g_slice_last_first_insn,
                    g_slice_last_committed,
                    g_slice_last_istat,
                    g_slice_last_imask,
                    g_slice_last_sr,
                    g_slice_entry_deliverable,
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
                    recent_total,
                    (unsigned long long)g_pczero_count,
                    g_pczero_addr,
                    g_pczero_ra,
                    g_pczero_in_exc,
                    g_pczero_async_rfe,
                    g_pczero_dirty_safe,
                    (unsigned long long)g_nestgate_depth,
                    (unsigned long long)g_nestgate_rfepend,
                    (unsigned long long)g_nestgate_escreason,
                    (unsigned long long)g_nestgate_iec);
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

    /* Optional frame-window filter — the "query the ring for the window of
     * interest" primitive.  Lets a caller reach entries in the MIDDLE of a deep,
     * high-traffic ring (which oldest-N / newest-N paging cannot). -1 = unbounded. */
    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);

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
        if (frame_lo >= 0 && (int)e->frame < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame > frame_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"old\":\"0x%08X\","
                        "\"new\":\"0x%08X\",\"ra\":\"0x%08X\",\"func\":\"0x%08X\","
                        "\"pc\":\"0x%08X\",\"cpu_pc\":\"0x%08X\",\"sp\":\"0x%08X\","
                        "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\","
                        "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                        "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\","
                        "\"s0\":\"0x%08X\",\"s1\":\"0x%08X\","
                        "\"s2\":\"0x%08X\",\"s3\":\"0x%08X\","
                        "\"s4\":\"0x%08X\",\"s5\":\"0x%08X\","
                        "\"frame\":%u,\"w\":%u,\"dma_ch\":%d}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->old_val, e->new_val, e->ra, e->func_addr,
                        e->pc, e->cpu_pc, e->sp,
                        e->v0, e->v1, e->a0, e->a1, e->a2, e->a3, e->t0, e->t1,
                        e->s0, e->s1, e->s2, e->s3, e->s4, e->s5,
                        e->frame, (unsigned)e->width, (int)e->dma_ch);
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
    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);

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
        if (frame_lo >= 0 && (int)e->frame < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame > frame_hi) continue;
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

/* capture_freeze: freeze the deep high-traffic rings (wtrace RAM writes, MMIO
 * read/write traces) at frame N so a post-window event storm cannot evict the
 * causal window before a probe reads it. {"frame":N} arms; {"frame":0} clears.
 * The always-on rings keep everything up to N; the game keeps running. */
static void handle_capture_freeze(int id, const char *json)
{
    int frame = json_get_int(json, "frame", -1);
    if (frame >= 0) g_capture_freeze_frame = (uint32_t)frame;
    send_fmt("{\"id\":%d,\"ok\":true,\"freeze_frame\":%u,\"cur_frame\":%u,\"frozen\":%d}",
             id, g_capture_freeze_frame, (uint32_t)s_frame_count, capture_frozen() ? 1 : 0);
}

/* ---- rtrace (MMIO-READ trace) command handlers ----
 * Field names of the dumped entries are a SUPERSET of the oracle's rtrace_dump
 * (addr/val/pc/ra/frame/w common; func/cpu_pc/sr/epc/i_stat/i_mask extra) so a
 * cross-port tool reads both ports with one parser (Rule 16). */
static void handle_rtrace_dump(int id, const char *json)
{
    if (!s_mmio_rtrace) { send_err(id, "rtrace not initialized"); return; }

    char addr_str[32];
    uint32_t filter_addr = 0;
    int has_filter = json_get_str(json, "addr", addr_str, sizeof(addr_str)) != NULL;
    if (has_filter) filter_addr = hex_to_u32(addr_str);
    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);

    uint64_t total = s_mmio_rtrace_seq;
    uint32_t avail = (total < MMIO_TRACE_CAP) ? (uint32_t)total : MMIO_TRACE_CAP;
    uint32_t start = (total < MMIO_TRACE_CAP) ? 0 : s_mmio_rtrace_head;

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
            uint32_t newest = (s_mmio_rtrace_head + MMIO_TRACE_CAP - 1u) % MMIO_TRACE_CAP;
            idx = (newest + MMIO_TRACE_CAP - (i % MMIO_TRACE_CAP)) % MMIO_TRACE_CAP;
        } else {
            idx = (start + i) % MMIO_TRACE_CAP;
        }
        MmioTraceEntry *e = &s_mmio_rtrace[idx];
        if (has_filter && e->addr != filter_addr) continue;
        if (frame_lo >= 0 && (int)e->frame < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame > frame_hi) continue;
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

static void handle_rtrace_clear(int id, const char *json)
{
    (void)json;
    s_mmio_rtrace_seq = 0;
    s_mmio_rtrace_head = 0;
    if (s_mmio_rtrace) memset(s_mmio_rtrace, 0, (size_t)MMIO_TRACE_CAP * sizeof(MmioTraceEntry));
    send_ok(id);
}

static void handle_rtrace_arm(int id, const char *json)
{
    if (s_mmio_rtrace_range_count >= MMIO_RTRACE_MAX_RANGES) {
        send_err(id, "max ranges reached"); return;
    }
    char lo_str[32], hi_str[32];
    if (!json_get_str(json, "lo", lo_str, sizeof(lo_str))) { send_err(id, "missing lo"); return; }
    if (!json_get_str(json, "hi", hi_str, sizeof(hi_str))) { send_err(id, "missing hi"); return; }
    int slot = s_mmio_rtrace_range_count++;
    s_mmio_rtrace_ranges[slot].lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    s_mmio_rtrace_ranges[slot].hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
             id, slot, s_mmio_rtrace_ranges[slot].lo, s_mmio_rtrace_ranges[slot].hi);
}

static void handle_rtrace_ranges(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%d,\"ranges\":[",
             id, s_mmio_rtrace_range_count);
    for (int i = 0; i < s_mmio_rtrace_range_count; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("{\"slot\":%d,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
                 i, s_mmio_rtrace_ranges[i].lo, s_mmio_rtrace_ranges[i].hi);
    }
    send_fmt("]}");
}

static void handle_rtrace_stats(int id, const char *json)
{
    (void)json;
    uint64_t total = s_mmio_rtrace_seq;
    uint64_t oldest = (total <= (uint64_t)MMIO_TRACE_CAP) ? 0 : total - (uint64_t)MMIO_TRACE_CAP;
    uint64_t newest = (total > 0) ? total - 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%d,"
             "\"oldest_seq\":%llu,\"newest_seq\":%llu,\"ranges\":%d}",
             id, (unsigned long long)total, (int)MMIO_TRACE_CAP,
             (unsigned long long)oldest, (unsigned long long)newest,
             s_mmio_rtrace_range_count);
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

/* fmv_state: report the RESOLVED FMV-skip + config state — the runtime truth for
 * "is the FMV-skip feature on / what config actually loaded", so we observe instead
 * of inferring from the .toml on disk. Reports the loaded config path, the resolved
 * auto_skip_fmv + frame-count-table params, the live MDEC decode count and XA-stream
 * flag (the feature's detection inputs), and the current pad word (so a phantom
 * skip-press is visible). */
static void handle_fmv_state(int id, const char *json)
{
    (void)json;
    extern void debug_get_fmv_config(int *, uint32_t *, uint32_t *, int *, const char **);
    extern uint32_t mdec_get_decode_count(void);
    extern int cdrom_xa_stream_active(void);
    extern uint16_t sio_get_pad_buttons(void);
    extern uint64_t cdrom_get_dataready_fires(void);
    extern uint64_t g_cdrom_deliver_count;
    int auto_skip = -1, no_xa_hold = 0;
    uint32_t total_table = 0, movie_id = 0; const char *cfg = "(null)";
    char cfg_json[1024];
    debug_get_fmv_config(&auto_skip, &total_table, &movie_id, &no_xa_hold, &cfg);
    json_escape_string(cfg_json, sizeof(cfg_json), cfg ? cfg : "(null)");
    MDECDebugState s; mdec_debug_get_state(&s);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"config_path\":\"%s\","
             "\"auto_skip_fmv\":%d,"
             "\"fmv_skip_no_xa_hold\":%d,"
             "\"fmv_skip_total_table\":\"0x%08X\",\"fmv_skip_movie_id\":\"0x%08X\","
             "\"mdec_decode_count\":%u,\"mdec_decode_macroblocks\":%u,\"mdec_dma_out_words\":%u,"
             "\"xa_stream_active\":%d,"
             "\"cd_dataready_fires\":%llu,\"cd_irq_delivered\":%llu,"
             "\"pad1\":\"0x%04X\"}",
             id, cfg_json, auto_skip, no_xa_hold,
             total_table, movie_id,
             mdec_get_decode_count(), s.decode_macroblocks, s.dma_out_words,
             cdrom_xa_stream_active(),
             (unsigned long long)cdrom_get_dataready_fires(),
             (unsigned long long)g_cdrom_deliver_count,
             (unsigned)(sio_get_pad_buttons() & 0xFFFFu));
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
    /* Paging: "before" (exclusive upper seq bound) returns the `count`
     * entries ending just below it, so a client can walk the whole ring
     * in chunks without hitting the response-size cap. */
    int64_t before = (int64_t)json_get_int(json, "before", -1);

    uint64_t total = s_card_mgr_trace_seq;
    uint64_t oldest = (total > CARD_MGR_TRACE_CAP) ? total - CARD_MGR_TRACE_CAP : 0;
    uint64_t end = total;
    if (before >= 0 && (uint64_t)before < total) end = (uint64_t)before;
    if (end < oldest) end = oldest;
    uint64_t available = end - oldest;
    if ((uint64_t)count > available) count = (int)available;
    uint64_t start = end - (uint64_t)count;
    if (start < oldest) start = oldest;

    size_t buf_sz = 256u + (size_t)count * 688u;
    char *buf = (char *)malloc(buf_sz);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, buf_sz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest);

    int first = 1;
    for (int i = 0; i < count && pos < buf_sz - 688u; i++) {
        uint64_t seq = newest ? (end - 1ull - (uint64_t)i) : (start + (uint64_t)i);
        if (seq < oldest || seq >= total) continue;
        const CardMgrTraceEntry *e = &s_card_mgr_trace[seq % CARD_MGR_TRACE_CAP];
        if (e->seq != seq) continue;
        pos += snprintf(buf + pos, buf_sz - pos,
            "%s{\"seq\":%llu,\"src\":%u,\"rep\":%u,\"frame\":%u,\"func\":\"0x%08X\","
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
            (unsigned long long)e->seq, e->source, e->repeat, e->frame, e->func_addr,
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

/* CD response-overwrite (dropped-response) accounting — proves the instant-mode
 * race: a new controller response clobbering an unacked one. */
static void handle_cd_overwrite(int id, const char *json)
{
    (void)json;
    extern uint32_t g_cd_overwrite_count, g_cd_overwrite_first_frame,
                    g_cd_overwrite_last_frame;
    extern uint8_t  g_cd_overwrite_first_prev, g_cd_overwrite_first_new,
                    g_cd_overwrite_last_prev,  g_cd_overwrite_last_new;
    send_fmt("{\"id\":%d,\"ok\":true,\"overwrites\":%u,"
             "\"first\":{\"prev_int\":%u,\"new_int\":%u,\"frame\":%u},"
             "\"last\":{\"prev_int\":%u,\"new_int\":%u,\"frame\":%u}}\n",
             id, g_cd_overwrite_count,
             g_cd_overwrite_first_prev, g_cd_overwrite_first_new,
             g_cd_overwrite_first_frame,
             g_cd_overwrite_last_prev, g_cd_overwrite_last_new,
             g_cd_overwrite_last_frame);
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

/* turbo_audio_sink: opt-in host-output discard while turbo loads run. Guest
 * SPU state continues advancing; only accelerated playback samples are sunk. */
static void handle_turbo_audio_sink(int id, const char *json)
{
    extern int      g_turbo_audio_sink_enabled;
    extern int      g_turbo_audio_sink_active;
    extern uint64_t g_turbo_audio_sink_frames;
    int n = json_get_int(json, "n", -1);
    if (n == 0 || n == 1) g_turbo_audio_sink_enabled = n;
    send_fmt("{\"id\":%d,\"ok\":true,\"enabled\":%d,\"active\":%d,"
             "\"discarded_spu_frames\":%llu}\n", id,
             g_turbo_audio_sink_enabled, g_turbo_audio_sink_active,
             (unsigned long long)g_turbo_audio_sink_frames);
}

/* load_transitions: state-edge log for physical reads, the bridged logical
 * load predicate, and host-side turbo pacing. Newest entries are returned
 * last so adjacent edges can be read as a timeline. */
static void handle_load_transitions(int id, const char *json)
{
    int count = json_get_int(json, "count", 128);
    if (count < 1) count = 1;
    if (count > 512) count = 512;
    uint64_t total = load_transition_total();
    uint64_t start = total > (uint64_t)count ? total - (uint64_t)count : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"edges\":[",
             id, (unsigned long long)total);
    int emitted = 0;
    for (uint64_t seq = start; seq < total; seq++) {
        LoadTransitionEntry e;
        if (!load_transition_get(seq, &e)) continue;
        send_fmt("%s{\"seq\":%llu,\"frame\":%u,\"host_ms\":%u,"
                 "\"read\":%u,\"load\":%u,\"turbo\":%u,\"load_run\":%u}",
                 emitted++ ? "," : "", (unsigned long long)seq, e.frame,
                 e.host_ms, e.read_active, e.load_active, e.turbo_active,
                 e.load_run);
    }
    send_fmt("]}\n");
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

/* cdrom_timing: passive L1.5 physical deadline -> buffer -> INT1 exposure
 * summary. `reset:1` starts a fresh measurement window without touching the
 * controller or its schedules. late_bins are exact, 1..64, 65..1024,
 * 1025..5000, and >5000 guest cycles. */
static void handle_cdrom_timing(int id, const char *json)
{
    if (json_get_int(json, "reset", 0) == 1) cdrom_timing_reset();
    char stats[2048];
    cdrom_timing_stats_json(stats, (int)sizeof(stats));
    send_fmt("{\"id\":%d,\"ok\":true,%s}\n", id, stats);
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
    extern uint64_t overlay_autocapture_last_insns_delta(void);
    extern void overlay_autocapture_get_futility(uint32_t *backoff,
                                                 uint32_t *futile);
    extern const char *overlay_autocapture_gate_name(void);
    extern uint64_t overlay_autocapture_ms_since_sample(void);
    extern void overlay_autocapture_get_gates(int *capture_active,
                                              int *write_state,
                                              int *write_job_pending,
                                              unsigned *write_job_attempts,
                                              int *provider_pending,
                                              unsigned *provider_attempts);
    (void)json;
    int      ac_en = 0;
    uint32_t trig = 0;
    uint64_t delta = 0;
    uint32_t backoff = 0, futile = 0;
    overlay_autocapture_get_status(&ac_en, &trig, &delta);
    overlay_autocapture_get_futility(&backoff, &futile);
    int cap_active = 0, wstate = 0, wjob = 0, ppend = 0;
    unsigned wattempts = 0, pattempts = 0;
    overlay_autocapture_get_gates(&cap_active, &wstate, &wjob, &wattempts,
                                  &ppend, &pattempts);
    /* Pressure sampling is host-time paced, so a long dry stretch means a
     * tick gate latched: autocapture is wedged and nothing new will compile
     * until restart. Say so instead of looking merely idle. */
    uint64_t dry_ms = overlay_autocapture_ms_since_sample();
    int wedged = (ac_en && dry_ms > 30000ull);
    char comp[4096];
    autocompile_status_json(comp, sizeof(comp));
    send_fmt("{\"id\":%d,\"ok\":true,\"autocapture_enabled\":%d,"
             "\"triggers\":%u,\"futile_skips\":%u,\"backoff_mult\":%u,"
             "\"last_pressure\":%llu,"
             "\"last_insns_pressure\":%llu,"
             "\"autocapture\":{\"wedged\":%d,\"gate\":\"%s\","
             "\"ms_since_sample\":%llu,\"capture_active\":%d,"
             "\"write_state\":%d,\"write_job_pending\":%d,"
             "\"write_job_attempts\":%u,\"provider_pending\":%d,"
             "\"provider_attempts\":%u},"
             "\"compile\":%s}\n",
             id, ac_en, trig, futile, backoff,
             (unsigned long long)delta,
             (unsigned long long)overlay_autocapture_last_insns_delta(),
             wedged, overlay_autocapture_gate_name(),
             (unsigned long long)dry_ms, cap_active, wstate, wjob, wattempts,
             ppend, pattempts, comp);
}

/* (sljit removed 2026-07-15: the sljit_status, sljit_async, and sljit_try TCP
 * command handlers lived here. They reported the deprecated in-process JIT
 * tier's backend/worker/probe state, which no longer exists.) */

/* autocompile_run: manually kick the active code provider's batch production
 * (gcc: spawn the configured background compile). */
static void handle_autocompile_run(int id, const char *json)
{
    (void)json;
    const CodeProvider *cp = code_provider_active();
    int started = (cp->request) ? cp->request() : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"started\":%d,\"backend\":\"%s\"}\n",
             id, started, cp->name);
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
    char buf[4096];
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
            "\"reval_attempts\":%u,\"reval_crc_miss\":%u,\"last_reval_crc\":\"0x%08X\","
            "\"gen_fastpath\":%llu,\"range_links\":%d,\"range_index_overflow\":%d,"
            "\"lazy_manifests\":%d,\"lazy_manifest_overflow\":%d,"
            "\"candidate_overflow\":%llu,\"pair_aliases\":%llu",
            r0v, r0w, r0lo, r0hi, r0crc, ratt, rmiss, rlast,
            (unsigned long long)overlay_loader_gen_fastpath(),
            overlay_loader_range_link_count(),
            overlay_loader_range_index_overflow(),
            overlay_loader_lazy_manifest_count(),
            overlay_loader_lazy_manifest_overflow(),
            (unsigned long long)overlay_loader_candidate_overflow(),
            (unsigned long long)overlay_loader_pair_aliases());
        uint64_t nd=0, ni=0, sn=0, ss=0, sc=0, sx=0;
        psx_interrupt_delivery_diag(&nd, &ni, &sn, &ss, &sc, &sx);
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"irq_need_defer\":%llu,\"irq_need_delivery\":%llu,"
            "\"irq_skip_none\":%llu,\"irq_skip_sr\":%llu,"
            "\"irq_skip_cooldown\":%llu,\"irq_skip_nested\":%llu",
            (unsigned long long)nd, (unsigned long long)ni,
            (unsigned long long)sn, (unsigned long long)ss,
            (unsigned long long)sc, (unsigned long long)sx);
        extern volatile long g_overlay_image_warm_loaded;
        extern volatile long g_overlay_image_warm_pending;
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"image_warm_loaded\":%ld,\"image_warm_pending\":%ld",
            g_overlay_image_warm_loaded, g_overlay_image_warm_pending);
#ifdef PSX_HAS_OVERLAY_DISPATCH
        {
            uint64_t checks=0, hits=0, variant_misses=0, address_misses=0;
            uint64_t rehashes=0, crc_misses=0, static_gen_fastpath=0;
            extern void psx_overlay_static_get_stats(uint64_t *, uint64_t *,
                                                     uint64_t *, uint64_t *);
            extern void overlay_loader_static_match_stats(uint64_t *, uint64_t *,
                                                          uint64_t *);
            psx_overlay_static_get_stats(&checks, &hits, &variant_misses,
                                         &address_misses);
            overlay_loader_static_match_stats(&rehashes, &crc_misses,
                                              &static_gen_fastpath);
            n += snprintf(buf + n, sizeof(buf) - n,
                ",\"static_checks\":%llu,\"static_hits\":%llu,"
                "\"static_variant_misses\":%llu,\"static_address_misses\":%llu,"
                "\"static_rehashes\":%llu,\"static_crc_misses\":%llu,"
                "\"static_gen_fastpath\":%llu",
                (unsigned long long)checks,
                (unsigned long long)hits,
                (unsigned long long)variant_misses,
                (unsigned long long)address_misses,
                (unsigned long long)rehashes,
                (unsigned long long)crc_misses,
                (unsigned long long)static_gen_fastpath);
        }
#endif
    }
    snprintf(buf + n, sizeof(buf) - n, "}\n");
    send_fmt("%s", buf);
}

/* overlay_candidates: dump the per-entry candidate table (addr, state, stored
 * vs live code hash, generation) so reload-on-return can be inspected directly. */
static void handle_overlay_candidates(int id, const char *json)
{
    extern int overlay_loader_dump_candidates(char *out, int cap);
    extern int overlay_loader_dump_candidates_at(uint32_t addr, char *out, int cap);
    extern int overlay_loader_dump_lazy_at(uint32_t addr, char *out, int cap);
    static char cbuf[65536];
    char pcbuf[32];
    int filtered = json_get_str(json, "pc", pcbuf, sizeof(pcbuf)) != NULL;
    int lazy = json_get_int(json, "lazy", 0);
    int len = filtered
        ? (lazy ? overlay_loader_dump_lazy_at(hex_to_u32(pcbuf), cbuf, (int)sizeof(cbuf))
                : overlay_loader_dump_candidates_at(hex_to_u32(pcbuf), cbuf, (int)sizeof(cbuf)))
        : overlay_loader_dump_candidates(cbuf, (int)sizeof(cbuf));
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
    static char rbuf[2 * 1024 * 1024];
    int len = overlay_loader_dump_native_ring(rbuf, (int)sizeof(rbuf));
    if (len < 0) len = 0;
    int cap = len + 128;
    char *out = (char *)malloc((size_t)cap);
    if (!out) { send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"oom\"}\n", id); return; }
    snprintf(out, (size_t)cap, "{\"id\":%d,\"ok\":true,\"ring\":%s}", id, rbuf);
    debug_server_send_line(out);
    free(out);
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

/* insn_freeze_target <target>: freeze the insn ring the instant an interpreted
 * instruction transfers to (or falls through to) <target>. Used to capture the
 * Tomba2 worker wild-jump to 0x49422E54 with the offending jr's register
 * snapshot as the ring tail. target=0 disarms. */
/* s3_smear_watch lo=<hex> hi=<hex> [excl=<hex insn>] — arm the callee-smear
 * tripwire (dirty_ram_interp.c): latches the first interp instruction in
 * [lo,hi) whose execution changes $s3 (a jalr's exec_one spans the whole
 * nested callee, so the latch names the callee that clobbered a callee-saved
 * register). excl is an optional exact instruction encoding to ignore, so a
 * watched loop's own $s3 advance doesn't trip the latch. Called with no
 * args, reports the latch. lo=0 disarms. */
static void handle_s3_smear_watch(int id, const char *json)
{
    extern uint32_t g_s3_smear_lo, g_s3_smear_hi, g_s3_smear_excl;
    extern uint32_t g_s3_smear_pc, g_s3_smear_insn, g_s3_smear_old,
                    g_s3_smear_new, g_s3_smear_tgt, g_s3_smear_frame;
    extern int g_s3_smear_valid;
    char buf[32];
    if (json_get_str(json, "lo", buf, sizeof(buf))) {
        g_s3_smear_lo = hex_to_u32(buf);
        /* each arming fully re-specifies the watch: omitted = cleared */
        g_s3_smear_hi = json_get_str(json, "hi", buf, sizeof(buf))
                            ? hex_to_u32(buf) : 0u;
        g_s3_smear_excl = json_get_str(json, "excl", buf, sizeof(buf))
                            ? hex_to_u32(buf) : 0u;
        g_s3_smear_valid = 0;
    }
    /* `armed` so a never-tripped watch (valid:0) can be told apart from one
     * that was never recording — the same ambiguity the callret ring had. */
    send_fmt("{\"id\":%d,\"ok\":true,\"armed\":%s,"
             "\"lo\":\"0x%08X\",\"hi\":\"0x%08X\","
             "\"excl\":\"0x%08X\","
             "\"valid\":%d,\"pc\":\"0x%08X\",\"insn\":\"0x%08X\","
             "\"s3_old\":\"0x%08X\",\"s3_new\":\"0x%08X\","
             "\"call_target\":\"0x%08X\",\"frame\":%u}\n",
             id, (g_s3_smear_hi > g_s3_smear_lo) ? "true" : "false",
             g_s3_smear_lo, g_s3_smear_hi, g_s3_smear_excl,
             g_s3_smear_valid,
             g_s3_smear_pc, g_s3_smear_insn, g_s3_smear_old, g_s3_smear_new,
             g_s3_smear_tgt, g_s3_smear_frame);
}

/* callret_watch lo=<hex> hi=<hex> — arm the call-resolution ring
 * (dirty_ram_interp.c): every interp JALR whose call PC lies in [lo,hi)
 * records its resolution tier + full post-call outcome. No args = dump the
 * ring (newest last). lo=0 disarms. */
static void handle_callret_watch(int id, const char *json)
{
    /* MUST stay field-for-field identical to CallRetEnt in
     * dirty_ram_interp.c (a divergence dumps garbage with no compiler
     * diagnostic — the ring is only visible here as an opaque extern). */
    typedef struct {
        uint64_t cycle; uint32_t frame;
        uint32_t pc, target, sp_b, ra_b, s0_b, s3_b;
        uint32_t path;
        uint32_t pc_a, ra_a, sp_a, s0_a, s3_a, v0_a;
        uint32_t bail_a, rfe_a, esc_a, in_exc_a;
        uint32_t dstatic, dblocks, dexc;
        uint32_t last_func_a;
    } E;
    extern uint32_t g_callret_lo, g_callret_hi;
    extern int callret_armed(void);
    extern E g_callret_ring[]; extern uint64_t g_callret_seq;
    const uint32_t cap = 64u;   /* MUST match CALLRET_CAP (dirty_ram_interp.c) */
    char buf[32];
    /* Explicit disarm, and the documented legacy spelling for it. `lo` with no
     * `hi` used to mean "disarm" only because the gate tested lo != 0; keep
     * that contract for lo=0 (scripts rely on it) but answer it explicitly
     * instead of silently. */
    const int want_disarm = json_get_int(json, "disarm", 0) != 0;
    if (want_disarm) {
        g_callret_lo = g_callret_hi = 0;
        g_callret_seq = 0;
        send_fmt("{\"id\":%d,\"ok\":true,\"armed\":false,\"lo\":\"0x00000000\","
                 "\"hi\":\"0x00000000\"}\n", id);
        return;
    }
    if (json_get_str(json, "lo", buf, sizeof(buf))) {
        const uint32_t new_lo = hex_to_u32(buf);
        char hibuf[32];
        const int have_hi = json_get_str(json, "hi", hibuf, sizeof(hibuf)) != NULL;
        if (!have_hi) {
            /* Legacy disarm: {"lo":"0"} with no hi. Honour it, name it. */
            if (new_lo == 0) {
                g_callret_lo = g_callret_hi = 0;
                g_callret_seq = 0;
                send_fmt("{\"id\":%d,\"ok\":true,\"armed\":false,"
                         "\"lo\":\"0x00000000\",\"hi\":\"0x00000000\","
                         "\"note\":\"disarmed (legacy lo=0 spelling; prefer"
                         " disarm=true)\"}\n", id);
                return;
            }
            /* A floor with no ceiling silently inherited the previous hi —
             * usually 0, i.e. an empty window that recorded nothing while
             * replying ok. Refuse instead of guessing. */
            send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"lo without hi\","
                     "\"armed\":%s,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\","
                     "\"note\":\"pass BOTH lo and hi (hi > lo) to arm, or"
                     " disarm=true to stop; window left unchanged\"}\n",
                     id, callret_armed() ? "true" : "false",
                     g_callret_lo, g_callret_hi);
            return;
        }
        g_callret_lo = new_lo;
        g_callret_hi = hex_to_u32(hibuf);
        g_callret_seq = 0;
        /* Report `armed` rather than leaving the caller to infer it: an empty
         * window records nothing, and "total: 0" from an unarmed ring reads
         * exactly like a real "these events never happened" answer. */
        const int armed = callret_armed();
        send_fmt("{\"id\":%d,\"ok\":true,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\","
                 "\"armed\":%s%s}\n",
                 id, g_callret_lo, g_callret_hi, armed ? "true" : "false",
                 armed ? ""
                       : ",\"note\":\"empty window (hi <= lo) - NOT recording\"");
        return;
    }
    uint64_t total = g_callret_seq;
    uint32_t avail = total < cap ? (uint32_t)total : cap;
    size_t BUF_SZ = 512u + (size_t)avail * 512u;
    char *out = (char *)malloc(BUF_SZ); if (!out) { send_err(id, "oom"); return; }
    /* Carry the window and armed state on the READ too: "total": 0 is
     * otherwise indistinguishable between "armed, nothing matched" and "never
     * armed", and the second reads as evidence that the events did not happen. */
    size_t pos = (size_t)snprintf(out, BUF_SZ,
        "{\"id\":%d,\"ok\":true,\"armed\":%s,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\","
        "\"total\":%llu,\"entries\":[",
        id, callret_armed() ? "true" : "false", g_callret_lo, g_callret_hi,
        (unsigned long long)total);
    for (uint32_t i = 0; i < avail && pos < BUF_SZ - 600; i++) {
        E *e = &g_callret_ring[(total - avail + i) & (cap - 1u)];
        pos += (size_t)snprintf(out + pos, BUF_SZ - pos,
            "%s{\"cyc\":%llu,\"f\":%u,\"pc\":\"0x%08X\",\"tgt\":\"0x%08X\","
            "\"path\":%u,"
            "\"sp_b\":\"0x%08X\",\"ra_b\":\"0x%08X\",\"s0_b\":\"0x%08X\",\"s3_b\":\"0x%08X\","
            "\"pc_a\":\"0x%08X\",\"ra_a\":\"0x%08X\",\"sp_a\":\"0x%08X\","
            "\"s0_a\":\"0x%08X\",\"s3_a\":\"0x%08X\",\"v0_a\":\"0x%08X\","
            "\"bail\":%u,\"rfe\":%u,\"esc\":%u,\"in_exc\":%u,"
            "\"dstatic\":%u,\"dblocks\":%u,\"dexc\":%u,\"last_func\":\"0x%08X\"}",
            i ? "," : "", (unsigned long long)e->cycle, e->frame, e->pc, e->target,
            e->path,
            e->sp_b, e->ra_b, e->s0_b, e->s3_b,
            e->pc_a, e->ra_a, e->sp_a, e->s0_a, e->s3_a, e->v0_a,
            e->bail_a, e->rfe_a, e->esc_a, e->in_exc_a,
            e->dstatic, e->dblocks, e->dexc, e->last_func_a);
    }
    pos += (size_t)snprintf(out + pos, BUF_SZ - pos, "]}");
    debug_server_send_line(out);
    free(out);
}

static void handle_insn_freeze_target(int id, const char *json)
{
    extern uint32_t g_insn_freeze_on_target;
    extern int g_insn_log_frozen, g_freeze_snap_valid;
    char buf[32];
    if (json_get_str(json, "target", buf, sizeof(buf)))
        g_insn_freeze_on_target = hex_to_u32(buf);
    g_insn_log_frozen   = 0;
    g_freeze_snap_valid = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"freeze_on_target\":\"0x%08X\"}\n",
             id, g_insn_freeze_on_target);
}

/* insn_freeze_snapshot: report the register file captured at the jr/jalr that
 * hit the watched wild target (g_insn_freeze_on_target). Reveals the source
 * register (scan for the reg whose value == the target) -> ra=stack/sp bug,
 * t9/v0/v1=filename-ptr-as-fn-ptr, etc. */
static void handle_insn_freeze_snapshot(int id, const char *json)
{
    (void)json;
    extern int g_freeze_snap_valid;
    extern uint32_t g_freeze_snap_pc, g_freeze_snap_insn, g_freeze_snap_tcb;
    extern uint32_t g_freeze_snap_gpr[32];
    extern uint32_t g_insn_freeze_on_target;
    static const char *RN[32] = {
        "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3",
        "t4","t5","t6","t7","s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra" };
    char buf[2048];
    size_t pos = 0;
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "{\"id\":%d,\"ok\":true,\"valid\":%d,\"pc\":\"0x%08X\",\"insn\":\"0x%08X\","
        "\"tcb\":\"0x%08X\",\"target\":\"0x%08X\",\"regs\":{",
        id, g_freeze_snap_valid, g_freeze_snap_pc, g_freeze_snap_insn,
        g_freeze_snap_tcb, g_insn_freeze_on_target);
    for (int r = 0; r < 32; r++)
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%s\"%s\":\"0x%08X\"",
                        r ? "," : "", RN[r], g_freeze_snap_gpr[r]);
    pos += snprintf(buf+pos, sizeof(buf)-pos, "}}\n");
    (void)pos;
    send_fmt("%s", buf);
}

/* ra_load_watch <value>: arm/read capture of the instruction that sets $ra to
 * <value> (the wild target). With no "value" arg, reports the captured snapshot:
 * the loading pc/insn, the source stack address (for an lw), and the full GPR
 * file. value=0 disarms+clears. */
static void handle_ra_load_watch(int id, const char *json)
{
    extern uint32_t g_ra_load_watch, g_ra_load_snap_pc, g_ra_load_snap_insn;
    extern uint32_t g_ra_load_snap_before_ra, g_ra_load_snap_srcaddr;
    extern uint32_t g_ra_load_snap_gpr[32];
    extern int g_ra_load_snap_valid;
    char vbuf[32];
    if (json_get_str(json, "value", vbuf, sizeof(vbuf))) {
        g_ra_load_watch = hex_to_u32(vbuf);
        g_ra_load_snap_valid = 0;
    }
    static const char *RN[32] = {
        "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3",
        "t4","t5","t6","t7","s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra" };
    char buf[2048]; size_t pos = 0;
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "{\"id\":%d,\"ok\":true,\"watch\":\"0x%08X\",\"valid\":%d,\"load_pc\":\"0x%08X\","
        "\"insn\":\"0x%08X\",\"before_ra\":\"0x%08X\",\"src_addr\":\"0x%08X\",\"regs\":{",
        id, g_ra_load_watch, g_ra_load_snap_valid, g_ra_load_snap_pc,
        g_ra_load_snap_insn, g_ra_load_snap_before_ra, g_ra_load_snap_srcaddr);
    for (int r = 0; r < 32; r++)
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%s\"%s\":\"0x%08X\"",
                        r ? "," : "", RN[r], g_ra_load_snap_gpr[r]);
    pos += snprintf(buf+pos, sizeof(buf)-pos, "}}\n");
    (void)pos;
    send_fmt("%s", buf);
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

/* overlay_native_block: per-function native-disable for bisection. Forces the
 * named overlay function(s) through the sanctioned dirty-RAM interpreter (the
 * function still runs — NOT skipped/stubbed), so you can binary-search which
 * compiled function's native execution causes a divergence without rebuilding.
 *   {"cmd":"overlay_native_block","addr":"0x80050B08"}  -> add (keyed by phys)
 *   {"cmd":"overlay_native_block","clear":1}            -> clear all
 *   {"cmd":"overlay_native_block"}                      -> just report state */
static void handle_overlay_native_block(int id, const char *json)
{
    extern int      overlay_loader_native_block_add(uint32_t addr);
    extern void     overlay_loader_native_block_clear(void);
    extern int      overlay_loader_native_block_list(uint32_t *out, int cap);
    extern uint64_t overlay_loader_native_block_hits(void);
    char abuf[32];
    if (json_get_int(json, "clear", 0)) overlay_loader_native_block_clear();
    if (json_get_str(json, "addr", abuf, sizeof(abuf)))
        overlay_loader_native_block_add(hex_to_u32(abuf));
    uint32_t list[64];
    int n = overlay_loader_native_block_list(list, 64);
    char buf[1200]; size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
        "{\"id\":%d,\"ok\":true,\"count\":%d,\"hits\":%llu,\"blocked\":[",
        id, n, (unsigned long long)overlay_loader_native_block_hits());
    for (int i = 0; i < n && i < 64; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
            "%s\"0x%08X\"", i ? "," : "", list[i]);
    snprintf(buf + pos, sizeof(buf) - pos, "]}\n");
    send_fmt("%s", buf);
}

/* overlay_cps_probe: arm/dump the CPS interior-continuation dispatch probe.
 *   {"cmd":"overlay_cps_probe","addr":"0x80050B30"} -> arm for that PC
 *   {"cmd":"overlay_cps_probe"}                     -> dump last decision
 * outcome: 0=find<0, 1=crc-miss->interp, 2=ran native, 3=device->interp, 4=blocked */
static void handle_overlay_cps_probe(int id, const char *json)
{
    extern void overlay_loader_cps_probe_set(uint32_t pc);
    extern void overlay_loader_cps_probe_get(uint32_t *pc, uint64_t *cnt,
        uint32_t *found, int *ci, int *nrange, int *matched, int *outcome, int *ncand);
    char abuf[32];
    if (json_get_str(json, "addr", abuf, sizeof(abuf)))
        overlay_loader_cps_probe_set(hex_to_u32(abuf));
    uint32_t pc = 0, found = 0; uint64_t cnt = 0;
    int ci = 0, nrange = 0, matched = 0, outcome = 0, ncand = 0;
    overlay_loader_cps_probe_get(&pc, &cnt, &found, &ci, &nrange, &matched, &outcome, &ncand);
    send_fmt("{\"id\":%d,\"ok\":true,\"probe_pc\":\"0x%08X\",\"count\":%llu,"
             "\"chosen_addr\":\"0x%08X\",\"ci\":%d,\"chosen_nranges\":%d,"
             "\"cands_in_range\":%d,\"crc_matched\":%d,\"outcome\":%d}\n",
             id, pc, cnt, found, ci, nrange, ncand, matched, outcome);
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

static void handle_game_options(int id, const char *json)
{
    (void)json;
    extern int game_options_debug_json(char *out, int cap);
    char buf[3072];
    game_options_debug_json(buf, sizeof(buf));
    send_fmt("{\"id\":%d,\"ok\":true,\"go\":%s}", id, buf);
}

/* On-the-fly string translation (text_xlate.cpp). Always-on capture inventory +
 * apply stats/dump, queried over TCP (no log files — Rule 3). sub: stats (def) |
 * dump | todo | reload. */
static void handle_xlate(int id, const char *json)
{
    extern int text_xlate_debug_json(const char *subcmd, char *out, int cap);
    char sub[32] = {0};
    if (!json_get_str(json, "sub", sub, sizeof(sub))) strcpy(sub, "stats");
    static char buf[1 << 20];   /* 1 MB — the inventory dump can be large */
    int n = text_xlate_debug_json(sub, buf, (int)sizeof(buf));
    if (n < 0) n = 0;
    if (n < (int)sizeof(buf)) buf[n] = 0; else buf[sizeof(buf) - 1] = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"xlate\":%s}", id, buf);
}

/* Live host-stack-usage profile (RECURSION_BUG.md §17): read the always-on ring
 * while the game is still responsive to distinguish a gradual cross-frame leak
 * (used_kb climbs linearly with frame) from a within-one-frame runaway. */
static void handle_stack_profile(int id, const char *json)
{
    (void)json;
    extern int stack_profile_json(char *out, int cap);
    static char buf[24576];
    stack_profile_json(buf, (int)sizeof(buf));
    send_fmt("{\"id\":%d,\"ok\":true,\"sp\":%s}", id, buf);
}

/* Live boundary control-flow flight recorder (RECURSION_BUG.md §18): per-frame
 * crossing/depth summary + per-crossing onset detail. Read while responsive to
 * see whether the interp<->compiled recursion accumulates across frames or
 * explodes within one. */
static void handle_xprobe(int id, const char *json)
{
    (void)json;
    extern int dirty_ram_xprobe_json(char *out, int cap);
    static char buf[1048576];
    /* send_fmt's formatting buffer is 64KB — cap the live dump (summary + a
     * detail window) under that. The full rings go to the crash report file. */
    dirty_ram_xprobe_json(buf, 56000);
    send_fmt("{\"id\":%d,\"ok\":true,\"xprobe\":%s}", id, buf);
}

/* §19 compiled-entry depth profile: per-frame max host-stack-used sampled at
 * EVERY compiled function entry. Read live to see whether the frozen frame's
 * stack actually climbs (real recursion) or stays flat (garbage guard trip). */
static void handle_ce_profile(int id, const char *json)
{
    (void)json;
    extern int ce_profile_json(char *out, int cap);
    static char buf[49152];
    ce_profile_json(buf, (int)sizeof(buf));
    send_fmt("{\"id\":%d,\"ok\":true,\"ce\":%s}", id, buf);
}

/* Arm the §18 boundary trip at runtime once the idle baseline is known:
 * xprobe_arm {"frame_trip":N,"stk_kb":K,"warmup":F}. Any 0 disables that arm. */
/* "xprobe_watch": get or set the JAL/JALR watched-target list that filters the
 * xprobe `watched` dump. Previously a hardcoded list of one title's addresses,
 * so every other game silently watched nothing. Pass `targets` (comma /
 * semicolon / space separated, 0x or decimal) to replace the list; pass an
 * empty string to clear it; pass nothing to read it back. Also settable before
 * the process starts via PSX_XPROBE_WATCH, so a target can be watched from
 * instruction zero. Always reports the active list, so an empty `watched` dump
 * is never ambiguous between "not called" and "not watched". */
static void handle_xprobe_watch(int id, const char *json)
{
    extern void dirty_ram_xprobe_watch_set(const char *spec);
    extern int  dirty_ram_xprobe_watch_get(int index, uint32_t *phys_out);
    extern int  dirty_ram_xprobe_watch_count(void);
    char spec[512];
    if (json_get_str(json, "targets", spec, sizeof(spec)))
        dirty_ram_xprobe_watch_set(spec);
    char list[768];
    size_t pos = 0;
    const int n = dirty_ram_xprobe_watch_count();
    for (int i = 0; i < n && pos < sizeof(list) - 24; i++) {
        uint32_t phys = 0;
        if (!dirty_ram_xprobe_watch_get(i, &phys)) break;
        pos += (size_t)snprintf(list + pos, sizeof(list) - pos, "%s\"0x%08X\"",
                                i ? "," : "", phys);
    }
    list[pos] = '\0';
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%d,\"watching\":[%s]%s}",
             id, n, list,
             n ? "" : ",\"note\":\"no targets watched - the xprobe watched dump"
                      " will be empty regardless of what the game calls; pass"
                      " targets=0x... to watch\"");
}

static void handle_xprobe_arm(int id, const char *json)
{
    extern void dirty_ram_xprobe_arm(int frame_trip, int stk_kb, int warmup);
    int ft = json_get_int(json, "frame_trip", 0);
    int sk = json_get_int(json, "stk_kb", 0);
    int wu = json_get_int(json, "warmup", 0);
    dirty_ram_xprobe_arm(ft, sk, wu);
    send_fmt("{\"id\":%d,\"ok\":true,\"frame_trip\":%d,\"stk_kb\":%d,\"warmup\":%d}",
             id, ft, sk, wu);
}

/* "latency": input->photon latency summary from the always-on latency ring.
 * Optional args: window=N (frames to summarize, default 240), raw=1 (also
 * include the last `count` raw per-frame records, count default 120). */
static void handle_latency(int id, const char *json)
{
    int window = json_get_int(json, "window", 240);
    int raw    = json_get_int(json, "raw", 0);
    int count  = json_get_int(json, "count", 120);
    static char sum[2048];
    latency_ring_summary_json(sum, sizeof(sum), window);
    if (raw) {
        static char rawbuf[16384];
        latency_ring_dump_json(rawbuf, sizeof(rawbuf), count);
        send_fmt("{\"id\":%d,\"ok\":true,\"summary\":%s,\"frames\":%s}",
                 id, sum, rawbuf);
    } else {
        send_fmt("{\"id\":%d,\"ok\":true,\"summary\":%s}", id, sum);
    }
}

/* "vk_perf": last N Vulkan per-frame op counters (allocs/submits/pack/blit/...).
 * Always-on ring in gpu_vk_renderer.c; used to find which op explodes during a
 * transition stall. arg count=N (default 60). */
static void handle_vk_perf(int id, const char *json)
{
    extern int vk_perf_json(char *out, int cap, int count);
    int count = json_get_int(json, "count", 60);
    static char buf[49152];
    vk_perf_json(buf, (int)sizeof(buf), count);
    send_fmt("{\"id\":%d,\"ok\":true,\"vk_perf\":%s}", id, buf);
}

static void handle_lockstep(int id, const char *json) {
    /* #2 lockstep comparator. {"lo":N,"hi":M} arms the frame window; the reply
     * reports the first compiled-vs-interp block divergence (if any) so far. */
    extern void ls_set_window(uint32_t, uint32_t);
    extern void ls_set_record_only(int);
    extern int  ls_get_diverge_json(char*, int);
    int lo = json_get_int(json, "lo", -1);
    int hi = json_get_int(json, "hi", -1);
    int ro = json_get_int(json, "record_only", -1);
    if (ro >= 0) ls_set_record_only(ro);
    if (lo >= 0 && hi >= 0) ls_set_window((uint32_t)lo, (uint32_t)hi);
    char buf[4096];
    ls_get_diverge_json(buf, (int)sizeof(buf));
    send_fmt("{\"id\":%d,\"ok\":true,\"lockstep\":%s}", id, buf);
}

static void handle_lockstep_func(int id, const char *json) {
    /* Dispatch-segment lockstep comparator. Same arming shape as lockstep,
     * but the measured unit is one clean psx_dispatch_game_compiled() segment. */
    extern void ls_func_set_window(uint32_t, uint32_t);
    extern void ls_func_set_record_only(int);
    extern int  ls_get_func_json(char*, int);
    int lo = json_get_int(json, "lo", -1);
    int hi = json_get_int(json, "hi", -1);
    int ro = json_get_int(json, "record_only", -1);
    if (ro >= 0) ls_func_set_record_only(ro);
    if (lo >= 0 && hi >= 0) ls_func_set_window((uint32_t)lo, (uint32_t)hi);
    char buf[8192];
    ls_get_func_json(buf, (int)sizeof(buf));
    send_fmt("{\"id\":%d,\"ok\":true,\"lockstep_func\":%s}", id, buf);
}

/* ---- phase_profile: statistical wall-time attribution (always-on) ---------
 * A dedicated ~1 kHz sampler thread reads the emu thread's phase flags and
 * accumulates per-second buckets in a ring. It answers "what fraction of wall
 * time is the dirty-RAM interpreter / guest exception context" DIRECTLY,
 * without inference from dispatch or instruction counts. Two properties are
 * load-bearing:
 *  - It runs OFF the main thread. The TCP server itself pumps on the main
 *    thread, so a handler-side busy sampler would freeze the very thing it
 *    measures.
 *  - It samples g_exec_phase (dirty_ram_interp.c), the INNERMOST backend
 *    executing at that instant — NOT g_dirty_interp_active, which stays 1
 *    across native overlay calls made from the interpreter's call contract
 *    and therefore reads "inside the dispatch tree", not "interpreting".
 *    g_exec_phase is save/restored at every backend boundary and across
 *    exception delivery, so leaked intervals self-heal at the next bracket.
 * SDL init gives the process 1 ms timer resolution, so Sleep(1) really is
 * ~1 kHz. Query: {"cmd":"phase_profile","window":N} -> shares over the last
 * N whole seconds (default 10, in-progress second excluded) + cumulative. */
extern int psx_exec_phase(void);   /* 0=other 1=interp 2=native 3=static 4=gpu-gp0 */
extern int psx_get_in_exception(void);
extern uint32_t overlay_loader_native_inprogress(void);

#define PHASE_RING_SECS 64
static PSX_BSS volatile uint32_t s_phase_total [PHASE_RING_SECS];
static PSX_BSS volatile uint32_t s_phase_interp[PHASE_RING_SECS];
static PSX_BSS volatile uint32_t s_phase_native[PHASE_RING_SECS];
static PSX_BSS volatile uint32_t s_phase_static[PHASE_RING_SECS];
static PSX_BSS volatile uint32_t s_phase_gpu   [PHASE_RING_SECS];
static PSX_BSS volatile uint32_t s_phase_exc   [PHASE_RING_SECS];
static PSX_BSS volatile uint64_t s_phase_sec   [PHASE_RING_SECS];
static volatile uint64_t s_phase_samples_all = 0, s_phase_interp_all = 0;

/* Hot-function histogram: when a sample lands in a native overlay shard,
 * bucket the shard's registered entry address (cumulative since boot; a
 * client diffs two snapshots for a window). Open addressing, sampler-thread
 * writes only. */
#define PHOT_SLOTS 4096
static volatile uint32_t s_phot_addr[PHOT_SLOTS];
static volatile uint64_t s_phot_cnt [PHOT_SLOTS];
static volatile uint64_t s_phot_native_total = 0, s_phot_drops = 0;

/* Same histogram for STATIC-phase samples, keyed on the static-dispatch
 * stamp (g_debug_current_func_addr). Coarser than the native histogram —
 * the stamp is per static dispatch, not per C frame — but under CPS the
 * dispatch cadence is dense enough to attribute wall time to main-EXE
 * functions (the load-window decompressor question). */
static volatile uint32_t s_phots_addr[PHOT_SLOTS];
static volatile uint64_t s_phots_cnt [PHOT_SLOTS];
static volatile uint64_t s_phot_static_total = 0, s_phots_drops = 0;

static void phot_add_to(volatile uint32_t *ha, volatile uint64_t *hc,
                        volatile uint64_t *drops, uint32_t addr)
{
    if (!addr) return;
    uint32_t h = (addr >> 2) * 2654435761u;
    for (uint32_t p = 0; p < 16; p++) {
        uint32_t i = (h + p) & (PHOT_SLOTS - 1u);
        uint32_t a = ha[i];
        if (a == addr) { hc[i]++; return; }
        if (a == 0)    { ha[i] = addr; hc[i] = 1; return; }
    }
    (*drops)++;
}

static void phot_add(uint32_t addr)
{
    phot_add_to(s_phot_addr, s_phot_cnt, &s_phot_drops, addr);
}

#ifdef _WIN32
static DWORD WINAPI phase_sampler_main(LPVOID arg)
#else
static void *phase_sampler_main(void *arg)
#endif
{
    (void)arg;
    for (;;) {
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
#endif
        uint64_t sec = monotonic_ms() / 1000u;
        int slot = (int)(sec % PHASE_RING_SECS);
        if (s_phase_sec[slot] != sec) {          /* bucket rolls to a new second */
            s_phase_sec[slot]    = sec;
            s_phase_total[slot]  = 0;
            s_phase_interp[slot] = 0;
            s_phase_native[slot] = 0;
            s_phase_static[slot] = 0;
            s_phase_gpu[slot]    = 0;
            s_phase_exc[slot]    = 0;
        }
        s_phase_total[slot]++;
        s_phase_samples_all++;
        switch (psx_exec_phase()) {
        case 1: s_phase_interp[slot]++; s_phase_interp_all++; break;
        case 2: s_phase_native[slot]++;
                s_phot_native_total++;
                phot_add(overlay_loader_native_inprogress());
                break;
        case 3: s_phase_static[slot]++;
                s_phot_static_total++;
                {
                    extern volatile uint32_t g_psx_last_fn_entry;
                    phot_add_to(s_phots_addr, s_phots_cnt, &s_phots_drops,
                                g_psx_last_fn_entry);
                }
                break;
        case 4: s_phase_gpu[slot]++; break;
        default: break;                          /* 0 = host/other */
        }
        if (psx_get_in_exception())       { s_phase_exc[slot]++; }
    }
#ifndef _WIN32
    return NULL;
#endif
}

static void phase_sampler_start(void)
{
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, phase_sampler_main, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, phase_sampler_main, NULL) == 0)
        pthread_detach(t);
#endif
}

static void handle_phase_profile(int id, const char *json)
{
    int window = json_get_int(json, "window", 10);
    if (window < 1) window = 1;
    if (window > PHASE_RING_SECS - 2) window = PHASE_RING_SECS - 2;
    uint64_t now_sec = monotonic_ms() / 1000u;
    uint32_t tot = 0, itp = 0, nat = 0, sta = 0, gpu = 0, exc = 0;
    for (int k = 1; k <= window; k++) {          /* whole seconds only */
        uint64_t sec = now_sec - (uint64_t)k;
        int slot = (int)(sec % PHASE_RING_SECS);
        if (s_phase_sec[slot] == sec) {
            tot += s_phase_total[slot];
            itp += s_phase_interp[slot];
            nat += s_phase_native[slot];
            sta += s_phase_static[slot];
            gpu += s_phase_gpu[slot];
            exc += s_phase_exc[slot];
        }
    }
    uint32_t oth = tot - itp - nat - sta - gpu;
    send_fmt("{\"id\":%d,\"ok\":true,\"window_s\":%d,\"samples\":%u,"
             "\"interp_samples\":%u,\"interp_share\":%.4f,"
             "\"native_samples\":%u,\"native_share\":%.4f,"
             "\"static_samples\":%u,\"static_share\":%.4f,"
             "\"gpu_samples\":%u,\"gpu_share\":%.4f,"
             "\"other_samples\":%u,\"other_share\":%.4f,"
             "\"exc_samples\":%u,\"exc_share\":%.4f,"
             "\"samples_total\":%llu,\"interp_total\":%llu}",
             id, window, tot,
             itp, tot ? (double)itp / (double)tot : 0.0,
             nat, tot ? (double)nat / (double)tot : 0.0,
             sta, tot ? (double)sta / (double)tot : 0.0,
             gpu, tot ? (double)gpu / (double)tot : 0.0,
             oth, tot ? (double)oth / (double)tot : 0.0,
             exc, tot ? (double)exc / (double)tot : 0.0,
             (unsigned long long)s_phase_samples_all,
             (unsigned long long)s_phase_interp_all);
}

/* phase_hot: top guest functions by native wall-time samples (cumulative
 * since boot — diff two snapshots for a window). {"cmd":"phase_hot","top":N}
 * Optional {"set":"static"} ranks the STATIC-phase histogram instead
 * (main-EXE functions via the static-dispatch stamp). */
static void handle_phase_hot(int id, const char *json)
{
    int top = json_get_int(json, "top", 20);
    if (top < 1)  top = 1;
    if (top > 64) top = 64;
    char set[16] = "native";
    json_get_str(json, "set", set, sizeof(set));
    int is_static = (set[0] == 's');
    volatile uint32_t *ha = is_static ? s_phots_addr : s_phot_addr;
    volatile uint64_t *hc = is_static ? s_phots_cnt  : s_phot_cnt;
    uint32_t best_addr[64];
    uint64_t best_cnt[64];
    int n = 0;
    for (int i = 0; i < PHOT_SLOTS; i++) {
        uint32_t a = ha[i];
        if (!a) continue;
        uint64_t c = hc[i];
        int j = n < top ? n : top - 1;
        if (n < top) n++;
        else if (c <= best_cnt[j]) continue;
        while (j > 0 && best_cnt[j - 1] < c) {
            best_addr[j] = best_addr[j - 1];
            best_cnt[j]  = best_cnt[j - 1];
            j--;
        }
        best_addr[j] = a;
        best_cnt[j]  = c;
    }
    uint64_t tot = is_static ? s_phot_static_total : s_phot_native_total;
    char buf[4096];
    int len = snprintf(buf, sizeof(buf),
                       "{\"id\":%d,\"ok\":true,\"set\":\"%s\","
                       "\"phase_samples_total\":%llu,"
                       "\"hash_drops\":%llu,\"top\":[",
                       id, is_static ? "static" : "native",
                       (unsigned long long)tot,
                       (unsigned long long)(is_static ? s_phots_drops
                                                      : s_phot_drops));
    for (int i = 0; i < n && len < (int)sizeof(buf) - 96; i++) {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len,
                        "%s{\"addr\":\"0x%08X\",\"samples\":%llu,\"share\":%.4f}",
                        i ? "," : "", best_addr[i],
                        (unsigned long long)best_cnt[i],
                        tot ? (double)best_cnt[i] / (double)tot : 0.0);
    }
    len += snprintf(buf + len, sizeof(buf) - (size_t)len, "]}");
    (void)len;
    send_fmt("%s", buf);
}

/* idle_skip: idle-loop cycle-skip status + runtime toggle.
 *   {"cmd":"idle_skip"}              -> counters
 *   {"cmd":"idle_skip","enable":0|1} -> toggle, then counters */
static void handle_idle_skip(int id, const char *json)
{
    extern int      g_idle_skip_enabled;
    extern uint64_t g_idle_skip_count, g_idle_skip_cycles;
    extern uint32_t g_idle_skip_last_pc, g_idle_skip_last_quantum;
    int en = json_get_int(json, "enable", -1);
    if (en == 0 || en == 1) g_idle_skip_enabled = en;
    send_fmt("{\"id\":%d,\"ok\":true,\"enabled\":%d,"
             "\"skips\":%llu,\"cycles_skipped\":%llu,"
             "\"last_pc\":\"0x%08X\",\"last_quantum\":%u}",
             id, g_idle_skip_enabled,
             (unsigned long long)g_idle_skip_count,
             (unsigned long long)g_idle_skip_cycles,
             g_idle_skip_last_pc, g_idle_skip_last_quantum);
}

/* starv_ring: query the always-on starvation/PC-sample ring (16K entries,
 * continuous since boot). {"cmd":"starv_ring","count":N,"kind":K} -> last N
 * entries (newest last), K = StarvationEventKind filter (15 = PC samples;
 * -1/omitted = all kinds). PC samples carry (cyc, host_us, current_func):
 * consecutive deltas give guest-throughput over wall time, and current_func
 * localizes where the emu thread was — the ring-buffer answer to "where did
 * the last N seconds go" without arming anything. */
static void handle_starv_ring(int id, const char *json)
{
    int count = json_get_int(json, "count", 128);
    int kind  = json_get_int(json, "kind", -1);
    if (count < 1)    count = 1;
    if (count > 2048) count = 2048;
    uint64_t total = starvation_ring_total();
    /* Walk backward collecting up to `count` matches, then emit oldest-first. */
    static uint64_t match_seq[2048];
    int n = 0;
    uint64_t seq = total;
    StarvationEntry e;
    while (n < count && seq > 0) {
        seq--;
        if (!starvation_ring_get(seq, &e)) break;   /* fell off the ring */
        if (kind >= 0 && e.kind != (uint8_t)kind) continue;
        match_seq[n++] = seq;
    }
    size_t cap = 160u * (size_t)(n + 2);
    char *buf = (char *)malloc(cap);
    if (!buf) { send_err(id, "oom"); return; }
    int len = snprintf(buf, cap,
                       "{\"id\":%d,\"ok\":true,\"total\":%llu,\"returned\":%d,"
                       "\"entries\":[", id, (unsigned long long)total, n);
    int emitted = 0;
    for (int i = n - 1; i >= 0; i--) {              /* oldest first */
        if (!starvation_ring_get(match_seq[i], &e)) continue;
        len += snprintf(buf + len, cap - (size_t)len,
                        "%s{\"seq\":%llu,\"kind\":%u,\"cyc\":%llu,\"us\":%llu,"
                        "\"func\":\"0x%08X\",\"store_pc\":\"0x%08X\",\"in_exc\":%u}",
                        emitted++ ? "," : "",
                        (unsigned long long)e.seq, e.kind,
                        (unsigned long long)e.psx_cycle_count,
                        (unsigned long long)e.host_us,
                        e.current_func, e.last_store_pc, e.in_exception);
        if ((size_t)len + 192 > cap) break;         /* never overrun */
    }
    len += snprintf(buf + len, cap - (size_t)len, "]}");
    (void)len;
    send_fmt("%s", buf);
    free(buf);
}

/* data_shards: memoized pure-function replay counters (data_shards.c).
 *   {"cmd":"data_shards"}              -> counters
 *   {"cmd":"data_shards","enable":0|1} -> toggle, then counters */
static void handle_data_shards(int id, const char *json)
{
    extern void ds_stats_json(char* buf, int cap);
    extern void ds_set_enabled(int on);
    int en = json_get_int(json, "enable", -1);
    if (en == 0 || en == 1) ds_set_enabled(en);
    char body[1024];
    ds_stats_json(body, sizeof(body));
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, body);
}

/* vsync_query_hle: cycle-faithful VSync(-1) query acceleration counters. */
static void handle_vsync_query_hle(int id, const char *json)
{
    extern void psx_vsync_query_hle_stats_json(char* buf, int cap);
    extern void psx_vsync_query_hle_set_enabled(int on);
    extern void psx_vsync_query_hle_set_horizon_enabled(int on);
    extern void psx_vsync_query_hle_set_extra_horizon_enabled(int on);
    int en = json_get_int(json, "enable", -1);
    int horizon = json_get_int(json, "horizon", -1);
    int extra = json_get_int(json, "extra", -1);
    if (en == 0 || en == 1) psx_vsync_query_hle_set_enabled(en);
    if (horizon == 0 || horizon == 1)
        psx_vsync_query_hle_set_horizon_enabled(horizon);
    if (extra == 0 || extra == 1)
        psx_vsync_query_hle_set_extra_horizon_enabled(extra);
    char body[512];
    psx_vsync_query_hle_stats_json(body, sizeof(body));
    send_fmt("{\"id\":%d,\"ok\":true,%s}", id, body);
}

/* warm_cd_route: live A/B toggle and fail-closed route counters. */
static void handle_warm_cd_route(int id, const char *json)
{
    int en = json_get_int(json, "enable", -1);
    if (en == 0 || en == 1) cdrom_warm_route_set_enabled(en);
    char body[512];
    cdrom_warm_route_stats_json(body, sizeof(body));
    send_fmt("{\"id\":%d,\"ok\":true,%s}\n", id, body);
}

static const CmdEntry s_commands[] = {
    { "phase_profile",     handle_phase_profile },
    { "starv_ring",        handle_starv_ring },
    { "data_shards",       handle_data_shards },
    { "vsync_query_hle",   handle_vsync_query_hle },
    { "warm_cd_route",     handle_warm_cd_route },
    { "phase_hot",         handle_phase_hot },
    { "idle_skip",         handle_idle_skip },
    { "lockstep",          handle_lockstep },
    { "lockstep_func",     handle_lockstep_func },
    { "ping",              handle_ping },
    { "xlate",             handle_xlate },
    { "parity_dump",       handle_parity_dump },
    { "parity_ctl",        handle_parity_ctl },
    { "devtrace_dump",     handle_devtrace_dump },
    { "devtrace_ctl",      handle_devtrace_ctl },
    { "latency",           handle_latency },
    { "vk_perf",           handle_vk_perf },
    { "game_options",      handle_game_options },
    { "stack_profile",     handle_stack_profile },
    { "xprobe",            handle_xprobe },
    { "xprobe_arm",        handle_xprobe_arm },
    { "xprobe_watch",      handle_xprobe_watch },
    { "ce_profile",        handle_ce_profile },
    { "frame",             handle_frame },
    { "frame_fingerprint", handle_frame_fingerprint },
    { "record_frame",      handle_record_frame },
    { "record_frame_dump", handle_record_frame_dump },
    { "record_reads_dump", handle_record_reads_dump },
    { "get_registers",     handle_get_registers },
    { "read_ram",          handle_read_ram },
    { "dump_ram",          handle_read_ram },   /* alias: one request, one response */
    { "write_ram",         handle_write_ram },
    { "gpu_state",         handle_gpu_state },
    { "geom_correction",   handle_geom_correction },
    { "pgxp",              handle_pgxp },
    { "ws_aspect_cone_site", handle_ws_aspect_cone_site },
    { "ws_margin",         handle_ws_margin },
    { "ws_hud_mode",       handle_ws_hud_mode },
    { "kernel_bless",      handle_kernel_bless },
    { "ws_aspect",         handle_ws_aspect },
    { "ws_nw",             handle_ws_nw },
    { "ws_backdrop_ring",  handle_ws_backdrop_ring },
    { "ws_ui_groups",      handle_ws_ui_groups },
    { "tex_pack",          handle_tex_pack },
    { "ws_menu_edge_fill", handle_ws_menu_edge_fill },
    { "pgxp_depth",        handle_pgxp_depth },
    { "zbuf",              handle_zbuf },
    { "ws_backdrop_margin", handle_ws_backdrop_margin },
    { "ws_backdrop_stretch", handle_ws_backdrop_stretch },
    { "ws_dbg_stretch",    handle_ws_dbg_stretch },
    { "ws_far_threshold",  handle_ws_far_threshold },
    { "ws_dome",           handle_ws_dome },
    { "ws_dome_probe",     handle_ws_dome_probe },
    { "ws_census",         handle_ws_census },
    { "mmx6_freshfix",     handle_mmx6_freshfix },
    { "mem_words",         handle_mem_words },
    { "vram_peek",         handle_vram_peek },
    { "gl_coh_ring",       handle_gl_coh_ring },
    { "gl_present_ring",   handle_gl_present_ring },
    { "present_ring",      handle_present_ring },
    { "frame_perf",        handle_frame_perf },
    { "gl_ws_ablate",      handle_gl_ws_ablate },
    { "gl_interp",         handle_gl_interp },
    { "gl_wide_fast",      handle_gl_wide_fast },
    { "synth_recurse",     handle_synth_recurse },
    { "gl_fbo_peek",       handle_gl_fbo_peek },
    { "gl_vram_diff",      handle_gl_vram_diff },
    { "irq_state",         handle_irq_state },
    { "vblank_rate",       handle_vblank_rate },
    { "cycles_to_next_event", handle_cycles_to_next_event },
    { "timers_state",      handle_timers_state },
    { "cdrom_state",       handle_cdrom_state },
    { "cdrom_sector_dump", handle_cdrom_sector_dump },
    { "cdrom_sector_history", handle_cdrom_sector_history },
    { "cdrom_sector_history_clear", handle_cdrom_sector_history_clear },
    { "cdrom_command_history", handle_cdrom_command_history },
    { "cdrom_command_history_clear", handle_cdrom_command_history_clear },
    { "cdrom_trace_dump",  handle_cdrom_trace_dump },
    { "cdrom_trace_clear", handle_cdrom_trace_clear },
    { "dma_state",         handle_dma_state },
    { "dma_trace_dump",    handle_dma_trace_dump },
    { "dma_trace_clear",   handle_dma_trace_clear },
    { "dma_cdrom_history", handle_dma_cdrom_history },
    { "sio_state",         handle_sio_state },
    { "sio1_state",        handle_sio1_state },
    { "dual_state",        handle_dual_state },
    { "dual_input",        handle_dual_input },
    { "mc_status",         handle_mc_status },
    { "spu_status",        handle_spu_status },
    { "spu_voices",        handle_spu_voices },
    { "spu_ram",           handle_spu_ram },
    { "spu_events",        handle_spu_events },
    { "spu_events_reset",  handle_spu_events_reset },
    { "audio_stats",       handle_audio_stats },
    { "audio_wav",         handle_audio_wav },
    { "audio_events",      handle_audio_events },
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
    { "bios_info",         handle_bios_info },
    { "hle_dump",          handle_hle_dump },
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
    { "card_handoff",      handle_card_handoff },
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
    { "d44_ring",          handle_d44_ring },
    { "irqctx_ring",       handle_irqctx_ring },
    { "sp_ring",           handle_sp_ring },
    { "disp_ring",         handle_disp_ring },
    { "cyc_watch",         handle_cyc_watch },
    { "cyc_watch_dump",    handle_cyc_watch_dump },
    { "cyc_watch_clear",   handle_cyc_watch_clear },
    { "pc_probe_arm",      handle_pc_probe_arm },
    { "pc_probe_dump",     handle_pc_probe_dump },
    { "pc_probe_clear",    handle_pc_probe_clear },
    { "mmio_dump",         handle_mmio_dump },
    { "mmio_clear",        handle_mmio_clear },
    { "capture_freeze",    handle_capture_freeze },
    { "rtrace_dump",       handle_rtrace_dump },
    { "rtrace_clear",      handle_rtrace_clear },
    { "rtrace_arm",        handle_rtrace_arm },
    { "rtrace_ranges",     handle_rtrace_ranges },
    { "rtrace_stats",      handle_rtrace_stats },
    { "gp1_dump",          handle_gp1_dump },
    { "mdec_state",        handle_mdec_state },
    { "mdec_trace",        handle_mdec_trace },
    { "fmv_state",         handle_fmv_state },
    { "mdec_trace_clear",  handle_mdec_trace_clear },
    { "set_input",         handle_set_input },
    { "press",             handle_press },
    { "pad_status",        handle_pad_status },
    { "clear_input",       handle_clear_input },
    { "input_route_clear", handle_input_route_clear },
    { "input_route_append",handle_input_route_append },
    { "input_route_start", handle_input_route_start },
    { "input_route_stop",  handle_input_route_stop },
    { "input_route_status",handle_input_route_status },
    { "savestate",         handle_savestate },
    { "pgxp_census",       handle_pgxp_census },
    { "pgxp_texcensus",    handle_pgxp_texcensus },
    { "overclock",         handle_overclock },
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
    { "screenshot",        handle_present_screenshot },
    { "screenshot_file",   handle_screenshot_file },
    { "screenshot_hires",  handle_screenshot_hires },
    { "present_shot",      handle_present_shot },
    { "present_shot_seq",  handle_present_shot_seq },
    { "display_ring_get",  handle_display_ring_get },
    { "display_ring_aux",  handle_display_ring_aux },
    { "display_ring_stats", handle_display_ring_stats },
    { "dump_buffer",       handle_dump_buffer },
    { "wide_full",         handle_wide_full },
    { "wide_shot",         handle_wide_shot },
    { "gpu_opcodes",       handle_gpu_opcodes },
    { "gpu_ring_stats",    handle_gpu_ring_stats },
    { "gpu_frame_dump",    handle_gpu_frame_dump },
    { "a0_history",        handle_a0_history },
    { "c0_history",        handle_c0_history },
    { "capture_quads",     handle_capture_quads },
    { "get_quads",         handle_get_quads },
    { "gte_state",         handle_gte_state },
    { "gte_ring_dump",     handle_gte_ring_dump },
    { "gte_intpl_dump",    handle_gte_intpl_dump },
    { "gte_frame_stats",   handle_gte_frame_stats },
    { "gte_latch_dump",    handle_gte_latch_dump },
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
    { "insn_freeze_target",   handle_insn_freeze_target },
    { "s3_smear_watch",       handle_s3_smear_watch },
    { "callret_watch",        handle_callret_watch },
    { "insn_freeze_snapshot", handle_insn_freeze_snapshot },
    { "ra_load_watch",        handle_ra_load_watch },
    { "overlay_native_on",    handle_overlay_native_on },
    { "overlay_native_off",   handle_overlay_native_off },
    { "overlay_native_block", handle_overlay_native_block },
    { "overlay_cps_probe",    handle_overlay_cps_probe },
    { "overlay_capture_dump", handle_overlay_capture_dump },
    { "cdrom_instant_rate",   handle_cdrom_instant_rate },
    { "cd_overwrite",         handle_cd_overwrite },
    { "cdrom_bursts",         handle_cdrom_bursts },
    { "cdrom_timing",         handle_cdrom_timing },
    { "turbo_loads",          handle_turbo_loads },
    { "turbo_audio_sink",     handle_turbo_audio_sink },
    { "load_transitions",     handle_load_transitions },
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
            /* Suppress lockstep memory recording for the WHOLE handler.
             * Commands run at debug_server_poll() safe points on the emu
             * thread, which can land inside an armed lockstep record window
             * (leader-to-leader). Any handler that reads guest RAM for
             * diagnostics (read_ram's psx_read_byte loop, probes, disasm)
             * would otherwise leak observer ops into the recorded trace as
             * phantom guest ops -> false compiled-vs-interp divergence
             * (seen live: a read_ram poll of 0x8006FC24 flagged block
             * 0x62FE8 as "read-addr"). Chokepoint here so no individual
             * handler can forget; nested per-handler suppress calls are
             * fine (counter). */
            ls_suppress_begin();
            e->handler(id, line);
            ls_suppress_end();
            return;
        }
    }

    send_err(id, "unknown command");
}

/* ---- Public API ---- */

/* Extended init that accepts a CPU state pointer for register queries. */
static CPUState *s_init_cpu = NULL;
CPUState *debug_cpu_ptr = NULL; /* Global, used by memory.c watchpoints */

/* Guest $ra accessor for other TUs (e.g. gpu.c GP0 ring caller capture) that
 * must not depend on the CPUState layout. Returns 0 before the CPU is bound. */
uint32_t debug_guest_ra(void) { return debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0; }
uint32_t debug_guest_sp(void) { return debug_cpu_ptr ? debug_cpu_ptr->gpr[29] : 0; }

void debug_server_set_cpu(CPUState *cpu)
{
    s_cpu = cpu;
    debug_cpu_ptr = cpu;
}

void debug_server_init(int port)
{
    if (port > 0) s_port = port;

    /* Race-free recorder arming: PSX_RECORD_FRAME=<N> arms the unified ordered
     * access recorder from boot (instruction 0), so it deterministically
     * captures guest frame N no matter when a probe connects — the same
     * always-on-from-boot model as PSX_NATIVE_BLOCK / PSX_OVERLAY_NATIVE_OFF.
     * Never race a connect against the target frame; seed it and free-run. */
    {
        const char *rf = getenv("PSX_RECORD_FRAME");
        if (rf && *rf) {
            s_rec_frame = (int64_t)strtoll(rf, NULL, 0);
            s_rec_count = 0; s_rec_overflow = 0;
        }
        /* PSX_READ_WATCH="<lo>,<hi>" arms the targeted main-RAM read watch from
         * boot (phys range, hex/dec ok). Reads in [lo,hi) during the recorded
         * frame land in the unified buffer as REC_KIND_RAM_R. */
        const char *rw = getenv("PSX_READ_WATCH");
        if (rw && *rw) {
            char tmp[64]; snprintf(tmp, sizeof(tmp), "%s", rw);
            char *comma = strchr(tmp, ',');
            if (comma) {
                *comma = '\0';
                s_rwatch_lo = (uint32_t)strtoul(tmp, NULL, 0);
                s_rwatch_hi = (uint32_t)strtoul(comma + 1, NULL, 0);
                if (s_rwatch_hi > s_rwatch_lo) g_ram_read_watch_active = 1;
            }
        }
        /* PSX_ND_INTRO_PROBE=1 PolyG4; =2 OT; =3 wood; =4 depth; =5 wood DL.
         * PSX_PC_PROBE="0xA,0xB" arms an explicit list (can combine). */
        {
            const char *nd = getenv("PSX_ND_INTRO_PROBE");
            const char *pcs = getenv("PSX_PC_PROBE");
            if ((nd && *nd && *nd != '0') || (pcs && *pcs)) {
                pc_probe_clear_state();
                s_pc_probe_sample_max = 48;
                if (nd && *nd && *nd != '0') {
                    int ndv = (int)strtol(nd, NULL, 0);
                    if (ndv == 5) pc_probe_arm_nd_intro_wood_dl_defaults();
                    else if (ndv == 4) pc_probe_arm_nd_intro_depth_defaults();
                    else if (ndv == 3) pc_probe_arm_nd_intro_wood_defaults();
                    else if (ndv == 2) pc_probe_arm_nd_intro_ot_defaults();
                    else pc_probe_arm_nd_intro_defaults();
                }
                if (pcs && *pcs) pc_probe_parse_list(pcs);
                if (s_pc_probe_n > 0) {
                    s_pc_probe_armed = 1;
                    fprintf(stdout, "psxrecomp: pc_probe armed (%d pcs)\n", s_pc_probe_n);
                }
            }
        }
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == SOCK_INVALID) {
        s_listen_err = sock_error();
        fprintf(stdout, "psxrecomp: debug server socket() FAILED\n");
        return;
    }

    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)s_port);

    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        s_listen_err = sock_error();
        fprintf(stdout, "psxrecomp: debug server bind(%d) FAILED\n", s_port);
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
    fprintf(stdout, "psxrecomp: debug server LISTENING on 127.0.0.1:%d\n",
            s_port);

    /* Always-on wall-time phase sampler (phase_profile). Own thread: the
     * server pumps on the main thread, so sampling must not live there. */
    phase_sampler_start();
    /* Blocking accept — the dedicated I/O thread waits on it (no busy-poll). */

    /* Start the TCP I/O thread + its handoff primitives. */
    s_io_mutex   = SDL_CreateMutex();
    s_io_req_cv  = SDL_CreateCond();
    s_io_resp_cv = SDL_CreateCond();
    s_resp_cap   = 65536;
    s_resp_buf   = (char *)malloc(s_resp_cap);
    if (s_io_mutex && s_io_req_cv && s_io_resp_cv && s_resp_buf) {
        s_io_running = 1;
        s_io_thread = SDL_CreateThread(io_thread_main, "psx-dbg-io", NULL);
        if (!s_io_thread) s_io_running = 0;
    }

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
    /* Ape LOAD-GAME higher-layer (libcard + game card-manager) always-on
     * capture. Oracle diff (2026-07-07 session 3) proved the low-level card
     * protocol succeeds on both runtimes, but ours stays in top-scene 1
     * ("Checking") while Beetle advances to scene 4 (file-select). These cells
     * are the higher-layer state that diverges; capturing every writer from
     * boot (never probe-time armed) attributes WHICH libcard op-state stalls
     * and what completion it awaits. Masked-physical (KUSEG) addresses.
     *   0x800b4e30 = libcard op struct field 0 (count/op-state; 0 ours vs 1 Beetle)
     *   0x800b4e50 = save-name ptr + filename buffer ("bu00:BASCUS-94423SYS" on Beetle, zeros ours)
     *   0x800b4ed0 = libcard op-handler ptr (0x80020f4c ours vs 0x80020bc8 Beetle)
     *   0x800e3880 = SCENE (no-op flip-flop) + 0x800e3884 = TRIG (= top-scene index; 1 ours vs 4 Beetle)
     *   0x8013af50 = game card-menu substate block (0x8013af56 = 0 ours vs 1 Beetle)
     *   0x00007264 = kernel card-driver per-state byte M8[0x7264] (handoff: stuck at 1) */
    s_wtrace_ranges[16].lo = 0x000B4E2Cu;
    s_wtrace_ranges[16].hi = 0x000B4E40u;
    s_wtrace_ranges[17].lo = 0x000B4E50u;
    s_wtrace_ranges[17].hi = 0x000B4E68u;
    s_wtrace_ranges[18].lo = 0x000B4ECCu;
    s_wtrace_ranges[18].hi = 0x000B4ED4u;
    s_wtrace_ranges[19].lo = 0x000E3880u;
    s_wtrace_ranges[19].hi = 0x000E3888u;
    s_wtrace_ranges[20].lo = 0x0013AF50u;
    s_wtrace_ranges[20].hi = 0x0013AF60u;
    s_wtrace_ranges[21].lo = 0x00007260u;
    s_wtrace_ranges[21].hi = 0x00007270u;
    s_wtrace_range_count = 22;

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

    /* MMIO-READ trace ring (range-filtered). Default-armed from init to the CD
     * regs + I_STAT/I_MASK so the IRQ handler's reads of the CD IRQ-flag and
     * interrupt status are captured continuously (Rule: always-on ring; probes
     * query the window). Reuses MMIO_TRACE_CAP entries (~21 MB). */
    if (!s_mmio_rtrace) {
        s_mmio_rtrace = (MmioTraceEntry *)calloc(MMIO_TRACE_CAP, sizeof(MmioTraceEntry));
    }
    s_mmio_rtrace_seq = 0;
    s_mmio_rtrace_head = 0;
    s_mmio_rtrace_ranges[0].lo = 0x1F801800u; s_mmio_rtrace_ranges[0].hi = 0x1F801804u; /* CD regs (idx/IRQ-flag) */
    s_mmio_rtrace_ranges[1].lo = 0x1F801070u; s_mmio_rtrace_ranges[1].hi = 0x1F801078u; /* I_STAT / I_MASK */
    s_mmio_rtrace_range_count = 2;

    memset(s_watchpoints, 0, sizeof(s_watchpoints));
    memset(s_snapshot_addrs, 0, sizeof(s_snapshot_addrs));
    memset(s_snapshot_active, 0, sizeof(s_snapshot_active));
}

/* Read one '\n'-terminated line from a blocking socket into buf. Returns the
 * line length (NUL-terminated, newline stripped), or -1 on close/error. */
static int recv_line(sock_t c, char *buf, int cap)
{
    int len = 0;
    while (len < cap - 1) {
        int n = recv(c, buf + len, cap - 1 - len, 0);
        if (n <= 0) return -1;
        len += n;
        buf[len] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) { *nl = '\0'; return (int)(nl - buf); }
    }
    buf[cap - 1] = '\0';
    return cap - 1;   /* over-long line: take what we have */
}

/* The TCP I/O thread: owns accept/recv/send. Hands each request to the emu
 * thread (processed in debug_server_poll at a safe point) and sends the
 * buffered response. `ping` is answered directly here so liveness is queryable
 * even when the emu thread is buried or frozen. */
static int io_thread_main(void *arg)
{
    (void)arg;
    while (s_io_running) {
        struct sockaddr_in caddr;
        int clen = sizeof(caddr);
        sock_t c = accept(s_listen, (struct sockaddr *)&caddr, &clen);
        if (c == SOCK_INVALID) { if (!s_io_running) break; SDL_Delay(5); continue; }

        char req[RECV_BUF_SIZE];
        int rl = recv_line(c, req, sizeof(req));
        if (rl < 0) { sock_close(c); continue; }
        if (rl > 0 && req[rl - 1] == '\r') req[rl - 1] = '\0';

        /* Lock-free liveness fast-path (survives an emu-thread freeze). */
        if (strstr(req, "\"ping\"")) {
            const char *pong = "{\"id\":0,\"ok\":true,\"pong\":true,\"io_thread\":true}\n";
            send_all_blocking(c, pong, strlen(pong));
            sock_close(c);
            continue;
        }

        /* Hand the request to the emu thread and wait (bounded) for its reply. */
        SDL_LockMutex(s_io_mutex);
        strncpy(s_io_req, req, sizeof(s_io_req) - 1);
        s_io_req[sizeof(s_io_req) - 1] = '\0';
        s_io_state = IO_REQ;
        SDL_CondSignal(s_io_req_cv);
        int waited = 0;
        while (s_io_state != IO_RESP && s_io_running) {
            if (SDL_CondWaitTimeout(s_io_resp_cv, s_io_mutex, 1000) == SDL_MUTEX_TIMEDOUT) {
                waited += 1000;
                if (waited >= 30000) break;   /* emu wedged: give up on this one */
            }
        }
        if (s_io_state == IO_RESP) {
            if (s_resp_len > 0) send_all_blocking(c, s_resp_buf, s_resp_len);
            s_io_state = IO_IDLE;
        } else {
            const char *busy = "{\"ok\":false,\"err\":\"emu busy or frozen\"}\n";
            send_all_blocking(c, busy, strlen(busy));
            /* leave state as IO_REQ; the emu's poll discards it (state!=IO_REQ
             * check) once it finally finishes — handled in debug_server_poll. */
        }
        SDL_UnlockMutex(s_io_mutex);
        sock_close(c);
    }
    return 0;
}

void debug_server_get_status(int *listening, int *port, int *error)
{
    if (listening) *listening = (s_listen != SOCK_INVALID);
    if (port)      *port      = s_port;
    if (error)     *error     = s_listen_err;
}

void debug_server_poll(void)
{
    /* Phase 1.0e-e2 starvation watchdog heartbeat. Refreshes the
     * "last poll wall-clock" timestamp; if too much time passes
     * between calls, the ring is dumped and the runtime aborts. */
    extern void starvation_watchdog_heartbeat(void);
    starvation_watchdog_heartbeat();

    if (!s_io_mutex || s_in_command) return;   /* no server, or re-entrant */

    SDL_LockMutex(s_io_mutex);
    if (s_io_state != IO_REQ) { SDL_UnlockMutex(s_io_mutex); return; }
    char req[RECV_BUF_SIZE];
    strncpy(req, s_io_req, sizeof(req) - 1);
    req[sizeof(req) - 1] = '\0';
    SDL_UnlockMutex(s_io_mutex);

    /* Execute on the emu thread (safe point). send_line appends to s_resp_buf. */
    s_resp_len = 0;
    s_resp_overflow = 0;
    s_in_command = 1;
    if (req[0]) process_command(req);
    s_in_command = 0;

    SDL_LockMutex(s_io_mutex);
    /* Only deliver if the I/O thread is still waiting (it may have timed out and
     * moved on, in which case the response is discarded). */
    if (s_io_state == IO_REQ) {
        s_io_state = IO_RESP;
        SDL_CondSignal(s_io_resp_cv);
    }
    SDL_UnlockMutex(s_io_mutex);
}

void debug_server_record_frame(void)
{
    if (s_fmv_quiet) {
        s_history_count = s_frame_count + 1;
        s_frame_count++;
        return;
    }
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
    /* Side-effect-free peeks: sio_read() advances sio_tick and clears the ACK bit,
     * so recording it every vblank corrupted the SIO/DualShock handshake (Mega Man
     * X boot FMV skipped in dev builds only). Observability must not alter state. */
    r->sio_stat = sio_peek_stat();
    r->sio_ctrl = sio_peek_ctrl();

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

    /* Always-on display ring: raw display-area pixels for THIS frame number
     * (pre-increment, matching what the `frame` command reports right now). */
    disp_ring_capture();

    s_history_count = s_frame_count + 1;
    s_frame_count++;
    /* Layer-1 first-divergence: snapshot the cumulative write fingerprint for
     * the frame that just completed. Tagged with the new frame number so two
     * runs line up by frame index. */
    fp_snapshot((uint32_t)s_frame_count);

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
    if (s_fmv_quiet) return;
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
    /* Stop the I/O thread: clear the flag, close the listen socket to break the
     * blocking accept(), then join. */
    s_io_running = 0;
    if (s_listen != SOCK_INVALID) {
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
    }
    if (s_io_thread) {
        if (s_io_mutex) { SDL_LockMutex(s_io_mutex); SDL_CondSignal(s_io_resp_cv); SDL_UnlockMutex(s_io_mutex); }
        SDL_WaitThread(s_io_thread, NULL);
        s_io_thread = NULL;
    }
    if (s_client != SOCK_INVALID) {
        sock_close(s_client);
        s_client = SOCK_INVALID;
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
    if (s_input_route_active && s_input_route_index < s_input_route_count) {
        int current = (int)s_input_route[s_input_route_index].buttons;
        if (s_input_route_remaining > 0 && --s_input_route_remaining == 0) {
            s_input_route_index++;
            if (s_input_route_index < s_input_route_count) {
                s_input_route_remaining =
                    s_input_route[s_input_route_index].frames;
            } else {
                s_input_route_active = 0;
            }
        }
        return current;
    }
    int current = s_input_override;
    if (s_input_override >= 0 && s_input_frames > 0) {
        if (--s_input_frames == 0)
            s_input_override = -1;
    }
    return current;
}

int debug_server_get_axis_override(unsigned char st[4])
{
    if (!s_axis_override) return 0;
    st[0] = s_axis_st[0]; st[1] = s_axis_st[1];
    st[2] = s_axis_st[2]; st[3] = s_axis_st[3];
    return 1;
}

int debug_server_turbo_enabled(void)
{
    return s_turbo_enabled != 0;
}
