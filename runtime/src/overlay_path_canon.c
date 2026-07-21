/* overlay_path_canon.c — see header. Windows resolves via
 * GetFinalPathNameByHandle (the loader's own view of the object); POSIX via
 * realpath. No allocation, no logging: callers decide how loud a rejection
 * is. */
#include "overlay_path_canon.h"

#include <string.h>

#ifdef _WIN32

int overlay_path_canonicalize_handle(HANDLE h, char *out, size_t cap) {
    if (h == NULL || h == INVALID_HANDLE_VALUE || !out || cap < 2) return 0;
    char buf[1024];
    DWORD n = GetFinalPathNameByHandleA(h, buf, (DWORD)sizeof(buf),
                                        FILE_NAME_NORMALIZED |
                                            VOLUME_NAME_DOS);
    if (n == 0 || n >= sizeof(buf)) return 0;
    /* Strip the \\?\ (or rewrite \\?\UNC\ to \\) prefix so the result
     * compares against ordinary Win32 paths. */
    const char *p = buf;
    char unc[1024];
    if (strncmp(buf, "\\\\?\\UNC\\", 8) == 0) {
        unc[0] = '\\';
        unc[1] = '\\';
        strncpy(unc + 2, buf + 8, sizeof(unc) - 3);
        unc[sizeof(unc) - 1] = '\0';
        p = unc;
    } else if (strncmp(buf, "\\\\?\\", 4) == 0) {
        p = buf + 4;
    }
    size_t len = strlen(p);
    if (len + 1 > cap) return 0;
    memcpy(out, p, len + 1);
    return 1;
}

int overlay_path_canonicalize(const char *path, char *out, size_t cap) {
    if (!path || !path[0]) return 0;
    /* dwDesiredAccess=0 opens for attribute access only (works even on
     * read-locked files); BACKUP_SEMANTICS is required to open directories. */
    HANDLE h = CreateFileA(path, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = overlay_path_canonicalize_handle(h, out, cap);
    CloseHandle(h);
    return ok;
}

#else /* POSIX */

#include <limits.h>
#include <stdlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int overlay_path_canonicalize(const char *path, char *out, size_t cap) {
    if (!path || !path[0] || !out || cap < 2) return 0;
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return 0;
    size_t len = strlen(resolved);
    if (len + 1 > cap) return 0;
    memcpy(out, resolved, len + 1);
    return 1;
}

#endif
