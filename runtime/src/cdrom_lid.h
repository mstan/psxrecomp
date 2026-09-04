#ifndef PSXRECOMP_CDROM_LID_H
#define PSXRECOMP_CDROM_LID_H

#include <stdint.h>

/* The PS1 CPU clock is 33.8688 MHz. Keep inserted media hidden from the
 * guest for two seconds so software can observe a physical open/close cycle. */
#define CDROM_LID_CLOSE_DELAY_CYCLES (33868800ull * 2ull)

typedef struct CdromLidState {
    uint64_t close_due;
    uint8_t physical_open;
    uint8_t shell_open_latched;
} CdromLidState;

static inline void cdrom_lid_reset(CdromLidState *state)
{
    state->close_due = 0;
    state->physical_open = 0;
    state->shell_open_latched = 0;
}

static inline void cdrom_lid_begin_open(CdromLidState *state, uint64_t now)
{
    state->close_due = now + CDROM_LID_CLOSE_DELAY_CYCLES;
    state->physical_open = 1;
    state->shell_open_latched = 1;
}

static inline int cdrom_lid_media_ready(const CdromLidState *state,
                                        int host_media_present)
{
    return host_media_present && !state->physical_open;
}

static inline int cdrom_lid_close_if_due(CdromLidState *state, uint64_t now)
{
    if (!state->physical_open || now < state->close_due)
        return 0;
    state->physical_open = 0;
    state->close_due = 0;
    return 1;
}

/* GetStat returns the latched ShellOpen bit once after the physical lid has
 * closed. The next GetStat can report the inserted disc normally. */
static inline int cdrom_lid_acknowledge_closed_shell(CdromLidState *state)
{
    if (state->physical_open || !state->shell_open_latched)
        return 0;
    state->shell_open_latched = 0;
    return 1;
}

#endif
