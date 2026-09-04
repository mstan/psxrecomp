#ifndef PSX_NETPLAY_RB_H
#define PSX_NETPLAY_RB_H

/*
 * MotK rollback episode + snap-ring host (PSX_NET_MODE=rollback).
 * Delay-sync path never calls these.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
struct RNetSession;
/* Alias Rbe* — not `struct Netplay*`; that tag is a distinct incomplete type
 * from `typedef RbeHashConfirm NetplayHashConfirm` in the MotK shims. */
typedef struct RbeInputHist NetplayInputHist;
typedef struct RbeHashConfirm NetplayHashConfirm;

typedef struct PsxNetplayRbBindings {
    struct RNetSession **session;
    struct CPUState **cpu;
    NetplayInputHist *ih;
    NetplayHashConfirm *hc;
    uint32_t *bios_checksum;
    uint32_t *entry_pc;
    int *slot_count;
    int *local_slot;
    int *input_delay;
    void (*publish_sio)(uint32_t tick); /* publish history or sealed rows to SIO */
    void (*apply_frame_slot)(int slot, uint32_t tick, uint16_t buttons,
                             int8_t sx, int8_t sy, uint8_t analog);
} PsxNetplayRbBindings;

void psx_netplay_rb_bind(const PsxNetplayRbBindings *b);
void psx_netplay_rb_start(void);
void psx_netplay_rb_shutdown(void);
/* Soft-return / rematch: wipe RB host residue that rb_shutdown leaves
 * (follow-NACK, tip-dense FIFO, episode wire, dig0 latch). Safe with no session. */
void psx_netplay_rb_cold_reset(void);

/* Safe BB-edge poll (mirror savestate_poll). Saves pending snap / applies load
 * without longjmp. Call psx_netplay_rb_flush_resume() afterward from a C BB-edge
 * (or after all C++ RAII in the vblank path has been destroyed). */
void psx_netplay_rb_poll(struct CPUState *cpu, uint32_t resume_pc);

/* If a baseline snap was applied without resume, longjmp to the scheduler
 * (same as savestate_poll). No-op when nothing is pending. Never returns on
 * success. */
void psx_netplay_rb_flush_resume(void);

/* 1 after flush_resume longjmp until finish_frame / abort — top-level
 * dispatch has no native call chain under the resume PC. */
int  psx_netplay_rb_top_level_resume_active(void);

/* After outermost psx_dispatch returns pc==0 following an RB resume: retry
 * at $ra / sticky BB if safe. Returns 1 and arms RESUME_CURRENT via out_pc. */
int  psx_netplay_rb_recover_null_pc(struct CPUState *cpu, uint32_t *out_pc);

/* After a live tick completes: request snap at tick for next safe poll. */
void psx_netplay_rb_request_snap(uint32_t tick);

/* Initiator: start episode from invent/contract rewind.
 * Returns 1 if an episode opened, 0 if refused (no snap, already active,
 * abort/storm cooldown, FMV lockstep, …). On refuse, reconcile should promote
 * wire (or tip-extend if active). Clean commit does not arm cooldown. */
int  psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot);

/* Active episode: grow target / resign sealed rows for a late wire edge
 * (press+release coalesce). Host must promote wire into hist first.
 * Returns 1 if the correction was absorbed into the current episode. */
int  psx_netplay_rb_tip_extend(uint32_t mismatch_tick, int slot);

/* 1 while begin_rewind is suppressed (cooldown after abort / realign). */
int  psx_netplay_rb_rewind_suppressed(void);

/* 1 during depth24 / recent MDEC / short post-FMV settle — MotK FMV.
 * Post-FMV digest lockstep no longer blocks invent (§26). */
int  psx_netplay_rb_fmv_defer_rewind(void);

/* 1 while depth24 or recent MDEC (not settle). */
int  psx_netplay_rb_fmv_media_active(void);

/* 1 during FMV media + post-FMV lockstep MIN (§26/§113) — admit waits for
 * remote wire through loading tip+1 (not settle-only). Ticks FMV→settle→MIN.
 * Also 1 while §93 MAX-unmatched DESYNC invent-hold is armed (expires).
 * §111: post-FMV heal sticky invent-hold only while remote_lead > 0. */
int  psx_netplay_rb_lockstep_no_invent(void);

/* 1 while sim is still inside the short post-FMV settle tail (before MIN). */
int  psx_netplay_rb_fmv_settle_active(void);

/* §93/§94: 1 while MAX-unmatched DESYNC invent-hold is armed (not media/settle).
 * Stall tag should say fmv_desync_hold, not fmv_settle. */
int  psx_netplay_rb_fmv_desync_hold(void);

/* §94: drop invent-hold after a successful netplay SAVE (tip hole refill) or
 * when the hold expires. Episode media-range refuse is unchanged. */
void psx_netplay_rb_clear_fmv_desync_hold(const char *why);

/* §109: next begin_rewind is an apply-only MEDIA_KF heal (target=load).
 * Call from hc-fork / resim-diverge escalate before begin_rewind.
 * §114: tip+1 verify-span abandoned (platform nondet); heal Live arms
 * invent-off + KF stream instead of lockstep RELEASE. */
void psx_netplay_rb_request_post_fmv_heal_kf(void);

/* §110: 1 while post-FMV lockstep, DESYNC hold, or heal sticky — hc-fork
 * may open apply-only KF even when invent is held. */
int  psx_netplay_rb_post_fmv_heal_eligible(void);

/* §111: 1 while heal sticky window is live (invent hold may be tip-lead only). */
int  psx_netplay_rb_post_fmv_heal_sticky(void);

/* §115: 1 if this tip+1 fork was accepted (HC primed past it) — hc-fork /
 * begin must not re-open apply-only heal for it. */
int  psx_netplay_rb_platform_fork_accepted(uint32_t fork_tick);

/* §93: 1 if tick sits in the last FMV media bout / settle tail (or DESYNC
 * hold). Begin/follow/hc-fork must not open episodes that load there.
 * §97 MEDIA_KF (default on): returns 0 for media-range — episodes allowed. */
int  psx_netplay_rb_fmv_episode_unsafe(uint32_t tick);

/* §97: guest probe reply — 1 if local snap at episode load matches size/crc. */
int  psx_netplay_rb_media_kf_probe_match(uint32_t size, uint32_t crc);
/* §97: apply RB_KF state blob (or host finish). Returns 1 if handled. */
int  psx_netplay_rb_media_kf_on_ready(const void *data, size_t size);
/* §100: 1 while a MEDIA-KF episode is waiting on probe/transfer/pin (sim
 * stalled — present should hold-last so the intro FMV does not freeze). */
int  psx_netplay_rb_media_kf_busy(void);

/* Mid-guest resim pump: abort if Replay has made no finish_frame progress.
 * Full rb_pump stays admit/present-edge only (host-asymmetric). */
void psx_netplay_rb_poll_replay_stall(void);

/* 1 once after abort/realign cooldown arm — reconcile should promote all late
 * wire without opening another episode (clears invent poison). */
int  psx_netplay_rb_take_promote_sweep(void);

/* 1 after FMV lockstep RELEASE while sim < dense_until (UNLOCK_GRACE).
 * Invent is already allowed (§26); reconcile should soft-promote invent→
 * release mispredicts here so sticky hold-last D-pad does not open tip
 * episodes into the title/menu. */
int  psx_netplay_rb_fmv_unlock_grace_active(void);

/* Drain peer RB_* + drive Seal/Baseline/Replay/Verify. Call from pump. */
void psx_netplay_rb_pump(void);

/* 1 while Live should not leave dig0: local dig0 not published yet, or peer
 * dig0 not received. Latches open once both digests match (sticky latches —
 * HC ring may age tick-0 out; must not re-stall after sync). */
int  psx_netplay_rb_boot_dig0_gate(void);
/* Record dig0 cores outside the HC ring (emit / peer FC drain). */
void psx_netplay_rb_boot_dig0_note_local(uint32_t core);
void psx_netplay_rb_boot_dig0_note_peer(uint32_t core);

/* 1 while episode is active (seal/baseline/replay/verify). */
int  psx_netplay_rb_active(void);
int  psx_netplay_rb_is_resimulating(void);
int  psx_netplay_rb_tip_holding(void);

/* Episode tip (target_tick) while an episode is active; 0 if none. */
uint32_t psx_netplay_rb_episode_target(void);

/* TipHold Live invent slack past tip (tip_seal_slack). Stall invent beyond.
 * Returns 0 while a deferred tip-hold rereplay is pending (§80 park Live). */
uint32_t psx_netplay_rb_tip_hold_invent_slack(void);

/* 1 while TipHold has batched tip-extend rereplay waiting for flush (§66/§80). */
int psx_netplay_rb_tip_hold_rereplay_pending(void);

/* POST tip we deferred from (0 if none). Admit parks Live here (§81). */
uint32_t psx_netplay_rb_tip_hold_rereplay_from(void);

/* TipHold coalesce runway (tip_runway). Host scans wire tip+1..tip+runway for
 * release edges when Live is stalled at invent-cap. */
uint32_t psx_netplay_rb_tip_runway(void);

/* §34: while TipHold cannot tip-extend/begin an already-seen wire edge,
 * block wall-clock quiet finalize (release must not be dropped by commit). */
void psx_netplay_rb_tip_hold_block_quiet(int block);

/* 1 if a peer FRAME_COMMIT should be dropped (TipHold invent still on the
 * wire after tip-extend hc_prime). Call from FRAME_COMMIT drain. */
int  psx_netplay_rb_ignore_peer_frame_commit(uint32_t tick, uint32_t hash);

/* FRAME_COMMIT mismatch during Replay — abort before a false POST commit.
 * Returns 1 if an episode was aborted. */
int  psx_netplay_rb_abort_resim_core_mismatch(uint32_t tick, uint32_t local_core,
                                              uint32_t peer_core);
/* 1 while a baseline/realign snap load is queued or resume is deferred —
 * poll_admit must stall (do not run live invent). */
int  psx_netplay_rb_load_pending(void);

/* §71: nonzero while choose_load is bisecting below a failed baseline load
 * (g_bl_fork_cap). hc-fork recovery must not reopen into the same doomed
 * snap ladder every 32 ticks. */
uint32_t psx_netplay_rb_baseline_fork_cap(void);

/*
 * During resim: publish sealed inputs for current sim tick and admit.
 * Returns 1 if guest should run this tick, 0 to wait (seal/baseline).
 */
int  psx_netplay_rb_try_admit(void);

/* After guest frame during resim: advance episode clock; may commit. */
void psx_netplay_rb_finish_frame(void);

/* §47: greatest contiguous confirmed tick T such that every seat has a
 * confirmed (non-predicted) row for every tick from `from_tick` through T.
 * If `from_tick` itself is incomplete, returns from_tick-1 (or 0).
 * Pass from_tick = replay_sim + 1. */
uint32_t psx_netplay_rb_confirmed_frontier(uint32_t from_tick);

/* Contiguous confirmed ticks still ahead of current sim (0 = exhausted). */
uint32_t psx_netplay_rb_confirmed_remaining(void);

/* §47 ownership step: tip-extend toward contiguous frontier while Replay
 * owns progress. Call after reconcile during resim (before finish_frame). */
void psx_netplay_rb_ownership_step(void);

/* Present-edge digest prep: copy `in` → `out` and clear PC. At vblank
 * finish_frame the host often parks cpu->pc=0 while a peer may still hold a
 * live BB PC; sticky substitutes were host-local and forked dig_cpu with
 * matched RAM/clk. GPRs/COP0/cycles remain. */
void psx_netplay_rb_cpu_for_present_digest(struct CPUState *out,
                                           const struct CPUState *in);

/* Diag */
uint32_t psx_netplay_rb_episode_count(void);
int      psx_netplay_rb_phase(void); /* RNetRbPhase cast to int */
uint32_t psx_netplay_rb_snap_count(void);

/* Ticks armed into Replay since the last call (counter resets on read). Used
 * by the host [FPS] line (PSX_NETPLAY_TIMING=1) to report what fraction of
 * the window was spent resimulating vs running Live. */
uint64_t psx_netplay_rb_take_replay_ticks(void);
/* §56: cumulative replay ticks (never reset) — scheduler scorecard deltas. */
uint64_t psx_netplay_rb_replay_ticks_total(void);

/* Live network-latency estimate (ms), EMA'd from the POST handshake's own
 * send/receive timestamps each episode commit; 0 = no episode has round-
 * tripped yet this session. Used by np_invent_grace_stall() (psx_netplay.c)
 * to size its "wait before hold-last invent" budget from actual measured
 * link latency instead of the local sim's own tick-to-tick cadence — see
 * docs/ROLLBACK_MOTK_HOOKUP.md section 12. */
uint32_t psx_netplay_rb_rtt_estimate_ms(void);

/* Sticky BB-edge resume PC (MotK wait-loop canonicalize). Used by
 * gpu_vblank_flush_present when IRQ resume latches are 0. */
uint32_t psx_netplay_rb_sticky_bb_pc(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_RB_H */
