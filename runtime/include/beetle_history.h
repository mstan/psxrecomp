/* beetle_history.h — psx-beetle per-frame snapshot ring.
 *
 * Mirrors runtime/src/debug_server.c's PSXFrameRecord history so beetle
 * exposes the SAME time-series surface as psx-runtime: history,
 * get_frame, frame_range, frame_timeseries, first_failure,
 * read_frame_ram, set_snapshot, get_snapshots.
 *
 * Capacity matches the runtime side (36000 frames ~= 10 min @ 60 Hz)
 * so cross-backend tooling can assume identical retention windows.
 *
 * The record fields are a strict subset of PSXFrameRecord — the fields
 * recomp can populate but beetle cannot (e.g. dispatch_count tied to
 * psx_dispatch() call gates) are left zero and explicitly documented
 * so consumers see real-vs-stub at a glance.
 */
#ifndef PSXRECOMP_V4_BEETLE_HISTORY_H
#define PSXRECOMP_V4_BEETLE_HISTORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BEETLE_FRAME_HISTORY_CAP   36000
#define BEETLE_RAM_SNAPSHOT_REGIONS 4
#define BEETLE_RAM_SNAPSHOT_SIZE    128

typedef struct {
    uint32_t frame_number;
    int      verify_pass;   /* -1: not checked (no verify mode on beetle today) */
    int      diff_count;    /* 0 */

    /* MIPS CPU state (read via PSX_CPU::GetRegister). */
    uint32_t gpr[32];
    uint32_t hi, lo;
    uint32_t cop0_sr;
    uint32_t cop0_cause;
    uint32_t cop0_epc;

    /* Interrupt controller (IRQ_GetRegister). */
    uint32_t i_stat;
    uint32_t i_mask;

    /* GPU display state (PS_GPU fields). */
    uint16_t display_area_x, display_area_y;
    uint16_t display_w, display_h;
    uint8_t  display_disabled;

    /* SIO / pad. pad_buttons mirrors recomp's pad_buttons word. */
    uint16_t pad_buttons;
    uint16_t sio_stat;
    uint16_t sio_ctrl;

    /* Timing (left as stubs on beetle for now; documented). */
    uint32_t dispatch_count;
    uint64_t total_dispatches;   /* mirrors frame_number on beetle */

    /* Per-frame configurable RAM snapshot regions (MainRAM byte copies). */
    uint32_t snapshot_addr[BEETLE_RAM_SNAPSHOT_REGIONS];
    uint8_t  snapshot_data[BEETLE_RAM_SNAPSHOT_REGIONS][BEETLE_RAM_SNAPSHOT_SIZE];
} BeetleFrameRecord;

/* ---- Lifecycle hooks (called from beetle_libretro.cpp) ---- */
void beetle_history_init(void);
void beetle_history_record_frame(void);

/* ---- Query surface (called from beetle_debug_server.c) ----
 * All "frame" parameters are absolute frame numbers (0..s_history_count-1).
 * Frames older than (s_history_count - cap) are evicted and look like
 * "not available" in the per-frame APIs. */

void beetle_history_get_bounds(uint64_t *out_count,
                               uint64_t *out_oldest,
                               uint64_t *out_newest);

const BeetleFrameRecord *beetle_history_get_frame(uint32_t frame);

int beetle_history_first_failure(uint32_t *out_frame, int *out_diff_count);

int beetle_history_set_snapshot(int slot, uint32_t addr);
int beetle_history_get_snapshot(int slot, uint32_t *out_addr, int *out_active);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_BEETLE_HISTORY_H */
