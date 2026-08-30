#ifndef PSXRECOMP_FREEZE_ARTIFACTS_H
#define PSXRECOMP_FREEZE_ARTIFACTS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREEZE_ARTIFACT_DEFAULT_DIR "."
#define FREEZE_ARTIFACT_DEFAULT_KEEP 4u

/* Resolve PSX_FREEZE_DUMP_DIR and PSX_FREEZE_DUMP_KEEP. Invalid or unsafe
 * values fall back to the bounded defaults. */
void freeze_artifacts_config(char *dir, size_t dir_cap, unsigned *keep);

/* Create dir (including parents), sanitize backend into a filename token, and
 * return a path for one dump. Returns zero if the path cannot be represented. */
int freeze_artifacts_dump_path(char *out, size_t out_cap, const char *dir,
                               const char *backend, long long wall_clock);

/* Remove all but the newest keep regular freeze-dump files in dir. Files that
 * do not exactly match the runtime's dump-name grammar are never removed. */
int freeze_artifacts_prune(const char *dir, unsigned keep);

#ifdef __cplusplus
}
#endif

#endif
