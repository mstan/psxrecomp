/*
 * netplay_ram_dirty.h -- page-dirty tracking for the per-tick RAM digest.
 *
 * netplay_core_digest_parts used to CRC the full guest RAM (2 MiB, 8 MiB with
 * the RAM mod) EVERY tick in BOTH the client and the link follower — ~0.8 ms
 * a tick of pure hashing per process against a 20 ms PAL budget. This module
 * keeps a per-4KiB-page CRC cache: stores mark their page dirty (one test +
 * one OR on the already-chokepointed store path, see memory.c), the digest
 * re-hashes only dirty pages and folds the page-CRC array.
 *
 * The RAM partition digest value therefore CHANGES (fold-of-page-CRCs instead
 * of one linear CRC). That is safe: nothing persists it and it is only ever
 * compared between same-build peers (see crc32.c's contract note and the
 * netplay_state_digest.c consumers).
 *
 * Tracking self-arms on the first digest call (marks everything dirty), so
 * offline play never pays the per-store cost. Bulk writers that bypass the
 * store chokepoint (savestate/rollback RAM restore, overlay shadow restore,
 * RAM bank switch) must call np_ram_dig_mark_all(); see the call sites.
 *
 * A periodic self-check re-hashes every page and screams if a cached CRC is
 * stale (i.e. some writer path is missing a mark) — then heals. Cadence via
 * PSX_NP_RAM_DIG_VERIFY (digest calls between checks; 0 disables; default
 * 1024 ≈ 20 s of netplay).
 */
#ifndef NETPLAY_RAM_DIRTY_H
#define NETPLAY_RAM_DIRTY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_RAM_DIG_PAGE_SHIFT 12u                       /* 4 KiB */
#define NP_RAM_DIG_MAX_BYTES  (8u << 20)                /* 8 MiB RAM mod */
#define NP_RAM_DIG_MAX_PAGES  (NP_RAM_DIG_MAX_BYTES >> NP_RAM_DIG_PAGE_SHIFT)

extern int      g_np_ram_dig_tracking;
extern uint32_t g_np_ram_dig_dirty[NP_RAM_DIG_MAX_PAGES / 32u];

/* Store chokepoint mark: a few ops, off until the first digest call. phys is
 * the RAM-relative offset (caller already range-checked it). Aligned word /
 * half stores never straddle a 4 KiB page, so one mark suffices. */
static inline void np_ram_dig_note_write(uint32_t phys) {
    if (!g_np_ram_dig_tracking) return;
    {
        uint32_t pg = phys >> NP_RAM_DIG_PAGE_SHIFT;
        g_np_ram_dig_dirty[pg >> 5u] |= 1u << (pg & 31u);
    }
}

static inline void np_ram_dig_note_range(uint32_t phys, uint32_t len) {
    if (!g_np_ram_dig_tracking || len == 0u) return;
    {
        uint32_t pg = phys >> NP_RAM_DIG_PAGE_SHIFT;
        uint32_t pg_end = (phys + len - 1u) >> NP_RAM_DIG_PAGE_SHIFT;
        for (; pg <= pg_end && pg < NP_RAM_DIG_MAX_PAGES; pg++)
            g_np_ram_dig_dirty[pg >> 5u] |= 1u << (pg & 31u);
    }
}

/* Every page recomputes on the next digest. For writers that bypass the
 * store chokepoint (full-RAM restore, bank switch, shadow restore). */
void np_ram_dig_mark_all(void);

/* The RAM partition digest: raw (un-finalized, crc32_update convention)
 * CRC over the page-CRC array, dirty pages re-hashed first. */
uint32_t np_ram_dig_crc_raw(const uint8_t *ram, uint32_t bytes);

#ifdef __cplusplus
}
#endif
#endif /* NETPLAY_RAM_DIRTY_H */
