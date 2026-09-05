/*
 * test_starvation_watchdog.c — pin the race-free staleness decision behind
 * starvation_watchdog_check().
 *
 * Regression test for the false watchdog trip seen on Breath of Fire III
 * (2026-09-02): the debug-server IO thread stamps the heartbeat on every
 * send() chunk, the emu thread's check sampled the clock first and the
 * heartbeat second, so a stamp landing between the two reads made
 * `last > now`; `now - last` wrapped to ~1.8e19 us, beat the 4 s threshold,
 * and a healthy 60 fps runtime exited with a dump whose meta line showed the
 * heartbeat was 402 us old. starvation_watchdog_stale() refuses a negative gap
 * by construction; this test pins that property.
 *
 * Build: gcc -I../include -o test_starvation_watchdog test_starvation_watchdog.c
 */
#include <stdio.h>
#include <stdint.h>

#include "starvation_ring.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    const uint64_t TIMEOUT = 4 * 1000000ull;   /* runtime default: 4 s */
    const uint64_t T       = 322798613022ull;  /* the 2026-09-02 heartbeat */

    /* 1. The false-trip scenario: a heartbeat stamped by the IO thread AFTER
     *    the emu thread sampled the clock. Old code wrapped here. */
    CHECK(starvation_watchdog_stale(T + 1, T, TIMEOUT) == 0,
          "heartbeat 1 us newer than the clock sample does not fire");
    CHECK(starvation_watchdog_stale(T + 5000, T, TIMEOUT) == 0,
          "heartbeat 5 ms newer than the clock sample does not fire");
    CHECK(starvation_watchdog_stale(T, T, TIMEOUT) == 0,
          "heartbeat equal to the clock sample does not fire");

    /* 2. Fresh heartbeats never fire (the 402 us case from the dump). */
    CHECK(starvation_watchdog_stale(T, T + 402, TIMEOUT) == 0,
          "402 us staleness does not fire a 4 s watchdog");
    CHECK(starvation_watchdog_stale(T, T + TIMEOUT, TIMEOUT) == 0,
          "staleness exactly at the threshold does not fire");

    /* 3. A genuine stall still fires. */
    CHECK(starvation_watchdog_stale(T, T + TIMEOUT + 1, TIMEOUT) == 1,
          "staleness one past the threshold fires");
    CHECK(starvation_watchdog_stale(T, T + 60 * 1000000ull, TIMEOUT) == 1,
          "60 s staleness fires");

    /* 4. Disable / not-yet-initialised guards. */
    CHECK(starvation_watchdog_stale(T, T + 60 * 1000000ull, 0) == 0,
          "timeout 0 (PSX_STARVATION_TIMEOUT_US=0) never fires");
    CHECK(starvation_watchdog_stale(0, T, TIMEOUT) == 0,
          "no heartbeat yet never fires");

    /* 5. Wrap is impossible for any pair, not just the observed one. */
    CHECK(starvation_watchdog_stale(UINT64_MAX, 1, TIMEOUT) == 0,
          "max heartbeat vs tiny clock does not fire");
    CHECK(starvation_watchdog_stale(1, UINT64_MAX, TIMEOUT) == 1,
          "tiny heartbeat vs max clock fires");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all starvation watchdog checks passed\n");
    return 0;
}
