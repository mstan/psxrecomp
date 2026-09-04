/*
 * Validate PS1 CD-ROM interrupt mask matching.
 *
 * Build/run: ctest -R cdrom_irq_mask_test
 */
#include "cdrom_irq.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(expected, mask, reason, label) do {                              \
    int actual = cdrom_irq_mask_matches_reason((uint8_t)(mask),                \
                                                (uint8_t)(reason));             \
    if (actual != (expected)) {                                                \
        fprintf(stderr, "FAIL: %s (mask=0x%02X reason=%u expected=%d got=%d)\n",\
                (label), (unsigned)(mask), (unsigned)(reason),                 \
                (expected), actual);                                           \
        failures++;                                                            \
    }                                                                          \
} while (0)

int main(void) {
    /* The conventional 0x07 mask must deliver every encoded HC05 reason. */
    for (uint8_t reason = 1; reason <= 5; reason++) {
        CHECK(1, 0x07, reason, "mask 0x07 enables INT1 through INT5");
    }

    /* The mask/status comparison is bitwise, not a reason-to-one-hot lookup. */
    CHECK(0, 0x01, 2, "bit 0 does not match INT2");
    CHECK(1, 0x01, 3, "bit 0 matches encoded INT3");
    CHECK(0, 0x02, 4, "bit 1 does not match INT4");
    CHECK(1, 0x04, 4, "bit 2 matches encoded INT4");
    CHECK(1, 0x01, 5, "bit 0 matches encoded INT5");
    CHECK(1, 0x04, 5, "bit 2 matches encoded INT5");

    CHECK(0, 0x00, 1, "zero mask suppresses an interrupt");
    CHECK(0, 0x07, 0, "reason zero means no interrupt");
    CHECK(0, 0x18, 5, "audio-buffer mask bits do not match an HC05 reason");

    if (failures) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    puts("ALL PASS");
    return 0;
}
