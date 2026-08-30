#include "freeze_artifacts.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define FREEZE_SEP '\\'
#else
#include <dirent.h>
#include <unistd.h>
#define FREEZE_SEP '/'
#endif

#define FREEZE_NAME_PREFIX "psx_freeze_dump_"
#define FREEZE_NAME_SUFFIX ".json"
#define FREEZE_MAX_KEEP 64u

typedef struct FreezeArtifactEntry {
    char name[256];
    unsigned long long modified;
} FreezeArtifactEntry;

static int freeze_is_sep(char c) {
    return c == '/' || c == '\\';
}

static int freeze_name_matches(const char *name) {
    const size_t prefix_n = sizeof(FREEZE_NAME_PREFIX) - 1u;
    const size_t suffix_n = sizeof(FREEZE_NAME_SUFFIX) - 1u;
    size_t n;
    if (!name || strncmp(name, FREEZE_NAME_PREFIX, prefix_n) != 0) return 0;
    n = strlen(name);
    if (n <= prefix_n + suffix_n || n >= 256u) return 0;
    if (strcmp(name + n - suffix_n, FREEZE_NAME_SUFFIX) != 0) return 0;
    for (size_t i = prefix_n; i < n - suffix_n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int freeze_join(char *out, size_t cap, const char *dir, const char *name) {
    size_t n;
    int written;
    if (!out || !cap || !dir || !dir[0] || !name || !name[0]) return 0;
    n = strlen(dir);
    if (freeze_is_sep(dir[n - 1u]))
        written = snprintf(out, cap, "%s%s", dir, name);
    else
        written = snprintf(out, cap, "%s%c%s", dir, FREEZE_SEP, name);
    return written > 0 && (size_t)written < cap;
}

static int freeze_mkdir_one(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0 || errno == EEXIST) return 1;
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 1;
#endif
    return 0;
}

static int freeze_mkdirs(const char *dir) {
    char path[1024];
    size_t n;
    if (!dir || !dir[0]) return 0;
    n = strlen(dir);
    if (n >= sizeof(path)) return 0;
    memcpy(path, dir, n + 1u);
    for (size_t i = 0; i < n; ++i) {
        if (!freeze_is_sep(path[i])) continue;
        if (i == 0 || (i == 2 && path[1] == ':')) continue;
        path[i] = 0;
        if (path[0] && !freeze_mkdir_one(path)) return 0;
        path[i] = FREEZE_SEP;
    }
    return freeze_mkdir_one(path);
}

void freeze_artifacts_config(char *dir, size_t dir_cap, unsigned *keep) {
    const char *configured_dir = getenv("PSX_FREEZE_DUMP_DIR");
    const char *configured_keep = getenv("PSX_FREEZE_DUMP_KEEP");
    unsigned result_keep = FREEZE_ARTIFACT_DEFAULT_KEEP;
    const char *result_dir = FREEZE_ARTIFACT_DEFAULT_DIR;
    char *end = NULL;
    unsigned long parsed;

    if (configured_dir && configured_dir[0] && strlen(configured_dir) < 1024u)
        result_dir = configured_dir;
    if (configured_keep && configured_keep[0]) {
        errno = 0;
        parsed = strtoul(configured_keep, &end, 10);
        if (!errno && end && *end == 0 && parsed >= 1u && parsed <= FREEZE_MAX_KEEP)
            result_keep = (unsigned)parsed;
    }
    if (dir && dir_cap) {
        snprintf(dir, dir_cap, "%s", result_dir);
        dir[dir_cap - 1u] = 0;
    }
    if (keep) *keep = result_keep;
}

int freeze_artifacts_dump_path(char *out, size_t out_cap, const char *dir,
                               const char *backend, long long wall_clock) {
    char token[64];
    char name[128];
    size_t j = 0;
    int written;
    if (!backend || !backend[0]) backend = "runtime";
    for (size_t i = 0; backend[i] && j + 1u < sizeof(token); ++i) {
        unsigned char c = (unsigned char)backend[i];
        token[j++] = (char)((isalnum(c) || c == '_' || c == '-') ? c : '_');
    }
    token[j] = 0;
    written = snprintf(name, sizeof(name), FREEZE_NAME_PREFIX "%s_%lld" FREEZE_NAME_SUFFIX,
                       token, wall_clock);
    if (written <= 0 || (size_t)written >= sizeof(name) || !freeze_name_matches(name))
        return 0;
    if (!freeze_mkdirs(dir)) return 0;
    return freeze_join(out, out_cap, dir, name);
}

static int freeze_newest_first(const void *a, const void *b) {
    const FreezeArtifactEntry *ea = (const FreezeArtifactEntry *)a;
    const FreezeArtifactEntry *eb = (const FreezeArtifactEntry *)b;
    if (ea->modified > eb->modified) return -1;
    if (ea->modified < eb->modified) return 1;
    return -strcmp(ea->name, eb->name);
}

static int freeze_append_entry(FreezeArtifactEntry **entries, size_t *count,
                               size_t *cap, const char *name,
                               unsigned long long modified) {
    FreezeArtifactEntry *grown;
    if (*count == *cap) {
        size_t new_cap = *cap ? *cap * 2u : 16u;
        grown = (FreezeArtifactEntry *)realloc(*entries, new_cap * sizeof(**entries));
        if (!grown) return 0;
        *entries = grown;
        *cap = new_cap;
    }
    snprintf((*entries)[*count].name, sizeof((*entries)[*count].name), "%s", name);
    (*entries)[*count].modified = modified;
    ++*count;
    return 1;
}

static int freeze_remove_regular(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return 0;
    return DeleteFileA(path) != 0;
#else
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return unlink(path) == 0;
#endif
}

int freeze_artifacts_prune(const char *dir, unsigned keep) {
    FreezeArtifactEntry *entries = NULL;
    size_t count = 0, cap = 0;
    int removed = 0;
    if (!dir || !dir[0] || keep < 1u || keep > FREEZE_MAX_KEEP) return 0;

#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE find;
    char pattern[1024];
    if (!freeze_join(pattern, sizeof(pattern), dir, FREEZE_NAME_PREFIX "*" FREEZE_NAME_SUFFIX))
        return 0;
    find = FindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            ULARGE_INTEGER stamp;
            if (!freeze_name_matches(data.cFileName)) continue;
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
            stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
            if (!freeze_append_entry(&entries, &count, &cap, data.cFileName,
                                     stamp.QuadPart)) {
                FindClose(find); free(entries); return 0;
            }
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
#else
    DIR *stream = opendir(dir);
    struct dirent *item;
    if (!stream) return 0;
    while ((item = readdir(stream)) != NULL) {
        char full[1024];
        struct stat st;
        if (!freeze_name_matches(item->d_name)) continue;
        if (!freeze_join(full, sizeof(full), dir, item->d_name)) continue;
        if (lstat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!freeze_append_entry(&entries, &count, &cap, item->d_name,
                                 (unsigned long long)st.st_mtime)) {
            closedir(stream); free(entries); return 0;
        }
    }
    closedir(stream);
#endif

    qsort(entries, count, sizeof(*entries), freeze_newest_first);
    for (size_t i = keep; i < count; ++i) {
        char full[1024];
        /* Revalidate both the name and file type immediately before deletion. */
        if (!freeze_name_matches(entries[i].name)) continue;
        if (!freeze_join(full, sizeof(full), dir, entries[i].name)) continue;
        if (freeze_remove_regular(full)) ++removed;
    }
    free(entries);
    return removed;
}
