/* Validate the guest-visible CD tray interval used for disc reinsertion. */
#include "cdrom_lid.h"

#include <stdio.h>

static int failures;

#define CHECK(expr, label) do {                                                \
    if (!(expr)) {                                                             \
        fprintf(stderr, "FAIL: %s\n", (label));                              \
        failures++;                                                            \
    }                                                                          \
} while (0)

int main(void)
{
    CdromLidState lid;
    const uint64_t start = 1234567ull;

    cdrom_lid_reset(&lid);
    CHECK(cdrom_lid_media_ready(&lid, 1), "mounted disc starts readable");

    cdrom_lid_begin_open(&lid, start);
    CHECK(lid.physical_open, "reinsert opens the physical lid");
    CHECK(lid.shell_open_latched, "reinsert latches ShellOpen");
    CHECK(!cdrom_lid_media_ready(&lid, 1),
          "mounted image stays hidden while the lid is open");
    CHECK(!cdrom_lid_close_if_due(
              &lid, start + CDROM_LID_CLOSE_DELAY_CYCLES - 1),
          "lid remains open for the complete delay");
    CHECK(cdrom_lid_close_if_due(
              &lid, start + CDROM_LID_CLOSE_DELAY_CYCLES),
          "lid closes exactly at the deadline");
    CHECK(cdrom_lid_media_ready(&lid, 1),
          "mounted image becomes readable after the lid closes");
    CHECK(cdrom_lid_acknowledge_closed_shell(&lid),
          "first closed-lid GetStat consumes the ShellOpen latch");
    CHECK(!cdrom_lid_acknowledge_closed_shell(&lid),
          "later GetStat calls do not repeat ShellOpen");

    if (failures) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    puts("ALL PASS");
    return 0;
}
