#ifndef PGXP_VERTEX_CACHE_H
#define PGXP_VERTEX_CACHE_H

/* Frame-local canonicalization for PGXP screen vertices.
 *
 * A GP0 primitive is submitted immediately, so a later neighbour cannot
 * retroactively change an already-rasterized edge.  The first occurrence of
 * an integer screen vertex therefore chooses the frame's canonical position;
 * every later occurrence of the same packet vertex uses that exact 16.16
 * position, whether its own PGXP lookup succeeded or failed.  The source word
 * address is part of the identity: two independent vertices that happen to
 * land on one native pixel must not inherit one another's fraction.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGXP_VERTEX_CACHE_CAPACITY 65536u

typedef struct PgxpVertexCacheEntry {
    uint64_t frame;
    uint32_t source_addr;
    uint32_t _pad0;
    int32_t native_x;
    int32_t native_y;
    int32_t canonical_x16;
    int32_t canonical_y16;
    uint8_t occupied;
    uint8_t canonical_precise;
    uint8_t _pad[6];
} PgxpVertexCacheEntry;

typedef struct PgxpVertexCache {
    PgxpVertexCacheEntry entries[PGXP_VERTEX_CACHE_CAPACITY];
} PgxpVertexCache;

/* Drop all frame-local canonical positions.  This is required whenever the
 * emulated timeline jumps (reset or savestate restore); the frame counter is
 * not itself a sufficient epoch after a rewind. */
void pgxp_vertex_cache_reset(PgxpVertexCache *cache);

enum {
    PGXP_VERTEX_CACHE_INSERTED = 0,
    PGXP_VERTEX_CACHE_REUSED = 1,
    PGXP_VERTEX_CACHE_OVERFLOW = 2,
};

/* Resolve one final (post-offset/post-widescreen) integer vertex.  On first
 * sight of a source word in a frame, candidate_x/y become canonical when
 * candidate_precise is true; otherwise the native integer position is
 * canonical.  Repeated source/native keys return the first position exactly. */
int pgxp_vertex_cache_resolve(PgxpVertexCache *cache, uint64_t frame,
                              uint32_t source_addr,
                              int32_t native_x, int32_t native_y,
                              int candidate_precise,
                              int32_t candidate_x16,
                              int32_t candidate_y16, int32_t *out_x16,
                              int32_t *out_y16,
                              int *out_canonical_precise);

#ifdef __cplusplus
}
#endif

#endif /* PGXP_VERTEX_CACHE_H */
