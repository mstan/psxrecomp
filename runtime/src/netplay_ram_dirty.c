/*
 * netplay_ram_dirty.c -- page-dirty RAM digest cache (see the header).
 *
 * Modeled on the validated cosim page-hash cache (cosim_state.c): dirty
 * bitmap marked at the store chokepoint, per-page hash recompute on demand,
 * whole-cache invalidation for bulk writers. CRC32 instead of FNV so the
 * partition stays on the one canonical polynomial (crc32.c).
 */
#include "netplay_ram_dirty.h"
#include "crc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int      g_np_ram_dig_tracking = 0;
uint32_t g_np_ram_dig_dirty[NP_RAM_DIG_MAX_PAGES / 32u];

static uint32_t s_page_crc[NP_RAM_DIG_MAX_PAGES];
static uint32_t s_bytes;          /* RAM size the cache was built for */
static int      s_all_dirty = 1;
static uint32_t s_verify_every = 0xFFFFFFFFu;  /* unresolved sentinel */
static uint32_t s_calls;

void np_ram_dig_mark_all(void) {
    s_all_dirty = 1;
}

static uint32_t verify_every(void) {
    if (s_verify_every == 0xFFFFFFFFu) {
        const char *e = getenv("PSX_NP_RAM_DIG_VERIFY");
        s_verify_every = (e && e[0]) ? (uint32_t)strtoul(e, NULL, 0) : 1024u;
    }
    return s_verify_every;
}

uint32_t np_ram_dig_crc_raw(const uint8_t *ram, uint32_t bytes) {
    uint32_t pages, pg, fold;
    const uint32_t page = 1u << NP_RAM_DIG_PAGE_SHIFT;

    if (!ram || bytes == 0u || bytes > NP_RAM_DIG_MAX_BYTES) {
        /* Out-of-model size: hash linearly, keep the cache out of it. */
        return crc32_update(0xFFFFFFFFu, ram, bytes);
    }
    if (!g_np_ram_dig_tracking) {
        /* Self-arm: from here on every store marks its page. Everything is
         * dirty for this first pass, which equals a full hash. */
        g_np_ram_dig_tracking = 1;
        s_all_dirty = 1;
    }
    if (bytes != s_bytes) {
        s_bytes = bytes;
        s_all_dirty = 1;
    }
    pages = (bytes + page - 1u) >> NP_RAM_DIG_PAGE_SHIFT;

    for (pg = 0; pg < pages; pg++) {
        if (!s_all_dirty &&
            !(g_np_ram_dig_dirty[pg >> 5u] & (1u << (pg & 31u))))
            continue;
        {
            uint32_t off = pg << NP_RAM_DIG_PAGE_SHIFT;
            uint32_t len = bytes - off < page ? bytes - off : page;
            s_page_crc[pg] = crc32_compute(ram + off, len);
        }
    }
    memset(g_np_ram_dig_dirty, 0,
           ((pages + 31u) / 32u) * sizeof(g_np_ram_dig_dirty[0]));
    s_all_dirty = 0;

    /* Periodic staleness audit: a writer path missing its mark corrupts the
     * digest silently (worse: deterministically, so peers may still agree).
     * Re-hash everything at low cadence and scream + heal on drift. */
    if (verify_every() && ++s_calls >= verify_every()) {
        uint32_t bad = 0, first_bad = 0;
        s_calls = 0;
        for (pg = 0; pg < pages; pg++) {
            uint32_t off = pg << NP_RAM_DIG_PAGE_SHIFT;
            uint32_t len = bytes - off < page ? bytes - off : page;
            uint32_t fresh = crc32_compute(ram + off, len);
            if (fresh != s_page_crc[pg]) {
                if (!bad) first_bad = pg;
                bad++;
                s_page_crc[pg] = fresh;
            }
        }
        if (bad) {
            fprintf(stderr,
                    "psxrecomp: np_ram_dig STALE PAGES — %u page(s), first pg=%u "
                    "(a RAM writer path is missing np_ram_dig_note_write; "
                    "healed this pass)\n",
                    (unsigned)bad, (unsigned)first_bad);
            fflush(stderr);
        }
    }

    fold = crc32_update(0xFFFFFFFFu, (const uint8_t *)s_page_crc,
                        pages * (uint32_t)sizeof(s_page_crc[0]));
    return fold;
}
