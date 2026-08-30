#include "pgxp_vertex_cache.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    ++failures; \
} } while (0)

static PgxpVertexCache cache;

static void corrected_then_native(void) {
    int32_t x, y;
    int precise;
    memset(&cache, 0, sizeof(cache));
    CHECK(pgxp_vertex_cache_resolve(&cache, 10, 160, 80, 1,
                                    (160 << 16) + 0xC000,
                                    (80 << 16) + 0x4000,
                                    &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_INSERTED);
    CHECK(precise && x == (160 << 16) + 0xC000 &&
          y == (80 << 16) + 0x4000);
    CHECK(pgxp_vertex_cache_resolve(&cache, 10, 160, 80, 0,
                                    0, 0, &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_REUSED);
    CHECK(precise && x == (160 << 16) + 0xC000 &&
          y == (80 << 16) + 0x4000);
}

static void native_then_corrected(void) {
    int32_t x, y;
    int precise;
    memset(&cache, 0, sizeof(cache));
    CHECK(pgxp_vertex_cache_resolve(&cache, 20, 160, 80, 0,
                                    0, 0, &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_INSERTED);
    CHECK(!precise && x == (160 << 16) && y == (80 << 16));
    /* 0.75 px is deliberately above the old 0.5 seam workaround.  The edge
     * remains identical because the frame canonical, not tolerance, decides. */
    CHECK(pgxp_vertex_cache_resolve(&cache, 20, 160, 80, 1,
                                    (160 << 16) + 0xC000,
                                    (80 << 16) + 0x4000,
                                    &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_REUSED);
    CHECK(!precise && x == (160 << 16) && y == (80 << 16));
}

static void transform_and_frame_are_part_of_identity(void) {
    int32_t x, y;
    int precise;
    memset(&cache, 0, sizeof(cache));
    pgxp_vertex_cache_resolve(&cache, 30, 160, 80, 1,
                              (160 << 16) + 1, (80 << 16) + 2,
                              &x, &y, &precise);
    pgxp_vertex_cache_resolve(&cache, 30, 170, 80, 0,
                              0, 0, &x, &y, &precise);
    CHECK(!precise && x == (170 << 16));
    pgxp_vertex_cache_resolve(&cache, 31, 160, 80, 0,
                              0, 0, &x, &y, &precise);
    CHECK(!precise && x == (160 << 16) && y == (80 << 16));
}

int main(void) {
    corrected_then_native();
    native_then_corrected();
    transform_and_frame_are_part_of_identity();
    if (failures) return 1;
    puts("test_pgxp_vertex_cache: all checks passed");
    return 0;
}
