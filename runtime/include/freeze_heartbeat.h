/* freeze_heartbeat.h — observability infrastructure.
 *
 * Starts a background thread that snapshots runtime state to
 * `psx_freeze_heartbeat.json` every ~100 ms. Survives main-thread
 * freezes (Windows / SDL "Not Responding") because the writer thread
 * is independent of the main loop and the debug-server TCP path.
 *
 * Reader: any external process that reads the file. The file is
 * overwritten in place — no log growth, no CLAUDE.md §3 violation.
 *
 * Not a fix for anything. Pure observability so we can see what state
 * the runtime was in just before it stalled.
 */

#ifndef PSXRECOMP_V4_FREEZE_HEARTBEAT_H
#define PSXRECOMP_V4_FREEZE_HEARTBEAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the heartbeat thread. Idempotent — second call is a no-op.
 * `backend_label` is written into the heartbeat JSON as "backend"
 * (e.g. "psx-runtime" or "psx-beetle") so a reader can tell which
 * binary produced the file. */
void freeze_heartbeat_start(const char *backend_label);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_FREEZE_HEARTBEAT_H */
