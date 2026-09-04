/*
 * host_time.h — cross-platform monotonic ms + high-res sleep.
 *
 * Windows netplay/pacing must not use GetTickCount64 / Sleep for short
 * slices (coarse ~1–15.6 ms). Match recomp-net / BattleShip: QPC + waitable
 * timer. Unix: CLOCK_MONOTONIC + usleep.
 */
#ifndef PSX_HOST_TIME_H
#define PSX_HOST_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t psx_host_mono_ms(void);
void psx_host_sleep_micros(unsigned usec);
void psx_host_sleep_ms(unsigned ms);

#ifdef __cplusplus
}
#endif

#endif /* PSX_HOST_TIME_H */
