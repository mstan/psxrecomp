#ifndef PGXP_VERTEX_CACHE_H
#define PGXP_VERTEX_CACHE_H

/* Frame-local canonicalization for PGXP screen vertices.
 *
 * A GP0 primitive is submitted immediately, so a later neighbour cannot
 * retroactively change an already-rasterized edge.  The first occurrence of
 * an integer screen vertex therefore chooses the frame's canonical position;
 * every later occurrence uses that exact 16.16 position, whether its own PGXP
 * lookup succeeded or failed.  The final native screen position is the edge
 * identity: unused packet bits, draw offsets, and widescreen/UI transforms
 * cannot make two raster-identical endpoints choose different fractions.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGXP_VERTEX_CACHE_CAPACITY 65536u

typedef struct PgxpVertexCacheEntry {
    uint64_t frame;
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

enum {
    PGXP_VERTEX_CACHE_INSERTED = 0,
    PGXP_VERTEX_CACHE_REUSED = 1,
    PGXP_VERTEX_CACHE_OVERFLOW = 2,
};

/* Resolve one final (post-offset/post-widescreen) integer vertex.  On first
 * sight in a frame, candidate_x/y become canonical when candidate_precise is
 * true; otherwise the native integer position is canonical.  Repeated keys
 * return the first position exactly. */
int pgxp_vertex_cache_resolve(PgxpVertexCache *cache, uint64_t frame,
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
