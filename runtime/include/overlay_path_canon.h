/* overlay_path_canon.h — filesystem-resolved path canonicalization for the
 * overlay cache's confinement boundary. A lexical prefix check can never be
 * made correct against `..` segments or reparse points (symlinks/junctions):
 * the only sound comparison resolves BOTH sides through the filesystem, the
 * same way LoadLibrary/dlopen will resolve them, and prefix-checks the
 * results. All functions fail CLOSED (0) on any resolution error. */
#ifndef PSXRECOMP_OVERLAY_PATH_CANON_H
#define PSXRECOMP_OVERLAY_PATH_CANON_H

#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve `path` (file or directory; must exist) to its final normalized
 * form — `..`, short names, and every reparse point resolved. Returns 1 and
 * NUL-terminates `out` on success; 0 on any failure (missing, inaccessible,
 * result longer than cap). */
int overlay_path_canonicalize(const char *path, char *out, size_t cap);

#ifdef _WIN32
/* Same resolution from an already-open handle. Lets a caller open the file
 * with restrictive sharing FIRST and validate the canonical identity of the
 * object it actually holds — closing the validate-then-swap (TOCTOU) window
 * before LoadLibrary. */
int overlay_path_canonicalize_handle(HANDLE h, char *out, size_t cap);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_OVERLAY_PATH_CANON_H */
