/*
 * psx_link_pair.h -- netplay link-lobby pair driver.
 *
 * A PSX-Link netplay session couples, ON EVERY MACHINE, the netplay CLIENT
 * (the visible instance simulating the console the local seat belongs to)
 * with a spawned headless FOLLOWER simulating the OTHER console. The pair is
 * cabled over psx_link_shm in PAIR mode and rolls back ATOMICALLY: the
 * client's rollback engine drives both instances via the shm command stream
 * (START / TICK / SAVE / LOAD / STOP), so the serial cable stays local and
 * deterministic and never needs a wire format, prediction, or retraction.
 *
 * Seat model: one 4-seat session; seats {0,1} = console A, {2,3} = console B.
 * The client applies its own console's two seats to SIO pads and forwards the
 * other pair's rows (predicted or sealed, exactly as its engine applied them)
 * to the follower. Every A instance in the session computes identical state,
 * ditto B, so each console's cable bytes are identical on every machine.
 *
 * DRIVER call map (netplay client):
 *   launch:      psx_link_pair_client_start()  (before netplay tick 0)
 *   apply site:  psx_link_pair_stage_row()     (foreign-seat pad rows)
 *   admit T:     psx_link_pair_on_admit(T)     (barrier + TICK emit)
 *   ring save:   psx_link_pair_on_save(T)      (mirrors driver snap stores)
 *   ring load:   psx_link_pair_after_load(T)   (AFTER local restore, BEFORE
 *                                               guest resume: LOAD fence)
 *   teardown:    psx_link_pair_shutdown()
 *   health:      psx_link_pair_failed()        (poll in the admit loop)
 *
 * FOLLOWER call map (spawned process, PSX_LINK_FOLLOWER=1):
 *   boot:        psx_link_pair_follower_boot() (pre-scheduler park)
 *   vblank:      psx_link_pair_follower_admit()
 */
#ifndef PSX_LINK_PAIR_H
#define PSX_LINK_PAIR_H

#include <stdint.h>
#include "psx_link_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

typedef struct PsxLinkPairClientCfg {
    int         base_seat;        /* my console's first seat: 0 or 2 */
    uint32_t    session_id;
    uint32_t    tick_len_cycles;  /* vblank period upper bound */
    uint32_t    latency_cycles;   /* cable latency; >= tick_len (0 = auto) */
    uint32_t    flags;            /* PSX_LINK_PAIR_F_* determinism envelope */
    uint32_t    bios_id;
    uint32_t    codegen_hash;
    uint32_t    mod_plan_hash;    /* applied mod plan digest (0 = vanilla) */
    const char *shm_name;         /* segment name (session+pid derived) */
    const char *exe_path;         /* follower binary (self) */
    /* NULL-terminated argv for the follower (argv[0] = exe). The spawn adds
     * nothing: the CALLER owns the full argument surface. */
    char *const *child_argv;
    /* NULL-terminated "KEY=VALUE" strings applied in the child before exec
     * (on top of the inherited environment). */
    char *const *child_env;
} PsxLinkPairClientCfg;

/* ===== driver (netplay client) ========================================== */

int  psx_link_pair_client_start(const PsxLinkPairClientCfg *cfg);
int  psx_link_pair_active(void);
int  psx_link_pair_base_seat(void);
/* Foreign-seat pad row for `tick` (rel = seat - other_base, 0..1), packed as
 * the 8-byte netplay pad blob. Idempotent per (tick, rel). */
void psx_link_pair_stage_row(uint32_t tick, int rel, const uint8_t row[8]);
/* Pipeline barrier + TICK emit for the tick about to run. 0 = follower dead
 * (pair marked failed; caller aborts the session). */
int  psx_link_pair_on_admit(uint32_t tick);
/* Driver stored a ring snap keyed `tick`; forward so the follower stores its
 * own boundary snap under the same key. */
void psx_link_pair_on_save(uint32_t tick);
/* Driver RESTORED its live machine to snap `tick` (state applied, guest not
 * yet resumed): LOAD fence -- the follower restores and acks before any
 * driver guest code can consume cable bytes. 0 = follower dead/failed. */
int  psx_link_pair_after_load(uint32_t tick);
void psx_link_pair_shutdown(void);
int  psx_link_pair_failed(void);

/* ===== follower ========================================================= */

/* PSX_LINK_FOLLOWER=1 in the environment. */
int  psx_link_pair_follower_mode(void);
int  psx_link_pair_follower_booted(void);
/* Attach + verify the driver's determinism config; create the snap ring.
 * `own_flags`/`bios_id`/`codegen_hash` describe THIS process; any mismatch
 * against the driver's published cfg refuses the pair (a silent config fork
 * here surfaces as an unexplainable A-group desync minutes later). Blocks
 * until the driver publishes cfg (or dies). 1 = ok. */
int  psx_link_pair_follower_boot(struct CPUState *cpu, uint32_t bios_checksum,
                                 uint32_t entry_pc, uint32_t own_flags,
                                 uint32_t bios_id, uint32_t codegen_hash,
                                 uint32_t mod_plan_hash);
/* Vblank-boundary barrier: publishes completion of the tick that just ran,
 * then executes commands until a TICK arms the next one (returns 1) or the
 * session ends (returns 0: STOP or driver death -- caller exits cleanly).
 * LOAD longjmps back into the guest and does not return. */
int  psx_link_pair_follower_admit(void);
/* Called at the vblank present body's finish_frame point: digest the tick
 * that just ran at the SAME boundary phase the client digests its own. */
void psx_link_pair_follower_note_finish(void);

/* ===== PSX_LINK_PERF guest-window breakdown ============================= */

/* The LINKPERF "guest" window spans from one admit to the next, so it holds
 * more than emulation: digest work, the GL present, the frame pacer sleep,
 * and the netplay admit spin (which itself splits into "peer input not here
 * yet" — the network — and everything else). Contributors attribute their
 * time into one of these slots; the reporter prints the split and derives
 * emu as the residual. All calls are no-ops unless PSX_LINK_PERF=1. */
enum {
    PSX_LINK_PERF_GW_DIGEST = 0,  /* FRAME_COMMIT / follower digest compute */
    PSX_LINK_PERF_GW_PRESENT,     /* GL present work on the sim thread      */
    PSX_LINK_PERF_GW_PACE,        /* frame pacer sleep                      */
    PSX_LINK_PERF_GW_NETIN,       /* admit spin: waiting on peer input      */
    PSX_LINK_PERF_GW_SPIN,        /* admit spin: everything else            */
    PSX_LINK_PERF_GW_COUNT
};

int      psx_link_pair_perf_enabled(void);   /* PSX_LINK_PERF=1 */
uint64_t psx_link_pair_perf_now_us(void);
void     psx_link_pair_perf_gw_add(int slot, uint64_t us);

#ifdef __cplusplus
}
#endif
#endif /* PSX_LINK_PAIR_H */
