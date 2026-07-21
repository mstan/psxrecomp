#include "overlay_posix.h"

#include <stdio.h>
#include <string.h>

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex8(const char *text, uint32_t *value) {
    uint32_t parsed = 0;
    for (int i = 0; i < 8; i++) {
        int nibble = hex_nibble(text[i]);
        if (nibble < 0) return 0;
        parsed = (parsed << 4) | (uint32_t)nibble;
    }
    if (value) *value = parsed;
    return 1;
}

int psx_overlay_cache_name_parse(const char *name, uint32_t *region_start,
                                 uint32_t *content_crc) {
    uint32_t addr = 0, crc = 0, artifact = 0;
#ifdef _WIN32
    static const char extension[] = ".dll";
#else
    static const char extension[] = ".so";
#endif
    if (!name) return 0;
    size_t name_len = strlen(name);
    size_t ext_len = sizeof(extension) - 1u;
    if (name_len < ext_len) return 0;
    size_t stem_len = name_len - ext_len;
    if (
        (stem_len != 17u && stem_len != 26u) || name[8] != '_' ||
        strcmp(name + stem_len, extension) != 0 ||
        !parse_hex8(name, &addr) || !parse_hex8(name + 9, &crc) ||
        (stem_len == 26u &&
         (name[17] != '_' || !parse_hex8(name + 18, &artifact))))
        return 0;
    if (region_start) *region_start = addr;
    if (content_crc) *content_crc = crc;
    (void)artifact;
    return 1;
}

#ifndef _WIN32

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

int psx_overlay_posix_scan_cache_dir(const char *dir,
                                     PsxOverlayCacheVisitor visitor,
                                     void *opaque) {
    DIR *dp = opendir(dir);
    if (!dp) return 0;

    int visited = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        PsxOverlayCacheFile file;
        memset(&file, 0, sizeof(file));
        if (!psx_overlay_cache_name_parse(de->d_name, &file.region_start,
                                          &file.content_crc))
            continue;
        int n = snprintf(file.path, sizeof(file.path), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(file.path)) continue;
        memcpy(file.name, de->d_name, sizeof(file.name) - 1);
        file.name[sizeof(file.name) - 1] = '\0';

        struct stat st;
        if (stat(file.path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        file.mtime = (uint64_t)st.st_mtime;
        visited++;
        if (visitor && visitor(&file, opaque)) break;
    }
    closedir(dp);
    return visited;
}

static int note_first_cache_file(const PsxOverlayCacheFile *file, void *opaque) {
    (void)file;
    *(int *)opaque = 1;
    return 1;
}

int psx_overlay_posix_find_other_cache_tag(const char *base_dir,
                                           const char *expected_tag,
                                           char *found_tag,
                                           size_t found_tag_size) {
    DIR *dp = opendir(base_dir);
    if (!dp) return 0;

    int found = 0;
    struct dirent *de;
    while (!found && (de = readdir(dp)) != NULL) {
        if (strncmp(de->d_name, "cg", 2) != 0 ||
            strcmp(de->d_name, expected_tag) == 0)
            continue;
        char candidate[768];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", base_dir,
                         de->d_name);
        if (n < 0 || (size_t)n >= sizeof(candidate)) continue;
        int has_cache_file = 0;
        psx_overlay_posix_scan_cache_dir(candidate, note_first_cache_file,
                                         &has_cache_file);
        if (!has_cache_file) continue;
        if (found_tag && found_tag_size)
            snprintf(found_tag, found_tag_size, "%s", de->d_name);
        found = 1;
    }
    closedir(dp);
    return found;
}

void *psx_overlay_posix_library_open(const char *path, char *error,
                                     size_t error_size) {
    dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle && error && error_size) {
        const char *message = dlerror();
        snprintf(error, error_size, "%s", message ? message : "unknown dlopen error");
    }
    return handle;
}

void *psx_overlay_posix_library_symbol(void *handle, const char *name) {
    return handle ? dlsym(handle, name) : NULL;
}

void *psx_overlay_posix_library_entry(void *handle, uint32_t entry) {
    char name[32];
    snprintf(name, sizeof(name), "func_%08X", entry);
    return psx_overlay_posix_library_symbol(handle, name);
}

void psx_overlay_posix_library_close(void *handle) {
    if (handle) dlclose(handle);
}

#else

int psx_overlay_posix_scan_cache_dir(const char *dir,
                                     PsxOverlayCacheVisitor visitor,
                                     void *opaque) {
    (void)dir; (void)visitor; (void)opaque;
    return 0;
}
int psx_overlay_posix_find_other_cache_tag(const char *base_dir,
                                           const char *expected_tag,
                                           char *found_tag,
                                           size_t found_tag_size) {
    (void)base_dir; (void)expected_tag; (void)found_tag; (void)found_tag_size;
    return 0;
}
void *psx_overlay_posix_library_open(const char *path, char *error,
                                     size_t error_size) {
    (void)path; (void)error; (void)error_size;
    return NULL;
}
void *psx_overlay_posix_library_symbol(void *handle, const char *name) {
    (void)handle; (void)name;
    return NULL;
}
void *psx_overlay_posix_library_entry(void *handle, uint32_t entry) {
    (void)handle; (void)entry;
    return NULL;
}
void psx_overlay_posix_library_close(void *handle) { (void)handle; }

#endif
