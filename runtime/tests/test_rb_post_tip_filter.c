/*
 * test_rb_post_tip_filter.c — tip-extend stale POST must not latch.
 *
 * Soak class (rb-diag 2026-07-31): both peers POST@1007 (matched), tip-extend
 * to 1008, both POST@1008 (matched); initiator aborted local@1008 vs peer@1007.
 *
 * Build/run: ctest -R rb_post_tip_filter_test
 */
#include "netplay_rb_post.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL: %s\n", msg);                                         \
            failures++;                                                        \
        } else {                                                               \
            printf("ok:   %s\n", msg);                                         \
        }                                                                      \
    } while (0)

int main(void)
{
    const uint32_t tip_old = 1007u;
    const uint32_t tip_new = 1008u;

    CHECK(netplay_rb_peer_post_tip_ok(tip_old, tip_old), "POST@T ok while tip=T");
    CHECK(netplay_rb_peer_post_tip_ok(tip_new, tip_new), "POST@T+1 ok while tip=T+1");
    CHECK(!netplay_rb_peer_post_tip_ok(tip_old, tip_new),
          "stale POST@T rejected during Verify tip=T+1");
    CHECK(!netplay_rb_peer_post_tip_ok(tip_new, tip_old),
          "future POST@T+1 rejected while tip=T");
    CHECK(!netplay_rb_peer_post_tip_ok(0u, tip_new), "zero tip rejected");

    if (failures == 0) {
        printf("test_rb_post_tip_filter: ok\n");
        return 0;
    }
    printf("test_rb_post_tip_filter: %d failure(s)\n", failures);
    return 1;
}
