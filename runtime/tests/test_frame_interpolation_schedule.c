#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "frame_interpolation.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

static int run_interval(FrameInterpolationSchedule *schedule, uint64_t now,
                        double source_hz, double target_hz,
                        float *first_alpha, float *last_alpha) {
    uint64_t deadline;
    float alpha;
    int count = 0;

    CHECK(frame_interpolation_schedule_begin(
              schedule, now, 1000000u, source_hz, target_hz),
          "valid rates should produce an interval");
    while (frame_interpolation_schedule_next(
               schedule, now, &deadline, &alpha)) {
        CHECK(deadline >= now, "presentation deadline must not be in the past");
        CHECK(deadline <= frame_interpolation_schedule_end(schedule),
              "presentation deadline must stay inside the source interval");
        if (count == 0 && first_alpha) *first_alpha = alpha;
        if (last_alpha) *last_alpha = alpha;
        count++;
    }
    return count;
}

int main(void) {
    FrameInterpolationSchedule schedule = {0};
    float first = 0.0f, last = 0.0f;
    uint64_t end;

    CHECK(run_interval(&schedule, 1000000u, 60.0, 120.0,
                       &first, &last) == 2,
          "120 Hz should schedule two presents per 60 Hz source interval");
    CHECK(fabsf(first - 0.5f) < 0.001f && fabsf(last - 1.0f) < 0.001f,
          "120 Hz blend phases should be one-half and complete");

    frame_interpolation_schedule_reset(&schedule);
    end = 2000000u;
    int total = 0;
    for (int i = 0; i < 5; i++) {
        total += run_interval(&schedule, end, 60.0, 144.0, NULL, NULL);
        end = frame_interpolation_schedule_end(&schedule) + 1000u;
    }
    CHECK(total == 12,
          "144 Hz should retain fractional cadence across five source frames");

    frame_interpolation_schedule_reset(&schedule);
    CHECK(run_interval(&schedule, 3000000u, 60.0, 165.0, NULL, NULL) >= 2,
          "165 Hz should schedule multiple presents");
    end = frame_interpolation_schedule_end(&schedule);
    CHECK(frame_interpolation_schedule_begin(
              &schedule, end + 500u, 1000000u, 60.0, 165.0),
          "normal guest work should keep the existing cadence anchor");
    CHECK(frame_interpolation_schedule_end(&schedule) == end + 16667u ||
          frame_interpolation_schedule_end(&schedule) == end + 16666u,
          "guest work must consume, not extend, the next source interval");

    CHECK(!frame_interpolation_schedule_begin(
               &schedule, 0u, 0u, 60.0, 120.0),
          "zero host frequency should be rejected");
    CHECK(!frame_interpolation_schedule_begin(
               &schedule, 0u, 1000000u, 60.0, 59.0),
          "output below source cadence should be rejected");

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
