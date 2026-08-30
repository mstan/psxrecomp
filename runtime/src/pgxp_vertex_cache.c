#include "pgxp_vertex_cache.h"

static uint32_t pgxp_vertex_hash(int32_t native_x, int32_t native_y) {
    uint32_t h = (uint32_t)native_x * 0x85EBCA77u;
    h ^= (uint32_t)native_y * 0xC2B2AE3Du;
    h ^= h >> 16;
    return h;
}

int pgxp_vertex_cache_resolve(PgxpVertexCache *cache, uint64_t frame,
                              int32_t native_x, int32_t native_y,
                              int candidate_precise,
                              int32_t candidate_x16,
                              int32_t candidate_y16, int32_t *out_x16,
                              int32_t *out_y16,
                              int *out_canonical_precise) {
    const uint32_t mask = PGXP_VERTEX_CACHE_CAPACITY - 1u;
    uint32_t slot = pgxp_vertex_hash(native_x, native_y) & mask;

    for (uint32_t probe = 0; probe < PGXP_VERTEX_CACHE_CAPACITY; ++probe) {
        PgxpVertexCacheEntry *entry = &cache->entries[slot];
        if (!entry->occupied || entry->frame != frame) {
            entry->frame = frame;
            entry->native_x = native_x;
            entry->native_y = native_y;
            entry->canonical_precise = candidate_precise ? 1u : 0u;
            entry->canonical_x16 = candidate_precise
                                       ? candidate_x16
                                       : (int32_t)((int64_t)native_x * 65536);
            entry->canonical_y16 = candidate_precise
                                       ? candidate_y16
                                       : (int32_t)((int64_t)native_y * 65536);
            entry->occupied = 1u;
            if (out_x16) *out_x16 = entry->canonical_x16;
            if (out_y16) *out_y16 = entry->canonical_y16;
            if (out_canonical_precise)
                *out_canonical_precise = entry->canonical_precise;
            return PGXP_VERTEX_CACHE_INSERTED;
        }
        if (entry->native_x == native_x && entry->native_y == native_y) {
            if (out_x16) *out_x16 = entry->canonical_x16;
            if (out_y16) *out_y16 = entry->canonical_y16;
            if (out_canonical_precise)
                *out_canonical_precise = entry->canonical_precise;
            return PGXP_VERTEX_CACHE_REUSED;
        }
        slot = (slot + 1u) & mask;
    }

    /* A pathological frame that exhausts the table fails faithful: the caller
     * gets the native position and records overflow telemetry. */
    if (out_x16) *out_x16 = (int32_t)((int64_t)native_x * 65536);
    if (out_y16) *out_y16 = (int32_t)((int64_t)native_y * 65536);
    if (out_canonical_precise) *out_canonical_precise = 0;
    return PGXP_VERTEX_CACHE_OVERFLOW;
}
