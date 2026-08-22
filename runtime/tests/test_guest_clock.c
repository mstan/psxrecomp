/* Pure guest-clock invariants used by the PAL x2 runtime audit. */
#include <stdint.h>
#include <stdio.h>

#include "guest_clock.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint64_t hz_num(uint32_t period) {
    return psx_guest_crtc_hz_num(period);
}

static uint32_t hz_den(uint32_t period) {
    return psx_guest_crtc_hz_den(period);
}

int main(void) {
    const uint32_t ntsc = 564480u;
    const uint32_t pal  = 677376u;

    const uint32_t pal_stock = psx_guest_crtc_period_cycles(pal, 1u);
    const uint32_t pal_x2    = psx_guest_crtc_period_cycles(pal, 2u);
    const uint32_t ntsc_x2   = psx_guest_crtc_period_cycles(ntsc, 2u);

    CHECK(hz_num(pal_stock) == 33868800ull && hz_den(pal_stock) == 677376u,
          "PAL stock remains a 50 Hz rational clock");
    CHECK(hz_num(pal_x2) == 33868800ull && hz_den(pal_x2) == 338688u,
          "PAL x2 exposes a 100 Hz rational clock");
    CHECK(hz_num(ntsc_x2) == 33868800ull && hz_den(ntsc_x2) == 282240u,
          "NTSC x2 exposes a 120 Hz rational clock");
    CHECK(psx_guest_crtc_period_cycles(pal, 0u) == pal,
          "zero multiplier is safely treated as stock");

    /* One second of guest time must produce exactly 44,100 SPU frames at
     * PAL x2: 338,688 cycles per VBlank × 100 VBlanks = CPU clock. */
    uint64_t carry = 0;
    uint64_t frames = 0;
    for (int i = 0; i < 100; i++)
        frames += psx_guest_audio_frames_for_cycles(pal_x2, &carry);
    CHECK(frames == PSX_GUEST_AUDIO_SAMPLE_HZ && carry == 0,
          "PAL x2 guest-clocked SPU produces exactly 44.1 kHz");

    /* The same identity must hold when device servicing splits the cycle
     * stream at arbitrary event boundaries. */
    carry = 0;
    frames = 0;
    uint64_t expected_cycles = 0;
    for (uint32_t i = 0; i < 1000u; i++)
    {
        uint64_t chunk = (uint64_t)pal_x2 / 10u + (i % 7u);
        frames += psx_guest_audio_frames_for_cycles(chunk, &carry);
        expected_cycles += chunk;
    }
    CHECK(frames == expected_cycles / PSX_GUEST_AUDIO_CYCLES_PER_SAMPLE &&
          carry == expected_cycles % PSX_GUEST_AUDIO_CYCLES_PER_SAMPLE,
          "SPU cycle carry survives event-bounded servicing");

    /* CDDA Red Book cadence is 75 sectors/sec × 588 stereo frames = 44.1 kHz;
     * this is a diagnostic invariant, not a speed-up permission. */
    CHECK(75u * 588u == PSX_GUEST_AUDIO_SAMPLE_HZ,
          "CDDA native sector cadence equals 44.1 kHz");

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
