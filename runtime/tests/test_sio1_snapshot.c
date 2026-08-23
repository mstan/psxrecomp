/*
 * test_sio1_snapshot.c -- SIO1 snapshot round-trips.
 *
 * Covers: mid-shift round-trip into a FRESH device+crossover pair with the
 * peer receiving at the identical guest cycle; clock-shifted restore keeping
 * the baud-timer field stable relative to the new clock (anchor + due_cycle
 * serialize as deltas, never absolute); blob-length rejection; queued wire
 * chars with latency surviving the trip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sio1.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail = 1; \
    } \
} while (0)

static void cfg(Sio1Device *d, uint16_t ctrl, uint64_t now) {
    sio1_device_write(d, 0x1F80105A, 2, 0x0040, now);
    sio1_device_write(d, 0x1F801058, 2, 0x00CE, now);
    sio1_device_write(d, 0x1F80105E, 2, 0x00D8, now);
    sio1_device_write(d, 0x1F80105A, 2, ctrl, now);
}

static void snap(Sio1Device *d, uint8_t **buf, uint32_t *len, uint64_t now) {
    *len = sio1_device_snap_bytes(d);
    *buf = (uint8_t *)malloc(*len);
    sio1_device_snap_write(d, *buf, now);
}

static void test_mid_shift_roundtrip(void) {
    PsxLinkEndpoint *ea, *eb, *fa, *fb;
    Sio1Device *A, *B, *A2, *B2;
    uint8_t *ba, *bb;
    uint32_t la, lb, half, rest;
    uint64_t now = 7000000;

    psx_link_crossover_create(&ea, &eb);
    A = sio1_device_create(); B = sio1_device_create();
    sio1_device_attach(A, ea); sio1_device_attach(B, eb);
    sio1_device_reset(A, now); sio1_device_reset(B, now);
    cfg(A, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN, now);
    cfg(B, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN, now);

    sio1_device_write(A, 0x1F801050, 1, 0x3C, now);
    half = sio1_device_char_cycles(A) / 2u;
    rest = sio1_device_char_cycles(A) - half;
    now += half;
    sio1_device_advance(A, half, now);
    sio1_device_advance(B, half, now);

    snap(A, &ba, &la, now);
    snap(B, &bb, &lb, now);

    /* Restore into a FRESH pair. */
    psx_link_crossover_create(&fa, &fb);
    A2 = sio1_device_create(); B2 = sio1_device_create();
    sio1_device_attach(A2, fa); sio1_device_attach(B2, fb);
    CHECK(sio1_device_snap_read(A2, ba, la, now));
    CHECK(sio1_device_snap_read(B2, bb, lb, now));
    /* Same derived timing after load (recomputed, not serialized). */
    CHECK(sio1_device_char_cycles(A2) == sio1_device_char_cycles(A));

    /* Advance the ORIGINAL pair and the RESTORED pair identically: the byte
     * must land at the same guest cycle in both. */
    now += rest - 1;
    sio1_device_advance(A, rest - 1, now);  sio1_device_advance(B, rest - 1, now);
    sio1_device_advance(A2, rest - 1, now); sio1_device_advance(B2, rest - 1, now);
    CHECK((sio1_device_read(B,  0x1F801054, 4, now) & SIO1_STAT_RXNE) == 0);
    CHECK((sio1_device_read(B2, 0x1F801054, 4, now) & SIO1_STAT_RXNE) == 0);
    now += 1;
    sio1_device_advance(A, 1, now);  sio1_device_advance(B, 1, now);
    sio1_device_advance(A2, 1, now); sio1_device_advance(B2, 1, now);
    CHECK(sio1_device_read(B,  0x1F801050, 1, now) == 0x3Cu);
    CHECK(sio1_device_read(B2, 0x1F801050, 1, now) == 0x3Cu);

    /* Digest parity: fresh snapshots of original and restored pair match
     * byte-for-byte outside the meta section. */
    {
        uint8_t *ra, *r2;
        uint32_t n1, n2, ends[3];
        snap(A, &ra, &n1, now);
        snap(A2, &r2, &n2, now);
        sio1_device_snap_section_ends(A, ends);
        CHECK(n1 == n2);
        CHECK(memcmp(ra, r2, ends[1]) == 0);   /* regs + fsm + wire */
        free(ra); free(r2);
    }

    free(ba); free(bb);
    sio1_device_destroy(A); sio1_device_destroy(B);
    sio1_device_destroy(A2); sio1_device_destroy(B2);
    psx_link_crossover_destroy(ea, eb);
    psx_link_crossover_destroy(fa, fb);
}

static void test_clock_shifted_restore(void) {
    Sio1Device *d = sio1_device_create();
    Sio1Device *e = sio1_device_create();
    uint8_t *b;
    uint32_t n, t_before, t_after;
    uint64_t now = 9000000, now2 = 123456789;

    sio1_device_attach(d, psx_link_null());
    sio1_device_attach(e, psx_link_null());
    sio1_device_reset(d, now);
    cfg(d, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN, now);
    now += 777;                                  /* arbitrary phase */
    t_before = sio1_device_read(d, 0x1F801054, 4, now) >> 11;
    snap(d, &b, &n, now);
    /* Restore under a WILDLY different clock: the timer field must be
     * unchanged relative to the new clock (delta-encoded anchor). */
    CHECK(sio1_device_snap_read(e, b, n, now2));
    t_after = sio1_device_read(e, 0x1F801054, 4, now2) >> 11;
    CHECK(t_before == t_after);
    /* Length policing: short and long blobs rejected. */
    CHECK(!sio1_device_snap_read(e, b, n - 1, now2));
    {
        uint8_t *big = (uint8_t *)malloc(n + 1);
        memcpy(big, b, n);
        big[n] = 0;
        CHECK(!sio1_device_snap_read(e, big, n + 1, now2));
        free(big);
    }
    free(b);
    sio1_device_destroy(d);
    sio1_device_destroy(e);
}

static void test_latency_queue_roundtrip(void) {
    PsxLinkEndpoint *ea, *eb, *fa, *fb;
    Sio1Device *A, *B, *A2, *B2;
    uint8_t *ba, *bb;
    uint32_t la, lb, lat = 5000;
    uint64_t now = 4000000;

    psx_link_crossover_create(&ea, &eb);
    psx_link_set_latency_cycles(ea, lat);       /* shared: sets both dirs */
    A = sio1_device_create(); B = sio1_device_create();
    sio1_device_attach(A, ea); sio1_device_attach(B, eb);
    sio1_device_reset(A, now); sio1_device_reset(B, now);
    cfg(A, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN, now);
    cfg(B, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN, now);

    /* Complete the shift; char now rides the wire for `lat` more cycles. */
    sio1_device_write(A, 0x1F801050, 1, 0x99, now);
    now += sio1_device_char_cycles(A);
    sio1_device_advance(A, sio1_device_char_cycles(A), now);
    sio1_device_advance(B, sio1_device_char_cycles(B), now);
    CHECK((sio1_device_read(B, 0x1F801054, 4, now) & SIO1_STAT_RXNE) == 0);

    snap(A, &ba, &la, now);
    snap(B, &bb, &lb, now);
    psx_link_crossover_create(&fa, &fb);
    A2 = sio1_device_create(); B2 = sio1_device_create();
    sio1_device_attach(A2, fa); sio1_device_attach(B2, fb);
    CHECK(sio1_device_snap_read(A2, ba, la, now));
    CHECK(sio1_device_snap_read(B2, bb, lb, now));

    now += lat;
    sio1_device_advance(A2, lat, now);
    sio1_device_advance(B2, lat, now);
    CHECK(sio1_device_read(B2, 0x1F801050, 1, now) == 0x99u);

    free(ba); free(bb);
    sio1_device_destroy(A); sio1_device_destroy(B);
    sio1_device_destroy(A2); sio1_device_destroy(B2);
    psx_link_crossover_destroy(ea, eb);
    psx_link_crossover_destroy(fa, fb);
}

int main(void) {
    test_mid_shift_roundtrip();
    test_clock_shifted_restore();
    test_latency_queue_roundtrip();
    if (g_fail) { fprintf(stderr, "test_sio1_snapshot: FAILED\n"); return 1; }
    printf("test_sio1_snapshot: OK\n");
    return 0;
}
