/*
 * guest_clock.h — pure guest-clock relationships shared by runtime code and
 * timing tests.
 *
 * The PS1 CPU clock is the common source for CRTC VBlank, SPU production, and
 * CD timing.  Keeping these relationships here makes PAL×2 audits test the
 * same arithmetic used by the runtime instead of reproducing it in a script.
 */
#ifndef PSXRECOMP_GUEST_CLOCK_H
#define PSXRECOMP_GUEST_CLOCK_H

#include <stdint.h>

#define PSX_GUEST_CPU_CLOCK_HZ       33868800ull
#define PSX_GUEST_AUDIO_SAMPLE_HZ    44100ull
#define PSX_GUEST_AUDIO_CYCLES_PER_SAMPLE 768ull

static inline uint32_t psx_guest_crtc_period_cycles(uint32_t base_cycles,
                                                     uint32_t multiplier) {
    if (multiplier < 1u) multiplier = 1u;
    if (base_cycles == 0u) return 0u;
    return base_cycles / multiplier;
}

/* Return the exact guest VBlank rate as a rational numerator/denominator.
 * Callers that need floating-point presentation can divide the two values;
 * tests can compare rates without introducing a rounding dependency. */
static inline uint64_t psx_guest_crtc_hz_num(uint32_t period_cycles) {
    return period_cycles ? PSX_GUEST_CPU_CLOCK_HZ : 0ull;
}

static inline uint32_t psx_guest_crtc_hz_den(uint32_t period_cycles) {
    return period_cycles;
}

/* Consume guest cycles into 44.1 kHz SPU frames while preserving the exact
 * fractional-cycle remainder across calls. */
static inline uint64_t psx_guest_audio_frames_for_cycles(uint64_t cycles,
                                                          uint64_t *carry) {
    uint64_t remainder = carry ? *carry : 0ull;
    uint64_t total = cycles + remainder;
    uint64_t frames = total / PSX_GUEST_AUDIO_CYCLES_PER_SAMPLE;
    if (carry) *carry = total % PSX_GUEST_AUDIO_CYCLES_PER_SAMPLE;
    return frames;
}

#endif /* PSXRECOMP_GUEST_CLOCK_H */
