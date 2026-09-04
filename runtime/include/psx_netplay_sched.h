/* psx_netplay_sched.h — MotK facade over retcomm-rbengine (rbe_sched_*).
 *
 * Policy lives in lib/retcomm-rbengine. This header keeps the historical
 * np_sched_* / PsxNpSchedBridge surface; MotK dig0/FMV/RTT enter through
 * gates in psx_netplay_sched.c.
 *
 * Env knobs: RBE_RB_* (preferred) with MotK PSX_RB_* fallback in rbengine.
 *
 * The scheduler's job (design contract, 2026-08-02):
 *   - keep both peers at the same simulation time (mispredict-driven pacing,
 *     pcap freeze);
 *   - keep the delay cushion FULL (consume at wire = sim, produce at
 *     wire = sim + D — see np_sched_wire_for_sim; prediction is an emergency
 *     brake, not a steady state);
 *   - spend prediction only on genuine runway starvation, and recover back
 *     to zero prediction afterwards (cushion rebuild);
 *   - resolve D from measured link latency (np_sched auto-delay) instead of
 *     paying for phase error with prediction.
 */
#ifndef PSX_NETPLAY_SCHED_H
#define PSX_NETPLAY_SCHED_H

#include <stdint.h>

/* Full session types only when recomp-net is linked. Stub builds keep opaque
 * pointers so this header stays includable with PSX_NETPLAY=OFF. */
#if defined(PSX_HAS_RECOMP_NET)
#include "recomp_net/session.h"
#else
typedef struct RNetSession RNetSession;
typedef struct RNetSessionStats RNetSessionStats;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Live pointers into psx_netplay.c state (session may repoint on restart). */
typedef struct PsxNpSchedBridge {
    RNetSession **session;
    int *input_delay;      /* committed D, ticks */
    int *input_prediction; /* P cap, ticks */
    int *local_slot;
    /* Sticky for the session (from launch cfg / PSX_NET_FORCE_TURN). */
    int force_turn;        /* 1 = ICE relay-only — auto-delay floor applies */
} PsxNpSchedBridge;

void np_sched_bind(const PsxNpSchedBridge *bridge);

/* Clear session-scoped pacing/invent state (cushion rebuild, pcap freeze,
 * timesync debt, tip cadence). Soft-return rematch reuses the process; without
 * this, a mid-match episode can leave cushion_rebuild/debt armed and freeze
 * the next session after lockstep armed. Called from np_sched_bind. */
void np_sched_reset_session(void);

/* sim→wire CONSUMPTION mapping. §44 real delay (default): guest tick T plays
 * the wire row T; the local sample taken at admit(T) is stored at wire T+D
 * (rnet_session_prepare_local_tip), so a press surfaces D ticks later and the
 * peer's row for T arrived ~D ticks minus transit before it is needed — a
 * real cushion. PSX_RB_ZERO_DELAY=1 restores the legacy mapping (T plays
 * wire T+D, the sample taken the same admit: zero local latency, zero
 * cushion, permanent pred_depth 1 — the pre-§44 behavior). */
uint32_t np_sched_wire_for_sim(uint32_t sim_tick);
int np_sched_real_delay_enabled(void);

/* Mid-session DELAY_SYNC commit → mirror into *input_delay. */
void np_sched_sync_delay_from_session(void);

/* Pre-admit gate, in original np_try_admit_rollback order: tip cadence
 * tracking, timesync throttle (live only), lead sampling, runway/phase logs,
 * cushion-rebuild clearing, auto-delay resolution. Returns 1 = stall this
 * admit (pacing debt), 0 = proceed. */
int np_sched_pre_admit(uint32_t sim, uint32_t wire, const RNetSessionStats *st);

/* Remote row missing at `wire` for `slot`. Returns 1 = stall the admit
 * (grace/freeze/cushion wait), 0 = invent hold-last now (*reason_out set,
 * telemetry already recorded). */
int np_sched_on_remote_miss(int slot, uint32_t sim, uint32_t wire,
                            const RNetSessionStats *st, int pred,
                            const char **reason_out);

/* Remote row present at the consumption wire (gap1 grace paid off). */
void np_sched_note_remote_hit(void);

/* End of a successful admit: leave pcap freeze, flush periodic stats. */
void np_sched_post_admit(int any_invent);

/* MotK-side admit stall tag for barrier logs. Rollback never calls
 * rnet_session_try_admit, so rnet last_stall stays "ok" while MotK invent /
 * FMV lockstep / wire-hole stalls. Cleared on successful admit. */
void np_sched_set_admit_stall(const char *tag);
void np_sched_clear_admit_stall(void);
const char *np_sched_admit_stall_tag(void); /* "" if none */

/* Reconcile caught a genuine mispredict (published != wire) that rode `age`
 * ticks before being caught — feeds the pacing debt of the ahead peer. */
void np_sched_note_mispredict(uint32_t age);

/* Episode boundary (commit/abort): clear the pegged-streak off-guard and arm
 * cushion rebuild. (Public alias: psx_netplay_timesync_on_episode_boundary.) */
void np_sched_note_episode_boundary(void);

/* §83 C: after baseline-abort Live realign, allow invent despite absurd
 * remote_lead for a short window so recovery can catch up (soak: lead=500
 * refuse invent → crawl). Tip-hold ring-age absurd leads stay blocked. */
void np_sched_arm_absurd_invent_catchup(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_SCHED_H */
