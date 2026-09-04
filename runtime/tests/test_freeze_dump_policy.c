#include "freeze_dump_policy.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT_EQ(actual, expected) do {                                      \
    unsigned got_ = (unsigned)(actual);                                       \
    unsigned want_ = (unsigned)(expected);                                    \
    if (got_ != want_) {                                                      \
        fprintf(stderr, "%s:%d: got %u, expected %u\n",                     \
                __FILE__, __LINE__, got_, want_);                             \
        failures++;                                                           \
    }                                                                         \
} while (0)

static int observe(FreezeDumpPolicy *policy, uint32_t kind) {
    return freeze_dump_policy_observe(policy, kind, 0);
}

static void observe_healthy(FreezeDumpPolicy *policy, uint32_t ticks) {
    for (uint32_t i = 0; i < ticks; i++) EXPECT_EQ(observe(policy, 0), 0);
}

int main(void) {
    FreezeDumpPolicy policy = {0};

    /* Authorization does not consume a slot until the file reaches disk. */
    EXPECT_EQ(observe(&policy, 5), 1);
    EXPECT_EQ(policy.automatic_dumps, 0);
    freeze_dump_policy_record_result(&policy, 1);
    EXPECT_EQ(policy.automatic_dumps, 1);
    EXPECT_EQ(policy.ordinary_slot_consumed, 1);
    EXPECT_EQ(observe(&policy, 5), 0);

    /* A short healthy gap does not turn one oscillating wedge into two. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS - 1u);
    EXPECT_EQ(observe(&policy, 5), 0);

    /* Lesser severity changes share the ordinary slot. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&policy, 3), 0);
    EXPECT_EQ(policy.suppressed_events, 1);

    /* A hard freeze owns the reserved second slot during the same event. */
    EXPECT_EQ(observe(&policy, 1), 1);
    freeze_dump_policy_record_result(&policy, 1);
    EXPECT_EQ(policy.automatic_dumps, FREEZE_DUMP_AUTO_LIMIT);
    EXPECT_EQ(policy.hard_slot_consumed, 1);
    EXPECT_EQ(observe(&policy, 1), 0);

    /* Re-entry storms are known events and are counted after the limit. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&policy, 2), 0);
    EXPECT_EQ(policy.suppressed_events, 2);

    /* Fatal state bypasses the automatic policy without consuming state. */
    FreezeDumpPolicy fatal = {0};
    EXPECT_EQ(freeze_dump_policy_observe(&fatal, 5, 1), 0);
    EXPECT_EQ(fatal.in_wedge, 0);
    EXPECT_EQ(fatal.automatic_dumps, 0);
    EXPECT_EQ(observe(&fatal, 5), 1);

    /* A failed write keeps its slot for a bounded retry. */
    FreezeDumpPolicy retry = {0};
    EXPECT_EQ(observe(&retry, 5), 1);
    freeze_dump_policy_record_result(&retry, 0);
    EXPECT_EQ(retry.automatic_dumps, 0);
    EXPECT_EQ(retry.failed_dumps, 1);
    EXPECT_EQ(observe(&retry, 5), 1);
    freeze_dump_policy_record_result(&retry, 1);
    EXPECT_EQ(retry.automatic_dumps, 1);
    EXPECT_EQ(retry.failed_dumps, 1);

    /* Three failed attempts consume the ordinary slot and stop retries. */
    FreezeDumpPolicy exhausted = {0};
    for (uint32_t i = 0; i < FREEZE_DUMP_MAX_FAILED_ATTEMPTS; i++) {
        EXPECT_EQ(observe(&exhausted, 5), 1);
        freeze_dump_policy_record_result(&exhausted, 0);
    }
    EXPECT_EQ(exhausted.failed_dumps, FREEZE_DUMP_MAX_FAILED_ATTEMPTS);
    EXPECT_EQ(exhausted.ordinary_slot_consumed, 1);
    EXPECT_EQ(observe(&exhausted, 5), 0);
    EXPECT_EQ(exhausted.suppressed_events, 1);

    /* An exhausted ordinary slot does not consume the hard-freeze slot. */
    EXPECT_EQ(observe(&exhausted, 1), 1);
    freeze_dump_policy_record_result(&exhausted, 1);
    EXPECT_EQ(exhausted.hard_slot_consumed, 1);
    EXPECT_EQ(exhausted.automatic_dumps, 1);

    /* Unknown future kinds do not consume or suppress an existing slot. */
    FreezeDumpPolicy unknown = {0};
    EXPECT_EQ(observe(&unknown, 7), 0);
    EXPECT_EQ(unknown.in_wedge, 0);
    EXPECT_EQ(unknown.suppressed_events, 0);

    /* A hard-first event leaves the ordinary evidence slot available. */
    FreezeDumpPolicy hard_first = {0};
    EXPECT_EQ(observe(&hard_first, 1), 1);
    freeze_dump_policy_record_result(&hard_first, 1);
    observe_healthy(&hard_first, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&hard_first, 5), 1);
    freeze_dump_policy_record_result(&hard_first, 1);
    EXPECT_EQ(hard_first.automatic_dumps, FREEZE_DUMP_AUTO_LIMIT);

    /* Equal wall seconds still produce distinct file names. */
    char path0[128], path1[128];
    EXPECT_EQ(freeze_dump_format_path(path0, sizeof(path0),
                                      "psx-runtime", 1234, 0), 1);
    EXPECT_EQ(freeze_dump_format_path(path1, sizeof(path1),
                                      "psx-runtime", 1234, 1), 1);
    EXPECT_EQ(strcmp(path0, path1) != 0, 1);
    EXPECT_EQ(freeze_dump_format_path(path0, 4, "psx-runtime", 1234, 0), 0);

    if (failures != 0) {
        fprintf(stderr, "freeze dump policy: %d failure(s)\n", failures);
        return 1;
    }
    puts("freeze dump policy: all checks passed");
    return 0;
}
