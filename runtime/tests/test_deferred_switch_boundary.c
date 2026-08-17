#include "interrupts.h"

#include <stdio.h>

static int failures;

#define CHECK(condition, message) do {                                      \
    if (!(condition)) {                                                     \
        fprintf(stderr, "FAIL: %s\n", (message));                          \
        failures++;                                                         \
    }                                                                       \
} while (0)

int main(void)
{
    CHECK(psx_deferred_switch_boundary_materialized(
              0, 1, 0, 0u, 0x80012340u),
          "an ordinary outermost block leader is materialized");
    CHECK(psx_deferred_switch_boundary_materialized(
              1, 1, 0, 0x80012340u, 0x80012340u),
          "an exact site-1 PC match is materialized");
    CHECK(psx_deferred_switch_boundary_materialized(
              1, 1, 0, 0xA0012340u, 0x80012340u),
          "KSEG aliases identify the same guest PC");

    CHECK(!psx_deferred_switch_boundary_materialized(
              1, 2, 0, 0x80012340u, 0x80012340u),
          "a nested dispatch is not materialized");
    CHECK(!psx_deferred_switch_boundary_materialized(
              1, 1, 1, 0x80012340u, 0x80012340u),
          "a live call-unit frame is not materialized");
    CHECK(!psx_deferred_switch_boundary_materialized(
              2, 1, 0, 0x80012340u, 0x80012340u),
          "other dirty interpreter pump sites remain ineligible");
    CHECK(!psx_deferred_switch_boundary_materialized(
              1, 1, 0, 0x80012340u, 0x80056780u),
          "different live and published PCs are not materialized");
    CHECK(!psx_deferred_switch_boundary_materialized(
              1, 1, 0, 0u, 0x80012340u),
          "an unpublished live PC is not materialized");
    CHECK(!psx_deferred_switch_boundary_materialized(
              1, 1, 0, 0x80012340u, 0u),
          "a missing resume PC is not materialized");
    CHECK(!psx_deferred_switch_boundary_materialized(
              0, 1, 0, 0u, 0x80000FF0u),
          "low BIOS and kernel RAM remains ineligible");

    if (failures != 0) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }

    printf("test_deferred_switch_boundary: all checks passed\n");
    return 0;
}
