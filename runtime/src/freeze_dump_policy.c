#include "freeze_dump_policy.h"

#include <stdio.h>

static unsigned wedge_priority(uint32_t wedge_kind) {
    switch (wedge_kind) {
    case 1: return 4; /* hard freeze */
    case 2: return 3; /* exception re-entry storm */
    case 3: return 2; /* slow frames */
    case 5: return 1; /* logical spin */
    default: return 0;
    }
}

int freeze_dump_policy_observe(FreezeDumpPolicy *policy, uint32_t wedge_kind,
                               int fatal_active) {
    if (!policy) return 0;
    if (fatal_active) return 0;

    unsigned priority = wedge_priority(wedge_kind);
    if (wedge_kind != 0 && priority == 0) return 0;

    if (wedge_kind != 0) {
        policy->healthy_ticks = 0;
        if (policy->in_wedge) {
            /* Only a hard freeze can reserve the second slot during an
             * existing event. Lesser severity changes stay in one incident. */
            if (wedge_kind != 1 || policy->wedge_kind == 1) return 0;
        }

        policy->in_wedge = 1;
        policy->wedge_kind = wedge_kind;

        if (wedge_kind == 1) {
            if (!policy->hard_slot_consumed) {
                policy->pending_kind = wedge_kind;
                return 1;
            }
        } else if (!policy->ordinary_slot_consumed) {
            policy->pending_kind = wedge_kind;
            return 1;
        }

        policy->suppressed_events++;
        return 0;
    }

    if (!policy->in_wedge) return 0;

    if (++policy->healthy_ticks >= FREEZE_DUMP_REARM_HEALTHY_TICKS) {
        policy->healthy_ticks = 0;
        policy->wedge_kind = 0;
        policy->in_wedge = 0;
    }
    return 0;
}

void freeze_dump_policy_record_result(FreezeDumpPolicy *policy, int written) {
    if (!policy || policy->pending_kind == 0) return;

    int hard = policy->pending_kind == 1;
    if (written) {
        if (hard)
            policy->hard_slot_consumed = 1;
        else
            policy->ordinary_slot_consumed = 1;
        policy->automatic_dumps++;
    } else {
        uint32_t *attempts = hard ? &policy->hard_failed_attempts
                                  : &policy->ordinary_failed_attempts;
        int *consumed = hard ? &policy->hard_slot_consumed
                             : &policy->ordinary_slot_consumed;
        policy->failed_dumps++;
        if (++*attempts >= FREEZE_DUMP_MAX_FAILED_ATTEMPTS) *consumed = 1;

        /* A failed attempt ends this event. The next sample can retry only
         * while the slot remains within its fixed attempt limit. */
        policy->healthy_ticks = 0;
        policy->wedge_kind = 0;
        policy->in_wedge = 0;
    }
    policy->pending_kind = 0;
}

int freeze_dump_format_path(char *out, size_t cap, const char *backend,
                            long long wall, uint32_t sequence) {
    if (!out || cap == 0 || !backend) return 0;
    int n = snprintf(out, cap, "psx_freeze_dump_%s_%lld_%u.json",
                     backend, wall, sequence);
    return n > 0 && (size_t)n < cap;
}
