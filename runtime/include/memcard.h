#ifndef PSXRECOMP_MEMCARD_H
#define PSXRECOMP_MEMCARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMCARD_SIZE         (128 * 1024)   /* 128KB per card */
#define MEMCARD_SECTOR_SIZE  128
#define MEMCARD_SECTORS      1024

/* Initialize memcard subsystem. dir = directory for .mcd files (can be NULL).
 * Loads (or formats+creates) card1.mcd / card2.mcd in `dir` for both slots. */
void memcard_init(const char* dir);

/* Per-slot configuration for memcard_init_slots(). */
typedef struct {
    const char* path;     /* explicit .mcd path; NULL => derive <dir>/card<N>.mcd */
    int         enabled;  /* 0 => slot is empty (no card inserted, never present) */
} MemcardSlotConfig;

/* Initialize memcard subsystem with per-slot paths + enable flags. A disabled
 * slot is left absent (memcard_is_present() returns 0, SIO reports no device).
 * An enabled slot loads its file, or formats+creates it if missing. `dir` is
 * only used to derive default paths for slots whose path is NULL. */
void memcard_init_slots(const char* dir, const MemcardSlotConfig slots[2]);

/* Stateless on-disk introspection (used by the launcher before boot). Parses a
 * .mcd file's directory frames without touching the live card array. */
typedef struct {
    int       exists;        /* file present and readable */
    int       valid;         /* 128KB and "MC" magic in frame 0 */
    int       used_blocks;   /* directory frames in use (0..15) */
    int       total_blocks;  /* always 15 */
    uint8_t   block_used[15];/* per-block: 1 = occupied, 0 = free */
    long long mtime;         /* file mtime (unix seconds); 0 if unknown */
    long long size_bytes;    /* on-disk file size */
} MemcardSummary;

/* Parse `path` into `out`. Returns 0 always (out->exists/valid report failure).
 * Returns -1 only for a NULL argument. */
int memcard_summary_path(const char* path, MemcardSummary* out);

/* Write a freshly formatted (blank) 128KB card image to `path`, creating or
 * overwriting it. Returns 0 on success, -1 on I/O failure. */
int memcard_format_file(const char* path);

/* Read/write 128-byte sectors */
int memcard_read_sector(int card, int sector, uint8_t* buf);
int memcard_write_sector(int card, int sector, const uint8_t* buf);

/* Flush pending writes to disk */
void memcard_flush(int card);
void memcard_flush_all(void);

/* Check if card is present */
int memcard_is_present(int card);

/* Debug accessors: file path used for slot, magic at offset 0..1, total bytes loaded.
 * Returns 0 on success, -1 if slot index is out of range. */
int memcard_debug_info(int card, const char **path_out,
                       uint8_t magic_out[2], int *present_out,
                       int *dirty_out);

/* Copy raw bytes out of the in-memory card image. Used by the debug server
 * to verify that what the runtime loaded matches the on-disk file.
 * Returns the number of bytes copied (0 if slot empty / range invalid). */
int memcard_debug_read_buffer(int card, uint32_t offset, uint32_t len,
                              uint8_t *dst);

/* Export / import a full 128KB card image (present cards only for export).
 * import marks dirty and flushes to the bound filepath. Returns 0 on success. */
int memcard_export_raw(int card, uint8_t *dst);
int memcard_import_raw(int card, const uint8_t *src);

/* Redirect on-disk paths for both slots (e.g. guest netplay sandbox) without
 * reloading RAM. Subsequent flushes write to the new paths. Returns 0. */
int memcard_rebind_dir(const char *dir);

/* Restore explicit per-slot filepaths (from a prior snapshot). NULL path skips. */
int memcard_rebind_paths(const char *path0, const char *path1);

/* Reload in-memory images from the currently bound filepaths (0 = ok). */
int memcard_reload_bound(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_MEMCARD_H */
