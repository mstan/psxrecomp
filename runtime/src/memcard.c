/*
 * memcard.c -- PS1 memory card image I/O
 *
 * 128KB per card, organized as 1024 sectors of 128 bytes each.
 * Stored as raw .mcd files on disk.
 *
 * Pure hardware simulation. No BIOS state, no HLE, no stubs.
 *
 * Ported from v3 with audit:
 *   - Removed all fprintf (CLAUDE.md rule #3)
 *   - No BIOS manipulation found (clean)
 *   - No fake events found (clean)
 */

#include "memcard.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define MAX_CARDS 2

typedef struct {
    uint8_t data[MEMCARD_SIZE];
    char filepath[512];
    int present;
    int dirty;
} MemCard;

static MemCard cards[MAX_CARDS];

/* XOR checksum of bytes 0x00-0x7E, stored at 0x7F. */
static uint8_t frame_checksum(const uint8_t *frame) {
    uint8_t xor_val = 0;
    for (int i = 0; i < 127; i++)
        xor_val ^= frame[i];
    return xor_val;
}

/* Format a 128KB card image to match a real PS1 blank card.
 * Layout verified against DuckStation's MemoryCardImage::Format(). */
static void memcard_format(uint8_t *data) {
    memset(data, 0xFF, MEMCARD_SIZE);

    /* Frame 0: Header ("MC" magic) */
    memset(&data[0], 0x00, 128);
    data[0] = 'M';
    data[1] = 'C';
    data[0x7F] = frame_checksum(&data[0]);

    /* Frames 1-15: Directory entries (all free) */
    for (int s = 1; s <= 15; s++) {
        int off = s * 128;
        memset(&data[off], 0x00, 128);
        data[off + 0] = 0xA0;  /* status: free/available */
        data[off + 8] = 0xFF;  /* next block pointer = none */
        data[off + 9] = 0xFF;
        data[off + 0x7F] = frame_checksum(&data[off]);
    }

    /* Frames 16-35: Broken sector list (no broken sectors) */
    for (int s = 16; s <= 35; s++) {
        int off = s * 128;
        memset(&data[off], 0x00, 128);
        data[off + 0] = 0xFF;
        data[off + 1] = 0xFF;
        data[off + 2] = 0xFF;
        data[off + 3] = 0xFF;
        data[off + 8] = 0xFF;
        data[off + 9] = 0xFF;
        data[off + 0x7F] = frame_checksum(&data[off]);
    }

    /* Frames 36-62: Broken sector replacement data + unused */
    for (int s = 36; s <= 62; s++) {
        memset(&data[s * 128], 0x00, 128);
    }

    /* Frame 63: Write test frame (copy of frame 0) */
    memcpy(&data[63 * 128], &data[0], 128);
}

static void memcard_ensure_dir(const char* dir) {
    if (!dir || !dir[0]) return;
#ifdef _WIN32
    (void)_mkdir(dir);
#else
    (void)mkdir(dir, 0755);
#endif
}

void memcard_init_slots(const char* dir, const MemcardSlotConfig slots[2]) {
    memset(cards, 0, sizeof(cards));
    memcard_ensure_dir(dir);

    for (int i = 0; i < MAX_CARDS; i++) {
        cards[i].present = 0;
        cards[i].dirty = 0;

        const int enabled = slots ? slots[i].enabled : 1;
        const char* path  = slots ? slots[i].path : NULL;
        if (!enabled) continue;  /* slot empty: no card inserted */

        if (path && path[0]) {
            snprintf(cards[i].filepath, sizeof(cards[i].filepath), "%s", path);
        } else if (dir) {
            snprintf(cards[i].filepath, sizeof(cards[i].filepath),
                     "%s/card%d.mcd", dir, i + 1);
        } else {
            continue;  /* no path and no dir: cannot resolve a file */
        }

        FILE* f = fopen(cards[i].filepath, "rb");
        if (f) {
            size_t n = fread(cards[i].data, 1, MEMCARD_SIZE, f);
            fclose(f);
            if (n == MEMCARD_SIZE) {
                cards[i].present = 1;
            }
        } else {
            memcard_format(cards[i].data);
            f = fopen(cards[i].filepath, "wb");
            if (f) {
                size_t n = fwrite(cards[i].data, 1, MEMCARD_SIZE, f);
                int flush_ok = (fflush(f) == 0);
                int close_ok = (fclose(f) == 0);
                if (n == MEMCARD_SIZE && flush_ok && close_ok) {
                    cards[i].present = 1;
                }
            }
        }
    }
}

void memcard_init(const char* dir) {
    const MemcardSlotConfig both[2] = { { NULL, 1 }, { NULL, 1 } };
    memcard_init_slots(dir, both);
}

int memcard_format_file(const char* path) {
    if (!path || !path[0]) return -1;
    uint8_t* data = (uint8_t*)malloc(MEMCARD_SIZE);
    if (!data) return -1;
    memcard_format(data);
    FILE* f = fopen(path, "wb");
    int ok = 0;
    if (f) {
        size_t n = fwrite(data, 1, MEMCARD_SIZE, f);
        int flush_ok = (fflush(f) == 0);
        int close_ok = (fclose(f) == 0);
        ok = (n == MEMCARD_SIZE && flush_ok && close_ok);
    }
    free(data);
    return ok ? 0 : -1;
}

int memcard_summary_path(const char* path, MemcardSummary* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->total_blocks = 15;
    if (!path || !path[0]) return 0;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    out->exists = 1;

    uint8_t* data = (uint8_t*)malloc(MEMCARD_SIZE);
    if (!data) { fclose(f); return 0; }
    size_t n = fread(data, 1, MEMCARD_SIZE, f);
    fclose(f);
    out->size_bytes = (long long)n;

    if (n == MEMCARD_SIZE && data[0] == 'M' && data[1] == 'C') {
        out->valid = 1;
        /* Directory frames 1..15: byte 0 is the block-allocation status.
         * 0xA0 = free; 0x51/0x52/0x53 = occupied (first/middle/last). */
        for (int s = 1; s <= 15; s++) {
            const uint8_t status = data[s * 128];
            const int used = ((status & 0xF0) == 0x50);
            out->block_used[s - 1] = (uint8_t)(used ? 1 : 0);
            if (used) out->used_blocks++;
        }
    }
    free(data);

#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) == 0) out->mtime = (long long)st.st_mtime;
#else
    struct stat st;
    if (stat(path, &st) == 0) out->mtime = (long long)st.st_mtime;
#endif
    return 0;
}

int memcard_read_sector(int card, int sector, uint8_t* buf) {
    if (card < 0 || card >= MAX_CARDS) return -1;
    if (!cards[card].present) return -1;
    if (sector < 0 || sector >= MEMCARD_SECTORS) return -1;

    memcpy(buf, cards[card].data + sector * MEMCARD_SECTOR_SIZE, MEMCARD_SECTOR_SIZE);
    return 0;
}

int memcard_write_sector(int card, int sector, const uint8_t* buf) {
    if (card < 0 || card >= MAX_CARDS) return -1;
    if (!cards[card].present) return -1;
    if (sector < 0 || sector >= MEMCARD_SECTORS) return -1;

    memcpy(cards[card].data + sector * MEMCARD_SECTOR_SIZE, buf, MEMCARD_SECTOR_SIZE);
    cards[card].dirty = 1;
    return 0;
}

void memcard_flush(int card) {
    if (card < 0 || card >= MAX_CARDS) return;
    if (!cards[card].dirty) return;
    if (cards[card].filepath[0] == '\0') return;

    FILE* f = fopen(cards[card].filepath, "wb");
    if (f) {
        size_t n = fwrite(cards[card].data, 1, MEMCARD_SIZE, f);
        int flush_ok = (fflush(f) == 0);
        int close_ok = (fclose(f) == 0);
        if (n == MEMCARD_SIZE && flush_ok && close_ok) {
            cards[card].dirty = 0;
        }
    }
}

void memcard_flush_all(void) {
    for (int i = 0; i < MAX_CARDS; i++) {
        memcard_flush(i);
    }
}

int memcard_is_present(int card) {
    if (card < 0 || card >= MAX_CARDS) return 0;
    return cards[card].present;
}

int memcard_debug_info(int card, const char **path_out,
                       uint8_t magic_out[2], int *present_out,
                       int *dirty_out) {
    if (card < 0 || card >= MAX_CARDS) return -1;
    if (path_out)    *path_out    = cards[card].filepath;
    if (magic_out)   { magic_out[0] = cards[card].data[0];
                       magic_out[1] = cards[card].data[1]; }
    if (present_out) *present_out = cards[card].present;
    if (dirty_out)   *dirty_out   = cards[card].dirty;
    return 0;
}

int memcard_debug_read_buffer(int card, uint32_t offset, uint32_t len,
                              uint8_t *dst) {
    if (card < 0 || card >= MAX_CARDS) return 0;
    if (!cards[card].present) return 0;
    if (!dst || len == 0) return 0;
    if (offset >= MEMCARD_SIZE) return 0;
    uint32_t avail = (uint32_t)MEMCARD_SIZE - offset;
    if (len > avail) len = avail;
    memcpy(dst, cards[card].data + offset, len);
    return (int)len;
}

int memcard_export_raw(int card, uint8_t *dst) {
    if (card < 0 || card >= MAX_CARDS || !dst) return -1;
    if (!cards[card].present) return -1;
    memcpy(dst, cards[card].data, MEMCARD_SIZE);
    return 0;
}

int memcard_import_raw(int card, const uint8_t *src) {
    if (card < 0 || card >= MAX_CARDS || !src) return -1;
    if (cards[card].filepath[0] == '\0') return -1;
    memcpy(cards[card].data, src, MEMCARD_SIZE);
    cards[card].present = 1;
    cards[card].dirty = 1;
    memcard_flush(card);
    return 0;
}

int memcard_rebind_dir(const char *dir) {
    int i;
    if (!dir || !dir[0]) return -1;
    memcard_ensure_dir(dir);
    for (i = 0; i < MAX_CARDS; i++) {
        if (!cards[i].present && cards[i].filepath[0] == '\0')
            continue;
        snprintf(cards[i].filepath, sizeof(cards[i].filepath),
                 "%s/card%d.mcd", dir, i + 1);
        /* Force a flush of current RAM into the sandbox path when dirty or
         * when establishing the guest mirror for the first time. */
        cards[i].dirty = 1;
        memcard_flush(i);
    }
    return 0;
}

int memcard_rebind_paths(const char *path0, const char *path1) {
    if (path0 && path0[0]) {
        snprintf(cards[0].filepath, sizeof(cards[0].filepath), "%s", path0);
    }
    if (path1 && path1[0]) {
        snprintf(cards[1].filepath, sizeof(cards[1].filepath), "%s", path1);
    }
    return 0;
}

int memcard_reload_bound(void) {
    int i;
    for (i = 0; i < MAX_CARDS; i++) {
        FILE *f;
        if (cards[i].filepath[0] == '\0') {
            cards[i].present = 0;
            cards[i].dirty = 0;
            continue;
        }
        f = fopen(cards[i].filepath, "rb");
        if (!f) {
            cards[i].present = 0;
            cards[i].dirty = 0;
            continue;
        }
        if (fread(cards[i].data, 1, MEMCARD_SIZE, f) == MEMCARD_SIZE) {
            cards[i].present = 1;
            cards[i].dirty = 0;
        } else {
            cards[i].present = 0;
            cards[i].dirty = 0;
        }
        fclose(f);
    }
    return 0;
}
