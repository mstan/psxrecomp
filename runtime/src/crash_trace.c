/* crash_trace.c — unified crash diagnostic dump.
 *
 * Writes a single JSON report file (psx_last_run_report.json) on:
 *   - signal (SIGSEGV / SIGABRT)
 *   - Windows SEH unhandled exception
 *   - atexit
 *   - fail-fast psx_unknown_dispatch
 *   - trap_crash
 *   - TCP "post_mortem_dump" command (future)
 *
 * Mirrors F:/Projects/SuperMarioWorldRecomp/src/post_mortem.c. The file
 * is OVERWRITTEN on each dump (last-write-wins, single file per run);
 * this is not a log per CLAUDE.md §3 — it's a one-shot final state
 * snapshot for crashes the running TCP server cannot intercept.
 *
 * All payload comes from already-existing rings; this module is a
 * serializer, not a recorder.
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu_state.h"
#include "crash_trace.h"

/* Output path — overwritten per dump. */
static const char *kReportPath = "psx_last_run_report.json";

/* CPU state pointer (set by debug server at init). */
extern CPUState *debug_cpu_ptr;

/* Frame counter from debug_server.c (non-static). */
extern uint64_t s_frame_count;

/* Globals from debug_server.c we want to capture. */
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;

/* Dispatch ring — accessor wrappers exported by debug_server.c. */
#define DISPATCH_TRACE_CAP (1 << 16)
extern uint32_t crash_trace_dispatch_ring_get(int idx);
extern uint64_t crash_trace_dispatch_seq_get(void);

/* Unknown-dispatch ring — layout must match debug_server.c's
 * UnknownDispatchEntry. Accessor wrappers exported by debug_server.c. */
typedef struct {
    uint64_t seq;
    uint32_t addr, phys, ra, a0, a1, frame, pad;
} UnknownDispatchEntry;
#define UNKNOWN_DISPATCH_CAP (1 << 16)
extern UnknownDispatchEntry crash_trace_unknown_get(uint64_t seq);
extern uint64_t crash_trace_unknown_seq_get(void);

/* Dirty-RAM block log (defined in dirty_ram_interp.c). */
#include "dirty_ram_interp.h"

/* JSON helpers. Hand-rolled to avoid allocations on the SEH path. */

static int append_str(char *buf, size_t cap, size_t *pos, const char *s) {
    size_t n = strlen(s);
    if (*pos + n >= cap) return 0;
    memcpy(buf + *pos, s, n);
    *pos += n;
    buf[*pos] = 0;
    return 1;
}

static int append_fmt(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos) return 0;
    *pos += (size_t)n;
    return 1;
}

/* Serialize a single uint32_t hex value as a JSON string. */
static void hex32(char *out, uint32_t v) {
    snprintf(out, 16, "\"0x%08X\"", v);
}

/* ── Main entry ──────────────────────────────────────────────────────── */

void psx_crash_trace_dump(const char *reason, void *seh_info) {
    /* Pre-allocate large stack buffer; avoid heap on SEH path. */
    static char buf[8 * 1024 * 1024]; /* 8 MB */
    size_t pos = 0;

    /* Header */
    char ts[64] = {0};
    {
        time_t now = time(NULL);
        struct tm *tm = gmtime(&now);
        if (tm) strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
    }

    append_fmt(buf, sizeof(buf), &pos,
        "{\n"
        "  \"reason\": \"%s\",\n"
        "  \"timestamp\": \"%s\",\n"
        "  \"frame\": %llu,\n"
        "  \"last_func_addr\": \"0x%08X\",\n"
        "  \"last_store_pc\": \"0x%08X\",\n",
        reason ? reason : "(unknown)",
        ts,
        (unsigned long long)s_frame_count,
        g_debug_current_func_addr,
        g_debug_last_store_pc);

#ifdef _WIN32
    if (seh_info) {
        EXCEPTION_POINTERS *info = (EXCEPTION_POINTERS *)seh_info;
        DWORD code = info->ExceptionRecord->ExceptionCode;
        void *addr = info->ExceptionRecord->ExceptionAddress;
        const char *kind = "?";
        ULONG_PTR fault_addr = 0;
        if (code == EXCEPTION_ACCESS_VIOLATION) {
            ULONG_PTR k = info->ExceptionRecord->ExceptionInformation[0];
            kind = (k == 0) ? "read" : (k == 1) ? "write" : "execute";
            fault_addr = info->ExceptionRecord->ExceptionInformation[1];
        }
        append_fmt(buf, sizeof(buf), &pos,
            "  \"seh\": {\n"
            "    \"code\": \"0x%08lX\",\n"
            "    \"address\": \"%p\",\n"
            "    \"access\": \"%s\",\n"
            "    \"fault_addr\": \"0x%p\"\n"
            "  },\n",
            code, addr, kind, (void *)fault_addr);
    }
#else
    (void)seh_info;
#endif

    /* CPU state */
    if (debug_cpu_ptr) {
        CPUState *cpu = debug_cpu_ptr;
        append_str(buf, sizeof(buf), &pos, "  \"cpu\": {\n");
        append_fmt(buf, sizeof(buf), &pos,
            "    \"pc\": \"0x%08X\",\n"
            "    \"hi\": \"0x%08X\",\n"
            "    \"lo\": \"0x%08X\",\n"
            "    \"sr\": \"0x%08X\",\n"
            "    \"cause\": \"0x%08X\",\n"
            "    \"epc\": \"0x%08X\",\n"
            "    \"gpr\": [",
            cpu->pc, cpu->hi, cpu->lo,
            cpu->cop0[12], cpu->cop0[13], cpu->cop0[14]);
        for (int i = 0; i < 32; i++) {
            append_fmt(buf, sizeof(buf), &pos,
                "%s\"0x%08X\"", i == 0 ? "" : ",", cpu->gpr[i]);
        }
        append_str(buf, sizeof(buf), &pos, "]\n  },\n");
    } else {
        append_str(buf, sizeof(buf), &pos, "  \"cpu\": null,\n");
    }

    /* dispatch_ring tail (last 256) */
    {
        uint64_t total = crash_trace_dispatch_seq_get();
        int avail = (total < DISPATCH_TRACE_CAP) ? (int)total : DISPATCH_TRACE_CAP;
        int count = avail < 256 ? avail : 256;
        append_fmt(buf, sizeof(buf), &pos,
            "  \"dispatch_tail\": {\n"
            "    \"total\": %llu,\n"
            "    \"count\": %d,\n"
            "    \"addrs\": [",
            (unsigned long long)total, count);
        uint64_t start = total - (uint64_t)count;
        for (int i = 0; i < count; i++) {
            uint32_t a = crash_trace_dispatch_ring_get((int)((start + i) & (DISPATCH_TRACE_CAP - 1)));
            append_fmt(buf, sizeof(buf), &pos, "%s\"0x%08X\"", i == 0 ? "" : ",", a);
        }
        append_str(buf, sizeof(buf), &pos, "]\n  },\n");
    }

    /* unknown_dispatch tail (last 50) */
    {
        uint64_t total = crash_trace_unknown_seq_get();
        int avail = (total < UNKNOWN_DISPATCH_CAP) ? (int)total : UNKNOWN_DISPATCH_CAP;
        int count = avail < 50 ? avail : 50;
        append_fmt(buf, sizeof(buf), &pos,
            "  \"unknown_dispatch_tail\": {\n"
            "    \"total\": %llu,\n"
            "    \"count\": %d,\n"
            "    \"entries\": [",
            (unsigned long long)total, count);
        uint64_t start = total - (uint64_t)count;
        for (int i = 0; i < count; i++) {
            UnknownDispatchEntry e = crash_trace_unknown_get(start + i);
            append_fmt(buf, sizeof(buf), &pos,
                "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"phys\":\"0x%08X\","
                "\"ra\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                "\"frame\":%u}",
                i == 0 ? "" : ",",
                (unsigned long long)e.seq, e.addr, e.phys,
                e.ra, e.a0, e.a1, e.frame);
        }
        append_str(buf, sizeof(buf), &pos, "]\n  },\n");
    }

    /* dirty_block_log tail (last 100) */
    {
        uint64_t total = g_dirty_ram_block_log_seq;
        uint64_t avail = (total < DIRTY_RAM_BLOCK_LOG_CAP) ? total : DIRTY_RAM_BLOCK_LOG_CAP;
        int count = (int)((avail < 100) ? avail : 100);
        append_fmt(buf, sizeof(buf), &pos,
            "  \"dirty_block_tail\": {\n"
            "    \"total\": %llu,\n"
            "    \"count\": %d,\n"
            "    \"entries\": [",
            (unsigned long long)total, count);
        uint64_t start = total - (uint64_t)count;
        for (int i = 0; i < count; i++) {
            DirtyRamBlockLogEntry *e =
                &g_dirty_ram_block_log[(start + i) & (DIRTY_RAM_BLOCK_LOG_CAP - 1u)];
            append_fmt(buf, sizeof(buf), &pos,
                "%s{\"seq\":%llu,\"target\":\"0x%08X\",\"ra\":\"0x%08X\","
                "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"frame\":%u}",
                i == 0 ? "" : ",",
                (unsigned long long)e->seq,
                e->target, e->ra, e->a0, e->a1, e->frame);
        }
        append_str(buf, sizeof(buf), &pos, "]\n  }\n");
    }

    append_str(buf, sizeof(buf), &pos, "}\n");

    /* Write to file. Overwrite previous report. */
    FILE *f = fopen(kReportPath, "wb");
    if (f) {
        fwrite(buf, 1, pos, f);
        fclose(f);
    }
}

/* ── Crash handlers ──────────────────────────────────────────────────── */

#include <signal.h>

static void psx_signal_handler(int sig) {
    char reason[64];
    snprintf(reason, sizeof(reason), "signal_%d", sig);
    psx_crash_trace_dump(reason, NULL);
    /* Reraise default handler so debugger / OS can also act. */
    signal(sig, SIG_DFL);
    raise(sig);
}

#ifdef _WIN32
static LONG WINAPI psx_seh_handler(EXCEPTION_POINTERS *info) {
    psx_crash_trace_dump("seh", info);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static void psx_atexit_handler(void) {
    /* Only dump if no crash dump has been written yet. We can't easily
     * detect that, so just always overwrite — last write wins. */
    psx_crash_trace_dump("atexit", NULL);
}

void psx_crash_trace_install_handlers(void) {
    signal(SIGSEGV, psx_signal_handler);
    signal(SIGABRT, psx_signal_handler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(psx_seh_handler);
    /* Suppress Windows error dialog so SEH unwinds straight to our
     * filter and we can write the report without the user having to
     * dismiss a popup first. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    atexit(psx_atexit_handler);
}
