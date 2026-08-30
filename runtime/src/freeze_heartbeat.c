/* freeze_heartbeat.c — see header for rationale. */

#include "freeze_heartbeat.h"
#include "debug_server.h"
#include "crash_trace.h"   /* g_psx_fatal_reason */
#include "cpu_state.h"     /* g_psx_bail_* call-contract counters */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

/* State accessors. All defined in other compilation units; declared here
 * to avoid pulling in heavy headers. */
extern uint64_t s_frame_count;                 /* debug_server.c */
extern uint32_t g_debug_current_func_addr;     /* debug_server.c */
extern uint32_t g_debug_last_store_pc;         /* debug_server.c */
extern uint32_t i_stat;                        /* memory.c */
extern uint32_t i_mask;                        /* memory.c */

extern uint64_t psx_get_cycle_count(void);
extern void     psx_get_freeze_diag(uint64_t *total_checks,
                                    uint32_t *dispatch_count,
                                    int *in_exception,
                                    int *post_exception_cooldown,
                                    uint64_t *exception_entries,
                                    uint64_t *exception_reentry_blocks);
extern void     sio_get_freeze_diag(int *irq_pending, int *irq_countdown,
                                    uint16_t *sio_stat_out, uint16_t *sio_ctrl_out,
                                    int *card_active);
extern int      sio_get_mc_max_state(void);
extern int      sio_get_tx_writes(void);

/* dirty_ram_interp counters — defined as plain globals in dirty_ram_interp.c.
 * Declared weakly here in case a future build drops the unit; missing symbol
 * would only fail at link time and is easy to catch. */
extern uint64_t g_dirty_ram_blocks_run;
extern uint64_t g_dirty_ram_insns_run;
extern uint64_t g_dirty_pump_max_gap_insns;  /* largest insn gap between IRQ pumps */
extern uint64_t g_dirty_pump_count;          /* region-independent dirty IRQ pumps */
/* Timer/RootCounter debug accessor (timers.c) — Tomba 2 RCnt-wait diagnosis. */
extern void timers_get_debug(int t, uint16_t *counter, uint16_t *target,
                             uint32_t *mode, uint64_t *irq_fired);
/* Bail-source ledger (traps.c) — Tomba 2 wild-return / splash-spin diagnosis. */
extern void psx_bail_ledger_top(uint32_t *site_ra, uint32_t *wild_pc,
                                uint32_t *site_sp, uint32_t *guest_sp,
                                uint64_t *count, uint32_t *unique);
extern uint32_t g_bail_first_site_ra, g_bail_first_wild_pc, g_bail_first_frame;
extern uint32_t g_bail_first_in_exc;  /* was the trigger bail during an exception? */
/* IRQ raise/deliver/ack telemetry (interrupts.c + memory.c) — exception-reentry. */
extern uint64_t g_vblank_raise_count, g_vblank_deliver_count, g_irq_deliver_count;
extern uint64_t g_vblank_ack_count;
/* $ra->1 corruption tripwire (traps.c) — pins the clobber site. */
extern uint32_t g_ra_tw_latched, g_ra_tw_site, g_ra_tw_pc, g_ra_tw_prev_ra;
extern uint32_t g_ra_tw_frame, g_ra_tw_in_exc, g_ra_tw_sp;

/* Vsync self-heal counters — defined in main.cpp. Included in the dump
 * so a slow_frames wedge can be attributed to driver present
 * backpressure straight from the JSON. */
extern uint32_t g_present_slow_count;
extern int      g_present_vsync_disabled;

/* CPU register file + scratchpad — captured into the heartbeat so a logical
 * hang's exact wedged state survives in the small, user-uploadable file.
 * debug_cpu_ptr (CPUState*) is declared in debug_server.h; CPUState layout
 * (gpr[32], pc) in cpu_state.h — both already included above. */
extern uint8_t *memory_get_scratchpad_ptr(void);   /* memory.c */
extern uint8_t *g_psx_ram;                         /* memory.c — 2 MiB DRAM */

static int s_started = 0;
static char s_backend[32] = "psx-runtime";

#ifdef _WIN32
static HANDLE s_thread = NULL;
static HANDLE s_main_thread = NULL;       /* DuplicateHandle of main thread */
static DWORD  s_main_thread_id = 0;
static int    s_sym_initialized = 0;
#endif

#define HB_FILE        "psx_freeze_heartbeat.json"
#define HB_INTERVAL_MS 100u

/* Wedge detection.
 *
 * Three failure modes to catch:
 *   A. HARD freeze — main thread is fully wedged; frame_count stops
 *      advancing entirely. Detected by zero frame_delta over the window.
 *      (Bug B root cause was this kind: the old pacing loop's double
 *      counter read underflowed into a ~24.7-day SDL_Delay.)
 *   B. REENTRY storm — frames advance but exception-handler reentry
 *      per frame is far above the chronic baseline. The baseline is
 *      ~2K reentry blocks per frame (the whole VSync callback chain
 *      runs in exception context, every block leader checks), so the
 *      storm test must be normalized PER FRAME, not per second. The
 *      old absolute threshold (10K per 2s window = 5K/sec) was below
 *      the healthy baseline (~2K/frame x 30-60 fps = 60-120K/sec) and
 *      labeled normal play, boots, and host-side slowdowns all as
 *      "reentry_storm" — which twice sent freeze investigations down
 *      the wrong path.
 *   C. SLOW frames — frames advance but pathologically slowly while
 *      per-frame PSX work is normal: the wall clock is being eaten
 *      outside the simulation (observed: NVIDIA GL SwapBuffers
 *      blocking ~1.5s per present, dump 1781045865). Multi-sample
 *      stack capture attributes the wait.
 *   D. SPIN freeze (logical hang) — frames advance at a HEALTHY rate
 *      (the IRQ/VSync/render loop keeps running) but the GAME logic is
 *      wedged: current_func and last_store_pc are pinned and no new
 *      dirty-RAM (interpreted) instructions retire. The classic shape
 *      is a guest poll loop spinning on a kernel call / event flag that
 *      never flips (Tomba dwarf-village dialogue freeze, issue #1:
 *      ~51 fps, exc_re ~1.6K/frame — UNDER every threshold above, so
 *      A/B/C all miss it). Detected by cur_fn + store_pc + dirty_insns
 *      all stable across the window while frame_delta is healthy.
 *
 * Window = 20 ticks = 2.0 sec. Long enough that legitimate startup
 * activity (boot, FMV decode, save load) doesn't trip it; short enough
 * that real wedges dump within seconds. */
#define WEDGE_WINDOW_TICKS 20u
#define WEDGE_EXC_REENTRY_PER_FRAME_THRESHOLD 20000u /* ~10x chronic 2K/frame */
#define WEDGE_SLOW_FRAMES_MAX_DELTA 10u   /* <5 fps avg over the 2s window */

/* Per-ring caps for auto-dump. Newest-first window. The old 4-16K caps
 * spanned well under a second of activity — too short to cross a
 * degradation transition (attract-idle investigation, 2026-06-10). The
 * underlying rings hold far more (wtrace_all / dirty-block: 4M entries);
 * dump a window that covers minutes, at the cost of a ~50-80 MB JSON. */
#define DUMP_CAP_WTRACE_ALL   262144u
#define DUMP_CAP_WTRACE        65536u
#define DUMP_CAP_FRAME_HISTORY  3600u
#define DUMP_CAP_SIO_PC        65536u
#define DUMP_CAP_THREAD_TRACE  65536u
#define DUMP_CAP_RESTORE_TRACE 65536u
#define DUMP_CAP_FN_ENTRY      65536u
#define DUMP_CAP_DIRTY_BLOCK  262144u

/* Pre-freeze history ring. Each entry = a snapshot taken at one heartbeat
 * tick (~100 ms). When the runtime freezes, all the "now" values stop
 * advancing but the past N entries still show what state it was in for
 * the seconds leading up to the stall. RING_CAP * 100ms = window length. */
#define RING_CAP 64
typedef struct {
    uint64_t frame_count;
    uint64_t psx_cycle_count;
    uint64_t exc_reentry;
    uint64_t dirty_ram_insns;
    uint32_t current_func;
    uint32_t last_store_pc;
    uint32_t i_stat;
    uint16_t sio_stat;
    uint16_t sio_ctrl;
    uint8_t  in_exception;
    uint8_t  mc_max_state;
    long long wall_clock;
    uint64_t tcp_stall_ms;
    uint16_t t1_count;        /* Timer1/RCnt1 counter — must advance if the wait is to complete */
    uint64_t t1_irq_fired;    /* Timer1 IRQ-fire count — flat == RCnt IRQ never fires */
} HbRingEntry;
static HbRingEntry s_ring[RING_CAP];
static uint32_t    s_ring_head = 0;
static uint32_t    s_ring_count = 0;

/* Wedge detection state.
 *   s_dump_armed - 1 if a wedge dump is allowed to fire; cleared after
 *                  firing, re-armed when the wedge clears (healthy tick). */
static int      s_dump_armed = 1;
static uint32_t s_last_wedge_kind = 0;  /* informational, last detected kind */

#ifdef _WIN32
/* Capture the main thread's call stack at the moment of a hard freeze.
 *
 * Called from the heartbeat thread when wedge_kind==1 (frame_count not
 * advancing). Suspends the main thread, walks its stack via StackWalk64,
 * symbolizes via SymFromAddr, then resumes. Each frame becomes a JSON
 * object {addr, symbol?, displacement?, module?}.
 *
 * Best-effort: if DbgHelp init fails, dump an empty array and continue. */
static void freeze_dump_main_stack_json(FILE *f) {
    if (!f) return;
    if (!s_main_thread) { fputs("[]", f); return; }
    /* Never suspend self: a dump running ON the main thread (fatal path)
     * must not walk the main thread — SuspendThread(self) parks forever. */
    if (GetCurrentThreadId() == s_main_thread_id) { fputs("[]", f); return; }

    if (!s_sym_initialized) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (SymInitialize(GetCurrentProcess(), NULL, TRUE)) {
            s_sym_initialized = 1;
        } else {
            fputs("[]", f);
            return;
        }
    }

    DWORD susp_count = SuspendThread(s_main_thread);
    if (susp_count == (DWORD)-1) { fputs("[]", f); return; }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(s_main_thread, &ctx)) {
        ResumeThread(s_main_thread);
        fputs("[]", f);
        return;
    }

    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#else
    DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    fputc('[', f);
    int first = 1;
    union {
        SYMBOL_INFO si;
        char buf[sizeof(SYMBOL_INFO) + 512];
    } sym_storage;
    SYMBOL_INFO *sym = &sym_storage.si;

    for (int depth = 0; depth < 128; depth++) {
        if (!StackWalk64(machine, GetCurrentProcess(), s_main_thread, &frame,
                         &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64,
                         NULL)) {
            break;
        }
        DWORD64 addr = frame.AddrPC.Offset;
        if (!addr) break;

        memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 511;
        DWORD64 displacement = 0;
        BOOL got_sym = SymFromAddr(GetCurrentProcess(), addr, &displacement, sym);

        IMAGEHLP_MODULE64 mod;
        memset(&mod, 0, sizeof(mod));
        mod.SizeOfStruct = sizeof(mod);
        BOOL got_mod = SymGetModuleInfo64(GetCurrentProcess(), addr, &mod);

        fprintf(f, "%s{\"depth\":%d,\"addr\":\"0x%016llX\"",
                first ? "" : ",", depth, (unsigned long long)addr);
        if (got_sym) {
            /* Escape JSON-special chars in symbol name (best-effort: bracket replace). */
            char safe[512];
            size_t k = 0;
            for (size_t i = 0; sym->Name[i] && k < sizeof(safe) - 2; i++) {
                char c = sym->Name[i];
                if (c == '"' || c == '\\') safe[k++] = '_';
                else if ((unsigned char)c < 0x20) safe[k++] = '_';
                else safe[k++] = c;
            }
            safe[k] = 0;
            fprintf(f, ",\"symbol\":\"%s\",\"displacement\":%llu",
                    safe, (unsigned long long)displacement);
        }
        if (got_mod && mod.ModuleName[0]) {
            fprintf(f, ",\"module\":\"%s\"", mod.ModuleName);
            /* Emit module-relative RVA. DbgHelp/SymFromAddr can't resolve mingw
             * DWARF symbols for the main module (the symbol field stays absent),
             * but addr2line CAN — given the RVA. addr - BaseOfImage is the input
             * for `addr2line -e psx-runtime.exe -f <rva>`. Always present so any
             * frame is offline-resolvable regardless of the DbgHelp gap. */
            fprintf(f, ",\"rva\":\"0x%llX\"",
                    (unsigned long long)(addr - (DWORD64)mod.BaseOfImage));
        }
        fputc('}', f);
        first = 0;
    }
    fputc(']', f);

    ResumeThread(s_main_thread);
}

/* Multi-sample stack capture for still-running wedges (wedge_kind 2 and 3).
 * The main thread is still running (bouncing through the exception/VSync
 * dispatch), so a single snapshot could land on a random recompiled block.
 * Taking N snapshots a few ms apart reveals the RECURRING frames (the
 * persistent dispatch/handler chain), which is what we want. Emits a JSON
 * array of per-sample frame arrays: [[...],[...],...]. */
static void freeze_dump_main_stack_samples_json(FILE *f, int n) {
    if (!f) return;
    /* Never suspend self (see freeze_dump_main_stack_json). */
    if (GetCurrentThreadId() == s_main_thread_id) { fputs("[]", f); return; }
    fputc('[', f);
    for (int i = 0; i < n; i++) {
        if (i) fputc(',', f);
        freeze_dump_main_stack_json(f);   /* one suspend/walk/resume snapshot */
        Sleep(2);
    }
    fputc(']', f);
}
#endif /* _WIN32 */

/* One dump at a time. The watchdog (heartbeat thread) and psx_fatal_halt
 * (main thread) can both reach freeze_dump_write concurrently; they share
 * the static io_buf via setvbuf and the stack walkers suspend the main
 * thread — racing them corrupted BOTH dump files and self-suspended the
 * main thread mid-dump (2026-06-10 chest-freeze postmortem). First dump
 * in wins; the loser skips (same rings either way). */
static volatile long s_dump_in_progress = 0;

static void freeze_dump_unlock(void) {
#ifdef _WIN32
    InterlockedExchange((volatile LONG *)&s_dump_in_progress, 0);
#else
    __sync_lock_release(&s_dump_in_progress);
#endif
}

/* Guest DRAM or scratchpad bytes at `vaddr`, as a JSON object. No MMIO.
 * 2 MiB RAM is mirrored across the first 8 MiB of each KSEG. */
static int hb_append_ram_peek(char *out, int n, size_t cap, uint32_t vaddr, int len) {
    static const char hexd[] = "0123456789abcdef";
    uint32_t phys = vaddr & 0x1FFFFFFFu;
    const uint8_t *src = NULL;
    int ncopy = len;
    if (ncopy < 0) ncopy = 0;
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        uint8_t *sp = memory_get_scratchpad_ptr();
        if (sp) {
            uint32_t off = phys - 0x1F800000u;
            src = sp + off;
            if (off + (uint32_t)ncopy > 0x400u)
                ncopy = (int)(0x400u - off);
        }
    } else if (phys < 0x00800000u && g_psx_ram) {
        uint32_t folded = phys & 0x1FFFFFu;
        src = g_psx_ram + folded;
        if (folded + (uint32_t)ncopy > 0x200000u)
            ncopy = (int)(0x200000u - folded);
    }
    if (!src) ncopy = 0;
    int m = snprintf(out + n, cap - (size_t)n,
                     "{\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"",
                     vaddr, ncopy);
    if (m > 0) n += m;
    for (int i = 0; i < ncopy && (size_t)(n + 3) < cap; i++) {
        out[n++] = hexd[(src[i] >> 4) & 0xF];
        out[n++] = hexd[src[i] & 0xF];
    }
    if ((size_t)(n + 2) < cap) { out[n++] = '"'; out[n++] = '}'; }
    return n;
}

/* If the insn at $ra-8 is j/jal, return its target (live RAM, not AOT). */
static uint32_t hb_jal_target_from_ra(uint32_t ra) {
    uint32_t pc, phys, insn, op;
    const uint8_t *src = NULL;
    if (ra < 8u) return 0;
    pc = (ra - 8u) & ~3u;
    phys = pc & 0x1FFFFFFFu;
    if (phys >= 0x1F800000u && phys + 4u <= 0x1F800400u) {
        uint8_t *sp = memory_get_scratchpad_ptr();
        if (sp) src = sp + (phys - 0x1F800000u);
    } else if (phys < 0x00800000u && g_psx_ram) {
        src = g_psx_ram + (phys & 0x1FFFFFu);
    }
    if (!src) return 0;
    memcpy(&insn, src, 4);
    op = (insn >> 26) & 0x3Fu;
    if (op != 2u && op != 3u) return 0; /* j / jal */
    return (pc & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
}

/* Format the CPU register file (pc + all 32 GPRs) and the full 1 KB
 * scratchpad as JSON into `out`. Returns bytes written (NUL-terminated,
 * clamped to cap). Shared by the small per-tick heartbeat and the big
 * auto-dump so both carry identical state.
 *
 * Why this is the decisive observability for a logical hang: the scratchpad
 * (0x1F800000..0x1F8003FF) is the R3000A's only fast working RAM, and PSX
 * engines keep per-frame command/entity-list state there. A spin-freeze
 * leaves its cause in those words (Tomba's dwarf-village dialogue processor
 * at 0x8010D464 gates its entire loop on scratchpad 0x19E/0x1FC/0x250/0x260/
 * 0x3C4). The register file pins down the poll site, and $t1 (gpr[9]) holds
 * the BIOS A0/B0/C0 function index while the guest is parked in a kernel-call
 * poll — i.e. "which B0 function is it waiting on" with no extra plumbing.
 * Emitting the GPRs in the same shape as psx_last_run_report.json's "cpu"
 * means existing analysis tooling reads it unchanged. COP0 sr/cause/epc
 * (plus IEc / IM2) are required to tell a VSync wait from a stuck IRQ
 * gate — R3000A has no EXL; IEc=0 is "cannot take VBlank". */
static int hb_format_cpu_scratchpad(char *out, size_t cap) {
    if (cap < 64) { if (cap) out[0] = 0; return 0; }
    const CPUState *cpu = debug_cpu_ptr;
    uint32_t sr = cpu ? cpu->cop0[12] : 0u;
    int n = snprintf(out, cap,
                     "  \"cpu\":{\"pc\":\"0x%08X\",\"hi\":\"0x%08X\",\"lo\":\"0x%08X\","
                     "\"sr\":\"0x%08X\",\"cause\":\"0x%08X\",\"epc\":\"0x%08X\","
                     "\"iec\":%u,\"im2\":%u,\"gpr\":[",
                     cpu ? cpu->pc : 0u,
                     cpu ? cpu->hi : 0u,
                     cpu ? cpu->lo : 0u,
                     sr,
                     cpu ? cpu->cop0[13] : 0u,
                     cpu ? cpu->cop0[14] : 0u,
                     (sr & 1u) ? 1u : 0u,
                     (sr & (1u << 10)) ? 1u : 0u);
    for (int i = 0; i < 32 && n > 0 && (size_t)n < cap; i++) {
        int m = snprintf(out + n, cap - (size_t)n, "%s\"0x%08X\"",
                         i ? "," : "", cpu ? cpu->gpr[i] : 0u);
        if (m <= 0) break;
        n += m;
    }
    if (n > 0 && (size_t)n < cap) {
        int m = snprintf(out + n, cap - (size_t)n, "]},\n  \"scratchpad\":\"");
        if (m > 0) n += m;
    }
    const uint8_t *sp = memory_get_scratchpad_ptr();
    if (sp) {
        static const char hexd[] = "0123456789abcdef";
        for (int i = 0; i < 1024 && (size_t)(n + 3) < cap; i++) {
            out[n++] = hexd[(sp[i] >> 4) & 0xF];
            out[n++] = hexd[sp[i] & 0xF];
        }
    }
    if ((size_t)(n + 2) < cap) out[n++] = '"';

    /* DRAM slices: $ra-32 (jalr at ra-8), $s0-16, j/jal target, overlay page. */
    {
        uint32_t ra = cpu ? (cpu->gpr[31] & ~3u) : 0u;
        uint32_t s0 = cpu ? (cpu->gpr[16] & ~3u) : 0u;
        uint32_t ra_peek = (ra >= 32u) ? (ra - 32u) : ra;
        uint32_t s0_peek = (s0 >= 16u) ? (s0 - 16u) : s0;
        uint32_t jal_tgt = hb_jal_target_from_ra(ra);
        int m = snprintf(out + n, cap - (size_t)n,
                         ",\n  \"ram_peeks\":{\"ra\":");
        if (m > 0) n += m;
        n = hb_append_ram_peek(out, n, cap, ra_peek, 64);
        m = snprintf(out + n, cap - (size_t)n, ",\"s0\":");
        if (m > 0) n += m;
        n = hb_append_ram_peek(out, n, cap, s0_peek, 48);
        m = snprintf(out + n, cap - (size_t)n, ",\"jal_target\":");
        if (m > 0) n += m;
        n = hb_append_ram_peek(out, n, cap, jal_tgt, jal_tgt ? 64 : 0);
        m = snprintf(out + n, cap - (size_t)n, ",\"overlay_80165000\":");
        if (m > 0) n += m;
        n = hb_append_ram_peek(out, n, cap, 0x80165000u, 64);
        if ((size_t)(n + 2) < cap) out[n++] = '}';
    }

    out[n] = 0;
    return n;
}

static void freeze_dump_write(long long wall, uint64_t frame, uint64_t cyc,
                              uint64_t exc_reentry, uint32_t cur_fn,
                              uint32_t last_store, uint32_t i_stat_v,
                              uint32_t i_mask_v, int in_exc,
                              uint64_t total_checks, uint32_t dispatch_count,
                              uint64_t exc_entries,
                              uint16_t sio_stat_v, uint16_t sio_ctrl_v,
                              int sio_card_active, int mc_max, int tx_writes)
{
    /* Snapshot the kind at entry — the global can be flipped mid-write by
     * the other thread, which is what sent the fatal (kind 4) path into
     * the SuspendThread stack walkers on the main thread. */
    uint32_t wedge_kind = s_last_wedge_kind;

#ifdef _WIN32
    if (InterlockedCompareExchange((volatile LONG *)&s_dump_in_progress, 1, 0) != 0)
        return;
#else
    if (__sync_lock_test_and_set(&s_dump_in_progress, 1) != 0)
        return;
#endif

    char path[128];
    snprintf(path, sizeof(path),
             "psx_freeze_dump_%s_%lld.json", s_backend, wall);

    FILE *f = fopen(path, "wb");
    if (!f) { freeze_dump_unlock(); return; }

    /* Large stdio buffer so multi-MB JSON arrays write efficiently. */
    static char io_buf[1 << 16];
    setvbuf(f, io_buf, _IOFBF, sizeof(io_buf));

    fprintf(f,
        "{\n"
        "  \"backend\":\"%s\",\n"
        "  \"wall_clock_epoch\":%lld,\n"
        "  \"frame_count\":%llu,\n"
        "  \"psx_cycle_count\":%llu,\n"
        "  \"current_func\":\"0x%08X\",\n"
        "  \"last_store_pc\":\"0x%08X\",\n"
        "  \"total_checks\":%llu,\n"
        "  \"dispatch_count\":%u,\n"
        "  \"in_exception\":%d,\n"
        "  \"exception_entries\":%llu,\n"
        "  \"exception_reentry_blocks\":%llu,\n"
        "  \"sio_stat\":\"0x%04X\",\n"
        "  \"sio_ctrl\":\"0x%04X\",\n"
        "  \"sio_card_active\":%d,\n"
        "  \"i_stat\":\"0x%08X\",\n"
        "  \"i_mask\":\"0x%08X\",\n"
        "  \"mc_max_state\":%d,\n"
        "  \"tx_writes\":%d,\n"
        "  \"dirty_ram_blocks\":%llu,\n"
        "  \"dirty_ram_insns\":%llu,\n"
        "  \"dirty_pump_max_gap_insns\":%llu,\n"
        "  \"dirty_pump_count\":%llu,\n"
        "  \"tcp_send_stall_ms\":%llu,\n"
        "  \"tcp_clients_dropped\":%u,\n"
        "  \"bail_first\":%llu,\n"
        "  \"bail_resolved\":%llu,\n"
        "  \"bail_flattened\":%llu,\n"
        "  \"bail_anomaly\":%llu,\n"
        "  \"present_slow_count\":%u,\n"
        "  \"present_vsync_disabled\":%d,\n"
        "  \"wedge_kind\":%u,\n"
        "  \"wedge_kind_name\":\"%s\",\n"
        "  \"caps\":{\"wtrace_all\":%u,\"wtrace\":%u,\"frames\":%u,"
                  "\"sio_pc\":%u,\"thread\":%u,\"restore\":%u,\"fn_entry\":%u,"
                  "\"dirty_block\":%u},\n",
        s_backend, wall,
        (unsigned long long)frame, (unsigned long long)cyc,
        cur_fn, last_store,
        (unsigned long long)total_checks, dispatch_count,
        in_exc,
        (unsigned long long)exc_entries,
        (unsigned long long)exc_reentry,
        (unsigned)sio_stat_v, (unsigned)sio_ctrl_v,
        sio_card_active,
        i_stat_v, i_mask_v,
        mc_max, tx_writes,
        (unsigned long long)g_dirty_ram_blocks_run,
        (unsigned long long)g_dirty_ram_insns_run,
        (unsigned long long)g_dirty_pump_max_gap_insns,
        (unsigned long long)g_dirty_pump_count,
        (unsigned long long)debug_server_get_tcp_stall_ms(),
        debug_server_get_tcp_drops(),
        (unsigned long long)g_psx_bail_first,
        (unsigned long long)g_psx_bail_resolved,
        (unsigned long long)g_psx_bail_flattened,
        (unsigned long long)g_psx_bail_anomaly,
        g_present_slow_count,
        g_present_vsync_disabled,
        wedge_kind,
        (wedge_kind == 1) ? "hard_freeze" :
        (wedge_kind == 2) ? "reentry_storm" :
        (wedge_kind == 3) ? "slow_frames" :
        (wedge_kind == 4) ? "fatal" :
        (wedge_kind == 5) ? "spin_freeze" : "unknown",
        (unsigned)DUMP_CAP_WTRACE_ALL,
        (unsigned)DUMP_CAP_WTRACE,
        (unsigned)DUMP_CAP_FRAME_HISTORY,
        (unsigned)DUMP_CAP_SIO_PC,
        (unsigned)DUMP_CAP_THREAD_TRACE,
        (unsigned)DUMP_CAP_RESTORE_TRACE,
        (unsigned)DUMP_CAP_FN_ENTRY,
        (unsigned)DUMP_CAP_DIRTY_BLOCK);

    fputs("  \"heartbeat_ring\":[\n", f);
    uint32_t avail = s_ring_count;
    uint32_t start = (s_ring_count < RING_CAP) ? 0 : s_ring_head;
    for (uint32_t i = 0; i < avail; i++) {
        HbRingEntry *e = &s_ring[(start + i) % RING_CAP];
        fprintf(f,
            "    {\"wall\":%lld,\"frame\":%llu,\"cyc\":%llu,"
            "\"exc_re\":%llu,\"dirty_insns\":%llu,"
            "\"cur_fn\":\"0x%08X\",\"store_pc\":\"0x%08X\","
            "\"i_stat\":\"0x%08X\",\"sio_stat\":\"0x%04X\","
            "\"sio_ctrl\":\"0x%04X\",\"in_exc\":%u,\"mc_max\":%u,"
            "\"t1_count\":%u,\"t1_irq_fired\":%llu,"
            "\"tcp_ms\":%llu}%s\n",
            e->wall_clock,
            (unsigned long long)e->frame_count,
            (unsigned long long)e->psx_cycle_count,
            (unsigned long long)e->exc_reentry,
            (unsigned long long)e->dirty_ram_insns,
            e->current_func, e->last_store_pc,
            e->i_stat, (unsigned)e->sio_stat, (unsigned)e->sio_ctrl,
            (unsigned)e->in_exception, (unsigned)e->mc_max_state,
            (unsigned)e->t1_count, (unsigned long long)e->t1_irq_fired,
            (unsigned long long)e->tcp_stall_ms,
            (i + 1 < avail) ? "," : "");
    }
    fputs("  ],\n", f);

    fputs("  \"wtrace_all\":", f);
    debug_server_freeze_dump_wtrace_all_json(f, DUMP_CAP_WTRACE_ALL);
    fputs(",\n", f);

    fputs("  \"wtrace\":", f);
    debug_server_freeze_dump_wtrace_json(f, DUMP_CAP_WTRACE);
    fputs(",\n", f);

    fputs("  \"frame_history\":", f);
    debug_server_freeze_dump_frame_history_json(f, DUMP_CAP_FRAME_HISTORY);
    fputs(",\n", f);

    fputs("  \"sio_pc_trace\":", f);
    debug_server_freeze_dump_sio_pc_json(f, DUMP_CAP_SIO_PC);
    fputs(",\n", f);

    fputs("  \"thread_trace\":", f);
    debug_server_freeze_dump_thread_trace_json(f, DUMP_CAP_THREAD_TRACE);
    fputs(",\n", f);

    fputs("  \"restore_trace\":", f);
    debug_server_freeze_dump_restore_trace_json(f, DUMP_CAP_RESTORE_TRACE);
    fputs(",\n", f);

    fputs("  \"fn_entry\":", f);
    debug_server_freeze_dump_fn_entry_json(f, DUMP_CAP_FN_ENTRY);
    fputs(",\n", f);

    fputs("  \"dirty_block\":", f);
    debug_server_freeze_dump_dirty_block_json(f, DUMP_CAP_DIRTY_BLOCK);
    fputs(",\n", f);

#ifdef _WIN32
    /* Main-thread call stack. Hard freeze (wedge_kind==1): one snapshot of the
     * wedged thread in `main_stack`. Still-running wedges (kind 2 reentry
     * storm, kind 3 slow frames): a single frame is noise — take multiple
     * samples in `main_stack_samples` to expose the recurring wait (the
     * dispatch/handler chain, or a driver present block). */
    fputs("  \"main_stack\":", f);
    if (wedge_kind == 1) {
        freeze_dump_main_stack_json(f);
    } else {
        fputs("[]", f);
    }
    fputs(",\n  \"main_stack_samples\":", f);
    if (wedge_kind == 2 || wedge_kind == 3 || wedge_kind == 5) {
        freeze_dump_main_stack_samples_json(f, 8);
    } else {
        fputs("[]", f);
    }
    fputs("\n", f);
#else
    fputs("  \"main_stack\":[]\n", f);
#endif

    {
        static char cs_buf[6144];
        hb_format_cpu_scratchpad(cs_buf, sizeof(cs_buf));
        fputs(",\n", f);
        fputs(cs_buf, f);
        fputs("\n", f);
    }

    fputs("}\n", f);
    fclose(f);
    freeze_dump_unlock();
}

/* Full ring dump for deliberate fatal sites (psx_fatal_halt). Runs ON the
 * faulting (main) thread, unlike the watchdog path — wedge kind 4 keeps
 * freeze_dump_write away from the SuspendThread-based stack walkers,
 * which would self-suspend here. Gathers the same diag snapshot the
 * heartbeat tick does so the dump is self-contained. */
void freeze_heartbeat_fatal_dump(const char *reason) {
    (void)reason;  /* reason travels via g_psx_fatal_reason in the heartbeat */

    /* First dump wins. A post-mortem TCP command served from the fatal
     * halt loop can trip a host fault whose SEH handler also calls this;
     * the original fatal state is the one worth keeping. */
    static int s_fatal_dumped = 0;
    if (s_fatal_dumped) return;
    s_fatal_dumped = 1;

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

    s_last_wedge_kind = 4;
    s_dump_armed = 0;  /* the watchdog must not overwrite the fatal dump */
    freeze_dump_write((long long)time(NULL), s_frame_count,
                      psx_get_cycle_count(), exc_reentry,
                      g_debug_current_func_addr, g_debug_last_store_pc,
                      i_stat, i_mask, in_exc, total_checks,
                      dispatch_count, exc_entries,
                      sio_stat, sio_ctrl, card_active,
                      sio_get_mc_max_state(), sio_get_tx_writes());
}

static void heartbeat_write(void) {
    uint64_t cyc = psx_get_cycle_count();
    uint64_t frame = s_frame_count;
    uint32_t cur_fn = g_debug_current_func_addr;
    uint32_t last_store = g_debug_last_store_pc;

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

    int mc_max = sio_get_mc_max_state();
    int tx_writes = sio_get_tx_writes();

    /* Wall-clock seconds since epoch — coarse but enough to spot stalls. */
    long long wall = (long long)time(NULL);

    /* Push current state into the ring. The ring captures the seconds
     * leading up to the freeze; when main thread stalls, "current" values
     * stop advancing but the ring's older entries still show the recent
     * trajectory. */
    HbRingEntry *re = &s_ring[s_ring_head];
    re->frame_count     = frame;
    re->psx_cycle_count = cyc;
    re->exc_reentry     = exc_reentry;
    re->dirty_ram_insns = g_dirty_ram_insns_run;
    re->current_func    = cur_fn;
    re->last_store_pc   = last_store;
    re->i_stat          = i_stat;
    re->sio_stat        = sio_stat;
    re->sio_ctrl        = sio_ctrl;
    re->in_exception    = (uint8_t)in_exc;
    re->mc_max_state    = (uint8_t)mc_max;
    re->wall_clock      = wall;
    re->tcp_stall_ms    = debug_server_get_tcp_stall_ms();
    { uint16_t _t1c = 0; uint64_t _t1f = 0;
      timers_get_debug(1, &_t1c, NULL, NULL, &_t1f);
      re->t1_count = _t1c; re->t1_irq_fired = _t1f; }
    s_ring_head = (s_ring_head + 1) % RING_CAP;
    if (s_ring_count < RING_CAP) s_ring_count++;

    /* ---- Wedge detection: arm-once auto-dump ----
     * Walk back WEDGE_WINDOW_TICKS in the heartbeat ring (just pushed
     * above) and compute deltas. Trigger if any of:
     *   A. frame_count delta == 0                  (hard freeze)
     *   B. exc_reentry delta PER FRAME > threshold (reentry storm)
     *   C. frame_count delta < slow-frames floor   (host-side stall)
     *
     * Only fires once the ring has enough history. When the wedge clears
     * (a healthy tick), re-arm. */
    uint32_t wedge_kind = 0;  /* 0=healthy 1=hard 2=reentry storm 3=slow frames */
    if (s_ring_count >= WEDGE_WINDOW_TICKS) {
        /* The just-pushed tick is at (s_ring_head - 1). The oldest in
         * our window is WEDGE_WINDOW_TICKS - 1 ticks before it. */
        uint32_t newest_idx = (s_ring_head + RING_CAP - 1u) % RING_CAP;
        uint32_t oldest_idx = (s_ring_head + RING_CAP - WEDGE_WINDOW_TICKS) % RING_CAP;
        uint64_t newest_frame = s_ring[newest_idx].frame_count;
        uint64_t oldest_frame = s_ring[oldest_idx].frame_count;
        uint64_t newest_excre = s_ring[newest_idx].exc_reentry;
        uint64_t oldest_excre = s_ring[oldest_idx].exc_reentry;
        uint64_t frame_delta = (newest_frame >= oldest_frame)
                               ? (newest_frame - oldest_frame) : 0;
        uint64_t excre_delta = (newest_excre >= oldest_excre)
                               ? (newest_excre - oldest_excre) : 0;

        /* Logical-hang (kind D) signature: the executing function, the last
         * store PC, and the retired dirty-RAM instruction count are all
         * unchanged across the whole window. Requiring all three pinned makes
         * this specific to a guest spin loop — a legitimate long native
         * compute would still move last_store_pc, and any interpreted/overlay
         * work would advance dirty_insns. Checked only when frames are
         * advancing healthily (kinds 1/2/3 take precedence below). */
        int logic_pinned =
            (s_ring[newest_idx].current_func    == s_ring[oldest_idx].current_func) &&
            (s_ring[newest_idx].last_store_pc   == s_ring[oldest_idx].last_store_pc) &&
            (s_ring[newest_idx].dirty_ram_insns == s_ring[oldest_idx].dirty_ram_insns);

        if (frame_delta == 0)
            wedge_kind = 1;
        else if (excre_delta / frame_delta > WEDGE_EXC_REENTRY_PER_FRAME_THRESHOLD)
            wedge_kind = 2;
        else if (frame_delta < WEDGE_SLOW_FRAMES_MAX_DELTA)
            wedge_kind = 3;
        else if (logic_pinned)
            wedge_kind = 5;  /* spin freeze: game wedged while frames advance */
    }

    if (wedge_kind != 0) {
        if (s_dump_armed) {
            s_dump_armed = 0;
            s_last_wedge_kind = wedge_kind;
            freeze_dump_write(wall, frame, cyc, exc_reentry, cur_fn, last_store,
                              i_stat, i_mask, in_exc, total_checks,
                              dispatch_count, exc_entries,
                              sio_stat, sio_ctrl, card_active,
                              mc_max, tx_writes);
        }
    } else {
        /* Healthy tick: re-arm for the next wedge. */
        s_dump_armed = 1;
    }

    /* Timer1/RootCounter1 decode (Tomba 2 RCnt-wait diagnosis): if the game
     * spins on an RCnt1-based wait that never completes, t1_irq_fired stays
     * flat and/or t1_count never reaches t1_target. clock-source (mode bits
     * 8-9) is decoded because a bad clock-source would make T1 never tick. */
    uint16_t t1_count = 0, t1_target = 0; uint32_t t1_mode = 0; uint64_t t1_irq_fired = 0;
    timers_get_debug(1, &t1_count, &t1_target, &t1_mode, &t1_irq_fired);
    unsigned t1_clk = (unsigned)((t1_mode >> 8) & 3u);
    unsigned t1_irq_on_target = (unsigned)((t1_mode >> 4) & 1u);

    /* Bail-source ledger: the dominant wild-return source (= the game wait-site
     * spinning into the kernel) + how many distinct sites bail. */
    uint32_t bl_site_ra = 0, bl_wild = 0, bl_ssp = 0, bl_gsp = 0, bl_uniq = 0;
    uint64_t bl_count = 0;
    psx_bail_ledger_top(&bl_site_ra, &bl_wild, &bl_ssp, &bl_gsp, &bl_count, &bl_uniq);

    /* FAIL-FAST reasons contain raw newlines; unescaped they break this JSON
     * (psx_freeze_heartbeat.json.tmp was invalid on the TM4 jalr dumps). */
    char fatal_esc[384];
    fatal_esc[0] = 0;
    if (g_psx_fatal_reason) {
        size_t w = 0;
        for (const char *p = g_psx_fatal_reason; *p && w + 2 < sizeof(fatal_esc); p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch == '\\' || ch == '"') {
                fatal_esc[w++] = '\\';
                fatal_esc[w++] = (char)ch;
            } else if (ch >= 0x20) {
                fatal_esc[w++] = (char)ch;
            } else {
                fatal_esc[w++] = ' ';
            }
        }
        fatal_esc[w] = 0;
    }

    /* Buffer sized for current-state JSON + ring (~256B per ring entry). */
    static char buf[64 * 1024];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"backend\":\"%s\",\n"
        "  \"wall_clock_epoch\":%lld,\n"
        "  \"frame_count\":%llu,\n"
        "  \"psx_cycle_count\":%llu,\n"
        "  \"current_func\":\"0x%08X\",\n"
        "  \"last_store_pc\":\"0x%08X\",\n"
        "  \"total_checks\":%llu,\n"
        "  \"dispatch_count\":%u,\n"
        "  \"in_exception\":%d,\n"
        "  \"post_exception_cooldown\":%d,\n"
        "  \"exception_entries\":%llu,\n"
        "  \"exception_reentry_blocks\":%llu,\n"
        "  \"sio_irq_pending\":%d,\n"
        "  \"sio_irq_countdown\":%d,\n"
        "  \"sio_stat\":\"0x%04X\",\n"
        "  \"sio_ctrl\":\"0x%04X\",\n"
        "  \"sio_card_active\":%d,\n"
        "  \"i_stat\":\"0x%08X\",\n"
        "  \"i_mask\":\"0x%08X\",\n"
        "  \"mc_max_state\":%d,\n"
        "  \"tx_writes\":%d,\n"
        "  \"dirty_ram_blocks\":%llu,\n"
        "  \"dirty_ram_insns\":%llu,\n"
        "  \"dirty_pump_max_gap_insns\":%llu,\n"
        "  \"dirty_pump_count\":%llu,\n"
        "  \"t1_count\":%u,\n"
        "  \"t1_target\":%u,\n"
        "  \"t1_mode\":\"0x%04X\",\n"
        "  \"t1_clock_src\":%u,\n"
        "  \"t1_irq_on_target\":%u,\n"
        "  \"t1_irq_fired\":%llu,\n"
        "  \"bail_top_site_ra\":\"0x%08X\",\n"
        "  \"bail_top_wild_pc\":\"0x%08X\",\n"
        "  \"bail_top_site_sp\":\"0x%08X\",\n"
        "  \"bail_top_guest_sp\":\"0x%08X\",\n"
        "  \"bail_top_count\":%llu,\n"
        "  \"bail_unique_keys\":%u,\n"
        "  \"bail_first_site_ra\":\"0x%08X\",\n"
        "  \"bail_first_wild_pc\":\"0x%08X\",\n"
        "  \"bail_first_frame\":%u,\n"
        "  \"bail_first_in_exc\":%u,\n"
        "  \"ra_tw_latched\":%u,\n"
        "  \"ra_tw_site\":%u,\n"
        "  \"ra_tw_pc\":\"0x%08X\",\n"
        "  \"ra_tw_prev_ra\":\"0x%08X\",\n"
        "  \"ra_tw_frame\":%u,\n"
        "  \"ra_tw_in_exc\":%u,\n"
        "  \"ra_tw_sp\":\"0x%08X\",\n"
        "  \"vblank_raise_count\":%llu,\n"
        "  \"vblank_deliver_count\":%llu,\n"
        "  \"vblank_ack_count\":%llu,\n"
        "  \"irq_deliver_count\":%llu,\n"
        "  \"tcp_send_stall_ms\":%llu,\n"
        "  \"tcp_clients_dropped\":%u,\n"
        "  \"bail_first\":%llu,\n"
        "  \"bail_resolved\":%llu,\n"
        "  \"bail_flattened\":%llu,\n"
        "  \"bail_anomaly\":%llu,\n"
        "  \"fatal\":%s%s%s\n"
        "}\n",
        s_backend,
        wall,
        (unsigned long long)frame,
        (unsigned long long)cyc,
        cur_fn,
        last_store,
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
        mc_max,
        tx_writes,
        (unsigned long long)g_dirty_ram_blocks_run,
        (unsigned long long)g_dirty_ram_insns_run,
        (unsigned long long)g_dirty_pump_max_gap_insns,
        (unsigned long long)g_dirty_pump_count,
        (unsigned)t1_count,
        (unsigned)t1_target,
        (unsigned)t1_mode,
        t1_clk,
        t1_irq_on_target,
        (unsigned long long)t1_irq_fired,
        bl_site_ra,
        bl_wild,
        bl_ssp,
        bl_gsp,
        (unsigned long long)bl_count,
        bl_uniq,
        g_bail_first_site_ra,
        g_bail_first_wild_pc,
        g_bail_first_frame,
        g_bail_first_in_exc,
        g_ra_tw_latched,
        g_ra_tw_site,
        g_ra_tw_pc,
        g_ra_tw_prev_ra,
        g_ra_tw_frame,
        g_ra_tw_in_exc,
        g_ra_tw_sp,
        (unsigned long long)g_vblank_raise_count,
        (unsigned long long)g_vblank_deliver_count,
        (unsigned long long)g_vblank_ack_count,
        (unsigned long long)g_irq_deliver_count,
        (unsigned long long)debug_server_get_tcp_stall_ms(),
        debug_server_get_tcp_drops(),
        (unsigned long long)g_psx_bail_first,
        (unsigned long long)g_psx_bail_resolved,
        (unsigned long long)g_psx_bail_flattened,
        (unsigned long long)g_psx_bail_anomaly,
        g_psx_fatal_reason ? "\"" : "",
        g_psx_fatal_reason ? fatal_esc : "null",
        g_psx_fatal_reason ? "\"" : "");

    if (n <= 0 || n >= (int)sizeof(buf)) return;

    /* Append pre-freeze history ring. Format: array of compact rows,
     * oldest first, newest last. Reader can spot the moment all values
     * stop advancing — that's the stall point. */
    /* Strip the closing brace of the main object so we can append. */
    if (n > 0 && buf[n - 1] == '\n') n--;     /* drop trailing newline */
    if (n > 0 && buf[n - 1] == '}')  n--;     /* drop closing brace */
    if (n > 0 && buf[n - 1] == '\n') n--;     /* and any preceding newline */

    int m = snprintf(buf + n, sizeof(buf) - (size_t)n, ",\n  \"ring\":[\n");
    if (m > 0) n += m;

    uint32_t avail = s_ring_count;
    uint32_t start = (s_ring_count < RING_CAP) ? 0
                   : (s_ring_head /* head points to next-to-write = oldest */);
    for (uint32_t i = 0; i < avail; i++) {
        HbRingEntry *e = &s_ring[(start + i) % RING_CAP];
        m = snprintf(buf + n, sizeof(buf) - (size_t)n,
            "    {\"wall\":%lld,\"frame\":%llu,\"cyc\":%llu,"
            "\"exc_re\":%llu,\"dirty_insns\":%llu,"
            "\"cur_fn\":\"0x%08X\",\"store_pc\":\"0x%08X\","
            "\"i_stat\":\"0x%08X\",\"sio_stat\":\"0x%04X\","
            "\"sio_ctrl\":\"0x%04X\",\"in_exc\":%u,\"mc_max\":%u,"
            "\"t1_count\":%u,\"t1_irq_fired\":%llu,"
            "\"tcp_ms\":%llu}%s\n",
            e->wall_clock,
            (unsigned long long)e->frame_count,
            (unsigned long long)e->psx_cycle_count,
            (unsigned long long)e->exc_reentry,
            (unsigned long long)e->dirty_ram_insns,
            e->current_func, e->last_store_pc,
            e->i_stat, (unsigned)e->sio_stat, (unsigned)e->sio_ctrl,
            (unsigned)e->in_exception, (unsigned)e->mc_max_state,
            (unsigned)e->t1_count, (unsigned long long)e->t1_irq_fired,
            (unsigned long long)e->tcp_stall_ms,
            (i + 1 < avail) ? "," : "");
        if (m <= 0 || (size_t)(n + m) >= sizeof(buf)) break;
        n += m;
    }
    m = snprintf(buf + n, sizeof(buf) - (size_t)n, "  ],\n");
    if (m > 0) n += m;

    /* CPU register file + full scratchpad: the wedged state of a logical
     * hang, present every tick so it survives even a plain force-close
     * (no auto-dump needed). See hb_format_cpu_scratchpad. */
    m = hb_format_cpu_scratchpad(buf + n, sizeof(buf) - (size_t)n);
    if (m > 0) n += m;

    m = snprintf(buf + n, sizeof(buf) - (size_t)n, "\n}\n");
    if (m > 0) n += m;

    /* Atomic overwrite via .tmp + rename. Avoids a reader catching a
     * mid-write file and parsing partial JSON. Cheap on Windows
     * (MoveFileEx with REPLACE_EXISTING). */
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), HB_FILE ".tmp");

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);

#ifdef _WIN32
    MoveFileExA(tmp_path, HB_FILE,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    rename(tmp_path, HB_FILE);
#endif
}

#ifdef _WIN32
static DWORD WINAPI heartbeat_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        heartbeat_write();
        Sleep(HB_INTERVAL_MS);
    }
}
#endif

void freeze_heartbeat_start(const char *backend_label) {
    /* INTENTIONALLY not gated by PSX_NO_DEBUG_TOOLS. The heartbeat thread
     * is the only observability mechanism that survives a main-thread
     * stall (TCP server is on main thread; debug log functions are
     * called from the stalled code). Cost is ~1 KB/sec disk write and
     * one extra thread — small enough to keep in production builds for
     * crash forensics. */
    if (s_started) return;
    if (backend_label && backend_label[0]) {
        size_t n = strlen(backend_label);
        if (n >= sizeof(s_backend)) n = sizeof(s_backend) - 1;
        memcpy(s_backend, backend_label, n);
        s_backend[n] = 0;
    }
#ifdef _WIN32
    /* Duplicate the main thread's pseudo-handle into a real handle so the
     * heartbeat thread can SuspendThread/GetThreadContext for stack capture
     * on hard freezes. GetCurrentThread() returns a pseudo-handle that's
     * only valid in the calling thread, so the duplication is required. */
    s_main_thread_id = GetCurrentThreadId();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_main_thread,
                         THREAD_ALL_ACCESS, FALSE, 0)) {
        s_main_thread = NULL;
    }
    s_thread = CreateThread(NULL, 0, heartbeat_thread, NULL, 0, NULL);
    if (s_thread) s_started = 1;
#endif
}
