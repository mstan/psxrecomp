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

void memcard_init(const char* dir) {
    memset(cards, 0, sizeof(cards));

    for (int i = 0; i < MAX_CARDS; i++) {
        cards[i].present = 0;
        cards[i].dirty = 0;

        if (dir) {
            snprintf(cards[i].filepath, sizeof(cards[i].filepath),
                     "%s/card%d.mcd", dir, i + 1);

            FILE* f = fopen(cards[i].filepath, "rb");
            if (f) {
                size_t n = fread(cards[i].data, 1, MEMCARD_SIZE, f);
                fclose(f);
                if (n == MEMCARD_SIZE) {
                    cards[i].present = 1;
                }
            }
            /* No card file = no card in slot. */
        }
    }
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
        fwrite(cards[card].data, 1, MEMCARD_SIZE, f);
        fclose(f);
        cards[card].dirty = 0;
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
