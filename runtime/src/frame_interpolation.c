#include "frame_interpolation.h"

#include <math.h>
#include <string.h>

void frame_interpolation_schedule_reset(FrameInterpolationSchedule *schedule) {
    if (schedule) memset(schedule, 0, sizeof(*schedule));
}

int frame_interpolation_schedule_begin(FrameInterpolationSchedule *schedule,
                                       uint64_t now, uint64_t frequency,
                                       double source_hz, double target_hz) {
    double source_period;
    double target_period;
    double now_d = (double)now;

    if (!schedule || frequency == 0 || !isfinite(source_hz) ||
        !isfinite(target_hz) || source_hz < 1.0 || target_hz < source_hz ||
        source_hz > 1000.0 || target_hz > 1000.0) {
        if (schedule) frame_interpolation_schedule_reset(schedule);
        return 0;
    }

    source_period = (double)frequency / source_hz;
    target_period = (double)frequency / target_hz;
    if (source_period < 1.0 || target_period < 1.0) {
        frame_interpolation_schedule_reset(schedule);
        return 0;
    }

    /* Re-anchor after startup, clock discontinuity, or a sustained overrun.
     * Normal guest work lands after the prior deadline but before the next one;
     * retaining the old anchor makes that work consume the pacing budget. */
    if (schedule->source_deadline <= 0.0 ||
        now_d + source_period < schedule->source_deadline ||
        now_d > schedule->source_deadline + source_period * 4.0) {
        schedule->frame_start = now_d;
        schedule->source_deadline = now_d + source_period;
        schedule->next_present_deadline = now_d + target_period;
    } else {
        schedule->frame_start = schedule->source_deadline;
        schedule->source_deadline += source_period;
    }

    schedule->frame_end = schedule->source_deadline;
    schedule->target_period = target_period;

    /* Coalesce stale output deadlines. Keep the newest missed deadline so an
     * over-budget guest frame can update the window once, then resume cadence. */
    while (schedule->next_present_deadline + target_period <= now_d)
        schedule->next_present_deadline += target_period;
    return 1;
}

int frame_interpolation_schedule_next(FrameInterpolationSchedule *schedule,
                                      uint64_t now, uint64_t *deadline,
                                      float *alpha) {
    double d;
    double span;
    double a;

    if (!schedule || schedule->target_period <= 0.0 ||
        schedule->next_present_deadline > schedule->frame_end + 0.5)
        return 0;

    d = schedule->next_present_deadline;
    schedule->next_present_deadline += schedule->target_period;
    if (d < (double)now) d = (double)now;
    if (d > schedule->frame_end) d = schedule->frame_end;

    span = schedule->frame_end - schedule->frame_start;
    a = span > 0.0 ? (d - schedule->frame_start) / span : 1.0;
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    if (deadline) *deadline = (uint64_t)(d + 0.5);
    if (alpha) *alpha = (float)a;
    return 1;
}

uint64_t frame_interpolation_schedule_end(
    const FrameInterpolationSchedule *schedule) {
    if (!schedule || schedule->frame_end <= 0.0) return 0;
    return (uint64_t)(schedule->frame_end + 0.5);
}
