/* autocompile.h — background overlay compile for variant-capture automation
 * (step 2.8). Spawns the configured compile command (compile_overlays.py
 * invocation from game.toml [runtime] overlay_autocompile_cmd) after an
 * automatic capture, collects its output into an in-memory ring (no log
 * files — CLAUDE.md §3; read it via the autocompile_status TCP command),
 * and on success has the emu thread rescan the overlay-DLL cache so the new
 * variant goes native in-session, without a restart. */
#ifndef PSXRECOMP_AUTOCOMPILE_H
#define PSXRECOMP_AUTOCOMPILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Store the command line + working directory. Empty cmd disables. */
void autocompile_configure(const char *cmd, const char *cwd);
int  autocompile_configured(void);

/* Pin the canonical overlay cache dir + captures file the LOADER actually uses
 * (<exe>/cache and <exe>/overlay_captures.json — computed once in main.cpp).
 * autocompile injects these into every spawned compile via the environment
 * (PSX_OVERLAY_CACHE_DIR / PSX_OVERLAY_CAPTURES), so the WRITE cache and the
 * READ captures can never diverge from where the loader reads — for every game,
 * dev or prod, gcc or tcc. compile_overlays.py / coverage_vault.py honor these
 * over any CLI --out-dir/--captures. This is the single source of truth for the
 * cache location: no per-game config, no drift. */
void autocompile_set_cache_paths(const char *cache_dir, const char *captures);

/* Probe whether a C compiler is actually reachable on PATH (gcc/cc/clang) — the
 * REAL "developer machine" signal, distinct from autocompile_configured() which
 * only reports that a command STRING is set (the shipped game.toml always sets
 * one). overlay_backend_resolve uses this so `auto` picks gcc only when the
 * toolchain can really build a shard, else tcc (toolchain-less production).
 * Memoized; safe to call repeatedly. */
int  autocompile_toolchain_available(void);

/* 1 while a compile process is running. */
int  autocompile_busy(void);

/* Kick a compile if configured and idle. Returns 1 if started. */
int  autocompile_request(void);

/* Emu-thread tick: applies a finished compile (cache rescan on success).
 * Must be called from the same thread that owns the overlay loader. */
void autocompile_poll_main(void);

/* Process-shutdown teardown, emu thread only: stops the publication pipeline,
 * kills the compiler process tree (job object), and JOINS the watcher and
 * preparer threads — waiting out any in-flight LoadLibrary rather than
 * abandoning a thread inside the Windows loader lock (which can deadlock
 * ExitProcess). Safe to call in any state, including mid-run; idempotent. */
void autocompile_shutdown(void);

/* JSON status blob for the debug server: state, run/fail counters, last
 * exit code, and the output tail. Returns bytes written. */
int  autocompile_status_json(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_AUTOCOMPILE_H */
