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
    CHECK(pgxp_vertex_cache_resolve(&cache, 10, 0x100, 160, 80, 1,
                                    (160 << 16) + 0xC000,
                                    (80 << 16) + 0x4000,
                                    &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_INSERTED);
    CHECK(precise && x == (160 << 16) + 0xC000 &&
          y == (80 << 16) + 0x4000);
    CHECK(pgxp_vertex_cache_resolve(&cache, 10, 0x100, 160, 80, 0,
                                    0, 0, &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_REUSED);
    CHECK(precise && x == (160 << 16) + 0xC000 &&
          y == (80 << 16) + 0x4000);
}

static void native_then_corrected(void) {
    int32_t x, y;
    int precise;
    memset(&cache, 0, sizeof(cache));
    CHECK(pgxp_vertex_cache_resolve(&cache, 20, 0x100, 160, 80, 0,
                                    0, 0, &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_INSERTED);
    CHECK(!precise && x == (160 << 16) && y == (80 << 16));
    /* 0.75 px is deliberately above the old 0.5 seam workaround.  The edge
     * remains identical because the frame canonical, not tolerance, decides. */
    CHECK(pgxp_vertex_cache_resolve(&cache, 20, 0x100, 160, 80, 1,
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
    pgxp_vertex_cache_resolve(&cache, 30, 0x100, 160, 80, 1,
                              (160 << 16) + 1, (80 << 16) + 2,
                              &x, &y, &precise);
    pgxp_vertex_cache_resolve(&cache, 30, 0x100, 170, 80, 0,
                              0, 0, &x, &y, &precise);
    CHECK(!precise && x == (170 << 16));
    pgxp_vertex_cache_resolve(&cache, 31, 0x100, 160, 80, 0,
                              0, 0, &x, &y, &precise);
    CHECK(!precise && x == (160 << 16) && y == (80 << 16));
}

static void reset_discards_rewound_frame(void) {
    int32_t x, y;
    int precise;
    memset(&cache, 0, sizeof(cache));
    CHECK(pgxp_vertex_cache_resolve(&cache, 42, 0x100, 160, 80, 1,
                                    (160 << 16) + 0x4000,
                                    (80 << 16) + 0x8000,
                                    &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_INSERTED);
    pgxp_vertex_cache_reset(&cache);
    CHECK(pgxp_vertex_cache_resolve(&cache, 42, 0x100, 160, 80, 1,
                                    (160 << 16) + 0xC000,
                                    (80 << 16) + 0x1000,
                                    &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_INSERTED);
    CHECK(precise && x == (160 << 16) + 0xC000 &&
          y == (80 << 16) + 0x1000);
}

static void distinct_source_vertices_do_not_alias(void) {
    int32_t x, y;
    int precise;
    memset(&cache, 0, sizeof(cache));
    CHECK(pgxp_vertex_cache_resolve(&cache, 50, 0x100, 160, 80, 1,
                                    (160 << 16) + 0x1000,
                                    (80 << 16) + 0x2000,
                                    &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_INSERTED);
    CHECK(pgxp_vertex_cache_resolve(&cache, 50, 0x200, 160, 80, 1,
                                    (160 << 16) + 0xD000,
                                    (80 << 16) + 0xE000,
                                    &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_INSERTED);
    CHECK(precise && x == (160 << 16) + 0xD000 &&
          y == (80 << 16) + 0xE000);
    CHECK(pgxp_vertex_cache_resolve(&cache, 50, 0x100, 160, 80, 0,
                                    0, 0, &x, &y, &precise) ==
          PGXP_VERTEX_CACHE_REUSED);
    CHECK(precise && x == (160 << 16) + 0x1000 &&
          y == (80 << 16) + 0x2000);
}

int main(void) {
    corrected_then_native();
    native_then_corrected();
    transform_and_frame_are_part_of_identity();
    reset_discards_rewound_frame();
    distinct_source_vertices_do_not_alias();
    if (failures) return 1;
    puts("test_pgxp_vertex_cache: all checks passed");
    return 0;
}
