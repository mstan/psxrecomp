/* load_accel.h -- opt-in, verified load-time acceleration primitives. */
#ifndef PSXRECOMP_LOAD_ACCEL_H
#define PSXRECOMP_LOAD_ACCEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

int  psx_vsync_query_hle_enter(struct CPUState* cpu, uint32_t func,
                               uint32_t counter_addr, uint32_t gpustat_ptr_addr,
                               uint32_t timer1_ptr_addr,
                               uint32_t timer1_cache_addr);
/* Runtime configure + dispatch-time try (works without per-func codegen emit).
 * Call configure once from game.toml load; dispatch calls try before entry->fn. */
void psx_vsync_query_hle_configure(uint32_t func, uint32_t counter_addr,
                                   uint32_t gpustat_ptr_addr,
                                   uint32_t timer1_ptr_addr,
                                   uint32_t timer1_cache_addr);
int  psx_vsync_query_hle_try(struct CPUState* cpu, uint32_t dispatch_addr);
void psx_vsync_query_hle_set_enabled(int on);
void psx_vsync_query_hle_set_horizon_enabled(int on);
void psx_vsync_query_hle_set_extra_horizon_enabled(int on);
/* When set, any VSync(-1) may event-horizon while CD is busy (not only listed
 * return PCs). MotK FMV polls hit many sites; the 16-site allowlist left ~99%
 * of query calls without a guest-time skip. */
void psx_vsync_query_hle_set_horizon_any(int on);
void psx_vsync_query_hle_add_event_horizon_site(uint32_t return_pc);
void psx_vsync_query_hle_add_extra_event_horizon_site(uint32_t return_pc);
void psx_vsync_query_hle_stats_json(char* buf, int cap);

#ifdef __cplusplus
}
#endif
#endif
