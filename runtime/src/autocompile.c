/* autocompile.c — see autocompile.h. Windows-first (the project's dev
 * platform); on other hosts the spawn is a graceful no-op and the manual
 * compile_overlays.py flow still works. */
#include "autocompile.h"
#include "overlay_loader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv — declared here; glibc/windows.h leak it, macOS SDK does not */
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

static char s_cmd[4096];   /* large: the runtime-constructed bundled tcc cmd has
                            * many absolute paths (python+script+recompiler+tcc+...) */
static char s_cwd[512];
/* Canonical loader cache dir + captures file (see autocompile_set_cache_paths).
 * Injected into the compile child's environment so its WRITE location is exactly
 * the loader's READ location, regardless of what the configured command says. */
static char s_cache_dir[512];
static char s_captures[512];

enum { AC_IDLE = 0, AC_RUNNING = 1, AC_DONE = 2 };
#ifdef _WIN32
static volatile LONG s_state     = AC_IDLE;
static volatile LONG s_exit_code = -1;
static int ac_state_load(void) {
    return (int)InterlockedCompareExchange(&s_state, AC_IDLE, AC_IDLE);
}
static void ac_state_store(int value) {
    InterlockedExchange(&s_state, (LONG)value);
}
#else
static int s_state     = AC_IDLE;
static int s_exit_code = -1;
static int ac_state_load(void) { return s_state; }
static void ac_state_store(int value) { s_state = value; }
#endif
static uint32_t     s_runs      = 0;
static uint32_t     s_fails     = 0;
static uint32_t     s_rescans   = 0;
/* Per-shard build accounting, parsed from compile_overlays.py's machine-readable
 * "PSX_SHARD_RESULT ok=N failed=M skipped=K" line. Before this, a compile run
 * that built zero shards because a header change broke EVERY shard compile
 * looked identical to a clean run: the script still exited 0 and the runtime
 * quietly ran everything interpreted. These counters make that visible over the
 * TCP debug server (autocompile_status). s_shard_fail_total accumulates across
 * runs so a single failing run is never overwritten by a later clean one. */
static uint32_t     s_shard_ok         = 0;   /* last run */
static uint32_t     s_shard_fail       = 0;   /* last run */
static uint32_t     s_shard_skipped    = 0;   /* last run */
static uint32_t     s_shard_fail_total = 0;   /* accumulated across all runs */
static int          s_shard_result_seen = 0;  /* did we parse a result line? */

/* Child-output tail ring. Watcher thread writes, TCP reads — guarded by a
 * critical section on Windows. Holds the TAIL (newest bytes win). */
#define AC_OUT_CAP 8192
static char s_out[AC_OUT_CAP];
static int  s_out_len = 0;
static unsigned s_publish_drops_run = 0;
static unsigned s_publish_load_fail_run = 0, s_publish_parse_fail_run = 0;

#ifdef _WIN32
static CRITICAL_SECTION s_out_lock;
static CONDITION_VARIABLE s_publish_cv;
static int              s_out_lock_init = 0;
static HANDLE           s_proc = NULL;
/* Kill-on-close job tying the whole cmd->python->gcc compile tree to this
 * process: any exit path (including a crash) closes the handle and the kernel
 * reaps the tree, so a compiler can never keep writing into the cache after
 * the runtime is gone. Owned exclusively by the emulation thread. */
static HANDLE           s_job = NULL;
static HANDLE           s_watch_thread = NULL;
static HANDLE           s_prepare_thread = NULL;

typedef struct PublishItem {
    struct PublishItem *next;
    OverlayPreparedImage *image;
    char path[768];
} PublishItem;
static PublishItem *s_publish_raw_head, *s_publish_raw_tail;
static PublishItem *s_publish_ready_head, *s_publish_ready_tail;
enum { AC_PUBLISH_READY_LIMIT = 1 };
static unsigned s_publish_ready_count;
static unsigned s_publish_ready_highwater;
static unsigned s_publish_preparing;
static unsigned s_publish_commit_active;
static int s_publish_input_done;
static int s_publish_stop;
static uint64_t s_publish_prepare_total_us;
static uint64_t s_publish_prepare_max_us;
static uint64_t s_publish_prepare_last_us;
static unsigned s_publish_prepare_count;
static unsigned s_publish_prepare_fail;
static unsigned s_publish_prepare_retry;
static unsigned s_publish_prepare_giveup;
static char s_child_line[1024];
static int  s_child_line_len = 0;
static int  s_child_line_overflow = 0;

/* Parse result markers while stdout is streaming, not from s_out after exit.
 * The configured child command may chain post-processing after
 * compile_overlays.py (coverage_vault.py is the production example).  That
 * output can exceed AC_OUT_CAP and evict the shard result from the diagnostic
 * tail even though the marker was complete and valid when it arrived.  Keep
 * the last valid result here; poll_main accounts it exactly once after both
 * child-output workers have joined. */
static int shard_result_line_locked(const char *line) {
    unsigned ok = 0, failed = 0, skipped = 0;
    int consumed = 0;
    if (sscanf(line, "PSX_SHARD_RESULT ok=%u failed=%u skipped=%u %n",
               &ok, &failed, &skipped, &consumed) != 3)
        return 0;
    for (const char *tail = line + consumed; *tail; tail++) {
        if (*tail != ' ' && *tail != '\t' && *tail != '\r') return 0;
    }
    s_shard_ok = ok;
    s_shard_fail = failed;
    s_shard_skipped = skipped;
    s_shard_result_seen = 1;
    return 1;
}

static void child_line_locked(void) {
    if (shard_result_line_locked(s_child_line)) return;
    static const char marker[] = "PSX_SHARD_PUBLISHED ";
    if (strncmp(s_child_line, marker, sizeof(marker) - 1) != 0) return;
    const char *path = s_child_line + sizeof(marker) - 1;
    if (!path[0]) return;
    if (strlen(path) >= sizeof(((PublishItem *)0)->path)) {
        s_publish_parse_fail_run++;
        return;
    }
    PublishItem *item = (PublishItem *)calloc(1, sizeof(*item));
    if (!item) {
        s_publish_drops_run++;
        return;
    }
    snprintf(item->path, sizeof(item->path), "%s", path);
    if (s_publish_raw_tail) s_publish_raw_tail->next = item;
    else s_publish_raw_head = item;
    s_publish_raw_tail = item;
    WakeConditionVariable(&s_publish_cv);
}

static void publish_parse_locked(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            s_child_line[s_child_line_len] = '\0';
            if (s_child_line_overflow)
                s_publish_parse_fail_run++;
            else
                child_line_locked();
            s_child_line_len = 0;
            s_child_line_overflow = 0;
        } else if (s_child_line_len < (int)sizeof(s_child_line) - 1) {
            s_child_line[s_child_line_len++] = c;
        } else {
            s_child_line_overflow = 1;
        }
    }
}

/* The pipe reader only queues paths. A separate worker first-maps them, so a
 * slow antivirus/disk/loader event cannot stop stdout draining and deadlock the
 * compiler. There is exactly one mapped handoff at a time. The worker also
 * waits for the preceding main-thread commit to finish before entering the
 * process-wide Windows loader lock again. */
static DWORD WINAPI publish_prepare_thread_main(LPVOID unused) {
    (void)unused;
    for (;;) {
        PublishItem *item = NULL;
        EnterCriticalSection(&s_out_lock);
        while (!s_publish_stop && !s_publish_raw_head &&
               !s_publish_input_done)
            SleepConditionVariableCS(&s_publish_cv, &s_out_lock, INFINITE);
        while (!s_publish_stop && s_publish_raw_head &&
               (s_publish_ready_count != 0 || s_publish_commit_active != 0))
            SleepConditionVariableCS(&s_publish_cv, &s_out_lock, INFINITE);
        if (s_publish_stop || (!s_publish_raw_head && s_publish_input_done)) {
            LeaveCriticalSection(&s_out_lock);
            break;
        }
        item = s_publish_raw_head;
        s_publish_raw_head = item->next;
        if (!s_publish_raw_head) s_publish_raw_tail = NULL;
        item->next = NULL;
        s_publish_preparing++;
        LeaveCriticalSection(&s_out_lock);

        /* A dropped publication silently reverts this artifact to a
         * synchronous emulation-thread first-map on its next dispatch miss —
         * the exact stall this pipeline exists to remove. Mapping failures
         * right after the writer finishes are usually transient (AV scan or
         * a sharing violation holding the file), so retry here where waiting
         * costs no frame time. Validation rejects also land here and burn two
         * pointless sleeps; that's an accepted cost of keeping prepare's
         * failure reasons opaque. */
        enum { AC_PREPARE_ATTEMPTS = 3, AC_PREPARE_RETRY_MS = 100 };
        LARGE_INTEGER q0, q1, qf;
        int attempts = 0;
        for (;;) {
            QueryPerformanceCounter(&q0);
            item->image = overlay_loader_prepare_published(item->path);
            QueryPerformanceCounter(&q1);
            attempts++;
            if (item->image || attempts >= AC_PREPARE_ATTEMPTS) break;
            int stopping;
            EnterCriticalSection(&s_out_lock);
            s_publish_prepare_retry++;
            stopping = s_publish_stop;
            LeaveCriticalSection(&s_out_lock);
            if (stopping) break;
            Sleep(AC_PREPARE_RETRY_MS);
        }
        QueryPerformanceFrequency(&qf);
        uint64_t elapsed_us = qf.QuadPart > 0
            ? (uint64_t)((q1.QuadPart - q0.QuadPart) * 1000000LL /
                         qf.QuadPart) : 0;
        if (!item->image) {
            EnterCriticalSection(&s_out_lock);
            s_publish_preparing--;
            s_publish_prepare_count++;
            s_publish_prepare_fail++;
            s_publish_prepare_giveup++;
            s_publish_prepare_last_us = elapsed_us;
            s_publish_prepare_total_us += elapsed_us;
            if (elapsed_us > s_publish_prepare_max_us)
                s_publish_prepare_max_us = elapsed_us;
            s_publish_load_fail_run++;
            LeaveCriticalSection(&s_out_lock);
            free(item);
            continue;
        }
        EnterCriticalSection(&s_out_lock);
        s_publish_preparing--;
        s_publish_prepare_count++;
        s_publish_prepare_last_us = elapsed_us;
        s_publish_prepare_total_us += elapsed_us;
        if (elapsed_us > s_publish_prepare_max_us)
            s_publish_prepare_max_us = elapsed_us;
        if (s_publish_stop) {
            LeaveCriticalSection(&s_out_lock);
            overlay_loader_discard_prepared(item->image);
            free(item);
            break;
        }
        /* The wait-before-prepare invariant makes this queue depth exactly one.
         * Keep a defensive condition in case future code adds another worker. */
        while (!s_publish_stop &&
               s_publish_ready_count >= AC_PUBLISH_READY_LIMIT)
            SleepConditionVariableCS(&s_publish_cv, &s_out_lock, INFINITE);
        if (s_publish_stop) {
            LeaveCriticalSection(&s_out_lock);
            overlay_loader_discard_prepared(item->image);
            free(item);
            break;
        }
        if (s_publish_ready_tail) s_publish_ready_tail->next = item;
        else s_publish_ready_head = item;
        s_publish_ready_tail = item;
        s_publish_ready_count++;
        if (s_publish_ready_count > s_publish_ready_highwater)
            s_publish_ready_highwater = s_publish_ready_count;
        LeaveCriticalSection(&s_out_lock);
    }
    if (!s_publish_stop)
        ac_state_store(AC_DONE);
    return 0;
}

static PublishItem *publish_ready_pop(void) {
    PublishItem *item;
    EnterCriticalSection(&s_out_lock);
    item = s_publish_ready_head;
    if (item) {
        s_publish_ready_head = item->next;
        if (!s_publish_ready_head) s_publish_ready_tail = NULL;
        item->next = NULL;
        s_publish_ready_count--;
        s_publish_commit_active++;
    }
    LeaveCriticalSection(&s_out_lock);
    return item;
}

static void publish_commit_finished(void) {
    EnterCriticalSection(&s_out_lock);
    if (s_publish_commit_active) s_publish_commit_active--;
    WakeAllConditionVariable(&s_publish_cv);
    LeaveCriticalSection(&s_out_lock);
}

static int publish_pending(void) {
    int pending;
    EnterCriticalSection(&s_out_lock);
    pending = s_publish_raw_head != NULL || s_publish_ready_head != NULL ||
              s_publish_preparing != 0 || s_publish_commit_active != 0;
    LeaveCriticalSection(&s_out_lock);
    return pending;
}

static void publish_note_load_failure(void) {
    EnterCriticalSection(&s_out_lock);
    s_publish_load_fail_run++;
    LeaveCriticalSection(&s_out_lock);
}

static int publish_discard_all(void) {
    PublishItem *raw, *ready;
    EnterCriticalSection(&s_out_lock);
    /* Only an idle provider may reset publication ownership. Never pretend an
     * image currently inside LoadLibrary is no longer preparing: doing so
     * would let a new run race the old watcher and corrupt the FIFO invariant. */
    if (s_publish_preparing != 0 || s_publish_commit_active != 0) {
        LeaveCriticalSection(&s_out_lock);
        return 0;
    }
    raw = s_publish_raw_head;
    ready = s_publish_ready_head;
    s_publish_raw_head = s_publish_raw_tail = NULL;
    s_publish_ready_head = s_publish_ready_tail = NULL;
    s_publish_ready_count = 0;
    WakeAllConditionVariable(&s_publish_cv);
    LeaveCriticalSection(&s_out_lock);
    while (raw) {
        PublishItem *next = raw->next;
        free(raw);
        raw = next;
    }
    while (ready) {
        PublishItem *next = ready->next;
        overlay_loader_discard_prepared(ready->image);
        free(ready);
        ready = next;
    }
    return 1;
}

static void out_append(const char *buf, int n) {
    EnterCriticalSection(&s_out_lock);
    publish_parse_locked(buf, n);
    if (n >= AC_OUT_CAP) {
        memcpy(s_out, buf + (n - AC_OUT_CAP), AC_OUT_CAP);
        s_out_len = AC_OUT_CAP;
    } else {
        if (s_out_len + n > AC_OUT_CAP) {
            int keep = AC_OUT_CAP - n;
            memmove(s_out, s_out + (s_out_len - keep), keep);
            s_out_len = keep;
        }
        memcpy(s_out + s_out_len, buf, n);
        s_out_len += n;
    }
    LeaveCriticalSection(&s_out_lock);
}

#ifdef PSX_AUTOCOMPILE_TEST
void autocompile_test_feed_output(const char *buf, int n) {
    out_append(buf, n);
}

int autocompile_test_start_preparer(void) {
    EnterCriticalSection(&s_out_lock);
    s_publish_input_done = 0;
    s_publish_stop = 0;
    LeaveCriticalSection(&s_out_lock);
    s_prepare_thread = CreateThread(NULL, 0, publish_prepare_thread_main,
                                    NULL, 0, NULL);
    return s_prepare_thread != NULL;
}

void autocompile_test_finish_input(void) {
    EnterCriticalSection(&s_out_lock);
    s_publish_input_done = 1;
    WakeAllConditionVariable(&s_publish_cv);
    LeaveCriticalSection(&s_out_lock);
}

int autocompile_test_join_preparer(DWORD timeout_ms) {
    if (!s_prepare_thread) return 1;
    if (WaitForSingleObject(s_prepare_thread, timeout_ms) != WAIT_OBJECT_0)
        return 0;
    CloseHandle(s_prepare_thread);
    s_prepare_thread = NULL;
    return 1;
}

int autocompile_test_ready_count(void) {
    int count = 0;
    EnterCriticalSection(&s_out_lock);
    for (PublishItem *item = s_publish_ready_head; item; item = item->next)
        count++;
    LeaveCriticalSection(&s_out_lock);
    return count;
}

int autocompile_test_ready_highwater(void) {
    int highwater;
    EnterCriticalSection(&s_out_lock);
    highwater = (int)s_publish_ready_highwater;
    LeaveCriticalSection(&s_out_lock);
    return highwater;
}

int autocompile_test_preparing_count(void) {
    int preparing;
    EnterCriticalSection(&s_out_lock);
    preparing = (int)s_publish_preparing;
    LeaveCriticalSection(&s_out_lock);
    return preparing;
}

void autocompile_test_discard_all(void) {
    (void)publish_discard_all();
}
#endif

typedef struct { HANDLE read_pipe; HANDLE proc; } WatchCtx;

static DWORD WINAPI watch_thread(LPVOID arg) {
    WatchCtx *ctx = (WatchCtx *)arg;
    char buf[1024];
    DWORD got;
    while (ReadFile(ctx->read_pipe, buf, sizeof(buf), &got, NULL) && got > 0)
        out_append(buf, (int)got);
    CloseHandle(ctx->read_pipe);
    WaitForSingleObject(ctx->proc, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(ctx->proc, &code);
    EnterCriticalSection(&s_out_lock);
    /* A marker/result line cut off by child termination is not trustworthy.
     * Completion will force the idempotent directory rescan instead. */
    if (s_child_line_len || s_child_line_overflow) {
        s_publish_parse_fail_run++;
        s_child_line_len = 0;
        s_child_line_overflow = 0;
    }
    if (s_proc == ctx->proc) s_proc = NULL;
    LeaveCriticalSection(&s_out_lock);
    CloseHandle(ctx->proc);
    HeapFree(GetProcessHeap(), 0, ctx);
    InterlockedExchange(&s_exit_code, (LONG)code);
    EnterCriticalSection(&s_out_lock);
    s_publish_input_done = 1;
    WakeAllConditionVariable(&s_publish_cv);
    LeaveCriticalSection(&s_out_lock);
    return 0;
}
#endif /* _WIN32 */

void autocompile_configure(const char *cmd, const char *cwd) {
    snprintf(s_cmd, sizeof(s_cmd), "%s", cmd ? cmd : "");
    snprintf(s_cwd, sizeof(s_cwd), "%s", cwd ? cwd : "");
#ifdef _WIN32
    if (!s_out_lock_init) {
        InitializeCriticalSection(&s_out_lock);
        InitializeConditionVariable(&s_publish_cv);
        s_out_lock_init = 1;
    }
#endif
}

void autocompile_set_cache_paths(const char *cache_dir, const char *captures) {
    snprintf(s_cache_dir, sizeof(s_cache_dir), "%s", cache_dir ? cache_dir : "");
    snprintf(s_captures,  sizeof(s_captures),  "%s", captures  ? captures  : "");
}

int autocompile_configured(void) { return s_cmd[0] != '\0'; }
int autocompile_busy(void)       { return ac_state_load() != AC_IDLE; }

/* Probe PATH for a real C compiler (gcc/cc/clang). A configured command STRING
 * (autocompile_configured) is not enough: the shipped game.toml always carries
 * overlay_autocompile_cmd, so it can't tell a dev box from a toolchain-less
 * player. This opens each candidate exe in each PATH dir — the actual "can we
 * compile a shard" signal. Memoized (PATH doesn't change mid-run). */
int autocompile_toolchain_available(void) {
    static int s_cached = -1;
    if (s_cached >= 0) return s_cached;
    const char *path = getenv("PATH");
    int found = 0;
    if (path && *path) {
#ifdef _WIN32
        const char sep = ';';
        static const char *exes[] = { "gcc.exe", "cc.exe", "clang.exe" };
#else
        const char sep = ':';
        static const char *exes[] = { "gcc", "cc", "clang" };
#endif
        const char *p = path;
        while (*p && !found) {
            const char *e = strchr(p, sep);
            size_t dlen = e ? (size_t)(e - p) : strlen(p);
            if (dlen > 0 && dlen < 480) {
                for (size_t k = 0; k < sizeof(exes) / sizeof(exes[0]); k++) {
                    char cand[512];
                    snprintf(cand, sizeof(cand), "%.*s/%s", (int)dlen, p, exes[k]);
                    FILE *f = fopen(cand, "rb");
                    if (f) { fclose(f); found = 1; break; }
                }
            }
            if (!e) break;
            p = e + 1;
        }
    }
    s_cached = found;
    return found;
}

int autocompile_request(void) {
    /* DONE owns an unconsumed cache rescan/result. Only the emulation-thread
     * poll may return it to IDLE; never overwrite it with another child. */
    if (!autocompile_configured() || ac_state_load() != AC_IDLE) return 0;
#ifdef _WIN32
    /* IDLE should imply empty queues; discard defensively so a prior aborted
     * run can never leak a speculative module reference into the next one. */
    if (!publish_discard_all()) return 0;
    EnterCriticalSection(&s_out_lock);
    s_publish_drops_run = 0;
    s_publish_load_fail_run = s_publish_parse_fail_run = 0;
    s_publish_ready_highwater = 0;
    s_publish_prepare_total_us = 0;
    s_publish_prepare_max_us = 0;
    s_publish_prepare_last_us = 0;
    s_publish_prepare_count = 0;
    s_publish_prepare_fail = 0;
    s_publish_prepare_retry = 0;
    s_publish_prepare_giveup = 0;
    s_publish_commit_active = 0;
    s_publish_input_done = 0;
    s_publish_stop = 0;
    s_child_line_len = 0;
    s_child_line_overflow = 0;
    s_out_len = 0;
    s_exit_code = -1;
    s_shard_ok = s_shard_fail = s_shard_skipped = 0;
    s_shard_result_seen = 0;
    LeaveCriticalSection(&s_out_lock);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    /* Pin the WRITE cache + READ captures to the loader's canonical locations.
     * Set on the PARENT env so the child (CreateProcessA lpEnvironment=NULL =>
     * inherit) sees them; compile_overlays.py / coverage_vault.py prefer these
     * over any CLI --out-dir/--captures, so the cache can never drift from where
     * the loader reads. This is THE fix for the read/write-location divergence. */
    if (s_cache_dir[0]) SetEnvironmentVariableA("PSX_OVERLAY_CACHE_DIR", s_cache_dir);
    if (s_captures[0])  SetEnvironmentVariableA("PSX_OVERLAY_CAPTURES",  s_captures);
    SetEnvironmentVariableA("PSX_OVERLAY_LIVE_AUTOCOMPILE", "1");

    /* cmd.exe /C resolves the command via PATH and supports the relative
     * paths in the configured line (cwd = project root). The WHOLE command is
     * wrapped in one extra pair of quotes: when the line after /C BEGINS with
     * a quote and contains further quotes (a quoted interpreter path plus
     * quoted args), cmd.exe strips the first and last quote characters and
     * mangles the line ("The filename, directory name, or volume label syntax
     * is incorrect" — every autocompile run failed and the reshard silently
     * never happened). With the outer quotes cmd strips exactly those two and
     * executes the inner command verbatim. */
    char full[4200];
    snprintf(full, sizeof(full), "cmd.exe /C \"%s\"", s_cmd);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    /* Kill-on-close job for the whole compile tree. Created before the child
     * and assigned while the child is still SUSPENDED, so cmd.exe cannot spawn
     * python/gcc grandchildren outside the job. If job setup fails (rare),
     * proceed without it — the shutdown path still terminates the direct
     * child; only crash-orphan protection is lost. */
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &jeli, sizeof(jeli))) {
            CloseHandle(job);
            job = NULL;
        }
    }

    /* Compilation is opportunistic: it must never outrank the interpreter that
     * keeps the current frame alive. cmd -> python -> gcc inherit below-normal
     * priority, while the live compile script also defaults to one worker. */
    BOOL ok = CreateProcessA(NULL, full, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS |
                                 CREATE_SUSPENDED,
                             NULL, s_cwd[0] ? s_cwd : NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        if (job) CloseHandle(job);
        CloseHandle(rd);
        s_fails++;
        return 0;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = NULL;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    WatchCtx *ctx = (WatchCtx *)HeapAlloc(GetProcessHeap(), 0, sizeof(*ctx));
    if (!ctx) {
        /* The child is already running. Leaving it detached would permit the
         * state to remain IDLE and a second writer to enter the same cache. */
        TerminateProcess(pi.hProcess, ERROR_NOT_ENOUGH_MEMORY);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);   /* reaps any grandchild */
        s_fails++;
        return 0;
    }
    ctx->read_pipe = rd;
    ctx->proc      = pi.hProcess;
    s_proc = pi.hProcess;
    s_job  = job;
    ac_state_store(AC_RUNNING);
    s_runs++;
    s_prepare_thread = CreateThread(NULL, 0, publish_prepare_thread_main,
                                    NULL, 0, NULL);
    if (!s_prepare_thread) {
        TerminateProcess(pi.hProcess, ERROR_NOT_ENOUGH_MEMORY);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        HeapFree(GetProcessHeap(), 0, ctx);
        s_proc = NULL;
        if (s_job) { CloseHandle(s_job); s_job = NULL; }
        ac_state_store(AC_IDLE);
        s_fails++;
        return 0;
    }
    s_watch_thread = CreateThread(NULL, 0, watch_thread, ctx, 0, NULL);
    if (!s_watch_thread) {
        /* Without the watcher nobody drains stdout, observes completion, or
         * closes the process handle. Cancel the just-created child fail-closed. */
        TerminateProcess(pi.hProcess, ERROR_NOT_ENOUGH_MEMORY);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        HeapFree(GetProcessHeap(), 0, ctx);
        s_proc = NULL;
        EnterCriticalSection(&s_out_lock);
        s_publish_stop = 1;
        WakeAllConditionVariable(&s_publish_cv);
        LeaveCriticalSection(&s_out_lock);
        WaitForSingleObject(s_prepare_thread, INFINITE);
        CloseHandle(s_prepare_thread);
        s_prepare_thread = NULL;
        if (s_job) { CloseHandle(s_job); s_job = NULL; }
        ac_state_store(AC_IDLE);
        s_fails++;
        return 0;
    }
    return 1;
#else
    return 0;  /* non-Windows hosts: manual compile flow only */
#endif
}

/* Scan the child-output tail ring for the LAST "PSX_SHARD_RESULT ok=N failed=M
 * skipped=K" line and record the counts. Returns 1 if a result line was found.
 * Called from autocompile_poll_main (emu thread) after the child exits. */
static int parse_shard_result(void) {
#ifdef _WIN32
    char buf[AC_OUT_CAP + 1];
    int n = 0;
    if (s_out_lock_init) {
        EnterCriticalSection(&s_out_lock);
        n = s_out_len;
        memcpy(buf, s_out, (size_t)n);
        LeaveCriticalSection(&s_out_lock);
    }
    buf[n] = '\0';
    /* Find the LAST occurrence (a run may print more than once over its life). */
    const char *marker = "PSX_SHARD_RESULT";
    const char *hit = NULL, *p = buf;
    for (;;) {
        const char *q = strstr(p, marker);
        if (!q) break;
        hit = q;
        p = q + 1;
    }
    if (!hit) return 0;
    unsigned ok = 0, failed = 0, skipped = 0;
    const char *ln_end = strchr(hit, '\n');
    size_t span = ln_end ? (size_t)(ln_end - hit) : strlen(hit);
    char line[256];
    size_t cp = span < sizeof(line) - 1 ? span : sizeof(line) - 1;
    memcpy(line, hit, cp);
    line[cp] = '\0';
    int consumed = 0;
    if (sscanf(line, "PSX_SHARD_RESULT ok=%u failed=%u skipped=%u %n",
               &ok, &failed, &skipped, &consumed) != 3)
        return 0;
    for (const char *tail = line + consumed; *tail; tail++) {
        if (*tail != ' ' && *tail != '\t' && *tail != '\r') return 0;
    }
    s_shard_ok      = ok;
    s_shard_fail    = failed;
    s_shard_skipped = skipped;
    return 1;
#else
    return 0;
#endif
}

void autocompile_poll_main(void) {
    /* Drain at most one atomic publication per frame. The producer only sends
     * mapped-but-uninitialized images; callback wiring, validation, and
     * candidate registration remain single-threaded here. */
#ifdef _WIN32
    /* Titles without overlay autocompile never call autocompile_configure(),
     * so their publication lock does not exist. Poll is still called by the
     * shared maintenance path; keep that unconfigured path a strict no-op. */
    if (!s_out_lock_init) return;
    PublishItem *published = publish_ready_pop();
    if (published) {
        if (overlay_loader_commit_published(published->image) <= 0)
            publish_note_load_failure();
        published->image = NULL; /* commit consumed it on every path */
        free(published);
        publish_commit_finished();
        s_rescans++;
    }
    if (ac_state_load() == AC_RUNNING) return;
#endif
    if (ac_state_load() != AC_DONE) return;
#ifdef _WIN32
    if (publish_pending()) return;
    /* DONE is published immediately before the preparer returns. Join both
     * per-run workers before exposing IDLE, so a new run cannot inherit a live
     * consumer of the shared FIFO. Neither worker needs the emulation thread. */
    if (s_prepare_thread) {
        WaitForSingleObject(s_prepare_thread, INFINITE);
        CloseHandle(s_prepare_thread);
        s_prepare_thread = NULL;
    }
    if (s_watch_thread) {
        WaitForSingleObject(s_watch_thread, INFINITE);
        CloseHandle(s_watch_thread);
        s_watch_thread = NULL;
    }
    if (s_job) {
        /* Child already exited (watcher joined); closing the kill-on-close
         * job only reaps any grandchild the tree left behind. */
        CloseHandle(s_job);
        s_job = NULL;
    }
#endif
    /* Streaming parsing survives arbitrarily large chained post-processing
     * output.  Retain the tail scan as a compatibility fallback for output
     * injected by older/test paths that bypass the line parser. */
    if (!s_shard_result_seen)
        s_shard_result_seen = parse_shard_result();
    if (s_shard_result_seen && s_shard_fail)
        s_shard_fail_total += s_shard_fail;
    ac_state_store(AC_IDLE);
    /* Direct markers minimize handoff latency, then one idempotent batch-end
     * rescan makes every successfully published artifact visible to additive
     * cache queries and the lazy manifest index. This is intentionally once per
     * compiler run, never once per DLL or interpreted instruction. */
    overlay_loader_rescan();
    s_rescans++;
    /* A run "failed" if the process exited non-zero OR it reported shard
     * failures. compile_overlays.py exits 2 when any shard that should have
     * built failed, so these usually agree; the parsed count is the detail. */
    if (s_exit_code != 0 || !s_shard_result_seen || s_shard_fail > 0 ||
        s_publish_drops_run != 0 || s_publish_load_fail_run != 0 ||
        s_publish_parse_fail_run != 0)
        s_fails++;
}

void autocompile_shutdown(void) {
#ifdef _WIN32
    if (!s_out_lock_init) return;   /* never configured — nothing to stop */
    /* Halt the pipeline first so the preparer can never START another
     * LoadLibrary; one already in flight is waited out below (terminating a
     * thread that holds the Windows loader lock can deadlock ExitProcess). */
    HANDLE proc_dup = NULL;
    EnterCriticalSection(&s_out_lock);
    s_publish_stop = 1;
    WakeAllConditionVariable(&s_publish_cv);
    /* s_proc is cleared+closed by the watcher on child exit; duplicate under
     * the lock so the kill below can never race that close onto a reused
     * handle value. */
    if (s_proc)
        DuplicateHandle(GetCurrentProcess(), s_proc, GetCurrentProcess(),
                        &proc_dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    LeaveCriticalSection(&s_out_lock);
    /* Kill the compile tree; the dying pipe write end unblocks the watcher. */
    if (s_job) TerminateJobObject(s_job, 1);
    else if (proc_dup) TerminateProcess(proc_dup, 1);
    if (proc_dup) CloseHandle(proc_dup);
    if (s_prepare_thread) {
        WaitForSingleObject(s_prepare_thread, INFINITE);
        CloseHandle(s_prepare_thread);
        s_prepare_thread = NULL;
    }
    if (s_watch_thread) {
        WaitForSingleObject(s_watch_thread, INFINITE);
        CloseHandle(s_watch_thread);
        s_watch_thread = NULL;
    }
    if (s_job) {
        CloseHandle(s_job);
        s_job = NULL;
    }
    /* Both workers are joined and this is the emulation thread, so nothing is
     * preparing or committing: the discard cannot be refused. */
    (void)publish_discard_all();
    ac_state_store(AC_IDLE);
#endif
}

/* Minimal JSON string escaper for the output tail. */
static int json_escape_into(char *dst, int cap, const char *src, int n) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 8; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r') { /* drop */ }
        else if (c == '\t') { dst[o++] = '\\'; dst[o++] = 't'; }
        else if (c < 0x20)  { o += snprintf(dst + o, cap - o, "\\u%04x", c); }
        /* Bytes >= 0x80 are cp1252 console output from the child compile
         * (e.g. 0x97 em-dash) — raw they make the JSON invalid UTF-8 and
         * break strict clients. Escape as \u00XX (Latin-1 view). */
        else if (c >= 0x80) { o += snprintf(dst + o, cap - o, "\\u%04x", c); }
        else dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return o;
}

int autocompile_status_json(char *out, int cap) {
    static const char *names[] = { "idle", "running", "done" };
    char tail[2048];
    int  tn = 0;
    unsigned publish_ready = 0, publish_ready_highwater = 0;
    unsigned publish_preparing = 0, publish_prepare_count = 0;
    unsigned publish_prepare_fail = 0;
    unsigned publish_prepare_retry = 0, publish_prepare_giveup = 0;
    uint64_t publish_prepare_total_us = 0, publish_prepare_max_us = 0;
    uint64_t publish_prepare_last_us = 0;
    tail[0] = '\0';
#ifdef _WIN32
    if (s_out_lock_init) {
        EnterCriticalSection(&s_out_lock);
        int take = s_out_len < 900 ? s_out_len : 900;   /* newest tail */
        tn = json_escape_into(tail, sizeof(tail),
                              s_out + (s_out_len - take), take);
        publish_ready = s_publish_ready_count;
        publish_ready_highwater = s_publish_ready_highwater;
        publish_preparing = s_publish_preparing;
        publish_prepare_count = s_publish_prepare_count;
        publish_prepare_fail = s_publish_prepare_fail;
        publish_prepare_retry = s_publish_prepare_retry;
        publish_prepare_giveup = s_publish_prepare_giveup;
        publish_prepare_total_us = s_publish_prepare_total_us;
        publish_prepare_max_us = s_publish_prepare_max_us;
        publish_prepare_last_us = s_publish_prepare_last_us;
        LeaveCriticalSection(&s_out_lock);
    }
#endif
    (void)tn;
    return snprintf(out, cap,
        "{\"configured\":%d,\"state\":\"%s\",\"runs\":%u,\"fails\":%u,"
        "\"rescans\":%u,\"last_exit\":%d,"
        "\"shard_ok\":%u,\"shard_fail\":%u,\"shard_skipped\":%u,"
        "\"shard_fail_total\":%u,\"shard_result_seen\":%d,"
        "\"publish_ready\":%u,\"publish_ready_highwater\":%u,"
        "\"publish_preparing\":%u,\"publish_prepare_count\":%u,"
        "\"publish_prepare_fail\":%u,"
        "\"publish_prepare_retry\":%u,\"publish_prepare_giveup\":%u,"
        "\"publish_prepare_total_us\":%llu,"
        "\"publish_prepare_max_us\":%llu,"
        "\"publish_prepare_last_us\":%llu,"
        "\"output_tail\":\"%s\"}",
        autocompile_configured(), names[ac_state_load() & 3], s_runs, s_fails,
        s_rescans, s_exit_code,
        s_shard_ok, s_shard_fail, s_shard_skipped,
        s_shard_fail_total, s_shard_result_seen,
        publish_ready, publish_ready_highwater, publish_preparing,
        publish_prepare_count, publish_prepare_fail,
        publish_prepare_retry, publish_prepare_giveup,
        (unsigned long long)publish_prepare_total_us,
        (unsigned long long)publish_prepare_max_us,
        (unsigned long long)publish_prepare_last_us, tail);
}
