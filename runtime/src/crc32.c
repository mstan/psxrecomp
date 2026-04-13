/*
 * crc32.c — Simple CRC32 implementation (no external dependencies).
 * Uses the standard IEEE 802.3 polynomial 0xEDB88320.
 */
#include "crc32.h"
#include <stddef.h>

static uint32_t s_table[256];
static int      s_table_ready = 0;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_table[i] = c;
    }
    s_table_ready = 1;
}

uint32_t crc32_compute(const uint8_t *data, size_t len) {
    if (!s_table_ready) crc32_init_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = s_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
