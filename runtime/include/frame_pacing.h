/*
 * frame_pacing.h — race-free wall-clock frame pacing.
 *
 * Replaces the open-coded pacing loops in main.cpp (sdl_vblank_present)
 * and beetle_main.cpp. The open-coded version read the performance
 * counter twice per iteration (once in the while-condition, once to
 * compute the remaining time); if the deadline was crossed between the
 * two reads — a preemption-sized window — the unsigned subtraction
 * underflowed and SDL_Delay received a ~24.7-day argument (2^64 ticks
 * * 1000 / 10 MHz, truncated to Uint32). That was the Bug B hard
 * freeze: the app parks in one SDL_Delay with every counter frozen,
 * wedged between `i_stat |= VBLANK` and exception delivery.
 *
 * Rules the implementation must keep:
 *   - exactly ONE counter read per decision; compare before subtract
 *   - the computed sleep is clamped to one frame period
 *   - a deadline already in the past never sleeps
 */
#ifndef FRAME_PACING_H
#define FRAME_PACING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FramePacer {
    uint64_t next_deadline;   /* perf-counter ticks; 0 = not started */
} FramePacer;

/* Pure decision function (unit-testable, no SDL).
 * Given a single counter sample `now`, the current deadline, the counter
 * frequency and the frame period (both in ticks), returns the number of
 * milliseconds to sleep right now, or 0 if the caller should stop
 * sleeping (deadline reached / close enough to spin).
 * Never returns a value larger than one period's worth of ms + 1. */
uint32_t frame_pacing_sleep_ms(uint64_t now, uint64_t deadline,
                               uint64_t freq, uint64_t period);

/* Block until the pacer's next deadline (sleep + final sub-ms spin),
 * then advance the deadline by one period. If the pacer has not started,
 * or the caller fell beyond the implementation's bounded transient-catch-up
 * window, re-anchor to now + period without sleeping. period_ms is the frame
 * period in milliseconds (e.g. 1000.0 / 59.94). */
void frame_pacer_wait(FramePacer *p, double period_ms);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_PACING_H */
