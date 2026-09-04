/*
 * test_netplay_snap_ring.c — ring bookkeeping without a live PSX machine.
 *
 * Stores synthetic blobs via netplay_snap_ring_store and checks has/peek,
 * same-tick overwrite, and oldest-tick eviction at capacity.
 *
 * Build/run: ctest -R netplay_snap_ring_test
 * (registered in runtime/CMakeLists.txt, which owns the source list and the
 *  NETPLAY_SNAP_RING_TEST_STUB_BOOT_STATE define. This file deliberately does
 *  not repeat a gcc recipe: an unexecuted recipe in a comment is what let this
 *  harness rot — it kept listing ../src/netplay_snap_ring.c, which the file
 *  already #includes, long after that became a duplicate-symbol error.)
 */

/* This TU pulls in ../src/netplay_snap_ring.c at the bottom, and that file
 * calls clock_gettime/CLOCK_MONOTONIC on non-Windows. The equivalent define in
 * netplay_snap_ring.c itself is too late here: <stdio.h> below already latched
 * glibc's <features.h>, so under a strict -std=cNN the POSIX declarations stay
 * hidden and the build dies on an implicit declaration. Must stay first. */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#ifndef PSX_HAS_RECOMP_NET
#  define PSX_HAS_RECOMP_NET 1
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Stub boot_state so the MotK save/load glue links without the full runtime. */
#ifdef NETPLAY_SNAP_RING_TEST_STUB_BOOT_STATE
#include "cpu_state.h"
int boot_state_save_buffer(const CPUState* cpu, uint32_t bios_checksum,
                           uint32_t entry_pc, uint8_t** out_data,
                           size_t* out_len) {
    (void)cpu; (void)bios_checksum; (void)entry_pc;
    *out_data = (uint8_t*)malloc(4);
    if (!*out_data) return 0;
    memcpy(*out_data, "save", 4);
    *out_len = 4;
    return 1;
}
int boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios_checksum,
                               uint32_t entry_pc, uint8_t** out_data,
                               size_t* out_len) {
    return boot_state_save_buffer(cpu, bios_checksum, entry_pc, out_data,
                                  out_len);
}
int boot_state_load_buffer(const uint8_t* file, size_t file_len,
                           uint32_t bios_checksum, uint32_t entry_pc,
                           CPUState* cpu) {
    (void)bios_checksum; (void)entry_pc; (void)cpu;
    return file && file_len == 4 && memcmp(file, "save", 4) == 0;
}
uint32_t boot_state_last_vram_dirty_rows(void) { return 0; }
int boot_state_last_vram_incremental(void) { return 0; }
#endif

#include "netplay_snap_ring.h"
#include "../src/netplay_snap_ring.c"

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint8_t* mkblob(uint32_t tick, size_t* out_len) {
    uint8_t* p = (uint8_t*)malloc(8);
    if (!p) return NULL;
    memcpy(p, &tick, 4);
    memcpy(p + 4, "SNAP", 4);
    *out_len = 8;
    return p;
}

int main(void) {
    NetplaySnapRing* r = netplay_snap_ring_create(4);
    size_t n = 0;
    const uint8_t* peek;
    uint32_t t;
    CPUState cpu;
    uint8_t* b;
    size_t blen;

    CHECK(r != NULL, "create depth 4");
    CHECK(netplay_snap_ring_depth(r) == 4u, "depth");
    CHECK(netplay_snap_ring_count(r) == 0u, "empty count");

    for (t = 10; t < 14; t++) {
        b = mkblob(t, &blen);
        CHECK(b && netplay_snap_ring_store(r, t, b, blen), "store tick");
    }
    CHECK(netplay_snap_ring_count(r) == 4u, "full");
    CHECK(netplay_snap_ring_has(r, 10), "has oldest");
    CHECK(netplay_snap_ring_has(r, 13), "has newest");
    CHECK(netplay_snap_ring_oldest_tick(r) == 10u, "oldest");
    CHECK(netplay_snap_ring_newest_tick(r) == 13u, "newest");

    peek = netplay_snap_ring_peek(r, 12, &n);
    CHECK(peek && n == 8 && memcmp(peek, "\x0c\x00\x00\x00SNAP", 8) == 0,
          "peek payload");

    /* Overwrite same tick keeps count. */
    b = mkblob(12, &blen);
    CHECK(b && netplay_snap_ring_store(r, 12, b, blen), "overwrite");
    CHECK(netplay_snap_ring_count(r) == 4u, "count stable on overwrite");

    /* Evict oldest (10) when inserting 14. */
    b = mkblob(14, &blen);
    CHECK(b && netplay_snap_ring_store(r, 14, b, blen), "evict store");
    CHECK(!netplay_snap_ring_has(r, 10), "oldest evicted");
    CHECK(netplay_snap_ring_has(r, 14), "new tick present");
    CHECK(netplay_snap_ring_oldest_tick(r) == 11u, "new oldest");
    CHECK(netplay_snap_ring_count(r) == 4u, "still full");

    memset(&cpu, 0, sizeof cpu);
    CHECK(netplay_snap_ring_save(r, 100, &cpu, 1, 0x80010000u),
          "save via stub boot_state");
    CHECK(netplay_snap_ring_has(r, 100), "saved tick");
    CHECK(netplay_snap_ring_load(r, 100, &cpu, 1, 0x80010000u),
          "load via stub boot_state");

    netplay_snap_ring_clear(r);
    CHECK(netplay_snap_ring_count(r) == 0u, "clear");
    netplay_snap_ring_destroy(r);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
