#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parsed legacy <addr8>_<logical8> or immutable
 * <addr8>_<logical8>_<artifact8>.{dll,so} cache filename. The parser is shared by the
 * Windows and POSIX directory walkers so both platforms reject partial hex
 * fields and lookalike suffixes identically. */
typedef struct PsxOverlayCacheFile {
    char name[40];
    char path[768];
    uint32_t region_start;
    uint32_t content_crc;
    uint64_t mtime;
} PsxOverlayCacheFile;

typedef int (*PsxOverlayCacheVisitor)(const PsxOverlayCacheFile *file,
                                      void *opaque);

int psx_overlay_cache_name_parse(const char *name, uint32_t *region_start,
                                 uint32_t *content_crc);

/* POSIX helpers used by overlay_loader.c and its focused regression test.
 * The Windows build provides inert stubs; it keeps using Win32 enumeration and
 * LoadLibrary while still sharing the strict filename parser above. */
int psx_overlay_posix_scan_cache_dir(const char *dir,
                                     PsxOverlayCacheVisitor visitor,
                                     void *opaque);
int psx_overlay_posix_find_other_cache_tag(const char *base_dir,
                                           const char *expected_tag,
                                           char *found_tag,
                                           size_t found_tag_size);
void *psx_overlay_posix_library_open(const char *path, char *error,
                                     size_t error_size);
void *psx_overlay_posix_library_symbol(void *handle, const char *name);
void *psx_overlay_posix_library_entry(void *handle, uint32_t entry);
void psx_overlay_posix_library_close(void *handle);

#ifdef __cplusplus
}
#endif
