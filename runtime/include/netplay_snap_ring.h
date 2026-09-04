#ifndef PSX_NETPLAY_SNAP_RING_H
#define PSX_NETPLAY_SNAP_RING_H

/*
 * MotK snap ring: opaque blob ring from retcomm-rbengine; save/load wrap
 * boot_state serialize (PSX-specific).
 */

#include <stddef.h>
#include <stdint.h>

#include "cpu_state.h"
#include "retcomm_rbengine/snap_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_SNAP_RING_DEFAULT_DEPTH RBE_SNAP_RING_DEFAULT_DEPTH
typedef RbeSnapRing NetplaySnapRing;

#define netplay_snap_ring_create      rbe_snap_ring_create
#define netplay_snap_ring_destroy     rbe_snap_ring_destroy
#define netplay_snap_ring_clear       rbe_snap_ring_clear
#define netplay_snap_ring_depth       rbe_snap_ring_depth
#define netplay_snap_ring_count       rbe_snap_ring_count
#define netplay_snap_ring_has         rbe_snap_ring_has
#define netplay_snap_ring_store       rbe_snap_ring_store
#define netplay_snap_ring_peek        rbe_snap_ring_peek
#define netplay_snap_ring_drop_after  rbe_snap_ring_drop_after
#define netplay_snap_ring_drop_tick   rbe_snap_ring_drop_tick
#define netplay_snap_ring_oldest_tick rbe_snap_ring_oldest_tick
#define netplay_snap_ring_newest_tick rbe_snap_ring_newest_tick

/* Serialize live machine into the ring at tick (overwrites same tick). */
int netplay_snap_ring_save(NetplaySnapRing *r, uint32_t tick,
                           const CPUState *cpu, uint32_t bios_checksum,
                           uint32_t entry_pc);

/* Restore machine from the snap at tick. Returns 0 if missing/reject. */
int netplay_snap_ring_load(NetplaySnapRing *r, uint32_t tick, CPUState *cpu,
                           uint32_t bios_checksum, uint32_t entry_pc);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_SNAP_RING_H */
