#ifndef PSX_FRAME_INTERPOLATION_H
#define PSX_FRAME_INTERPOLATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameInterpolationSchedule {
    double source_deadline;
    double next_present_deadline;
    double frame_start;
    double frame_end;
    double target_period;
} FrameInterpolationSchedule;

/* Plan one stock guest-frame interval. Presentation deadlines remain anchored
 * across calls so guest work consumes part of the interval instead of slowing
 * the simulation. Returns zero when the rates or host clock are invalid. */
int frame_interpolation_schedule_begin(FrameInterpolationSchedule *schedule,
                                       uint64_t now, uint64_t frequency,
                                       double source_hz, double target_hz);

/* Return each presentation deadline in the current source interval. Alpha is
 * the temporal blend position from the previous completed frame to the current
 * one. Deadlines missed by more than one output period are coalesced. */
int frame_interpolation_schedule_next(FrameInterpolationSchedule *schedule,
                                      uint64_t now, uint64_t *deadline,
                                      float *alpha);

uint64_t frame_interpolation_schedule_end(
    const FrameInterpolationSchedule *schedule);
void frame_interpolation_schedule_reset(FrameInterpolationSchedule *schedule);

#ifdef __cplusplus
}
#endif

#endif
