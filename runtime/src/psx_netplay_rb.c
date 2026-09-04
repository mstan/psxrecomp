#include "psx_netplay_rb.h"
#include "cpu_state.h" /* full CPUState — stub path assigns *out = *in */

#if !defined(PSX_HAS_RECOMP_NET)
void psx_netplay_rb_bind(const PsxNetplayRbBindings *b) { (void)b; }
void psx_netplay_rb_start(void) {}
void psx_netplay_rb_shutdown(void) {}
void psx_netplay_rb_cold_reset(void) {}
void psx_netplay_rb_poll(struct CPUState *cpu, uint32_t resume_pc)
{
    (void)cpu;
    (void)resume_pc;
}
void psx_netplay_rb_flush_resume(void) {}
int psx_netplay_rb_top_level_resume_active(void) { return 0; }
int psx_netplay_rb_recover_null_pc(struct CPUState *cpu, uint32_t *out_pc)
{
    (void)cpu;
    (void)out_pc;
    return 0;
}
void psx_netplay_rb_request_snap(uint32_t tick) { (void)tick; }
int psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot)
{
    (void)mismatch_tick;
    (void)slot;
    return 0;
}
int psx_netplay_rb_tip_extend(uint32_t mismatch_tick, int slot)
{
    (void)mismatch_tick;
    (void)slot;
    return 0;
}
int psx_netplay_rb_rewind_suppressed(void) { return 0; }
int psx_netplay_rb_fmv_defer_rewind(void) { return 0; }
int psx_netplay_rb_fmv_media_active(void) { return 0; }
int psx_netplay_rb_lockstep_no_invent(void) { return 0; }
int psx_netplay_rb_fmv_settle_active(void) { return 0; }
int psx_netplay_rb_fmv_desync_hold(void) { return 0; }
void psx_netplay_rb_clear_fmv_desync_hold(const char *why) { (void)why; }
void psx_netplay_rb_request_post_fmv_heal_kf(void) {}
int psx_netplay_rb_post_fmv_heal_eligible(void) { return 0; }
int psx_netplay_rb_fmv_episode_unsafe(uint32_t tick) { (void)tick; return 0; }
int psx_netplay_rb_media_kf_probe_match(uint32_t size, uint32_t crc)
{
    (void)size;
    (void)crc;
    return 0;
}
int psx_netplay_rb_media_kf_on_ready(const void *data, size_t size)
{
    (void)data;
    (void)size;
    return 0;
}
int psx_netplay_rb_media_kf_busy(void) { return 0; }
void psx_netplay_rb_poll_replay_stall(void) {}
int psx_netplay_rb_take_promote_sweep(void) { return 0; }
int psx_netplay_rb_fmv_unlock_grace_active(void) { return 0; }
void psx_netplay_rb_pump(void) {}
int psx_netplay_rb_boot_dig0_gate(void) { return 0; }
void psx_netplay_rb_boot_dig0_note_local(uint32_t core) { (void)core; }
void psx_netplay_rb_boot_dig0_note_peer(uint32_t core) { (void)core; }
int psx_netplay_rb_active(void) { return 0; }
int psx_netplay_rb_is_resimulating(void) { return 0; }
int psx_netplay_rb_tip_holding(void) { return 0; }
uint32_t psx_netplay_rb_episode_target(void) { return 0; }
uint32_t psx_netplay_rb_tip_hold_invent_slack(void) { return 0; }
int psx_netplay_rb_tip_hold_rereplay_pending(void) { return 0; }
uint32_t psx_netplay_rb_tip_hold_rereplay_from(void) { return 0; }
uint32_t psx_netplay_rb_tip_runway(void) { return 0; }
void psx_netplay_rb_tip_hold_block_quiet(int block) { (void)block; }
int psx_netplay_rb_ignore_peer_frame_commit(uint32_t tick, uint32_t hash)
{
    (void)tick;
    (void)hash;
    return 0;
}
int psx_netplay_rb_abort_resim_core_mismatch(uint32_t tick, uint32_t local_core,
                                             uint32_t peer_core)
{
    (void)tick;
    (void)local_core;
    (void)peer_core;
    return 0;
}
int psx_netplay_rb_load_pending(void) { return 0; }
uint32_t psx_netplay_rb_baseline_fork_cap(void) { return 0; }
int psx_netplay_rb_try_admit(void) { return 0; }
void psx_netplay_rb_finish_frame(void) {}
uint32_t psx_netplay_rb_confirmed_frontier(uint32_t from_tick)
{
    (void)from_tick;
    return 0;
}
uint32_t psx_netplay_rb_confirmed_remaining(void) { return 0; }
void psx_netplay_rb_ownership_step(void) {}
void psx_netplay_rb_cpu_for_present_digest(struct CPUState *out,
                                           const struct CPUState *in)
{
    if (!out || !in)
        return;
    *out = *in;
    out->pc = 0;
}
uint32_t psx_netplay_rb_episode_count(void) { return 0; }
uint64_t psx_netplay_rb_take_replay_ticks(void) { return 0; }
uint64_t psx_netplay_rb_replay_ticks_total(void) { return 0; }
int psx_netplay_rb_phase(void) { return 0; }
uint32_t psx_netplay_rb_snap_count(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
uint32_t psx_netplay_rb_rtt_estimate_ms(void) { return 0; }
#else

#include "netplay_hash_confirm.h"
#include "netplay_input_hist.h"
#include "netplay_rb_post.h"
#include "netplay_snap_ring.h"
#include "netplay_state_digest.h"
#include "psx_netplay.h"
#include "psx_netplay_sched.h"
#include "host_time.h"
#include "boot_state.h"
#include "cdrom.h"
#include "cpu_state.h"
#include "crc32.h"
#include "gpu.h"
#include "gpu_vram_dirty.h"
#include "interrupts.h"
#include "mdec.h"
#include "sio.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"
#include "savestate.h"
#include "spu.h"

#include "recomp_net/recomp_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PsxNetplayRbBindings g_b;
static int g_bound;
static RNetRbSession *g_rb;
static NetplaySnapRing *g_snaps;
/* Episode counter (NOT the wire epoch). Wire epoch ids are partitioned by
 * initiator slot — epoch = (counter << RB_EPOCH_SLOT_BITS) | initiator_slot —
 * so concurrent dual-initiation can never collide on an id and any epoch's
 * initiator is derivable for the tie-break. See rb_epoch_alloc(). */
static uint32_t g_epoch;
#define RB_EPOCH_SLOT_BITS 3u
#define RB_EPOCH_SLOT_MASK 7u
static uint32_t g_episode_count;
/* Ticks armed into Replay, accumulated for the host's periodic [FPS] line
 * (psx_netplay_rb_take_replay_ticks resets on read). Lets a soak confirm
 * "we spent NN% of this window resimulating" instead of inferring it from
 * episode density in the raw log. */
static uint64_t g_stat_replay_ticks;
/* §56 scorecard: cumulative (never reset on read) replay-tick counter so the
 * scheduler scorecard can compute per-window deltas independently of the FPS
 * logger's take-and-reset counter above. */
static uint64_t g_stat_replay_ticks_total;
static int g_pending_save_valid;
static uint32_t g_pending_save_tick;
/* Cleared in rb_start — function-locals survived rematch and hid snap logs. */
static int s_snap_fail_logged;
static int s_snap_ok_logged;
static int s_snap_pc_reject_logged;
static int s_snap_stale_logged;

/* Cleared in rb_start — function-locals survived rematch and hid snap logs. */
static int g_pending_load_valid;
static uint32_t g_pending_load_tick;
/* §75: highest tick whose ring snap was written by sealed Replay (or an
 * explicit pin refresh). Live invent must not overwrite these — tip-extend
 * rereplay was reloading invent-poisoned snaps and forking (SNAP-STALE). */
static uint32_t g_auth_snap_through;
/* Set after apply-from-pump/admit; flushed via psx_netplay_rb_flush_resume. */
static int g_pending_resume_valid;
static uint32_t g_pending_resume_pc;
/* §112: load==target (apply-only / post-FMV heal) — defer enter_verify_at_tip
 * until flush_resume so longjmp arms top-level resume before Verify/Live.
 * Entering Verify first blocked flush (Replay-only gate) then finalize cleared
 * pending → leaf PC=0 / "execution completed" (Win↔Linux TM4 loading). */
static int g_empty_span_verify_pending;
/* 1 only after a successful ring load for this episode — blocks baseline/replay
 * until restore (peer BASELINE can arrive while we are still sealing). */
static int g_episode_snap_applied;

/* Live-path snap every N ticks (raw boot_state ~3.5MB, no zlib). Resim still
 * snaps every tick so episode baselines stay dense.
 * Override: PSX_NET_SNAP_INTERVAL (default 16 — was 8; live memcpy was a
 * major FPS tax stacked on per-frame RAM digests). */
static uint32_t snap_interval(void)
{
    static int latched;
    static uint32_t iv = 16u;
    if (!latched) {
        const char *e = getenv("PSX_NET_SNAP_INTERVAL");
        if (e && e[0]) {
            unsigned v = 0;
            if (sscanf(e, "%u", &v) == 1 && v >= 1u && v <= 32u)
                iv = v;
        }
        latched = 1;
    }
    return iv;
}

static int rb_media_kf_enabled(void); /* §97 — defined near episode wire clear */
static void rb_arm_post_fmv_heal_sticky(uint32_t tip, uint32_t fork_tick);
static void rb_clear_post_fmv_heal_sticky(const char *why);
static void rb_post_fmv_platform_nondet_enter(uint32_t keep_tick, uint32_t fork_tick,
                                             uint32_t live_sim, const char *why);
static void rb_clear_post_fmv_platform_nondet(const char *why);
static void rb_post_fmv_platform_accept(uint32_t through, uint32_t fork,
                                       const char *why);
static int rb_post_fmv_heal_eligible(void);
static int rb_fmv_media_active(void); /* §96 — used by ownership budget during FMV */

/* §96/§98: FMV media snap interval. Default 4; MEDIA_KF uses 2 (near-tip
 * loads without every-tick 0.7 ms snap tax). Override: PSX_NET_FMV_SNAP_INTERVAL. */
static uint32_t fmv_media_snap_interval(void)
{
    static int latched;
    static uint32_t iv = 4u;
    if (!latched) {
        const char *e = getenv("PSX_NET_FMV_SNAP_INTERVAL");
        if (e && e[0]) {
            unsigned v = 0;
            if (sscanf(e, "%u", &v) == 1 && v >= 1u && v <= 16u)
                iv = v;
        } else if (rb_media_kf_enabled()) {
            iv = 2u;
        }
        latched = 1;
    }
    return iv;
}

/* §58 dense tip snaps: live saves every tick inside a sliding window so a
 * near-tip mispredict loads at (or within a tick of) the mismatch instead of
 * up to snap_interval-1 ticks below it (§57 soak: depth_avg 14–41 with
 * pred_depth 1–2 — replay work was dominated by snap granularity). The
 * window is bounded: non-interval ticks older than the window are dropped
 * from the ring so interval history is not evicted. PSX_NET_SNAP_DENSE
 * overrides the window size (0 disables, max 24). */
#define NP_DENSE_SNAP_MAX 24u
static uint32_t g_tip_dense_fifo[NP_DENSE_SNAP_MAX];
static uint32_t g_tip_dense_fifo_n;
static uint32_t g_tip_dense_fifo_head;

/* TipHold deferred-rereplay state — declared before tip_dense_push (§79
 * protects matched-prefix snaps while pending). */
static int g_tip_hold_rereplay_pending;
static uint32_t g_tip_hold_rereplay_from; /* inclusive tip to reload */
static uint32_t g_tip_hold_rereplay_mismatch; /* §78 earliest coalesce miss */

static uint32_t tip_dense_window(void)
{
    static int latched;
    static uint32_t win = 8u;
    if (!latched) {
        const char *e = getenv("PSX_NET_SNAP_DENSE");
        if (e && e[0]) {
            unsigned v = 0;
            if (sscanf(e, "%u", &v) == 1 && v <= NP_DENSE_SNAP_MAX)
                win = v;
        }
        latched = 1;
    }
    return win;
}

static void tip_dense_reset(void)
{
    g_tip_dense_fifo_n = 0;
    g_tip_dense_fifo_head = 0;
}

static int g_local_baseline_sent;
static uint32_t g_local_baseline_digest;
static uint32_t g_local_baseline_av;
static uint32_t g_local_baseline_aux; /* dig_c = ext = crc(aux,cd,spad,dma,sio) */
static int g_peer_baseline_ok;
static uint32_t g_peer_baseline_digest;
static uint32_t g_peer_baseline_av;
static uint32_t g_peer_baseline_aux;
static int g_peer_baseline_ready; /* dig_a flag: follower ACK after it has our baseline */
static int g_local_baseline_ready_sent;
static int g_local_post_sent;
static int g_peer_post_ok;
static uint32_t g_peer_post_digest;
static uint8_t g_peer_post_match;
static uint32_t g_post_digest;
static uint32_t g_post_target; /* tip we POSTed; peer POST must match */
static uint32_t g_post_av;
static uint32_t g_peer_post_av;
static int g_needs_advance;
static int g_seal_export_logged;
static int g_baseline_rexmit_logged;
static int g_post_rexmit_logged;
/* §52: seal-row keepalive — export_local_seals() fired only on discrete
 * events (begin / tip-extend / FOLLOW); one lost UDP chunk left the peer
 * silently stuck in AwaitingBaseline (all_peer_seal_rows_complete never
 * true) until the initiator's verify timeout aborted the episode. */
static uint64_t g_last_seal_rexmit_ms;
static int g_seal_rexmit_logged;
static int g_seal_wait_logged;
static uint64_t g_seal_wait_ms; /* CLOCK_MONOTONIC ms when SealInputs began */
static uint64_t g_verify_wait_ms;
/* Live network-latency estimate, EMA'd from the POST handshake's own
 * send/receive timestamps (no new wire message — reuses g_verify_wait_ms,
 * already recorded for the verify-timeout mechanism). Sampled once per
 * episode commit: elapsed time from "I sent my POST" to "peer's POST
 * arrived here" when I sent first (the common case), which is dominated by
 * one-way transit of the peer's packet plus its own send-side scheduling
 * jitter — a genuine measurement of *the link*, unlike the local sim's own
 * tick-to-tick cadence. See docs/ROLLBACK_MOTK_HOOKUP.md section 12
 * ("size wait-before-invent budgets from RTT, not local tick cadence") for
 * why this replaced np_invent_grace_stall's old tick-cadence-based scaling
 * term in psx_netplay.c. 0 = no sample yet (very start of a session, before
 * the first episode has round-tripped). */
static uint32_t g_rb_rtt_ema_ms;
static uint64_t g_replay_progress_ms; /* last arm / finish_frame */
static uint32_t g_last_good_bb_pc; /* sticky BB-edge resume for snaps / digest */
/* Once both peers publish dig0, stay open even if HC later drops tick-0
 * (hc_prime / episode abort). Re-checking HC caused sim=25 boot_dig0_wait hangs. */
static int g_boot_dig0_synced;
static int g_boot_dig0_mismatch_logged;
/* Sticky dig0 cores — HC ring is only 128 deep and rematch invent/FC flood
 * can reuse slot 0 before the peer's dig0 lands. Gate must not depend on HC. */
static uint32_t g_boot_dig0_local_core;
static uint8_t g_boot_dig0_local_valid;
static uint32_t g_boot_dig0_peer_core;
static uint8_t g_boot_dig0_peer_valid;
static uint32_t g_boot_dig0_last_rexmit_ms;
static int g_boot_dig0_wait_kind_logged;
static int g_follow_nack_pending;
static uint32_t g_follow_nack_epoch;
static uint32_t g_follow_nack_mismatch;
static uint32_t g_follow_nack_load;
static uint32_t g_follow_nack_target;
static int g_follow_nack_slot;
static int g_follow_nack_sends;
/* BASELINE that arrived before follow/begin (TURN race) — applied on episode open. */
static int g_stash_bl_valid;
static uint32_t g_stash_bl_epoch;
static uint32_t g_stash_bl_load;
static uint32_t g_stash_bl_dig_m;
static uint32_t g_stash_bl_dig_a;
static uint32_t g_stash_bl_dig_b;
static uint32_t g_stash_bl_dig_c;
static int g_stash_bl_logged;
/* SEAL_ROWS that arrived before the local episode opened (same TURN/UDP
 * reordering race as the baseline stash above — the initiator calls
 * export_local_seals() immediately after sending SYNC BEGIN, and a burst of
 * chunk packets can beat a delayed/reordered SYNC through NAT relay). Small
 * ring since export_local_seals() only ever emits a handful of chunks per
 * episode open; entries not consumed by the matching begin_follower/tip
 * extend are just dropped on the next episode (epoch mismatch). */
#define RB_STASH_SEAL_ROWS_MAX 8
static struct {
    int valid;
    uint32_t epoch;
    uint32_t mismatch;
    uint32_t target;
    uint8_t slot;
    uint32_t row_begin;
    RNetRbFrame rows[24];
    uint16_t row_count;
} g_stash_seal_rows[RB_STASH_SEAL_ROWS_MAX];
static int g_stash_seal_rows_logged;
/* §60: SYNC BEGIN that arrived while we were still in an active episode
 * (ownership-chain race — initiator finishes Verify+POST and emits the next
 * BEGIN before our POST drain runs). Was silently dropped; follower then
 * waited forever for FOLLOW. Latest-wins stash; applied once idle. */
static int g_stash_begin_valid;
static uint32_t g_stash_begin_epoch;
static uint32_t g_stash_begin_mismatch;
static uint32_t g_stash_begin_load;
static uint32_t g_stash_begin_target;
static int g_stash_begin_slot;
static uint8_t g_stash_begin_flags;
static int g_stash_begin_logged;
static uint64_t g_last_begin_rexmit_ms;
static int g_begin_rexmit_logged;
static uint64_t g_baseline_handshake_ms; /* when local baseline first sent */
static uint64_t g_last_baseline_burst_ms;
static int g_ready_timeout_logged;
static int g_wait_peer_bl_logged;
/* Last committed target — Live realign after POST/baseline abort. */
static int g_agreed_valid;
static uint32_t g_agreed_through;
/* Load tick of the last committed episode. Both peers replayed
 * [span_lo+1 .. agreed_through] and resim saves a snap at every replayed tick,
 * so any ring snap inside that span is guaranteed to exist on the peer too. */
static uint32_t g_agreed_span_lo;
/* Highest tick the peer advertised via RNET_RB_RESOLVED (tip-hold enter/commit,
 * episode commit, or §40 ADVANCE advertise). Distinct from
 * rnet_rb_resolved_through(), which also rises on *local* tip-hold/commit.
 * Initiator begin must not open load above this until the peer has caught up —
 * otherwise ADVANCE→begin races the follower's agreed and NACK-storms
 * (2026-08-02 post-§39 soak). */
static uint32_t g_peer_resolved_through;
/* §41: wall-clock when choose_load/begin first blocked on peer RESOLVED
 * (0 = not waiting). After RB_MOTK_PEER_RESOLVED_GATE_MS, force-open —
 * unbounded defer silently desynced Live (2026-08-02 post-§40 soak). */
static uint64_t g_peer_resolved_wait_t0_ms;
/* Sticky: gate timed out; stay open until peer RESOLVED advances. */
static int g_peer_resolved_gate_expired;
/* Last HC-confirmed / tip-hold watermark we burst on the wire (heartbeat). */
static uint32_t g_local_advertised_through;
static uint64_t g_peer_resolved_hb_last_ms;
/* 1: pending load is a Live realign (not an episode baseline). */
static int g_live_realign_pending;
static uint32_t g_episode_load_tick; /* captured for post-diverge realign */
static int g_episode_baseline_matched;
/* Frozen copy of the episode baseline snap — resim must not clobber it in the
 * ring (finish_frame request_snap overwrote load_tick and poisoned realign). */
static uint8_t *g_pin_data;
static size_t g_pin_size;
static uint32_t g_pin_tick;
static int g_pin_valid;

/* §97: FMV media keyframe episode — host forces identical snap at load_tick
 * before Replay (probe CRC, transfer on miss). Default ON; set
 * PSX_NET_FMV_MEDIA_KF=0 to restore §93 refuse / invent-off during media.
 * §100: invent stays off during live media (lockstep); KF still heals real
 * rewinds. Present hold-last while probe/xfer runs.
 * §109: also armed for post-FMV silent hc-fork (apply-only target=load). */
static int g_media_kf_episode;
static int g_media_kf_ready;      /* pin holds authoritative KF for load */
static int g_media_kf_host_armed; /* host started probe/send for this episode */
static int g_media_kf_sending;    /* host transfer in flight */
/* §109/§114: this episode is a post-FMV / DESYNC apply-only MEDIA_KF heal
 * (target=load). Verify-span tip+1 was abandoned — platform nondet. */
static int g_post_fmv_heal_kf;
/* §109: hc-fork / resim-escalate asked for heal on next begin. */
static int g_request_post_fmv_heal;
/* §110: sim < until → re-heal eligible + invent hold after heal RELEASE. */
static uint32_t g_post_fmv_heal_sticky_until;
/* §110: mismatch tick of last heal (diagnostics / sticky arm / loop cap). */
static uint32_t g_post_fmv_heal_fork_tick;
/* §110b: consecutive heal begins at g_post_fmv_heal_loop_fork. */
static uint32_t g_post_fmv_heal_loop_fork;
static uint32_t g_post_fmv_heal_loop_count;
/* §114: matched-baseline tip+1 nondeterminism — invent stays off until HC
 * rematch; initiator streams apply-only KF; realign keeps the matched pin
 * (not load-1 Live-walk). */
static int g_post_fmv_platform_nondet;
static uint32_t g_post_fmv_platform_keep_tick;
static uint32_t g_platform_kf_next_sim;
/* §115: KF stream opens while still pinned at keep_tick (livelock counter). */
static uint32_t g_platform_kf_stream_count;
/* §115: tip+1 fork accepted (HC primed past it) — refuse re-heal/stream. */
static uint32_t g_platform_accepted_fork;

/* §100: recent FMV media snap CRCs — prefer choose_load ticks we already
 * sealed, and let probe_match confirm without a ring edge-case miss. */
#define RB_MEDIA_CRC_RING 32u
typedef struct RbMediaCrc {
    uint32_t tick;
    uint32_t size;
    uint32_t crc;
} RbMediaCrc;
static RbMediaCrc g_media_crc[RB_MEDIA_CRC_RING];
static uint32_t g_media_crc_head;
static uint32_t g_media_crc_n;
/* Highest FMV media tick whose CRC was probe-matched (peer aligned). */
static uint32_t g_media_crc_peer_match_through;

/* §67 resim audit digest cache (fin state per replayed tick) — declared here
 * so pin_baseline_from_cpu can compare pin digest vs audit fin (§78 PIN-SKEW
 * diag). See log_resim_tick_audit for the cache contract. */
static uint32_t s_resim_audit_core;
static uint32_t s_resim_audit_sim;
static int s_resim_audit_valid;
/* After abort/realign only: refuse new episodes for N sim ticks (promote wire).
 * Successful commit does NOT arm this — late wire must open another episode. */
static uint32_t g_rewind_cooldown_until;
static int g_promote_sweep;
static uint32_t g_last_begin_mismatch = 0xffffffffu;
/* digest_a bit: follower has peer baseline + is entering / in Replay */
#define RB_BL_FLAG_READY 1u
#define RB_SEAL_TIMEOUT_MS 4000u
#define RB_VERIFY_TIMEOUT_MS 4000u
#define RB_REPLAY_STALL_MS 5000u
#define RB_FOLLOW_NACK_REXMIT 24
#define RB_BASELINE_BURST 8
#define RB_BASELINE_BURST_MS 40u
/* After one peer accepts the other's POST and enters TipHold it used to stop
 * retransmitting its own POST. On a single UDP loss the lagging peer then
 * sat in Verify until RB_VERIFY_TIMEOUT_MS and aborted — while the winner
 * had already advanced agreed_through to the unconfirmed tip. Burst POST +
 * RESOLVED on tip-hold entry so the lagging peer can still complete Verify
 * (see 2026-08-01 soak: tip=1437 peer_ok race → verify timeout → NACK storm). */
#define RB_POST_BURST 8
/* Initiator waits for follower ready-ACK; do NOT solo-enter Replay. */
#define RB_READY_TIMEOUT_MS 4000u
/* §49: WAN/TURN baseline GO can take multiple RTT; floor stays 4s. */
#define RB_READY_TIMEOUT_MAX_MS 12000u
/* §49/§73/§86/§104: ownership chain min confirmed ticks ahead of tip. tip+1
 * (§48) opened SPAN every Verify on WAN. tip+8 tip-held every POST on LAN
 * tip+4/+5 (§73/§144). Default floor tip+6; §86 also raises to max(floor, D-1)
 * so Force-TURN delay cushion is not treated as catch-up backlog. Env
 * PSX_RB_CHAIN_MIN_AHEAD (when set) overrides both. */
/* §104: floor tip+6 — session-144 fight soak chained tip+4/+5 every POST
 * (ownership final / hop storms → 12–79% replay windows). tip+6 still leaves
 * D≈5–7 cushion as Live exit, not SPAN. Env PSX_RB_CHAIN_MIN_AHEAD overrides. */
#define RB_OWNERSHIP_CHAIN_MIN_AHEAD 6u
/* §85: Replay must not chase Live forever. With Force-TURN D≈6–7 the
 * post-match frontier stays tip+5..+8 (> MIN_AHEAD), so §47 chaining +
 * mid-Replay tip-extends treadmilled ~800 ticks (audio mute / OBS cutout).
 * Cap chain hops and total tip advance from the first chain of a recovery;
 * freeze each episode's catch-up target so ownership_step cannot raise it
 * at wire pace (§74). Env: PSX_RB_CHAIN_MAX_HOPS / PSX_RB_CHAIN_MAX_TICKS. */
#define RB_OWNERSHIP_CHAIN_MAX_HOPS 2u
#define RB_OWNERSHIP_CHAIN_MAX_TICKS 48u
/* §86 C / §104: after BUDGET→Live, suppress re-chain for ~one tip-runway /
 * 800ms so fight button chatter does not immediately restart hop1.
 * Env: PSX_RB_CHAIN_SUPPRESS_TICKS / _MS. */
#define RB_OWNERSHIP_CHAIN_SUPPRESS_TICKS 48u
#define RB_OWNERSHIP_CHAIN_SUPPRESS_MS 800u
/* Failed episode (resim/baseline/post): calm from *live* sim before realign.
 * Was 60/120 — after abort, choose_load stuck at agreed tip so the next begin
 * resimmed the whole cooldown (depth 63→128) = "pushing further back". */
#define RB_ABORT_COOLDOWN_TICKS 24u
/* Repeated abort streak — longer promote-only (char-select storm). */
#define RB_STORM_COOLDOWN_TICKS 48u
/* §42 P4: consecutive same-tick baseline-mismatch aborts before declaring
 * an unrecoverable fork (loud DESYNC log, keep-live, long cooldown). */
#define RB_FORK_STORM_LIMIT 4u
#define RB_FORK_STORM_COOLDOWN_TICKS 600u
/* §93: matched-baseline resim diverge on the same sim twice is proven
 * nondeterminism (FMV/CD) — do not realign to the same pin again. */
#define RB_RESIM_DIVERGE_STORM_LIMIT 2u
/* After a genuine POST diverge where Live successfully realigned, do not
 * promote-only-block the next edges — cooldown was preventing the fork from
 * self-correcting (2026-08-01 §17 soak: tip=9203 abort → promote-no-resim
 * until 9228 while live digs stayed forked). Storm streak still cools. */
#define RB_POST_REALIGN_COOLDOWN_TICKS 0u
/* Cap initiator begin/follow-open resim span. Deep catch-up after abort is
 * chunked (commit → next episode) instead of one 120-tick Replay. */
#define RB_MAX_RESIM_SPAN 24u
/* Keep deferring a few frames after MDEC goes quiet (cutover / credits). */
#define RB_FMV_MDEC_HYSTERESIS 8u
/* After leaving FMV: defer rewind briefly so cutover heals. */
#define RB_FMV_SETTLE_TICKS 24u
/* Post-FMV no-invent lockstep (title Start / first menu safe). Floor keeps
 * invent off for at least MIN even when digests already agree; if cores have
 * not matched for CONFIRM contiguous ticks by then, hold until they do or
 * MAX. MotK soak: MIN=90 unlocked invent at title→menu and invent≠Cross at
 * +14 opened a 0x8006CDA0 tip storm — MIN=180 (~3s) covers first menu taps. */
#define RB_FMV_LOCKSTEP_MIN 180u
#define RB_FMV_LOCKSTEP_MAX 300u
#define RB_FMV_LOCKSTEP_CONFIRM 16u
/* §94: MAX-unmatched DESYNC invent-hold must not last forever. Soft-forked
 * digests never rematch CONFIRM ticks — invent stays off, then a SAVE/hitch
 * tip hole deadlocks admit (soak: stall=fmv_settle → lobby). Expire invent
 * hold after this many sim ticks; episode media-range refuse is separate. */
#define RB_FMV_DESYNC_INVENT_EXPIRE 600u
/* After cores match (or MAX): keep invent off this many more ticks so admit
 * waits for wire. Prevents hold-last sticky Up (pub=ffef vs wire=ffff) from
 * inventing at unlock+1 → FIRST CORE DIVERGE / tip episode. Matches dense
 * tip-snap +32 after invent unlock. Soak: grace=32 still saw Cross invent
 * at invent_at+1 (896) — bump to 64. */
#define RB_FMV_UNLOCK_GRACE 64u
/* §110/§114: after apply-only heal Live, keep re-heal + invent-hold (via
 * §114 platform nondet DESYNC, not lockstep RELEASE) so loading hitch
 * cannot tip-SPAN. */
#define RB_FMV_HEAL_STICKY_TICKS 300u
/* §110b/§114: verify-span past load used to catch tip+1, but matched-baseline
 * resim of tip+1 is Win↔Linux platform nondeterminism (+10 cyc @1036 soak) —
 * heal always aborts. §114 heals are apply-only (target=load); this span is
 * retained only as a diagnostic/legacy cap if a caller still raises target. */
#define RB_FMV_HEAL_VERIFY_SPAN 4u
/* §110b: apply-only heal loops at the same fork tick before §114 platform
 * nondet keep-live + KF stream (empty KF alone cannot fix tip+1 drift). */
#define RB_FMV_HEAL_LOOP_LIMIT 3u
/* §114: while platform nondet DESYNC is armed, initiator re-opens apply-only
 * MEDIA_KF this often (sim ticks) so both peers ride the host timeline
 * through loading instead of Live-walking a forked tip+1. */
#define RB_FMV_PLATFORM_KF_IV 16u
/* §115: same keep_tick re-stream limit — beyond this, prime HC past the
 * tip+1 fork and clear invent-hold (Win↔Linux loading-screen livelock). */
#define RB_FMV_PLATFORM_KF_STREAM_LIMIT 6u
/* MotK TipHold: quiet window so press→release→D-pad coalesce in one episode.
 * §27: seal slack uses library default (2) so Live may invent tip+1..tip+2
 * during tip-hold — FORCE0 parked presentation at the tip (mini delay-sync).
 * Runway is wall-clock frames at 60 Hz. */
#define RB_MOTK_TIP_RUNWAY 24u
/* TipHold deadlines (§27): brief coalesce, not a multi-hundred-ms park.
 * Quiet 80ms; thrash sooner when coalesce is busy; safety above quiet. */
/* §34: quiet must outlast micro-chatter idle holes (~2–5 ticks) without
 * approaching SAFETY; 80ms was committing between bounce pulses. */
#define RB_MOTK_TIP_HOLD_QUIET_MAX_MS 150u
#define RB_MOTK_TIP_HOLD_QUIET_MIN_MS 100u
#define RB_MOTK_TIP_HOLD_THRASH_COUNT 2u
#define RB_MOTK_TIP_HOLD_THRASH_WALL_MS 100u
#define RB_MOTK_TIP_HOLD_SAFETY_WALL_MS 250u
/* §35: SAFETY must not commit while digital still held (MotK key-repeat).
 * Absolute wall is the stuck-pad escape only. */
#define RB_MOTK_TIP_HOLD_ABSOLUTE_WALL_MS 2000u
/* §70: RACE+deferred-rereplay must not sit until ABSOLUTE (soak: 2s park at
 * tip=2562 while Live invent walked to sim=2610, then absorb 2562→2607 mega
 * flush). Cap race-pending defer so one flush lands once coalesce stalls.
 * §81: same wall for peer_past_tip + deferred (remote only +6 never hit
 * RACE_MARGIN 24 → ABSOLUTE 2000ms hitch). */
#define RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS 400u
/* §45: while SAFETY-deferred (digital held), if the peer tip races this many
 * ticks past our sealed tip, commit immediately. 2026-08-02 soak: slot1
 * SAFETY-committed ~250ms and kept Live while slot0 sat ABSOLUTE 2000ms on
 * stale held_remote at the sealed tip — remote_lead climbed to ~280, the
 * 128-entry wire ring aged out, then SPAN CAP / invent cliffs. One runway
 * of peer advance is enough proof they left TipHold. */
#define RB_MOTK_TIP_HOLD_RACE_MARGIN 24u
/* §82: remote this far past sealed tip (under RACE_MARGIN) still means
 * tip-extend FOLLOW is late/missing — clamp Absolute to RACE_PENDING so
 * we never sit ABSOLUTE 2000 (soak tip=6011 remote=+17 → guest invent cliff). */
#define RB_MOTK_TIP_HOLD_PEER_AHEAD_FLUSH 8u
/* §45: held_remote-only (we released; sealed tip still shows peer held)
 * Absolute escape — far shorter than the stuck-local-pad 2000ms wall.
 * Coalesce/tip-extend still owns the first SAFETY window. */
#define RB_MOTK_TIP_HOLD_REMOTE_HELD_WALL_MS 500u
/* §38: after tip-hold commit, suppress agreed ADVANCE so a still-tip-holding
 * peer (SAFETY deferred ~250ms) is not raced with load > frontier NACKs. */
#define RB_MOTK_POST_TIP_HOLD_AGREED_HOLDOFF_MS 300u
/* §41: max wait for peer RESOLVED before force-opening begin (NACK is
 * recoverable; silent forever-DEFER is not). */
#define RB_MOTK_PEER_RESOLVED_GATE_MS 500u
/* §41: retransmit last confirmed RESOLVED while peer lags. */
#define RB_MOTK_PEER_RESOLVED_HB_MS 100u
/* Baseline mismatch with no agreed tip — don't reopen every few ticks. */
#define RB_BASELINE_MISMATCH_COOLDOWN_TICKS 90u

/* §58/§79: track a live non-interval snap tick; evict the oldest dense snap
 * once the window is full (never the pinned baseline / episode load / commit
 * span — those stay until normal oldest-tick eviction).
 *
 * §79: TipHold Live can invent/wire-walk well past the default 8-tick dense
 * window before deferred flush (soak: prefer=3223 gone by sim=3232 → load tip
 * pin → SNAP-STALE → episode-pin depth). While TipHold owns the cursor,
 * retain the full NP_DENSE_SNAP_MAX capacity and never drop snaps above the
 * sealed tip (matched-prefix candidates for first_bad-1). */
static void tip_dense_push(uint32_t tick)
{
    uint32_t win = tip_dense_window();
    uint32_t protect_lo = 0u;
    if (win == 0u)
        return;
    if (g_tip_hold_rereplay_pending && g_tip_hold_rereplay_from != 0u)
        protect_lo = g_tip_hold_rereplay_from;
    else if (g_rb && rnet_rb_is_tip_holding(g_rb) && g_agreed_valid)
        protect_lo = g_agreed_through;
    if (protect_lo != 0u && win < NP_DENSE_SNAP_MAX)
        win = NP_DENSE_SNAP_MAX;
    while (g_tip_dense_fifo_n >= win) {
        uint32_t victim =
            g_tip_dense_fifo[(g_tip_dense_fifo_head + NP_DENSE_SNAP_MAX -
                          g_tip_dense_fifo_n) % NP_DENSE_SNAP_MAX];
        /* Matched-prefix / tip-hold invent snaps must survive until flush. */
        if (protect_lo != 0u && victim > protect_lo)
            break;
        g_tip_dense_fifo_n--;
        if (!(g_pin_valid && victim == g_pin_tick) &&
            !(g_rb && rnet_rb_is_active(g_rb) &&
              victim == rnet_rb_get_load_tick(g_rb)) &&
            !(g_agreed_valid && victim >= g_agreed_span_lo &&
              victim <= g_agreed_through)) {
            (void)netplay_snap_ring_drop_tick(g_snaps, victim);
        }
    }
    if (g_tip_dense_fifo_n >= NP_DENSE_SNAP_MAX)
        return; /* still request_snap-saves; ring holds protected prefix */
    g_tip_dense_fifo[g_tip_dense_fifo_head] = tick;
    g_tip_dense_fifo_head = (g_tip_dense_fifo_head + 1u) % NP_DENSE_SNAP_MAX;
    g_tip_dense_fifo_n++;
}

static int g_was_in_fmv;
static uint32_t g_fmv_settle_until;
static uint32_t g_fmv_lockstep_until;
static uint32_t g_fmv_media_end_sim;
static uint32_t g_fmv_core_match_streak;
static int g_fmv_lockstep_released; /* sticky: never re-lock after invent on */
/* Inclusive last tick both peers dense-snapped (media/lockstep/+tip). */
static uint32_t g_fmv_dense_through;
/* §93: media bout range (sim ticks). Begin/follow refuse loads/mismatches
 * inside [lo, unsafe_hi] even when Live tip has already left media — that
 * was the hole that opened ep8 load=176 / ep24 load=736 into FMV state. */
static uint32_t g_fmv_media_lo;
static uint32_t g_fmv_media_hi;
/* §93: FMV lockstep hit MAX with streak < CONFIRM — cores never agreed.
 * Keep invent/begin off until cores rematch CONFIRM ticks or session reset. */
static int g_fmv_unmatched_desync;
static uint32_t g_fmv_unmatched_desync_arm_sim;
/* §93: consecutive resim-core aborts on the same diverge sim. */
static uint32_t g_resim_diverge_tick;
static uint32_t g_resim_diverge_streak;
static uint32_t g_bl_mismatch_streak;
static uint32_t g_tip_hold_until;
/* TipHold invent-cap quiet: wall-clock ms when we first stalled at tip+slack
 * with pads idle and no pending wire edge (0 = not armed). */
static uint64_t g_tip_hold_quiet_t0_ms;
/* §34: coalesce/begin saw a wire edge TipHold could not absorb yet — do not
 * quiet-commit (was dropping releases → ghost re-press episodes). */
static int g_tip_hold_block_quiet;
/* Wall-clock enter time for TipHold deadlines (0 = inactive). */
static uint64_t g_tip_hold_enter_ms;
/* §38: wall-clock when tip-hold last committed (0 = none). ADVANCE holdoff. */
static uint64_t g_post_tip_hold_holdoff_t0_ms;
/* TipHold coalesce/tip-extend count this episode (thrash WALL trigger). */
static uint32_t g_tip_hold_coalesce_n;
/* g_tip_hold_rereplay_* live near tip_dense_fifo (§79). */
/* Mid-span tip-extend rereplay: reload snap without replacing episode pin. */
static int g_tip_extend_rereplay;
/* Inclusive tip of last tip-extend hc_prime; drop stale TipHold invent
 * FRAME_COMMITs above this until Verify/TipHold. 0 = inactive. */
static uint32_t g_tip_extend_prime_tick;
/* §42 P2: matched tip we extended FROM (TipHold/Verify coalesce-ahead).
 * If the peer's RESOLVED lands exactly here while we sit in Verify at the
 * extended tip, the peer committed at the old tip and left the episode —
 * our extension can never verify. Abandon it and realign to this tick
 * (both cores matched there) instead of riding into verify timeout and a
 * forked realign (2026-08-02 soak: 986 vs 944 permanent fork). 0 = none. */
static uint32_t g_tip_extend_from_tick;
/* §42 P1: epoch of the last episode we committed (tip-hold or legacy).
 * A peer ABORT for this epoch after our unilateral commit means the peer
 * rewound while we kept the tip — honor its wire realign tick. */
static uint32_t g_last_commit_epoch = 0xffffffffu;
/* §42c: peer's OP_COMMIT (epoch, tick) — the unambiguous "I committed and
 * left the episode" signal. The RESOLVED watermark cannot serve here: it
 * also rises on live-play HC interval confirms, and feeding it into the
 * tip-extend abandon fired on a stale watermark (768), aborted a HEALTHY
 * converging episode and forked the cores (2026-08-02 soak #4). */
static uint32_t g_peer_commit_epoch = 0xffffffffu;
static uint32_t g_peer_commit_tick;
/* §42 P4: consecutive baseline-mismatch aborts realigned to the same tick.
 * The 2026-08-02 fork soak repeated abort→realign→reopen at one tick
 * forever; after RB_FORK_STORM_LIMIT repeats, declare DESYNC loudly,
 * keep Live running (no more rubber-band rewinds) and arm a long cooldown. */
static uint32_t g_fork_streak_tick;
static uint32_t g_fork_streak;
/* §55: a baseline core mismatch at load L proves the fork predates L. Retrying
 * L (2026-08-02 soak: 4× at 4176) can never heal — the next episode load must
 * be strictly below it. 0 = no cap; cleared on verified commit / session reset. */
static uint32_t g_bl_fork_cap;
/* §62: a follow NACK with the peer's frontier AHEAD of the refused load means
 * the peer's snap ring has evicted that tick — reopening at or below it can
 * only NACK again (2026-08-02 soak: bisect 10231 vs peer ring oldest ≈10284
 * → NACK → 129-tick demote → permanent WIRE_HOLE). Loads ≤ this floor are
 * refused; cleared on verified commit / session reset. */
static uint32_t g_peer_nack_floor;

static RNetSession *sess(void); /* fwd — used by FMV lockstep window */
static void commit_episode(void); /* fwd — legacy immediate commit */
static void enter_tip_hold(uint32_t target);
static void enter_verify_at_tip(uint32_t done);
static void finalize_tip_hold(void);
static void poll_tip_hold_finalize(void);
static void clear_tip_extend_prime(void);
static void clear_tip_hold_rereplay_pending(void);
static int flush_tip_hold_deferred_rereplay(void);
static void arm_rewind_cooldown_ticks(uint32_t sim, uint32_t ticks, const char *why);
static void clear_rewind_cooldown(const char *why);
static void rb_clear_fmv_desync_hold(const char *why);
static void tip_hold_try_finalize(void);
static void schedule_episode_rereplay(uint32_t prefer_plus_one);
static void ownership_on_post_match(uint32_t tip);
static void ownership_chain_next_span(uint32_t tip, uint32_t frontier);
static int seat_tick_confirmed(int slot, uint32_t tick);
static void advertise_agreed_resolved(uint32_t through);
static uint64_t rb_mono_ms(void);

/* §102: after peer-ahead follow-NACK, force one short light tip reopen and
 * suppress ownership chaining (session-140: NACK keep-live → hc-fork SPAN
 * 6640→6660 → hops 32/40 → 96% replay hitch). */
#define RB_PEER_AHEAD_LIGHT_DEPTH 4u
static uint32_t g_peer_ahead_force_load; /* 0 = off; consumed in begin_rewind */
static int g_peer_ahead_light_episode;   /* 1 while that light tip is open */

/* §103: past-frontier NACK with demote depth > this → keep-live (no deep
 * Live rewind). Session 142: demote 827→763 forked cores → MEDIA-KF 3.7MB. */
#define RB_NACK_DEEP_REALIGN_MAX 8u
/* Sticky: peer_resolved gate must not expire until the follower advertises
 * past the refused tip (force-open after GATE_MS reopened the NACK storm). */
static int g_past_frontier_sticky;

/* §47: begin_rewind is an ownership SPAN chain (skip cooldown; force target). */
static int g_ownership_chain;
static uint32_t g_ownership_chain_frontier;
/* §51 Layer 3: SPAN chunk continue — state already at tip after Verify; do
 * not snap-reload (Replay ownership is one continuous recovery). */
static int g_ownership_skip_snap;
/* §85: frozen tip for this Replay episode — ownership_step must not tip-extend
 * past arrivals that showed up after begin (wire-pace treadmill). 0 = unset. */
static uint32_t g_replay_catchup_cap;
/* §85: hops / origin tip for the current ownership recovery (cleared on
 * tip-hold / abort). */
static uint32_t g_ownership_chain_hops;
static uint32_t g_ownership_chain_origin_tip;
/* §86 C: do not chain while tip < until_tip OR wall ms not elapsed. */
static uint32_t g_chain_suppress_until_tip;
static uint64_t g_chain_suppress_until_ms;

static void ownership_clear_chain_budget(void)
{
    g_replay_catchup_cap = 0u;
    g_ownership_chain_hops = 0u;
    g_ownership_chain_origin_tip = 0u;
}

static uint32_t ownership_chain_max_hops(void)
{
    const char *e = getenv("PSX_RB_CHAIN_MAX_HOPS");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 0 && v <= 64)
            return (uint32_t)v;
    }
    /* §100: during live FMV media, at most one ownership hop after MEDIA-KF
     * (logs showed hop1/hop2 stack + present hitch). */
    if (rb_fmv_media_active())
        return 1u;
    return RB_OWNERSHIP_CHAIN_MAX_HOPS;
}

static uint32_t ownership_chain_max_ticks(void)
{
    const char *e = getenv("PSX_RB_CHAIN_MAX_TICKS");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 1 && v <= 512)
            return (uint32_t)v;
    }
    if (rb_fmv_media_active())
        return 8u;
    return RB_OWNERSHIP_CHAIN_MAX_TICKS;
}

/* §86/§87 B / §104: floor tip+6; raise toward D but leave one tick of
 * headroom (max(6, D-1)) so Force-TURN frontier≈tip+D still chains once
 * instead of tip-hold invent-parking every POST (soak §86: min_ahead=D →
 * tip+9 final tip-hold → dual-init hang). Env PSX_RB_CHAIN_MIN_AHEAD
 * overrides. */
static uint32_t ownership_chain_min_ahead(void)
{
    uint32_t min_ahead = RB_OWNERSHIP_CHAIN_MIN_AHEAD;
    const char *e = getenv("PSX_RB_CHAIN_MIN_AHEAD");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 1 && v <= 64)
            return (uint32_t)v;
    }
    if (g_b.input_delay && *g_b.input_delay > 1) {
        uint32_t d1 = (uint32_t)(*g_b.input_delay - 1);
        if (d1 > min_ahead)
            min_ahead = d1;
    }
    return min_ahead;
}

static uint32_t ownership_chain_suppress_ticks(void)
{
    const char *e = getenv("PSX_RB_CHAIN_SUPPRESS_TICKS");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 0 && v <= 256)
            return (uint32_t)v;
    }
    /* §100: longer post-BUDGET quiet during intro FMV. */
    if (rb_fmv_media_active())
        return 48u;
    return RB_OWNERSHIP_CHAIN_SUPPRESS_TICKS;
}

static uint32_t ownership_chain_suppress_ms(void)
{
    const char *e = getenv("PSX_RB_CHAIN_SUPPRESS_MS");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 0 && v <= 5000)
            return (uint32_t)v;
    }
    if (rb_fmv_media_active())
        return 800u;
    return RB_OWNERSHIP_CHAIN_SUPPRESS_MS;
}

static void ownership_arm_chain_suppress(uint32_t tip)
{
    uint32_t ticks = ownership_chain_suppress_ticks();
    uint32_t ms = ownership_chain_suppress_ms();
    uint64_t now = rb_mono_ms();
    if (ticks > 0u)
        g_chain_suppress_until_tip = tip + ticks;
    else
        g_chain_suppress_until_tip = 0u;
    if (ms > 0u) {
        g_chain_suppress_until_ms = now + (uint64_t)ms;
        if (g_chain_suppress_until_ms == 0ull)
            g_chain_suppress_until_ms = 1ull;
    } else {
        g_chain_suppress_until_ms = 0ull;
    }
    fprintf(stderr,
            "psxrecomp: rb ownership chain suppress until tip=%u +%ums "
            "(from tip=%u)\n",
            (unsigned)g_chain_suppress_until_tip, (unsigned)ms, (unsigned)tip);
    fflush(stderr);
}

static int ownership_chain_suppressed(uint32_t tip)
{
    int by_tip = 0;
    int by_ms = 0;
    if (g_chain_suppress_until_tip != 0u) {
        if (tip < g_chain_suppress_until_tip)
            by_tip = 1;
        else
            g_chain_suppress_until_tip = 0u;
    }
    if (g_chain_suppress_until_ms != 0ull) {
        uint64_t now = rb_mono_ms();
        if (now < g_chain_suppress_until_ms)
            by_ms = 1;
        else
            g_chain_suppress_until_ms = 0ull;
    }
    return by_tip || by_ms;
}

/* §86 A: leave Replay ownership for Live without TipHold invent-cap park.
 * Confirmed ticks past tip stay Live's job (hist/wire already present).
 * Immediate enter+finalize reuses commit/RESOLVED/COMMIT notice without
 * arming invent_slack coalesce (soak: that parked → yield begin thrash). */
static void ownership_exit_to_live(uint32_t tip, uint32_t frontier,
                                  const char *why, int arm_suppress)
{
    fprintf(stderr,
            "psxrecomp: rb ownership Live tip=%u frontier=%u (%s) — no TipHold\n",
            (unsigned)tip, (unsigned)frontier, why ? why : "?");
    fflush(stderr);
    /* §109/§114/§115: apply-only heal converged. First pin at the soft-fork
     * keep → invent OFF + KF stream. Once a later host-tip KF advances past
     * the keep/fork, accept the timeline (§115) instead of re-pinning forever. */
    if (g_post_fmv_heal_kf) {
        uint32_t fork = g_last_begin_mismatch;
        if (fork == 0xffffffffu)
            fork = g_post_fmv_heal_fork_tick;
        g_post_fmv_heal_kf = 0;
        g_fmv_core_match_streak = 0; /* need fresh CONFIRM after Live walk */
        if (g_post_fmv_platform_nondet &&
            g_post_fmv_platform_keep_tick > 0u &&
            tip > g_post_fmv_platform_keep_tick) {
            rb_post_fmv_platform_accept(tip, fork,
                                       "heal Live past platform keep");
        } else {
            rb_post_fmv_platform_nondet_enter(tip, fork, tip,
                                             "post-FMV heal KF Live");
        }
    }
    if (g_stash_begin_valid && g_rb &&
        g_stash_begin_epoch == rnet_rb_get_epoch_id(g_rb)) {
        g_stash_begin_valid = 0;
        g_stash_begin_logged = 0;
    }
    ownership_clear_chain_budget();
    if (g_rb && rnet_rb_is_tip_holding(g_rb)) {
        finalize_tip_hold();
    } else if (g_rb && rnet_rb_is_active(g_rb)) {
        enter_tip_hold(tip);
        if (g_rb && rnet_rb_is_tip_holding(g_rb))
            finalize_tip_hold();
    } else {
        g_episode_count++;
        g_agreed_through = tip;
        g_agreed_span_lo = tip;
        g_agreed_valid = 1;
        if (g_b.hc)
            netplay_hc_prime_after(g_b.hc, tip);
        advertise_agreed_resolved(tip);
        psx_netplay_timesync_on_episode_boundary();
    }
    if (arm_suppress)
        ownership_arm_chain_suppress(tip);
}

static int rb_fmv_media_active(void)
{
    if (gpu_display_is_depth24())
        return 1;
    if (mdec_recently_active(RB_FMV_MDEC_HYSTERESIS))
        return 1;
    /* Real XA audio stream only. Do NOT treat reading+(mode&0x48) alone as
     * media: MotK boot/LoadExe sets XA mode bits on ordinary CD reads.
     * Rematch armed invent-lockstep at sim=0 (depth24=0 mdec=0 xa=0,
     * fmv_pend=1 reading=1 mode=e0) and never presented a frame. */
    if (cdrom_xa_stream_active())
        return 1;
    return 0;
}

/* After media: grow/shrink lockstep_until from hash_confirm agreement. */
static void rb_fmv_update_lockstep_gate(uint32_t sim)
{
    int matched = 0;
    uint32_t cap;
    uint32_t prev_until;
    if (!g_fmv_media_end_sim || rb_fmv_media_active())
        return;

    if (g_b.hc && !netplay_hc_peek_mismatch(g_b.hc, NULL, NULL, NULL)) {
        uint32_t need = (sim > 0u) ? (sim - 1u) : 0u;
        if (netplay_hc_confirm_through(g_b.hc, need))
            matched = 1;
    }
    if (matched)
        g_fmv_core_match_streak++;
    else
        g_fmv_core_match_streak = 0;

    /* §93: after MAX-unmatched DESYNC, keep counting so a later rematch can
     * clear invent/begin hold without requiring a new media bout. */
    if (g_fmv_lockstep_released) {
        if (g_fmv_unmatched_desync &&
            g_fmv_core_match_streak >= RB_FMV_LOCKSTEP_CONFIRM) {
            {
                char why[96];
                snprintf(why, sizeof(why),
                         "cores rematched streak=%u sim=%u",
                         (unsigned)g_fmv_core_match_streak, (unsigned)sim);
                rb_clear_fmv_desync_hold(why);
            }
        }
        /* §110: drop heal sticky once cores rematch CONFIRM after RELEASE.
         * §110b: tip rematch alone is not enough — apply-only / tip KF only
         * proves tip; the silent fork is tip+1. Require a confirmed walk
         * past the fork tick before dropping sticky (else heal storm). */
        if (g_post_fmv_heal_sticky_until > 0u &&
            g_fmv_core_match_streak >= RB_FMV_LOCKSTEP_CONFIRM) {
            if (g_post_fmv_heal_fork_tick > 0u &&
                sim <= g_post_fmv_heal_fork_tick) {
                static uint32_t s_sticky_hold_log_sim;
                if (sim != s_sticky_hold_log_sim) {
                    s_sticky_hold_log_sim = sim;
                    fprintf(stderr,
                            "psxrecomp: rb post-FMV heal sticky hold "
                            "(tip rematch sim=%u fork=%u — need walk past "
                            "fork before clear)\n",
                            (unsigned)sim,
                            (unsigned)g_post_fmv_heal_fork_tick);
                    fflush(stderr);
                }
            } else {
                char why[96];
                snprintf(why, sizeof(why),
                         "cores rematched streak=%u sim=%u",
                         (unsigned)g_fmv_core_match_streak, (unsigned)sim);
                rb_clear_post_fmv_heal_sticky(why);
            }
        }
        return;
    }

    if (sim < g_fmv_media_end_sim + RB_FMV_LOCKSTEP_MIN)
        return; /* accumulate streak; invent stays off through MIN */

    prev_until = g_fmv_lockstep_until;
    cap = g_fmv_media_end_sim + RB_FMV_LOCKSTEP_MAX;
    if (sim >= cap || g_fmv_core_match_streak >= RB_FMV_LOCKSTEP_CONFIRM) {
        /* Do not clamp until→sim: that unlocked invent on the same tick and
         * hold-last poison (sticky Up) opened ep1 at unlock+1. Keep invent
         * off for UNLOCK_GRACE while wire catches up; promote_sweep flushes
         * any predicted rows already in lookback without a tip episode. */
        uint32_t grace_until = sim + RB_FMV_UNLOCK_GRACE;
        int max_unmatched = (sim >= cap &&
                             g_fmv_core_match_streak < RB_FMV_LOCKSTEP_CONFIRM)
                                ? 1
                                : 0;
        if (grace_until > g_fmv_lockstep_until)
            g_fmv_lockstep_until = grace_until;
        g_fmv_lockstep_released = 1;
        g_promote_sweep = 1;
        /* §71/§93: MAX unlock with streak=0 means cores never confirmed after
         * FMV — do NOT invent-unlock into a soft fork. Arm DESYNC hold +
         * storm cooldown; invent/begin stay gated until rematch or reset. */
        if (max_unmatched) {
            uint32_t dense = sim + RB_FMV_UNLOCK_GRACE * 2u;
            if (dense > g_fmv_lockstep_until)
                g_fmv_lockstep_until = dense;
            g_fmv_unmatched_desync = 1;
            g_fmv_unmatched_desync_arm_sim = sim;
            arm_rewind_cooldown_ticks(sim, RB_FORK_STORM_COOLDOWN_TICKS,
                                      "FMV lockstep MAX unmatched "
                                      "(cores never confirmed)");
            fprintf(stderr,
                    "psxrecomp: rb DESYNC — FMV lockstep MAX unmatched "
                    "sim=%u streak=%u cap=%u — invent/begin held until "
                    "cores rematch (confirm=%u) or invent-hold expires "
                    "(+%u ticks) / netplay save\n",
                    (unsigned)sim, (unsigned)g_fmv_core_match_streak,
                    (unsigned)cap, (unsigned)RB_FMV_LOCKSTEP_CONFIRM,
                    (unsigned)RB_FMV_DESYNC_INVENT_EXPIRE);
            fflush(stderr);
        }
        fprintf(stderr,
                "psxrecomp: rb FMV lockstep RELEASE sim=%u streak=%u "
                "cap=%u dense_until=%u (unlock_grace=%u; promote_sweep%s; "
                "%s)\n",
                (unsigned)sim, (unsigned)g_fmv_core_match_streak,
                (unsigned)cap, (unsigned)g_fmv_lockstep_until,
                (unsigned)RB_FMV_UNLOCK_GRACE,
                max_unmatched ? "; MAX unmatched"
                              : ((sim >= cap) ? "; MAX" : "; cores matched"),
                max_unmatched
                    ? "DESYNC hold — invent off until rematch"
                    : "invent off through unlock_grace §113");
        fflush(stderr);
        (void)prev_until;
        return;
    }
    /* Past MIN, cores not confirmed — hold invent until match or MAX. */
    {
        uint32_t until = sim + 1u;
        if (until > cap)
            until = cap;
        if (until > g_fmv_lockstep_until)
            g_fmv_lockstep_until = until;
    }
    if (g_fmv_lockstep_until > prev_until) {
        static uint32_t s_ext_log;
        if (s_ext_log != sim) {
            fprintf(stderr,
                    "psxrecomp: rb FMV lockstep EXTEND until=%u sim=%u "
                    "streak=%u (waiting core confirm)\n",
                    (unsigned)g_fmv_lockstep_until, (unsigned)sim,
                    (unsigned)g_fmv_core_match_streak);
            fflush(stderr);
            s_ext_log = sim;
        }
    }
}

/* Track settle window after FMV ends. */
static void rb_fmv_tick_settle(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    int media = rb_fmv_media_active();

    if (media) {
        if (!g_was_in_fmv) {
            g_was_in_fmv = 1;
            g_fmv_media_lo = sim;
            fprintf(stderr,
                    "psxrecomp: rb FMV rewind-defer ON (depth24/mdec; no invent)\n");
            fflush(stderr);
            /* §50: TipHold through FMV is doomed — initiator tip-extends /
             * rereplays while the peer ownership-finals → Live, then follow
             * REFUSED (FMV lockstep) and tip-extend ABANDON realigns to a
             * BIOS sticky PC snap. Commit the sealed tip before media runs. */
            if (g_rb && rnet_rb_is_tip_holding(g_rb)) {
                fprintf(stderr,
                        "psxrecomp: rb FMV mid TipHold — tip-hold commit "
                        "through=%u\n",
                        (unsigned)rnet_rb_get_target_tick(g_rb));
                fflush(stderr);
                finalize_tip_hold();
            }
        }
        if (sim >= g_fmv_media_hi)
            g_fmv_media_hi = sim;
        if (g_fmv_media_lo == 0u || sim < g_fmv_media_lo)
            g_fmv_media_lo = sim;
        /* Heartbeat through the movie — digs are sparse while MDEC is hot. */
        {
            static uint32_t s_fmv_hb_sim;
            if (sim == 0u || (sim % 32u) == 0u) {
                if (s_fmv_hb_sim != sim) {
                    CDROMDebugState cds;
                    memset(&cds, 0, sizeof(cds));
                    cdrom_debug_snapshot(&cds);
                    fprintf(stderr,
                            "psxrecomp: rb FMV media sim=%u depth24=%d mdec=%d "
                            "xa=%d fmv_pend=%d cd_reading=%d mode=%02x "
                            "present_pend=%d\n",
                            (unsigned)sim, gpu_display_is_depth24(),
                            mdec_recently_active(RB_FMV_MDEC_HYSTERESIS),
                            cdrom_xa_stream_active(),
                            cdrom_fmv_stream_pending(), cds.reading,
                            (unsigned)cds.mode_reg,
                            gpu_vblank_present_pending());
                    fflush(stderr);
                    s_fmv_hb_sim = sim;
                }
            }
        }
        return;
    }
    if (g_was_in_fmv) {
        g_was_in_fmv = 0;
        g_fmv_media_end_sim = sim;
        if (sim >= g_fmv_media_hi)
            g_fmv_media_hi = sim;
        g_fmv_core_match_streak = 0;
        g_fmv_lockstep_released = 0;
        g_fmv_settle_until = sim + RB_FMV_SETTLE_TICKS;
        g_fmv_lockstep_until = sim + RB_FMV_LOCKSTEP_MIN;
        /* Live FRAME_COMMITs were skipped during depth24 — waive the gap so
         * hash_confirm can advance again once digests resume. */
        if (g_b.hc)
            netplay_hc_prime_after(g_b.hc, sim);
        fprintf(stderr,
                "psxrecomp: rb FMV settle until sim=%u invent_hold=%u "
                "dense_lockstep min=%u max=%u confirm=%u "
                "(no invent through lockstep MIN — covers loading tip+1; "
                "hc primed)\n",
                (unsigned)g_fmv_settle_until,
                (unsigned)g_fmv_lockstep_until,
                (unsigned)g_fmv_lockstep_until,
                (unsigned)(sim + RB_FMV_LOCKSTEP_MAX),
                (unsigned)RB_FMV_LOCKSTEP_CONFIRM);
        fflush(stderr);
    }
    rb_fmv_update_lockstep_gate(sim);
}

/* §93: true if an episode load/mismatch tick sits inside the last FMV media
 * bout or its settle tail — even when Live tip has already left media. */
static int rb_fmv_tick_unsafe_for_episode(uint32_t tick)
{
    uint32_t unsafe_hi;
    if (rb_fmv_media_active())
        return 1;
    if (g_fmv_unmatched_desync)
        return 1;
    if (g_fmv_media_hi == 0u && g_fmv_media_end_sim == 0u)
        return 0;
    unsafe_hi = g_fmv_settle_until;
    if (g_fmv_media_hi > 0u) {
        uint32_t media_settle = g_fmv_media_hi + RB_FMV_SETTLE_TICKS;
        if (media_settle > unsafe_hi)
            unsafe_hi = media_settle;
    }
    if (g_fmv_media_end_sim > 0u) {
        uint32_t end_settle = g_fmv_media_end_sim + RB_FMV_SETTLE_TICKS;
        if (end_settle > unsafe_hi)
            unsafe_hi = end_settle;
    }
    if (unsafe_hi == 0u)
        return 0;
    /* Media bout may start at 0 (boot). Treat lo==0 with hi set as from 0. */
    if (tick <= unsafe_hi &&
        (g_fmv_media_lo == 0u || tick >= g_fmv_media_lo))
        return 1;
    return 0;
}

/* §94: drop MAX-unmatched invent-hold (rematch, save complete, or expire). */
static void rb_clear_fmv_desync_hold(const char *why)
{
    if (!g_fmv_unmatched_desync)
        return;
    g_fmv_unmatched_desync = 0;
    g_fmv_unmatched_desync_arm_sim = 0;
    rb_clear_post_fmv_platform_nondet(why ? why : "FMV DESYNC invent-hold cleared");
    fprintf(stderr,
            "psxrecomp: rb FMV DESYNC invent-hold cleared (%s)\n",
            why ? why : "?");
    fflush(stderr);
    clear_rewind_cooldown(why ? why : "FMV DESYNC invent-hold cleared");
}

/* Expire invent-hold when soft-fork digests never rematch CONFIRM ticks.
 * §114: platform nondet stay invent-off until HC rematch (expire only
 * widens the fork via invent). */
static void rb_fmv_desync_invent_maybe_expire(uint32_t sim)
{
    if (!g_fmv_unmatched_desync || g_fmv_unmatched_desync_arm_sim == 0u)
        return;
    if (g_post_fmv_platform_nondet)
        return;
    if (sim < g_fmv_unmatched_desync_arm_sim + RB_FMV_DESYNC_INVENT_EXPIRE)
        return;
    rb_clear_fmv_desync_hold("invent-hold expired (no rematch)");
}

/* Defer rewind during movie + short settle.
 * §97: with MEDIA_KF, allow rewind/episodes into live media (keyframe
 * heals stream forks); settle + DESYNC hold still defer. */
static int rb_in_fmv_defer_rewind_window(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    rb_fmv_tick_settle();
    rb_fmv_desync_invent_maybe_expire(sim);
    if (rb_fmv_media_active())
        return rb_media_kf_enabled() ? 0 : 1;
    if (g_fmv_unmatched_desync)
        return 1;
    return sim < g_fmv_settle_until;
}

/* §111: sticky invent hold only when we would invent *ahead* of the peer
 * (absurd catch-up through a soft fork). Tip-starved lead<=0 must invent or
 * admit deadlocks (Win↔Linux loading-screen soak: follow NACK + lead=-1 →
 * stall=fmv_settle → lobby). */
static int rb_post_fmv_heal_sticky_invent_hold(uint32_t sim)
{
    RNetSession *s;
    RNetSessionStats st;
    if (g_post_fmv_heal_sticky_until == 0u || sim >= g_post_fmv_heal_sticky_until)
        return 0;
    s = sess();
    if (!s)
        return 1;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(s, &st);
    /* remote_lead > 0 → invent would catch up through the fork — hold.
     * lead <= 0 → fill tip hole; allow invent. */
    return (st.remote_lead > 0) ? 1 : 0;
}

static int rb_post_fmv_heal_sticky_active(uint32_t sim)
{
    return (g_post_fmv_heal_sticky_until > 0u && sim < g_post_fmv_heal_sticky_until)
               ? 1
               : 0;
}

/* Admit no-invent gate (§26/§113): media + post-FMV lockstep MIN.
 * §100: invent stays OFF during live media even with MEDIA_KF — GAP1 invent
 * was opening ~3.7 MB KF transfers mid-intro FMV. Wait for wire; real
 * rewinds still open episodes (defer_rewind allows MEDIA_KF).
 * §113: invent_hold used to end at settle (24 ticks) while dense_lockstep
 * MIN started ~180 later — Win↔Linux invent was free in the loading tip+1
 * window (fork@1480 before min=1510). Hold invent through lockstep_until.
 * §110/§111: heal sticky invent hold only while remote_lead > 0. */
static int rb_in_fmv_lockstep_window(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    rb_fmv_tick_settle();
    rb_fmv_desync_invent_maybe_expire(sim);
    if (rb_fmv_media_active())
        return 1;
    if (g_fmv_unmatched_desync)
        return 1;
    if (rb_post_fmv_heal_sticky_invent_hold(sim))
        return 1;
    return sim < g_fmv_lockstep_until;
}

static void rb_media_crc_note(uint32_t tick, uint32_t size, uint32_t crc)
{
    RbMediaCrc *e;
    uint32_t i;
    if (tick == 0u || size == 0u)
        return;
    for (i = 0; i < g_media_crc_n && i < RB_MEDIA_CRC_RING; ++i) {
        uint32_t idx = (g_media_crc_head + RB_MEDIA_CRC_RING - 1u - i) %
                       RB_MEDIA_CRC_RING;
        if (g_media_crc[idx].tick == tick) {
            g_media_crc[idx].size = size;
            g_media_crc[idx].crc = crc;
            return;
        }
    }
    e = &g_media_crc[g_media_crc_head % RB_MEDIA_CRC_RING];
    e->tick = tick;
    e->size = size;
    e->crc = crc;
    g_media_crc_head = (g_media_crc_head + 1u) % RB_MEDIA_CRC_RING;
    if (g_media_crc_n < RB_MEDIA_CRC_RING)
        g_media_crc_n++;
}

static int rb_media_crc_lookup(uint32_t tick, uint32_t *size_out, uint32_t *crc_out)
{
    uint32_t i;
    for (i = 0; i < g_media_crc_n && i < RB_MEDIA_CRC_RING; ++i) {
        uint32_t idx = (g_media_crc_head + RB_MEDIA_CRC_RING - 1u - i) %
                       RB_MEDIA_CRC_RING;
        if (g_media_crc[idx].tick == tick) {
            if (size_out)
                *size_out = g_media_crc[idx].size;
            if (crc_out)
                *crc_out = g_media_crc[idx].crc;
            return 1;
        }
    }
    return 0;
}

static void arm_rewind_cooldown_ticks(uint32_t sim, uint32_t ticks, const char *why)
{
    uint32_t until = sim + ticks;
    if (until > g_rewind_cooldown_until)
        g_rewind_cooldown_until = until;
    g_promote_sweep = 1;
    fprintf(stderr, "psxrecomp: rb rewind cooldown until sim=%u (%s)\n",
            (unsigned)g_rewind_cooldown_until, why ? why : "?");
    fflush(stderr);
}

static void clear_rewind_cooldown(const char *why)
{
    g_rewind_cooldown_until = 0;
    g_promote_sweep = 0;
    fprintf(stderr, "psxrecomp: rb rewind cooldown cleared (%s)\n",
            why ? why : "?");
    fflush(stderr);
}

/* Dump sealed pads + hist + FRAME_COMMIT digests for the failed episode span
 * BEFORE abort_episode clears the seal table. Cross-peer: both sides print
 * the same shape — compare tip_btn / FC first_mismatch to find the fork. */
static void dump_post_diverge_diag(const char *why)
{
    uint32_t load;
    uint32_t tip;
    uint32_t t;
    uint32_t first_fc_mis = 0;
    uint32_t first_fc_loc = 0;
    uint32_t first_fc_peer = 0;
    int n;
    int slot;
    int sealed_ok;

    if (!g_rb || !rnet_rb_is_active(g_rb))
        return;
    load = rnet_rb_get_load_tick(g_rb);
    tip = rnet_rb_get_target_tick(g_rb);
    sealed_ok = rnet_rb_inputs_sealed(g_rb) ? 1 : 0;
    n = g_b.slot_count ? *g_b.slot_count : 0;
    if (n < 1)
        n = 2;
    if (n > (int)RNET_RB_MAX_SLOTS)
        n = (int)RNET_RB_MAX_SLOTS;

    fprintf(stderr,
            "psxrecomp: rb POST-DIVERGE diag epoch=%u load=%u tip=%u "
            "seal_base=%u span=%u sealed=%d why=%s\n",
            (unsigned)rnet_rb_get_epoch_id(g_rb), (unsigned)load, (unsigned)tip,
            (unsigned)rnet_rb_get_seal_base_tick(g_rb),
            (unsigned)rnet_rb_get_seal_span(g_rb), sealed_ok,
            why ? why : "?");

    if (sealed_ok && tip >= load) {
        fprintf(stderr, "psxrecomp: rb POST-DIVERGE seals (btn; P=predicted):\n");
        for (t = load; t <= tip; ++t) {
            fprintf(stderr, "  t=%u", (unsigned)t);
            for (slot = 0; slot < n; ++slot) {
                RNetRbFrame seal;
                RNetRbFrame hist;
                int have_s = rnet_rb_get_sealed_frame(g_rb, slot, t, &seal) &&
                             seal.is_valid;
                int have_h = g_b.ih &&
                             netplay_ih_get(g_b.ih, slot, t, &hist) &&
                             hist.is_valid;
                if (have_s)
                    fprintf(stderr, " s%d=%04x%s", slot, (unsigned)seal.buttons,
                            seal.is_predicted ? "P" : "");
                else
                    fprintf(stderr, " s%d=----", slot);
                if (have_h) {
                    fprintf(stderr, "/h=%04x%s", (unsigned)hist.buttons,
                            hist.is_predicted ? "P" : "");
                    if (have_s && (seal.buttons != hist.buttons ||
                                   seal.stick_x != hist.stick_x ||
                                   seal.stick_y != hist.stick_y))
                        fprintf(stderr, "!");
                } else {
                    fprintf(stderr, "/h=----");
                }
            }
            fprintf(stderr, "\n");
        }
    } else {
        fprintf(stderr,
                "psxrecomp: rb POST-DIVERGE seals: (none — inputs not sealed "
                "or empty span)\n");
    }

    /* FRAME_COMMIT local vs peer across the span — first mismatch is the
     * earliest visible sim fork (often earlier than POST tip). */
    if (g_b.hc && tip >= load) {
        uint32_t n_local = 0, n_peer = 0, n_both = 0, n_ok = 0;
        fprintf(stderr, "psxrecomp: rb POST-DIVERGE fc (local/peer core):\n");
        for (t = load; t <= tip; ++t) {
            uint32_t ld = 0, pd = 0;
            int have_l = netplay_hc_local_digest(g_b.hc, t, &ld);
            int have_p = netplay_hc_peer_digest(g_b.hc, t, &pd);
            if (have_l)
                n_local++;
            if (have_p)
                n_peer++;
            fprintf(stderr, "  t=%u L=%08x%s P=%08x%s", (unsigned)t,
                    have_l ? (unsigned)ld : 0u, have_l ? "" : "?",
                    have_p ? (unsigned)pd : 0u, have_p ? "" : "?");
            if (have_l && have_p && ld != pd) {
                fprintf(stderr, " MISMATCH");
                n_both++;
                if (!first_fc_mis) {
                    first_fc_mis = t;
                    first_fc_loc = ld;
                    first_fc_peer = pd;
                }
            } else if (have_l && have_p) {
                fprintf(stderr, " ok");
                n_both++;
                n_ok++;
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr,
                "psxrecomp: rb POST-DIVERGE fc coverage local=%u peer=%u "
                "both=%u ok=%u span=%u\n",
                (unsigned)n_local, (unsigned)n_peer, (unsigned)n_both,
                (unsigned)n_ok, (unsigned)(tip - load + 1u));
        if (first_fc_mis) {
            fprintf(stderr,
                    "psxrecomp: rb POST-DIVERGE fc FIRST_MISMATCH t=%u "
                    "local=%08x peer=%08x (POST tip=%u — fork was %u ticks "
                    "earlier)\n",
                    (unsigned)first_fc_mis, (unsigned)first_fc_loc,
                    (unsigned)first_fc_peer, (unsigned)tip,
                    (unsigned)(tip - first_fc_mis));
        } else if (n_peer == 0u) {
            fprintf(stderr,
                    "psxrecomp: rb POST-DIVERGE fc FIRST_MISMATCH: none — "
                    "peer FC hole (0 peer digests in load..tip; relay loss?)\n");
        } else if (n_both == 0u) {
            fprintf(stderr,
                    "psxrecomp: rb POST-DIVERGE fc FIRST_MISMATCH: none — "
                    "no overlapping local+peer FC ticks in ring\n");
        } else {
            fprintf(stderr,
                    "psxrecomp: rb POST-DIVERGE fc FIRST_MISMATCH: none in "
                    "ring (%u overlapping FC ok — tip cores matched; wire "
                    "dig_m is core-only)\n",
                    (unsigned)n_ok);
        }
    }
    fflush(stderr);
}

#if defined(PSX_HAS_GAME_DISPATCH)
extern int psx_game_is_function_entry(uint32_t addr);
#endif

static RNetSession *sess(void)
{
    return (g_bound && g_b.session) ? *g_b.session : NULL;
}

static CPUState *cpu(void)
{
    return (g_bound && g_b.cpu) ? *g_b.cpu : NULL;
}

/* psx_is_dispatchable only rejects 0 / sentinel — 0xB0 etc. still pass and
 * produce lethal flush_resume targets. Require aligned BIOS or main RAM. */
static int rb_resume_pc_ok(uint32_t pc)
{
    uint32_t phys;
    if (!psx_is_dispatchable(pc))
        return 0;
    if ((pc & 3u) != 0u)
        return 0;
    /* Exception / reset vectors are not safe episode resume points. */
    if (pc == 0x80000080u || pc == 0xbfc00180u || pc == 0x80000000u)
        return 0;
    if ((pc & 0xfff00000u) == 0xbfc00000u)
        return 1; /* BIOS ROM */
    phys = pc & 0x1fffffffu;
    /* 2MB main RAM; skip the low scratch page (catches 0xB0 / 0x800000B0). */
    if (phys >= 0x1000u && phys < 0x200000u)
        return 1;
    return 0;
}

/* MotK title/menu wait ping-pong. Opposite edges with matched clocks fork
 * dig_cpu (v0 countdown vs 1) while FRAME_COMMIT clears PC and still matches.
 * Always store the post-present edge so both peers pin the same resume. */
#define RB_MOTK_WAIT_A 0x8006CD54u
#define RB_MOTK_WAIT_B 0x8006CDA0u
/* Post-FMV overlay wait (dirty-interp hole @0x80076880..). Same class: IRQ at
 * 0x800768C8 left v0=1 while the snap/resume edge is 0x80076880. */
#define RB_MOTK_WAIT2_A 0x800768C8u
#define RB_MOTK_WAIT2_B 0x80076880u

static uint32_t rb_canonicalize_resume_pc(uint32_t pc)
{
    if (pc == RB_MOTK_WAIT_A || pc == RB_MOTK_WAIT_B)
        return RB_MOTK_WAIT_B;
    if (pc == RB_MOTK_WAIT2_A || pc == RB_MOTK_WAIT2_B)
        return RB_MOTK_WAIT2_B;
    return pc;
}

static int rb_pc_is_bios(uint32_t pc)
{
    return ((pc & 0xfff00000u) == 0xbfc00000u) ? 1 : 0;
}

/* Tick-0 / dig0 snaps must resume in BIOS ROM. Prior-match game IRQ/$ra
 * latches are still rb_resume_pc_ok and would otherwise poison the pin
 * (live dig clears PC → dig0 match; baseline hashes snap.pc → abort). */
static int rb_boot_snap_tick(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    if (sim == 0u)
        return 1;
    if (g_pending_save_valid && g_pending_save_tick == 0u)
        return 1;
    return 0;
}

/* Prefer BB-edge / IRQ check PCs — finish_frame often sees cpu->pc==0 while the
 * guest is parked in the vblank present path. IRQ/sticky/$ra first: bare
 * function-entry snaps picked short jr-$ra leaves (0x8006A9F8) whose top-level
 * flush_resume re-dispatch published pc=0 (cps exit: final_ra=0x8006CCA0). */
static uint32_t pick_snap_resume_pc(const CPUState *c, uint32_t hint)
{
    const uint32_t cands[6] = {
        psx_last_irq_check_pc(),
        g_last_good_bb_pc,
        psx_compiled_irq_resume_pc(),
        c ? c->gpr[31] : 0u,
        hint,
        c ? c->pc : 0u,
    };
    const int boot0 = rb_boot_snap_tick();
    int i;
    if (boot0) {
        for (i = 0; i < 6; ++i) {
            if (rb_resume_pc_ok(cands[i]) && rb_pc_is_bios(cands[i]))
                return rb_canonicalize_resume_pc(cands[i]);
        }
        /* Defer rather than stamp main-RAM residue into dig0. */
        return 0;
    }
    for (i = 0; i < 6; ++i) {
        if (rb_resume_pc_ok(cands[i]))
            return rb_canonicalize_resume_pc(cands[i]);
    }
    return 0;
}

uint32_t psx_netplay_rb_sticky_bb_pc(void)
{
    return g_last_good_bb_pc;
}

static void note_good_bb_pc(uint32_t pc)
{
    if (rb_resume_pc_ok(pc))
        g_last_good_bb_pc = rb_canonicalize_resume_pc(pc);
}

void psx_netplay_rb_cpu_for_present_digest(struct CPUState *out,
                                           const struct CPUState *in)
{
    if (!out || !in)
        return;
    *out = *in;
    /* Drop host-parking / BB-edge PC noise from present-edge digests. */
    out->pc = 0;
}

/* Split dig_cpu so logs tell PC-only forks from true GPR/COP0/GTE forks. */
static void log_cpu_digest_split(const CPUState *raw, const char *tag)
{
    uint32_t gpr = 0xFFFFFFFFu;
    uint32_t hil = 0xFFFFFFFFu;
    uint32_t c0 = 0xFFFFFFFFu;
    uint32_t gte = 0xFFFFFFFFu;
    int dump_regs;
    int i;
    if (!raw)
        return;
    gpr = crc32_update(gpr, (const uint8_t *)raw->gpr, sizeof(raw->gpr));
    hil = crc32_update(hil, (const uint8_t *)&raw->hi, sizeof(raw->hi));
    hil = crc32_update(hil, (const uint8_t *)&raw->lo, sizeof(raw->lo));
    c0 = crc32_update(c0, (const uint8_t *)&raw->cop0[12], sizeof(uint32_t));
    c0 = crc32_update(c0, (const uint8_t *)&raw->cop0[13], sizeof(uint32_t));
    c0 = crc32_update(c0, (const uint8_t *)&raw->cop0[14], sizeof(uint32_t));
    gte = crc32_update(gte, (const uint8_t *)raw->gte_data, sizeof(raw->gte_data));
    gte = crc32_update(gte, (const uint8_t *)raw->gte_ctrl, sizeof(raw->gte_ctrl));
    fprintf(stderr,
            "psxrecomp: rb cpu-split %s raw_pc=%08x sticky=%08x gpr=%08x "
            "hi_lo=%08x cop0=%08x gte=%08x\n",
            tag ? tag : "?", (unsigned)raw->pc, (unsigned)g_last_good_bb_pc,
            (unsigned)(gpr ^ 0xFFFFFFFFu), (unsigned)(hil ^ 0xFFFFFFFFu),
            (unsigned)(c0 ^ 0xFFFFFFFFu), (unsigned)(gte ^ 0xFFFFFFFFu));
    /* Full GPR words on fin/abort — peers can diff which regs forked. */
    dump_regs = tag && (strcmp(tag, "fin") == 0 || strcmp(tag, "abort") == 0 ||
                        strcmp(tag, "post") == 0);
    if (dump_regs) {
        fprintf(stderr, "psxrecomp: rb gpr-dump %s", tag);
        for (i = 0; i < 32; i++)
            fprintf(stderr, " r%02d=%08x", i, (unsigned)raw->gpr[i]);
        fprintf(stderr, "\n");
        /* Last VBLANK irq restore — peers diff restored/v0 on r2-only forks. */
        interrupts_log_last_vblank_irqctx(tag);
    }
    fflush(stderr);
}

static void clear_episode_wire_state(void)
{
    g_local_baseline_sent = 0;
    g_local_baseline_digest = 0;
    g_local_baseline_av = 0;
    g_local_baseline_aux = 0;
    g_peer_baseline_ok = 0;
    g_peer_baseline_digest = 0;
    g_peer_baseline_av = 0;
    g_peer_baseline_aux = 0;
    g_peer_baseline_ready = 0;
    g_local_baseline_ready_sent = 0;
    g_local_post_sent = 0;
    g_peer_post_ok = 0;
    g_post_av = 0;
    g_peer_post_av = 0;
    g_post_target = 0;
    g_baseline_rexmit_logged = 0;
    g_post_rexmit_logged = 0;
    g_seal_wait_ms = 0;
    g_verify_wait_ms = 0;
    g_replay_progress_ms = 0;
    g_baseline_handshake_ms = 0;
    g_last_baseline_burst_ms = 0;
    g_ready_timeout_logged = 0;
    g_wait_peer_bl_logged = 0;
    g_last_seal_rexmit_ms = 0;
    g_seal_rexmit_logged = 0;
    g_seal_wait_logged = 0;
    g_last_begin_rexmit_ms = 0;
    g_begin_rexmit_logged = 0;
    g_media_kf_episode = 0;
    g_media_kf_ready = 0;
    g_media_kf_host_armed = 0;
    g_media_kf_sending = 0;
    /* §109: g_post_fmv_heal_kf survives wire clear — begin sets it before
     * clear and re-arms MEDIA_KF from it afterward. Cleared when a new
     * begin starts without heal, or episode ends. */
}

/* §97: default ON — invent/episodes into FMV media with host keyframe. */
static int rb_media_kf_enabled(void)
{
    static int latched = -1;
    if (latched < 0) {
        const char *e = getenv("PSX_NET_FMV_MEDIA_KF");
        latched = (e && e[0] == '0') ? 0 : 1;
    }
    return latched;
}

/* §109: post-FMV dense lockstep / DESYNC-hold window — tip-snap SPAN cannot
 * heal silent core forks (Win↔Linux soak: digs match @2368, FIRST CORE @2380,
 * resim diverge same tick, media_kf=0 → MAX unmatched). Force MEDIA_KF. */
static int rb_post_fmv_lockstep_active(void)
{
    RNetSession *s;
    uint32_t sim;
    uint32_t cap;
    if (!g_fmv_media_end_sim)
        return 0;
    if (rb_fmv_media_active())
        return 0;
    if (g_fmv_unmatched_desync)
        return 1;
    if (g_fmv_lockstep_released)
        return 0;
    s = sess();
    sim = s ? rnet_session_sim_tick(s) : 0u;
    cap = g_fmv_media_end_sim + RB_FMV_LOCKSTEP_MAX + RB_FMV_UNLOCK_GRACE;
    if (g_fmv_lockstep_until > cap)
        cap = g_fmv_lockstep_until + RB_FMV_UNLOCK_GRACE;
    return sim <= cap ? 1 : 0;
}

/* §110: heal KF allowed — lockstep window, DESYNC hold, or sticky after heal. */
static int rb_post_fmv_heal_eligible(void)
{
    RNetSession *s;
    uint32_t sim;
    if (!rb_media_kf_enabled())
        return 0;
    if (rb_fmv_media_active())
        return 0;
    if (g_fmv_unmatched_desync)
        return 1;
    if (rb_post_fmv_lockstep_active())
        return 1;
    s = sess();
    sim = s ? rnet_session_sim_tick(s) : 0u;
    if (g_post_fmv_heal_sticky_until > 0u && sim < g_post_fmv_heal_sticky_until)
        return 1;
    return 0;
}

static void rb_arm_post_fmv_heal_sticky(uint32_t tip, uint32_t fork_tick)
{
    uint32_t until = tip + RB_FMV_HEAL_STICKY_TICKS;
    if (until < tip)
        until = 0xffffffffu;
    if (until > g_post_fmv_heal_sticky_until)
        g_post_fmv_heal_sticky_until = until;
    if (fork_tick > 0u)
        g_post_fmv_heal_fork_tick = fork_tick;
    fprintf(stderr,
            "psxrecomp: rb post-FMV heal sticky until sim=%u "
            "(fork=%u tip=%u — re-heal + invent hold)\n",
            (unsigned)g_post_fmv_heal_sticky_until,
            (unsigned)g_post_fmv_heal_fork_tick, (unsigned)tip);
    fflush(stderr);
}

static void rb_clear_post_fmv_heal_sticky(const char *why)
{
    if (g_post_fmv_heal_sticky_until == 0u && g_post_fmv_heal_fork_tick == 0u)
        return;
    fprintf(stderr,
            "psxrecomp: rb post-FMV heal sticky cleared (%s)\n",
            why ? why : "?");
    fflush(stderr);
    g_post_fmv_heal_sticky_until = 0u;
    g_post_fmv_heal_fork_tick = 0u;
    g_post_fmv_heal_loop_fork = 0u;
    g_post_fmv_heal_loop_count = 0u;
}

/* §114: arm invent-off + KF stream after matched-baseline tip+1 nondet. */
static void rb_post_fmv_platform_nondet_enter(uint32_t keep_tick, uint32_t fork_tick,
                                             uint32_t live_sim, const char *why)
{
    uint32_t arm = live_sim ? live_sim : keep_tick;
    g_post_fmv_platform_nondet = 1;
    if (keep_tick > 0u)
        g_post_fmv_platform_keep_tick = keep_tick;
    g_fmv_unmatched_desync = 1;
    g_fmv_unmatched_desync_arm_sim = arm ? arm : 1u;
    if (keep_tick > 0u)
        rb_arm_post_fmv_heal_sticky(keep_tick, fork_tick ? fork_tick : keep_tick);
    else if (fork_tick > 0u)
        rb_arm_post_fmv_heal_sticky(fork_tick, fork_tick);
    if (keep_tick > 0u) {
        uint32_t until = keep_tick + RB_FMV_LOCKSTEP_MAX;
        if (until < keep_tick)
            until = 0xffffffffu;
        if (until > g_fmv_lockstep_until)
            g_fmv_lockstep_until = until;
    }
    g_platform_kf_next_sim = arm + RB_FMV_PLATFORM_KF_IV;
    g_post_fmv_heal_loop_count = 0u;
    g_request_post_fmv_heal = 0;
    g_post_fmv_heal_kf = 0;
    /* Allow stream heals through cooldown. */
    clear_rewind_cooldown("§114 platform nondet");
    fprintf(stderr,
            "psxrecomp: rb §114 platform nondet keep-live pin=%u fork=%u "
            "live=%u — invent off until rematch; KF stream iv=%u (%s)\n",
            (unsigned)g_post_fmv_platform_keep_tick, (unsigned)fork_tick,
            (unsigned)live_sim, (unsigned)RB_FMV_PLATFORM_KF_IV,
            why ? why : "?");
    fflush(stderr);
}

static void rb_clear_post_fmv_platform_nondet(const char *why)
{
    if (!g_post_fmv_platform_nondet && g_post_fmv_platform_keep_tick == 0u &&
        g_platform_kf_next_sim == 0u)
        return;
    fprintf(stderr,
            "psxrecomp: rb §114 platform nondet cleared (%s)\n",
            why ? why : "?");
    fflush(stderr);
    g_post_fmv_platform_nondet = 0;
    g_post_fmv_platform_keep_tick = 0u;
    g_platform_kf_next_sim = 0u;
    g_platform_kf_stream_count = 0u;
}

/* §115: accept a tip+1 platform fork — prime HC past it, drop invent-hold /
 * KF stream, and refuse re-heal for that fork tick. */
static void rb_post_fmv_platform_accept(uint32_t through, uint32_t fork,
                                       const char *why)
{
    uint32_t prime = through;
    if (fork > prime)
        prime = fork;
    if (g_b.hc && prime > 0u)
        netplay_hc_prime_after(g_b.hc, prime);
    if (fork > 0u)
        g_platform_accepted_fork = fork;
    else if (through > 0u)
        g_platform_accepted_fork = through;
    g_request_post_fmv_heal = 0;
    g_post_fmv_heal_kf = 0;
    g_platform_kf_stream_count = 0u;
    rb_clear_post_fmv_platform_nondet(why ? why : "§115 platform accept");
    rb_clear_post_fmv_heal_sticky(why ? why : "§115 platform accept");
    if (g_fmv_unmatched_desync)
        rb_clear_fmv_desync_hold(why ? why : "§115 platform accept");
    psx_netplay_hc_fork_recovery_clear();
    fprintf(stderr,
            "psxrecomp: rb §115 platform fork accepted through=%u fork=%u "
            "(%s) — invent unlocked; no re-heal for this tip+1\n",
            (unsigned)through, (unsigned)fork, why ? why : "?");
    fflush(stderr);
}

int psx_netplay_rb_platform_fork_accepted(uint32_t fork_tick)
{
    return (g_platform_accepted_fork != 0u &&
            fork_tick == g_platform_accepted_fork)
               ? 1
               : 0;
}

/* §109/§97: arm host keyframe for media-range OR requested post-FMV heal. */
static int rb_want_heal_kf(uint32_t mismatch, uint32_t load)
{
    if (!rb_media_kf_enabled())
        return 0;
    if (rb_fmv_tick_unsafe_for_episode(load) ||
        rb_fmv_tick_unsafe_for_episode(mismatch))
        return 1;
    /* Only when hc-fork / escalate asked — not every post-FMV pad episode. */
    if (g_request_post_fmv_heal || g_post_fmv_heal_kf)
        return 1;
    return 0;
}

static void clear_baseline_pin(void);
static void abort_episode(const char *why);

static int rb_install_media_kf_bytes(uint32_t tick, const uint8_t *data, size_t size)
{
    uint8_t *ring_copy;
    if (!data || !size)
        return 0;
    clear_baseline_pin();
    g_pin_data = (uint8_t *)malloc(size);
    if (!g_pin_data)
        return 0;
    memcpy(g_pin_data, data, size);
    g_pin_size = size;
    g_pin_tick = tick;
    g_pin_valid = 1;
    /* Ring store takes ownership — keep a separate copy from the pin. */
    if (g_snaps) {
        ring_copy = (uint8_t *)malloc(size);
        if (ring_copy) {
            memcpy(ring_copy, data, size);
            if (!netplay_snap_ring_store(g_snaps, tick, ring_copy, size))
                ; /* store frees ring_copy on hard failure */
        }
    }
    g_media_kf_ready = 1;
    return 1;
}

static int rb_media_kf_seal_local(uint32_t tick)
{
    size_t sz = 0;
    const uint8_t *p;
    if (!g_snaps || !netplay_snap_ring_has(g_snaps, tick))
        return 0;
    p = netplay_snap_ring_peek(g_snaps, tick, &sz);
    if (!p || !sz)
        return 0;
    return rb_install_media_kf_bytes(tick, p, sz);
}

static void rb_media_kf_host_drive(void)
{
    RNetSession *s = sess();
    uint32_t load;
    size_t sz = 0;
    const uint8_t *p;
    int match = 0;
    uint32_t crc;
    int local_slot;

    if (!g_media_kf_episode || !g_rb || !s || !rnet_rb_is_active(g_rb))
        return;
    if (g_media_kf_ready)
        return;
    local_slot = g_b.local_slot ? *g_b.local_slot : 0;
    if (local_slot != 0)
        return; /* host-only transfer */
    if (rnet_session_state_busy(s) && !g_media_kf_sending)
        return; /* SAVE/LOAD in flight — wait */

    load = rnet_rb_get_load_tick(g_rb);
    p = g_snaps ? netplay_snap_ring_peek(g_snaps, load, &sz) : NULL;
    if (!p || !sz) {
        static uint32_t s_miss_log;
        if (s_miss_log != load) {
            fprintf(stderr,
                    "psxrecomp: rb MEDIA-KF host — no snap at load=%u\n",
                    (unsigned)load);
            fflush(stderr);
            s_miss_log = load;
        }
        return;
    }
    /* Refuse to probe/send a snap the peer cannot load (host integrity still
     * 0/0 after start-before-configure left all ring snaps poisoned). */
    if (!g_b.bios_checksum || !g_b.entry_pc || *g_b.bios_checksum == 0u ||
        *g_b.entry_pc == 0u) {
        fprintf(stderr,
                "psxrecomp: rb MEDIA-KF host — integrity unset "
                "(bios=%08x entry=%08x)\n",
                (unsigned)(g_b.bios_checksum ? *g_b.bios_checksum : 0u),
                (unsigned)(g_b.entry_pc ? *g_b.entry_pc : 0u));
        fflush(stderr);
        abort_episode("MEDIA-KF integrity unset");
        return;
    }
    {
        char reason[256];
        if (!boot_state_check_buffer(p, sz, *g_b.bios_checksum, *g_b.entry_pc,
                                     reason, sizeof(reason))) {
            fprintf(stderr,
                    "psxrecomp: rb MEDIA-KF host — snap reject load=%u — %s\n",
                    (unsigned)load, reason[0] ? reason : "unknown");
            fflush(stderr);
            abort_episode("MEDIA-KF snap integrity reject");
            return;
        }
    }

    if (!g_media_kf_host_armed) {
        crc = rnet_checksum(p, sz);
        if (rnet_session_state_probe(s, RNET_STATE_OP_RB_KF, 0, (rnet_u32)sz,
                                     crc) != 0)
            return;
        g_media_kf_host_armed = 1;
        fprintf(stderr,
                "psxrecomp: rb MEDIA-KF probe load=%u bytes=%zu crc=%08x\n",
                (unsigned)load, sz, (unsigned)crc);
        fflush(stderr);
        return;
    }

    if (g_media_kf_sending) {
        /* Wait for take_ready on host side (sender also gets ready). */
        return;
    }

    if (rnet_session_state_probe_take_reply(s, &match)) {
        rnet_session_state_probe_finish(s);
        if (match) {
            if (rb_media_kf_seal_local(load)) {
                if (load > g_media_crc_peer_match_through)
                    g_media_crc_peer_match_through = load;
                fprintf(stderr,
                        "psxrecomp: rb MEDIA-KF hash match load=%u — local "
                        "pin sealed\n",
                        (unsigned)load);
                fflush(stderr);
            }
            return;
        }
        /* Miss: send host snap bytes. */
        if (rnet_session_state_begin(s, RNET_STATE_OP_RB_KF, 0, p, sz) != 0) {
            fprintf(stderr,
                    "psxrecomp: rb MEDIA-KF begin FAILED load=%u\n",
                    (unsigned)load);
            fflush(stderr);
            g_media_kf_host_armed = 0;
            return;
        }
        g_media_kf_sending = 1;
        (void)rb_media_kf_seal_local(load);
        fprintf(stderr,
                "psxrecomp: rb MEDIA-KF transfer start load=%u bytes=%zu\n",
                (unsigned)load, sz);
        fflush(stderr);
    }
}

static void clear_baseline_pin(void)
{
    free(g_pin_data);
    g_pin_data = NULL;
    g_pin_size = 0;
    g_pin_tick = 0;
    g_pin_valid = 0;
}

static void pin_baseline_from_ring(uint32_t tick)
{
    size_t sz = 0;
    const uint8_t *p;
    uint8_t *copy;
    if (!g_snaps)
        return;
    p = netplay_snap_ring_peek(g_snaps, tick, &sz);
    if (!p || sz == 0)
        return;
    copy = (uint8_t *)malloc(sz);
    if (!copy)
        return;
    memcpy(copy, p, sz);
    clear_baseline_pin();
    g_pin_data = copy;
    g_pin_size = sz;
    g_pin_tick = tick;
    g_pin_valid = 1;
    fprintf(stderr, "psxrecomp: rb baseline pinned tick=%u bytes=%zu\n",
            (unsigned)tick, sz);
    fflush(stderr);
}

/* §70: pin the live POST-matched tip before ownership-chain session_reset
 * lets Live invent walk past it (follower wait-FOLLOW). Also overwrite the
 * ring slot so realign/follow cannot reload tip-hold invent snaps. */
static int pin_baseline_from_cpu(uint32_t tick)
{
    CPUState *c = cpu();
    CPUState snap;
    uint8_t *data = NULL;
    size_t sz = 0;
    uint32_t pc;
    uint32_t dropped = 0;
    if (!c || !g_b.bios_checksum || !g_b.entry_pc)
        return 0;
    pc = pick_snap_resume_pc(c, 0u);
    if (!pc)
        return 0;
    snap = *c;
    snap.pc = pc;
    /* Raw (uncompressed) — same form as ring snaps / skip-snap restore. */
    if (!boot_state_save_buffer_raw(&snap, *g_b.bios_checksum, *g_b.entry_pc,
                                    &data, &sz) ||
        !data || sz == 0)
        return 0;
    clear_baseline_pin();
    g_pin_data = data;
    g_pin_size = sz;
    g_pin_tick = tick;
    g_pin_valid = 1;
    if (g_snaps) {
        (void)netplay_snap_ring_save(g_snaps, tick, &snap, *g_b.bios_checksum,
                                     *g_b.entry_pc);
        /* Invent snaps above the matched tip are a dead timeline. */
        dropped = netplay_snap_ring_drop_after(g_snaps, tick);
    }
    /* §75: pin is sealed POST state — Live must not re-poison this tick. */
    if (tick > g_auth_snap_through)
        g_auth_snap_through = tick;
    /* §78/§104 diag: digest the pin with its canonical resume pc AT SAVE.
     * If this already differs from the just-computed audit fin, the
     * SNAP-STALE class is the pc substitution (snap.pc = pick_snap_resume_pc
     * vs live pc), not load infidelity. Rate-limit — session-144 logged 32×
     * identical lines per fight (cosmetic; no control-plane effect). */
    {
        uint32_t pin_core = netplay_core_digest(&snap);
        if (s_resim_audit_valid && s_resim_audit_sim == tick &&
            pin_core != s_resim_audit_core) {
            static uint32_t s_pin_skew_log_tick;
            if (s_pin_skew_log_tick != tick) {
                fprintf(stderr,
                        "psxrecomp: rb PIN-SKEW tick=%u pin_core=%08x "
                        "fin_core=%08x live_pc=0x%08x snap_pc=0x%08x — pc "
                        "substitution shifts digest at save\n",
                        (unsigned)tick, (unsigned)pin_core,
                        (unsigned)s_resim_audit_core, (unsigned)c->pc,
                        (unsigned)pc);
                s_pin_skew_log_tick = tick;
            }
        }
        fprintf(stderr,
                "psxrecomp: rb baseline pinned from CPU tick=%u bytes=%zu "
                "core=%08x (ring refresh; dropped_after=%u)\n",
                (unsigned)tick, sz, (unsigned)pin_core, (unsigned)dropped);
    }
    fflush(stderr);
    return 1;
}

/* §55: an abort realign rewinds the state to `tick`, but every piece of sync
 * evidence above it (hash confirms, agreed watermark, peer RESOLVED, ring
 * snaps) describes the DISCARDED timeline. 2026-08-02 soak: right after
 * realigning to 4160 the guest ran `agreed ADVANCE 4160→4176 (hc=4179)` on
 * dead-timeline confirms; both peers then re-simmed 4161..4176 differently,
 * and choose_load kept certifying the forked 4176 snap as provably safe —
 * every recovery episode died on baseline core mismatch. Clamp it all. */
static void realign_invalidate_evidence(uint32_t tick)
{
    uint32_t dropped = 0;
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, tick);
    if (g_agreed_valid && g_agreed_through > tick) {
        g_agreed_through = tick;
        if (g_agreed_span_lo > tick)
            g_agreed_span_lo = tick;
    }
    if (g_peer_resolved_through > tick)
        g_peer_resolved_through = tick;
    if (g_local_advertised_through > tick)
        g_local_advertised_through = tick;
    if (g_snaps)
        dropped = netplay_snap_ring_drop_after(g_snaps, tick);
    if (g_auth_snap_through > tick)
        g_auth_snap_through = tick; /* §75 */
    fprintf(stderr,
            "psxrecomp: rb realign evidence clamp tick=%u (snaps_dropped=%u)\n",
            (unsigned)tick, (unsigned)dropped);
    fflush(stderr);
}

static void schedule_live_realign(uint32_t tick, const char *why)
{
    RNetSession *s = sess();
    int have_pin = g_pin_valid && g_pin_tick == tick && g_pin_data && g_pin_size;
    if (!have_pin && (!g_snaps || !netplay_snap_ring_has(g_snaps, tick))) {
        fprintf(stderr,
                "psxrecomp: rb realign SKIPPED tick=%u — snap missing (%s)\n",
                (unsigned)tick, why ? why : "?");
        fflush(stderr);
        return;
    }
    g_live_realign_pending = 1;
    g_pending_load_tick = tick;
    g_pending_load_valid = 1;
    g_pending_resume_valid = 0;
    /* §55: clamp immediately (not only at apply) — the pump's Live ADVANCE
     * runs between queue and apply and would promote agreed from the dead
     * timeline's hash confirms. */
    realign_invalidate_evidence(tick);
    /* Snap is post-tick state; continue Live at tick+1 (same as episode commit). */
    if (s)
        rnet_session_set_sim_tick(s, tick + 1u);
    fprintf(stderr, "psxrecomp: rb realign queued tick=%u → sim=%u pin=%d (%s)\n",
            (unsigned)tick, (unsigned)(tick + 1u), have_pin, why ? why : "?");
    fflush(stderr);
}

/* ---- Abort propagation (shared cooldown class) ----
 * Local aborts used to tear down silently: the peer kept sealing/verifying
 * until its own (skewed) timeout, then armed a cooldown computed from
 * different local evidence — the two peers re-armed on different schedules
 * and re-initiated into each other (abort storms). Now every wire-visible
 * abort sends OP_ABORT carrying the RNET_RB_ABORT_CLASS_* cooldown class so
 * the peer mirrors teardown + cooldown from the same class. */
static uint32_t g_abort_wire_class = RNET_RB_ABORT_CLASS_ABORT;
static uint32_t g_abort_wire_realign_tick;
static int g_abort_from_peer;          /* applying a peer ABORT — no echo */
static uint32_t g_peer_abort_last_epoch = 0xffffffffu; /* rexmit dedupe */

#define RB_ABORT_NOTICE_BURST 3

static void send_abort_notice(uint32_t epoch, uint32_t cls, uint32_t realign_tick)
{
    RNetSession *s = sess();
    int i;
    if (!s || g_abort_from_peer)
        return;
    for (i = 0; i < RB_ABORT_NOTICE_BURST; ++i)
        (void)rnet_session_send_rb_sync(s, epoch, cls, realign_tick, 0u, 0u,
                                        RNET_RB_SYNC_OP_ABORT, 0u);
}

/* §42c: episode commit notice — the unambiguous "committed tick T, left
 * epoch E" signal the tip-extend abandon needs (RESOLVED is a watermark
 * and cannot prove a commit; see g_peer_commit_epoch). */
static void send_commit_notice(uint32_t epoch, uint32_t tick)
{
    RNetSession *s = sess();
    int i;
    if (!s || tick == 0u)
        return;
    for (i = 0; i < RB_ABORT_NOTICE_BURST; ++i)
        (void)rnet_session_send_rb_sync(s, epoch, 0u, tick, 0u, 0u,
                                        RNET_RB_SYNC_OP_COMMIT, 0u);
}

static uint32_t abort_class_cooldown_ticks(uint32_t cls)
{
    switch (cls) {
    case RNET_RB_ABORT_CLASS_REALIGN: return RB_POST_REALIGN_COOLDOWN_TICKS;
    case RNET_RB_ABORT_CLASS_STORM: return RB_STORM_COOLDOWN_TICKS;
    case RNET_RB_ABORT_CLASS_NO_SNAP: return RB_BASELINE_MISMATCH_COOLDOWN_TICKS;
    case RNET_RB_ABORT_CLASS_ABORT:
    default: return RB_ABORT_COOLDOWN_TICKS;
    }
}

static void abort_episode(const char *why)
{
    fprintf(stderr, "psxrecomp: rb episode ABORT — %s\n", why ? why : "?");
    fflush(stderr);
    /* Propagate before the session reset clears the epoch. Class defaults to
     * ABORT; abort_episode_realign pre-stages a more specific class. */
    if (g_rb && rnet_rb_is_active(g_rb))
        send_abort_notice(rnet_rb_get_epoch_id(g_rb), g_abort_wire_class,
                          g_abort_wire_realign_tick);
    g_abort_wire_class = RNET_RB_ABORT_CLASS_ABORT;
    g_abort_wire_realign_tick = 0u;
    psx_netplay_timesync_on_episode_boundary();
    /* Preserve a queued Live realign load; clear only episode baseline loads.
     * The next enter_awaiting_baseline consumes leftover live_realign so a
     * follow-up episode (NACK realign → peer-ahead reopen) takes the
     * snap-applied handshake, not the live-realign early return. */
    if (!g_live_realign_pending)
        g_pending_load_valid = 0;
    g_pending_resume_valid = 0;
    g_empty_span_verify_pending = 0;
    g_episode_snap_applied = 0;
    g_needs_advance = 0;
    g_episode_baseline_matched = 0;
    /* §110b: heal episode aborted mid-verify — clear episode heal mark so
     * escalate's !g_post_fmv_heal_kf gate works. Keep g_request_post_fmv_heal
     * (escalate sets it before abort then begin_rewind). */
    g_post_fmv_heal_kf = 0;
    g_tip_hold_until = 0;
    g_tip_hold_quiet_t0_ms = 0ull;
    g_tip_hold_block_quiet = 0;
    g_tip_hold_enter_ms = 0ull;
    g_tip_hold_coalesce_n = 0u;
    g_tip_extend_rereplay = 0;
    g_tip_extend_from_tick = 0;
    clear_tip_hold_rereplay_pending();
    clear_tip_extend_prime();
    psx_scheduler_top_level_resume_clear();
    clear_episode_wire_state();
    g_ownership_skip_snap = 0;
    ownership_clear_chain_budget();
    /* Keep g_peer_ahead_force_load — NACK handler sets it after abort then
     * immediately begin_rewind. Clear a stale light-episode marker. */
    g_peer_ahead_light_episode = 0;
    if (g_rb && rnet_rb_is_active(g_rb)) {
        rnet_rb_set_phase(g_rb, nRNetRbPhaseAbort);
        rnet_rb_session_reset(g_rb);
    }
}

int psx_netplay_rb_top_level_resume_active(void)
{
    /* Shared with savestate/selfcheck — armed inside psx_scheduler_resume_at. */
    return psx_scheduler_top_level_resume_active();
}

/* Top-level flush_resume has no native chain under the resume PC. A leaf that
 * returns (or sentinel "continue native") publishes pc=0 → GUEST_EXIT. Recover
 * at $ra / sticky BB when that is a safe re-entry. */
int psx_netplay_rb_recover_null_pc(struct CPUState *cpu_in, uint32_t *out_pc)
{
    uint32_t cand;
    int i;
    if (!psx_scheduler_top_level_resume_active() || !cpu_in || !out_pc)
        return 0;
    {
        const uint32_t cands[4] = {
            cpu_in->gpr[31],
            g_last_good_bb_pc,
            psx_last_irq_check_pc(),
            psx_compiled_irq_resume_pc(),
        };
        for (i = 0; i < 4; ++i) {
            cand = cands[i];
            if (!rb_resume_pc_ok(cand))
                continue;
            fprintf(stderr,
                    "psxrecomp: rb recover null-pc → 0x%08x (ra=0x%08x sticky=0x%08x)\n",
                    (unsigned)cand, (unsigned)cpu_in->gpr[31],
                    (unsigned)g_last_good_bb_pc);
            fflush(stderr);
            *out_pc = cand;
            note_good_bb_pc(cand);
            return 1;
        }
    }
    fprintf(stderr,
            "psxrecomp: rb recover null-pc FAILED ra=0x%08x sticky=0x%08x\n",
            (unsigned)cpu_in->gpr[31], (unsigned)g_last_good_bb_pc);
    fflush(stderr);
    psx_scheduler_top_level_resume_clear();
    return 0;
}

/* Realign tip both peers already agreed on (pin > episode load > agreed).
 * Shared with the peer-ABORT handler so both sides pick the same policy.
 * §83: never return a tip ≥ g_bl_fork_cap — that snap already baseline-
 * mismatched (soak: realign always re-applied forked pin 3056 while bisect
 * tried 3040/3024). */
static int tip_ok_for_realign(uint32_t tick)
{
    if (g_bl_fork_cap > 0u && tick >= g_bl_fork_cap)
        return 0;
    return 1;
}

static int pick_snap_below_fork_cap(uint32_t *out_tick)
{
    uint32_t oldest;
    uint32_t t;
    if (!out_tick || !g_snaps || g_bl_fork_cap == 0u)
        return 0;
    oldest = netplay_snap_ring_oldest_tick(g_snaps);
    if (g_bl_fork_cap <= oldest)
        return 0;
    t = g_bl_fork_cap - 1u;
    for (;;) {
        if (netplay_snap_ring_has(g_snaps, t)) {
            *out_tick = t;
            return 1;
        }
        if (t <= oldest || t == 0u)
            break;
        t--;
    }
    return 0;
}

static int pick_realign_tip(uint32_t *out_tick)
{
    /* Prefer the frozen pre-resim baseline — ring load_tick was often overwritten. */
    if (g_episode_baseline_matched && g_pin_valid &&
        tip_ok_for_realign(g_pin_tick)) {
        *out_tick = g_pin_tick;
        return 1;
    }
    if (g_episode_baseline_matched && g_snaps &&
        tip_ok_for_realign(g_episode_load_tick) &&
        netplay_snap_ring_has(g_snaps, g_episode_load_tick)) {
        *out_tick = g_episode_load_tick;
        return 1;
    }
    if (g_agreed_valid && g_snaps && tip_ok_for_realign(g_agreed_through) &&
        netplay_snap_ring_has(g_snaps, g_agreed_through)) {
        *out_tick = g_agreed_through;
        return 1;
    }
    if (pick_snap_below_fork_cap(out_tick)) {
        fprintf(stderr,
                "psxrecomp: rb realign tip below fork cap %u → %u\n",
                (unsigned)g_bl_fork_cap, (unsigned)*out_tick);
        fflush(stderr);
        return 1;
    }
    return 0;
}

/* After a failed episode: rewind Live to a tip both peers already agreed on. */
static void abort_episode_realign(const char *why)
{
    RNetSession *s = sess();
    /* Capture LIVE sim BEFORE schedule_live_realign rewinds the clock.
     * Arming cooldown from the rewound tip let uncapped catch-up burn a
     * 12-tick window in ms → char-select episode storms (stale until < live). */
    uint32_t live_sim = s ? rnet_session_sim_tick(s) : 0u;
    uint32_t tick = 0;
    int have = 0;
    uint32_t cls;
    uint32_t cool_n;
    char cool_why[160];
    int hard_fail;
    int post_fail = why && strstr(why, "post ") != NULL;
    int baseline_fail = why && strstr(why, "baseline") != NULL;
    int resim_fail = why && strstr(why, "resim core") != NULL;
    int fmv_load_fail = why && strstr(why, "FMV media") != NULL;
    int arm_absurd_catchup = 0;

    /* §55/§83: raise fork_cap BEFORE pick_realign_tip so we never select the
     * just-failed (or previously failed) baseline as the Live heal tip. */
    if (baseline_fail || fmv_load_fail) {
        uint32_t failed_load = g_rb ? rnet_rb_get_load_tick(g_rb) : 0u;
        if (fmv_load_fail && failed_load == 0u && why) {
            const char *lp = strstr(why, "load=");
            if (lp)
                (void)sscanf(lp + 5, "%u", &failed_load);
        }
        if (failed_load > 0u &&
            (g_bl_fork_cap == 0u || failed_load < g_bl_fork_cap)) {
            g_bl_fork_cap = failed_load;
            fprintf(stderr,
                    "psxrecomp: rb fork cap — next load must be < %u "
                    "(%s)\n",
                    (unsigned)g_bl_fork_cap,
                    fmv_load_fail ? "FMV media load" : "baseline mismatch there");
            fflush(stderr);
        }
    }

    /* §93: matched-baseline resim diverge on the same sim is nondeterminism
     * (FMV/CD). Second hit → DESYNC keep-live; do not realign to the same pin. */
    if (resim_fail) {
        uint32_t diverge_sim = 0u;
        const char *p = strstr(why, "sim=");
        if (p && sscanf(p + 4, "%u", &diverge_sim) == 1 && diverge_sim > 0u) {
            if (diverge_sim == g_resim_diverge_tick)
                g_resim_diverge_streak++;
            else {
                g_resim_diverge_tick = diverge_sim;
                g_resim_diverge_streak = 1u;
            }
        } else {
            g_resim_diverge_streak++;
        }
        {
            uint32_t failed_load = g_rb ? rnet_rb_get_load_tick(g_rb) : 0u;
            /* §114: heal KF / platform mode already proved matched baseline —
             * keep the pin, invent-off, KF stream. Do NOT fork_cap→load-1
             * Live-walk (soak: realign 1034 → lockstep streak=0 forever). */
            if (g_post_fmv_heal_kf || g_post_fmv_platform_nondet) {
                fprintf(stderr,
                        "psxrecomp: rb §114 resim diverge during heal "
                        "sim=%u load=%u — keep matched pin (no load-1 "
                        "realign)\n",
                        (unsigned)(diverge_sim ? diverge_sim
                                               : g_resim_diverge_tick),
                        (unsigned)failed_load);
                fflush(stderr);
                if (g_bl_fork_cap == failed_load)
                    g_bl_fork_cap = 0u;
                abort_episode(why);
                rb_post_fmv_platform_nondet_enter(
                    failed_load,
                    diverge_sim ? diverge_sim : g_resim_diverge_tick, live_sim,
                    why);
                if (failed_load > 0u && g_snaps &&
                    netplay_snap_ring_has(g_snaps, failed_load))
                    schedule_live_realign(failed_load,
                                         "§114 platform nondet keep pin");
                return;
            }
            if (failed_load > 0u &&
                (g_bl_fork_cap == 0u || failed_load < g_bl_fork_cap)) {
                g_bl_fork_cap = failed_load;
                fprintf(stderr,
                        "psxrecomp: rb fork cap — next load must be < %u "
                        "(resim diverge there)\n",
                        (unsigned)g_bl_fork_cap);
                fflush(stderr);
            }
        }
        /* §109/§114: first post-FMV tip-SPAN resim diverge → apply-only
         * MEDIA_KF heal. Do NOT re-request when this fork already healed. */
        if (g_resim_diverge_streak == 1u && !g_post_fmv_heal_kf &&
            rb_post_fmv_heal_eligible() &&
            g_post_fmv_heal_fork_tick != g_resim_diverge_tick &&
            g_post_fmv_heal_loop_count == 0u) {
            uint32_t failed_load = g_rb ? rnet_rb_get_load_tick(g_rb) : 0u;
            fprintf(stderr,
                    "psxrecomp: rb post-FMV heal escalate "
                    "(resim diverge sim=%u load=%u — next begin apply-only "
                    "KF)\n",
                    (unsigned)g_resim_diverge_tick, (unsigned)failed_load);
            fflush(stderr);
            /* Undo fork_cap raise so heal can reload the same pin. */
            if (failed_load > 0u && g_bl_fork_cap == failed_load)
                g_bl_fork_cap = 0u;
            g_request_post_fmv_heal = 1;
            g_abort_wire_class = RNET_RB_ABORT_CLASS_ABORT;
            g_abort_wire_realign_tick = 0u;
            abort_episode(why);
            clear_rewind_cooldown("post-FMV heal escalate");
            /* Host reopens immediately as apply-only KF; guest FOLLOWs. */
            {
                int local = g_b.local_slot ? *g_b.local_slot : 0;
                int remote = (local == 0) ? 1 : 0;
                int n = g_b.slot_count ? *g_b.slot_count : 0;
                if (n < 2)
                    remote = local;
                if (local == 0 && g_resim_diverge_tick > 0u)
                    (void)psx_netplay_rb_begin_rewind(g_resim_diverge_tick,
                                                      remote);
            }
            return;
        }
        if (g_resim_diverge_streak >= RB_RESIM_DIVERGE_STORM_LIMIT) {
            uint32_t failed_load = g_rb ? rnet_rb_get_load_tick(g_rb) : 0u;
            fprintf(stderr,
                    "psxrecomp: rb DESYNC — resim diverge storm sim=%u "
                    "(%u× same tick; matched baseline nondeterminism) — "
                    "§114 keep-live KF stream, cooldown %u ticks\n",
                    (unsigned)g_resim_diverge_tick,
                    (unsigned)g_resim_diverge_streak,
                    (unsigned)RB_FORK_STORM_COOLDOWN_TICKS);
            fflush(stderr);
            g_abort_wire_class = RNET_RB_ABORT_CLASS_STORM;
            g_abort_wire_realign_tick = 0u;
            abort_episode(why);
            if (g_bl_fork_cap == failed_load)
                g_bl_fork_cap = 0u;
            rb_post_fmv_platform_nondet_enter(
                failed_load, g_resim_diverge_tick, live_sim,
                "resim diverge storm");
            if (failed_load > 0u && g_snaps &&
                netplay_snap_ring_has(g_snaps, failed_load))
                schedule_live_realign(failed_load,
                                     "§114 resim storm keep pin");
            arm_rewind_cooldown_ticks(live_sim, RB_FORK_STORM_COOLDOWN_TICKS,
                                      "DESYNC resim diverge storm");
            return;
        }
    } else if (!baseline_fail) {
        g_resim_diverge_tick = 0u;
        g_resim_diverge_streak = 0u;
    }

    have = pick_realign_tip(&tick);
    /* Sealed table dies in abort_episode — dump first on POST diverge. */
    if (post_fail)
        dump_post_diverge_diag(why);

    /* §42 P4: same-tick baseline-mismatch abort loop = unrecoverable fork.
     * Every reopen loads the same snap on both sides, both resim their own
     * forked histories, digests never re-match. Stop rubber-banding: declare
     * DESYNC loudly, keep Live running, arm a long cooldown so episodes stop
     * reopening into the doomed baseline (2026-08-02 soak: 986/944 fork,
     * ~40 abort-realign cycles). Any commit resets the streak.
     * §83: streak on fork_cap (not realign tip); peer-abort echo must not
     * reset it while a fork investigation is live. */
    if (baseline_fail) {
        uint32_t streak_key = g_bl_fork_cap ? g_bl_fork_cap : tick;
        if (streak_key == g_fork_streak_tick)
            g_fork_streak++;
        else {
            g_fork_streak_tick = streak_key;
            g_fork_streak = 1u;
        }
        if (g_fork_streak >= RB_FORK_STORM_LIMIT) {
            fprintf(stderr,
                    "psxrecomp: rb DESYNC — unrecoverable fork at tick=%u "
                    "(%u consecutive baseline mismatches: %s) — keep-live, "
                    "cooldown %u ticks\n",
                    (unsigned)(tick ? tick : streak_key),
                    (unsigned)g_fork_streak, why,
                    (unsigned)RB_FORK_STORM_COOLDOWN_TICKS);
            fflush(stderr);
            g_abort_wire_class = RNET_RB_ABORT_CLASS_STORM;
            g_abort_wire_realign_tick = 0u;
            abort_episode(why);
            arm_rewind_cooldown_ticks(live_sim, RB_FORK_STORM_COOLDOWN_TICKS,
                                      "DESYNC fork storm");
            return;
        }
    } else if (g_bl_fork_cap == 0u) {
        /* Clean non-baseline abort with no active fork bisect — reset. */
        g_fork_streak_tick = 0u;
        g_fork_streak = 0u;
    }

    /* Classify BEFORE abort_episode so the wire ABORT carries the shared
     * cooldown class and both peers re-arm on the same schedule. */
    if (post_fail && have) {
        /* POST diverge with a realign tip is a heal path, not a storm seed —
         * prior ready-timeout streak must not force STORM cooldown (soak §18:
         * streak=2 → promote-no-resim while Live stayed forked at the pin). */
        g_bl_mismatch_streak = 0;
        cls = RNET_RB_ABORT_CLASS_REALIGN;
    } else {
        hard_fail = !have ||
                    (why && (strstr(why, "baseline") != NULL ||
                             strstr(why, "resim core") != NULL ||
                             strstr(why, "FMV media") != NULL ||
                             post_fail ||
                             strstr(why, "desync") != NULL));
        if (hard_fail)
            g_bl_mismatch_streak++;
        else
            g_bl_mismatch_streak = 0;

        if (!have)
            cls = RNET_RB_ABORT_CLASS_NO_SNAP;
        else if (g_bl_mismatch_streak >= 2u)
            cls = RNET_RB_ABORT_CLASS_STORM;
        else
            cls = RNET_RB_ABORT_CLASS_ABORT;
    }
    cool_n = abort_class_cooldown_ticks(cls);

    g_abort_wire_class = cls;
    g_abort_wire_realign_tick = have ? tick : 0u;
    abort_episode(why);
    if (have) {
        schedule_live_realign(tick, why);
        /* §83 C: baseline-abort Live rewind can leave remote_lead absurd;
         * allow invent catch-up briefly so we don't crawl 500 ticks at 1×. */
        if (baseline_fail)
            arm_absurd_catchup = 1;
    } else if (baseline_fail) {
        fprintf(stderr,
                "psxrecomp: rb realign SKIPPED — no snap below fork cap %u "
                "(%s); keep-live\n",
                (unsigned)g_bl_fork_cap, why ? why : "?");
        fflush(stderr);
    }

    snprintf(cool_why, sizeof(cool_why), "%s (live=%u streak=%u class=%u)",
             why ? why : "realign", (unsigned)live_sim,
             (unsigned)g_bl_mismatch_streak, (unsigned)cls);
    if (cool_n == 0u)
        clear_rewind_cooldown(cool_why);
    else
        arm_rewind_cooldown_ticks(live_sim, cool_n, cool_why);
    if (arm_absurd_catchup)
        np_sched_arm_absurd_invent_catchup();
}

/* §42 P1: honor the realign tick carried on a peer OP_ABORT.
 *
 * The 2026-08-02 fork: slot0 hit verify timeout at the extended tip and
 * realigned to its pre-episode pin (944); slot1 had already unilaterally
 * committed the old tip (986), received the ABORT, and realigned to its OWN
 * pick (986). Both sides then lived on different bases forever ("baseline
 * core mismatch" storm). The wire ABORT already carries the sender's realign
 * tick — when we sit AHEAD of it (agreed/committed past it), demote to it
 * and rewind there so both peers land on the same tick.
 *
 * Returns 1 when it demoted+realigned (caller skips its own realign pick).
 * `snap_was_applied`: when set, also realign to the wire tick even if we are
 * not ahead of it, so the two peers pick the same tick instead of each
 * realigning to their own local evidence. */
static int honor_peer_abort_realign(uint32_t wire_tick, int snap_was_applied,
                                    const char *why)
{
    uint32_t t;
    uint32_t cap_tick = wire_tick;

    if (!g_rb || wire_tick == 0u || !g_snaps)
        return 0;
    /* §83: peer's realign tick may still be the forked pin — clamp below
     * our fork_cap before searching for a snap. */
    if (g_bl_fork_cap > 0u && cap_tick >= g_bl_fork_cap)
        cap_tick = g_bl_fork_cap - 1u;
    if (cap_tick == 0u && g_bl_fork_cap > 0u)
        return 0;
    /* Nearest held snap at/below the peer's realign tick. */
    t = cap_tick;
    for (;;) {
        if (netplay_snap_ring_has(g_snaps, t) && tip_ok_for_realign(t))
            break;
        if (t == 0u)
            return 0;
        t--;
    }
    if (g_agreed_valid && g_agreed_through > wire_tick) {
        /* Unilateral commit past the peer's rewind point — roll it back. */
        fprintf(stderr,
                "psxrecomp: rb peer abort realign honor %u (snap=%u, "
                "agreed was %u — unilateral tip rolled back)\n",
                (unsigned)wire_tick, (unsigned)t, (unsigned)g_agreed_through);
        fflush(stderr);
        g_agreed_through = wire_tick;
        g_agreed_span_lo = (t < wire_tick) ? t : wire_tick;
        rnet_rb_demote_resolved_through(g_rb, wire_tick);
        if (g_b.hc)
            netplay_hc_prime_after(g_b.hc, wire_tick);
        if (g_pin_valid && g_pin_tick > wire_tick)
            clear_baseline_pin();
        g_episode_baseline_matched = 0;
        schedule_live_realign(t, why);
        if (why && strstr(why, "baseline") != NULL)
            np_sched_arm_absurd_invent_catchup();
        return 1;
    }
    if (snap_was_applied) {
        /* Not ahead, but Live left the pre-episode tip — rewind to the
         * peer's tick (not our own pick) so both sides converge. */
        fprintf(stderr,
                "psxrecomp: rb peer abort realign follow %u (snap=%u)\n",
                (unsigned)wire_tick, (unsigned)t);
        fflush(stderr);
        schedule_live_realign(t, why);
        if (why && strstr(why, "baseline") != NULL)
            np_sched_arm_absurd_invent_catchup();
        return 1;
    }
    {
        /* §84: follow REFUSED / never joined the aborted epoch, but Live
         * invent walked past the initiator's NACK rewind tip — without this
         * the initiator rewinds alone and the fork is permanent (soak:
         * host NACK-realign 705, guest invents → live dig diverge @768). */
        RNetSession *ls = sess();
        uint32_t live = ls ? rnet_session_sim_tick(ls) : 0u;
        if (live > t + 1u) {
            fprintf(stderr,
                    "psxrecomp: rb peer abort realign live-ahead %u "
                    "(snap=%u live=%u)\n",
                    (unsigned)wire_tick, (unsigned)t, (unsigned)live);
            fflush(stderr);
            schedule_live_realign(t, why);
            if (live > t + 8u ||
                (why && strstr(why, "baseline") != NULL))
                np_sched_arm_absurd_invent_catchup();
            return 1;
        }
    }
    return 0;
}

static int host_save_state(void *ctx, uint32_t tick)
{
    CPUState *c = cpu();
    uint32_t pc;
    CPUState snap;
    (void)ctx;
    if (!g_snaps || !c || !g_b.bios_checksum || !g_b.entry_pc)
        return -1;
    pc = pick_snap_resume_pc(c, 0u);
    if (!pc)
        return -1;
    snap = *c;
    snap.pc = pc;
    return netplay_snap_ring_save(g_snaps, tick, &snap, *g_b.bios_checksum, *g_b.entry_pc)
               ? 0
               : -1;
}

static int host_load_state(void *ctx, uint32_t tick)
{
    (void)ctx;
    g_pending_load_tick = tick;
    g_pending_load_valid = 1;
    return 0;
}

static int host_advance_sim(void *ctx, uint32_t tick)
{
    (void)ctx;
    (void)tick;
    return 0; /* MotK advances via the live guest frame loop. */
}

static uint32_t host_state_digest(void *ctx, uint32_t tick, uint32_t partition)
{
    CPUState *c = cpu();
    (void)ctx;
    (void)tick;
    (void)partition;
    if (!c)
        return 0;
    /* Agree on CPU/RAM/IRQ/timers only — CDROM FSM still forks across peers
     * during MotK title resim (audit cd=); folding it aborted good corrections. */
    return netplay_core_digest(c);
}

static uint8_t host_hash_confirm_through(void *ctx, uint32_t tick)
{
    (void)ctx;
    if (!g_b.hc)
        return 0;
    return netplay_hc_confirm_through(g_b.hc, tick);
}

/* Session delay-ring → hist (authoritative). Tip-hold invent-cap stalls Live
 * so hist often lacks tip+N local rows even though the pad was sampled into
 * the wire ring at sim+delay earlier — seal must read that ring, not invent.
 * §44: the sim→wire consumption mapping is owned by the scheduler. */
static int host_promote_from_session(int slot, uint32_t tick, RNetRbFrame *out)
{
    RNetSession *s = sess();
    RNetInputSample sample;
    PsxNetPad pad;
    RNetRbFrame frame;
    rnet_u32 wire;
    int local;

    if (!s || !g_b.ih || slot < 0)
        return 0;
    wire = np_sched_wire_for_sim(tick);
    local = (g_b.local_slot && slot == *g_b.local_slot) ? 1 : 0;
    if (local) {
        if (!rnet_session_peek_input(s, slot, wire, &sample))
            return 0;
    } else if (!rnet_session_peek_remote_input(s, slot, wire, &sample)) {
        return 0;
    }
    if (!sample.valid || sample.size < PSX_NETPLAY_PAD_BYTES)
        return 0;
    memset(&pad, 0, sizeof(pad));
    pad.buttons = (uint16_t)sample.bytes[0] | ((uint16_t)sample.bytes[1] << 8);
    pad.lx = sample.bytes[2];
    pad.ly = sample.bytes[3];
    pad.rx = sample.bytes[4];
    pad.ry = sample.bytes[5];
    pad.analog = sample.bytes[6] ? 1u : 0u;
    pad.connected = 1;
    netplay_ih_pad_to_frame(&pad, tick, 0, &frame);
    if (!netplay_ih_promote(g_b.ih, slot, &frame))
        return 0;
    if (out)
        *out = frame;
    return 1;
}

/* §47: confirmed = delay-ring auth or hist with !is_predicted. Invent-idle
 * and hold-last predicted rows do not advance the contiguous frontier. */
static int seat_tick_confirmed(int slot, uint32_t tick)
{
    RNetRbFrame row;
    if (slot < 0 || !g_b.ih)
        return 0;
    if (host_promote_from_session(slot, tick, &row))
        return 1;
    if (!netplay_ih_get(g_b.ih, slot, tick, &row) || !row.is_valid)
        return 0;
    return row.is_predicted ? 0 : 1;
}

uint32_t psx_netplay_rb_confirmed_frontier(uint32_t from_tick)
{
    uint32_t t;
    uint32_t last;
    uint32_t walk;
    int slots;
    int slot;

    if (!g_b.ih)
        return (from_tick > 0u) ? (from_tick - 1u) : 0u;
    slots = g_b.slot_count ? *g_b.slot_count : 0;
    if (slots < 1)
        slots = 2;
    /* from incomplete → frontier = from-1 (no further confirmed sim). */
    last = (from_tick > 0u) ? (from_tick - 1u) : 0u;
    /* Bound walk to hist/ring depth so a missing tip cannot spin forever. */
    for (walk = 0u, t = from_tick; walk < NETPLAY_INPUT_HIST_DEPTH; ++walk, ++t) {
        int ok = 1;
        for (slot = 0; slot < slots; ++slot) {
            if (!seat_tick_confirmed(slot, t)) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            break;
        last = t;
        if (t == 0xffffffffu)
            break;
    }
    return last;
}

uint32_t psx_netplay_rb_confirmed_remaining(void)
{
    RNetSession *s = sess();
    uint32_t sim;
    uint32_t frontier;
    uint32_t rem;
    if (!s || !g_rb)
        return 0;
    if (!rnet_rb_is_resimulating(g_rb) && !rnet_rb_is_active(g_rb))
        return 0;
    sim = rnet_session_sim_tick(s);
    if (sim == 0xffffffffu)
        return 0;
    frontier = psx_netplay_rb_confirmed_frontier(sim + 1u);
    rem = (frontier > sim) ? (frontier - sim) : 0u;
    /* §64: catchup present keys off the *episode tip*, not confirmed work
     * past tip. Tip SPAN episodes (≤24) were reporting rem=37+ whenever the
     * contiguous frontier ran ahead, so §63's rem≤24 hold-last never engaged
     * (soak: catchup present=live on 1008..1032). Cap by target−sim. */
    if (rnet_rb_is_active(g_rb)) {
        uint32_t tip = rnet_rb_get_target_tick(g_rb);
        uint32_t tip_rem = (tip > sim) ? (tip - sim) : 0u;
        if (tip_rem < rem)
            rem = tip_rem;
    }
    return rem;
}

static void host_prefresh_seal_range(int slot, uint32_t lo, uint32_t hi)
{
    uint32_t t;
    if (slot < 0 || hi < lo)
        return;
    for (t = lo; t <= hi; ++t)
        (void)host_promote_from_session(slot, t, NULL);
}

static uint8_t host_get_input_row(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out)
{
    (void)ctx;
    if (!g_b.ih || !out)
        return 0;
    /* Prefer delay-ring (auth) over hist — invent-idle may have poisoned hist
     * while Live was invent-cap stalled during tip-hold. */
    if (host_promote_from_session((int)slot, tick, out))
        return 1u;
    if (netplay_ih_get(g_b.ih, (int)slot, tick, out))
        return 1u;
    /* Seal path requires is_valid=1 rows or the peer never completes SealInputs
     * (apply_peer_seal_rows ignores invalid). MotK: idle invent fills gaps —
     * hold-last reintroduced sticky D-pad into Replay seals. */
    return netplay_ih_invent_idle(g_b.ih, (int)slot, tick, out) ? 1u : 0u;
}

static uint8_t host_hash_confirm_promote(void *ctx)
{
    uint32_t *tickp = (uint32_t *)ctx;
    if (!tickp || !g_b.hc)
        return 0;
    return netplay_hc_confirm_through(g_b.hc, *tickp);
}

static uint64_t rb_mono_ms(void)
{
    return psx_host_mono_ms();
}

/* 1 for ~300ms after tip-hold commit — covers peer SAFETY-deferred tip-hold. */
static int post_tip_hold_holdoff_active(void)
{
    uint64_t now;
    if (g_post_tip_hold_holdoff_t0_ms == 0ull)
        return 0;
    now = rb_mono_ms();
    if (now < g_post_tip_hold_holdoff_t0_ms)
        return 1;
    return (now - g_post_tip_hold_holdoff_t0_ms) <
           (uint64_t)RB_MOTK_POST_TIP_HOLD_AGREED_HOLDOFF_MS;
}

/* §40/§41: tell the peer our HC-confirmed agreed watermark advanced so their
 * follow frontier (max(agreed, resolved_through)) can accept begin load.
 * Do NOT call for unconfirmed HEAL-FORCE / BOOTSTRAP (§41 soak deadlock). */
static void advertise_agreed_resolved(uint32_t through)
{
    RNetSession *s;
    int i;
    int fresh;
    uint64_t now;

    if (!g_rb || through == 0u)
        return;
    fresh = (through > g_local_advertised_through);
    rnet_rb_set_peer_convergence(g_rb, through);
    s = sess();
    if (!s)
        return;
    for (i = 0; i < RB_ABORT_NOTICE_BURST; ++i)
        (void)rnet_session_send_rb_resolved(s, through);
    g_local_advertised_through = through;
    now = rb_mono_ms();
    g_peer_resolved_hb_last_ms = (now != 0ull) ? now : 1ull;
    /* Fresh confirmed raise past peer: re-arm the bounded wait (not heartbeat). */
    if (fresh && through > g_peer_resolved_through) {
        g_peer_resolved_gate_expired = 0;
        g_peer_resolved_wait_t0_ms = 0ull;
    }
}

/* §41: 1 while choose_load/begin should clamp to g_peer_resolved_through.
 * After GATE_MS of waiting, sticky-expire until peer RESOLVED advances.
 * §103: past-frontier sticky never expires on the timer — only a peer
 * RESOLVED raise clears it (session 142: force-open → NACK → deep demote). */
static int peer_resolved_gate_blocks(uint32_t want_through)
{
    uint64_t now;

    if (g_peer_resolved_through == 0u)
        return 0;
    if (want_through <= g_peer_resolved_through) {
        if (g_past_frontier_sticky)
            g_past_frontier_sticky = 0;
        return 0;
    }
    if (g_past_frontier_sticky)
        return 1;
    if (g_peer_resolved_gate_expired)
        return 0;
    now = rb_mono_ms();
    if (g_peer_resolved_wait_t0_ms == 0ull) {
        g_peer_resolved_wait_t0_ms = (now != 0ull) ? now : 1ull;
        return 1;
    }
    if (now < g_peer_resolved_wait_t0_ms)
        return 1;
    if ((now - g_peer_resolved_wait_t0_ms) <
        (uint64_t)RB_MOTK_PEER_RESOLVED_GATE_MS)
        return 1;
    fprintf(stderr,
            "psxrecomp: rb peer RESOLVED stall force — waited %u ms "
            "want=%u peer=%u (open begin; NACK recoverable)\n",
            (unsigned)RB_MOTK_PEER_RESOLVED_GATE_MS,
            (unsigned)want_through, (unsigned)g_peer_resolved_through);
    fflush(stderr);
    g_peer_resolved_gate_expired = 1;
    g_peer_resolved_wait_t0_ms = 0ull;
    return 0;
}

/* §41: while peer lags our last confirmed advertise, retransmit RESOLVED. */
static void maybe_heartbeat_agreed_resolved(void)
{
    uint64_t now;

    if (!g_rb || g_local_advertised_through == 0u)
        return;
    if (g_peer_resolved_through >= g_local_advertised_through)
        return;
    now = rb_mono_ms();
    if (g_peer_resolved_hb_last_ms != 0ull &&
        now >= g_peer_resolved_hb_last_ms &&
        (now - g_peer_resolved_hb_last_ms) <
            (uint64_t)RB_MOTK_PEER_RESOLVED_HB_MS)
        return;
    advertise_agreed_resolved(g_local_advertised_through);
}

/* §42c / §91: tip-extend abandon on peer OP_COMMIT (never sticky RESOLVED).
 *
 * 2026-08-03 soak: guest ownership Live @T sent COMMIT while host TipHold
 * tip-extended T→T+N. Abandon only ran in Verify and required an exact snap
 * at extend_from — SNAP-STALE had dropped T — so host rode verify timeout
 * and the Live peer hit pcap_freeze. Honor COMMIT in TipHold/Replay too,
 * and realign to the nearest snap ≤ commit tick when T was dropped.
 *
 * TipHold path: finalize (clamp) at the peer commit tip — do NOT
 * tip_hold_try_finalize (that would flush deferred rereplay first).
 * Verify/Replay path: abort + live realign as before. */
static int maybe_abandon_tip_extend(void)
{
    RNetRbPhase phase;
    uint32_t tick;
    uint32_t snap_tick;
    uint32_t tip;
    uint32_t from;
    uint32_t oldest;
    int tip_hold_path;

    if (!g_rb || !rnet_rb_is_active(g_rb))
        return 0;
    phase = rnet_rb_get_phase(g_rb);
    tip_hold_path = (phase == nRNetRbPhaseTipHold) ? 1 : 0;
    if (phase != nRNetRbPhaseVerify && phase != nRNetRbPhaseTipHold &&
        phase != nRNetRbPhaseReplay)
        return 0;

    from = g_tip_extend_from_tick;
    if (from == 0u && tip_hold_path && g_tip_hold_rereplay_pending &&
        g_tip_hold_rereplay_from != 0u)
        from = g_tip_hold_rereplay_from;
    if (from == 0u && phase == nRNetRbPhaseVerify)
        return 0; /* plain Verify — wait for peer POST */
    if (from == 0u && phase == nRNetRbPhaseReplay)
        return 0;
    if (phase == nRNetRbPhaseVerify &&
        (!g_local_post_sent || g_post_target == 0u))
        return 0;
    if (g_peer_commit_epoch != rnet_rb_get_epoch_id(g_rb))
        return 0;
    tick = g_peer_commit_tick;
    if (tick == 0u)
        return 0;
    tip = rnet_rb_get_target_tick(g_rb);

    /* Peer committed our extended tip (or later) — normal Verify / TipHold. */
    if (phase == nRNetRbPhaseVerify && tick >= g_post_target)
        return 0;

    /* TipHold, no extension pending: peer committed the sealed tip → leave. */
    if (tip_hold_path && from == 0u && tick >= tip) {
        fprintf(stderr,
                "psxrecomp: rb tip-hold peer COMMIT epoch=%u tick=%u — "
                "finalize (peer left episode)\n",
                (unsigned)g_peer_commit_epoch, (unsigned)tick);
        fflush(stderr);
        finalize_tip_hold();
        return 1;
    }

    /* Extension doomed only if peer committed at/below the tip we left. */
    if (from != 0u) {
        if (tick > tip)
            return 0; /* peer somehow ahead of our raised tip — keep going */
        if (tick > from && tick < tip) {
            /* Mid-span commit — still abandon to that proof. */
        } else if (tick != from && !(tip_hold_path && tick <= tip)) {
            return 0;
        }
    } else {
        return 0;
    }

    snap_tick = tick;
    if (!g_snaps || !netplay_snap_ring_has(g_snaps, snap_tick)) {
        if (g_pin_valid && g_pin_tick > 0u && g_pin_tick <= tick)
            snap_tick = g_pin_tick;
        else if (g_snaps && netplay_snap_ring_count(g_snaps) > 0) {
            oldest = netplay_snap_ring_oldest_tick(g_snaps);
            snap_tick = 0u;
            {
                uint32_t t = tick;
                for (;;) {
                    if (netplay_snap_ring_has(g_snaps, t)) {
                        snap_tick = t;
                        break;
                    }
                    if (t <= oldest || t == 0u)
                        break;
                    t--;
                }
            }
            if (snap_tick == 0u)
                return 0;
        } else {
            return 0;
        }
        fprintf(stderr,
                "psxrecomp: rb tip-extend abandon snap fallback "
                "commit=%u → snap=%u\n",
                (unsigned)tick, (unsigned)snap_tick);
        fflush(stderr);
    }

    if (tip_hold_path) {
        fprintf(stderr,
                "psxrecomp: rb tip-extend ABANDON (TipHold) — peer COMMIT "
                "epoch=%u tick=%u (extend_from=%u tip=%u) — finalize\n",
                (unsigned)g_peer_commit_epoch, (unsigned)tick,
                (unsigned)from, (unsigned)tip);
        fflush(stderr);
        /* Clamp commit to snap/from — skip tip_hold_try_finalize (would
         * flush deferred rereplay into the doomed span). */
        g_tip_hold_rereplay_pending = 1;
        g_tip_hold_rereplay_from = snap_tick;
        g_tip_hold_block_quiet = 0;
        finalize_tip_hold();
        return 1;
    }

    fprintf(stderr,
            "psxrecomp: rb tip-extend ABANDON — peer COMMIT epoch=%u "
            "tick=%u (extend_from=%u post_target=%u) — commit there\n",
            (unsigned)g_peer_commit_epoch, (unsigned)tick,
            (unsigned)from, (unsigned)g_post_target);
    fflush(stderr);
    g_agreed_through = snap_tick;
    g_agreed_span_lo = snap_tick;
    g_agreed_valid = 1;
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, snap_tick);
    advertise_agreed_resolved(snap_tick);
    g_bl_mismatch_streak = 0;
    g_abort_wire_class = RNET_RB_ABORT_CLASS_REALIGN;
    g_abort_wire_realign_tick = snap_tick;
    abort_episode("tip-extend abandon (peer committed below extension)");
    schedule_live_realign(snap_tick, "tip-extend abandon");
    clear_rewind_cooldown("tip-extend abandon");
    psx_netplay_hc_fork_recovery_restart();
    return 1;
}

/* Apply inbound peer RESOLVED: raise g_peer_resolved_through + library
 * convergence, and complete Verify if POST was lost. */
static void apply_peer_resolved(uint32_t resolved)
{
    if (!g_rb || resolved == 0u)
        return;
    if (resolved > g_peer_resolved_through) {
        g_peer_resolved_through = resolved;
        g_peer_resolved_gate_expired = 0;
        g_peer_resolved_wait_t0_ms = 0ull;
        /* §103: follower caught up past sticky past-frontier NACK. */
        if (g_past_frontier_sticky)
            g_past_frontier_sticky = 0;
    }
    rnet_rb_set_peer_convergence(g_rb, resolved);
    /* Peer already POST-matched and tip-held this tip (or later). If we are
     * still stuck in Verify waiting for their POST, trust RESOLVED as the
     * missing half of the handshake — otherwise a single lost POST leaves us
     * here until verify timeout while they advance Live on the same tip
     * (2026-08-01 soak). */
    if (rnet_rb_is_active(g_rb) &&
        rnet_rb_get_phase(g_rb) == nRNetRbPhaseVerify &&
        g_local_post_sent && g_post_target > 0u &&
        resolved >= g_post_target) {
        fprintf(stderr,
                "psxrecomp: rb verify accept peer RESOLVED tip=%u "
                "(local_post=%u — POST may have been lost)\n",
                (unsigned)resolved, (unsigned)g_post_target);
        fflush(stderr);
        enter_tip_hold(g_post_target);
    }
    /* §42c: NO abandon here — RESOLVED is a watermark, not a commit proof.
     * The tip-extend abandon keys on OP_COMMIT only (see
     * maybe_abandon_tip_extend). */
}

/* Raise agreed_through to the newest interval snap still in the ring.
 * Used when hc cannot confirm anything in-ring (live core forks block
 * heal_stale_gap) so begin/follow are not stuck REFUSED forever.
 * do_advertise: only for HC-confirmed paths — never HEAL-FORCE/BOOTSTRAP. */
static int raise_agreed_to_newest_interval_snap(uint32_t oldest, uint32_t newest,
                                               uint32_t iv, const char *why,
                                               uint32_t hc_rt, int do_advertise)
{
    uint32_t t;
    uint32_t old;
    if (iv < 1u)
        iv = 1u;
    t = newest;
    for (;;) {
        if (netplay_snap_ring_has(g_snaps, t) &&
            ((iv <= 1u) || ((t % iv) == 0u))) {
            old = g_agreed_valid ? g_agreed_through : 0u;
            if (g_agreed_valid && t <= g_agreed_through)
                return 0;
            g_agreed_through = t;
            g_agreed_span_lo = t;
            g_agreed_valid = 1;
            fprintf(stderr,
                    "psxrecomp: rb agreed %s %u→%u (hc=%u oldest=%u)\n",
                    why, (unsigned)old, (unsigned)t, (unsigned)hc_rt,
                    (unsigned)oldest);
            fflush(stderr);
            if (do_advertise)
                advertise_agreed_resolved(t);
            return 1;
        }
        if (t <= oldest)
            break;
        t--;
    }
    return 0;
}

/* When tip-hold/commit agreed_through ages out of the snap ring, begin would
 * REFUSE forever (hard watermark ignores hc) while invent≠wire keeps forking
 * Live — soak: agreed=919, oldest=935, hc=1824, cores diverged by 1856 with
 * matched clocks. Raise agreed to the highest hc-confirmed snap still held.
 * Do NOT raise while any agreed-era snap remains (TipHold false-confirm
 * guard: never load above a live agreed tip via hc alone).
 *
 * If hc is stuck below ring oldest (heal_stale_gap blocked by live forks),
 * force-raise to the newest interval snap — otherwise slot-0 wire corrections
 * never open (soak: hc=714, oldest=982, agreed never tip-hold set). */
static void heal_agreed_watermark_if_aged_out(void)
{
    uint32_t oldest;
    uint32_t newest;
    uint32_t rt;
    uint32_t t;
    uint32_t old;
    uint32_t iv;
    int any_agreed_snap = 0;

    if (!g_snaps || !g_b.hc)
        return;
    if (netplay_snap_ring_count(g_snaps) == 0)
        return;

    oldest = netplay_snap_ring_oldest_tick(g_snaps);
    newest = netplay_snap_ring_newest_tick(g_snaps);
    iv = snap_interval();
    if (iv < 1u)
        iv = 1u;

    (void)netplay_hc_heal_stale_gap(g_b.hc);
    rt = netplay_hc_resolved_through(g_b.hc);

    /* Never tip-hold committed: hc watermark below ring → bootstrap agreed
     * so choose_load / follow can open episodes for late wire (host P1). */
    if (!g_agreed_valid) {
        if (netplay_hc_confirm_through(g_b.hc, rt) && rt >= oldest) {
            /* hc still usable as soft frontier — no agreed seed needed. */
            return;
        }
        if (rt < oldest || !netplay_hc_confirm_through(g_b.hc, rt)) {
            (void)raise_agreed_to_newest_interval_snap(
                oldest, newest, iv, "BOOTSTRAP", rt, 0);
        }
        return;
    }

    if (oldest <= g_agreed_through) {
        uint32_t hi = g_agreed_through;
        if (hi > newest)
            hi = newest;
        if (hi >= oldest) {
            for (t = hi;; --t) {
                if (netplay_snap_ring_has(g_snaps, t)) {
                    any_agreed_snap = 1;
                    break;
                }
                if (t <= oldest)
                    break;
            }
        }
    }
    if (any_agreed_snap)
        return;

    if (netplay_hc_confirm_through(g_b.hc, rt) && rt > g_agreed_through) {
        t = rt;
        if (t > newest)
            t = newest;
        for (;;) {
            /* Interval snaps only — matches choose_load mutual under span_lo=t. */
            if (netplay_snap_ring_has(g_snaps, t) &&
                ((iv <= 1u) || ((t % iv) == 0u)) &&
                netplay_hc_confirm_through(g_b.hc, t)) {
                old = g_agreed_through;
                g_agreed_through = t;
                g_agreed_span_lo = t;
                fprintf(stderr,
                        "psxrecomp: rb agreed HEAL %u→%u (aged out of ring; "
                        "hc=%u oldest=%u)\n",
                        (unsigned)old, (unsigned)t, (unsigned)rt,
                        (unsigned)oldest);
                fflush(stderr);
                advertise_agreed_resolved(t);
                return;
            }
            if (t == 0u)
                break;
            t--;
        }
    }

    /* Agreed tip gone and hc cannot confirm in-ring (live forks) — force.
     * §41: do NOT advertise RESOLVED — peer never confirmed this watermark. */
    if (g_agreed_through < oldest)
        (void)raise_agreed_to_newest_interval_snap(
            oldest, newest, iv, "HEAL-FORCE", rt, 0);
}

/* Advance agreed_through along hash-confirmed clean Live play. The hard
 * watermark used to move ONLY at episode commits: after a tip-hold commit,
 * hundreds of hc-confirmed live ticks (identical peer digs, soak 1344..2112)
 * were ignored, so the next edge reloaded from an ancient snap
 * (mismatch=2135 load=1361) and SPAN CAP chunked 24 ticks per episode while
 * Live galloped ahead — nonstop char-select resim storms with matching
 * states. The historical false-confirm source behind the hard cap (TipHold
 * Live invent FRAME_COMMITs with cleared PCs confirming forked tip snaps)
 * is structurally gone: TipHold Live no longer emits FRAME_COMMITs, and the
 * core digest now folds full CPU split + csv + icache. Advance only outside
 * episodes/TipHold/pending loads, to hc-confirmed interval snaps in-ring;
 * span_lo collapses to the new watermark (no dense span above a live
 * advance — only interval ticks are mutual). */
static void advance_agreed_watermark_from_hc(void)
{
    uint32_t rt;
    uint32_t t;
    uint32_t newest;
    uint32_t oldest;
    uint32_t iv;

    if (!g_agreed_valid || !g_snaps || !g_b.hc)
        return;
    if (psx_netplay_rb_active() || psx_netplay_rb_tip_holding() ||
        psx_netplay_rb_load_pending())
        return;
    /* §38: tip-hold commit just set agreed=tip; do not HC-ADVANCE past it
     * while the peer may still be tip-holding (follow NACK load>frontier). */
    if (post_tip_hold_holdoff_active())
        return;
    if (netplay_snap_ring_count(g_snaps) == 0)
        return;
    rt = netplay_hc_resolved_through(g_b.hc);
    if (rt <= g_agreed_through || !netplay_hc_confirm_through(g_b.hc, rt))
        return;
    newest = netplay_snap_ring_newest_tick(g_snaps);
    oldest = netplay_snap_ring_oldest_tick(g_snaps);
    iv = snap_interval();
    if (iv < 1u)
        iv = 1u;
    t = rt;
    if (t > newest)
        t = newest;
    for (;;) {
        if (t <= g_agreed_through)
            return;
        if (netplay_snap_ring_has(g_snaps, t) &&
            ((iv <= 1u) || ((t % iv) == 0u)) &&
            netplay_hc_confirm_through(g_b.hc, t)) {
            uint32_t old = g_agreed_through;
            g_agreed_through = t;
            g_agreed_span_lo = t;
            fprintf(stderr,
                    "psxrecomp: rb agreed ADVANCE %u→%u (hc=%u live confirm)\n",
                    (unsigned)old, (unsigned)t, (unsigned)rt);
            fflush(stderr);
            /* §40: peer follow frontier lags local agreed until RESOLVED lands. */
            advertise_agreed_resolved(t);
            return;
        }
        if (t <= oldest)
            return;
        t--;
    }
}

/* Pick a load snap ≤ mismatch that the lagging peer is likely to have.
 * Live snaps only land on snap_interval ticks; never invent a phantom tick.
 *
 * Layer 1 invariant (§51):
 *   choose_load returns the newest snapshot both peers can prove is safe
 *   to replay from — independent of how Replay later reaches the frontier.
 *
 * Provably-safe sources (any establishes the floor — take the newest):
 *  - tip-hold / episode commit watermark (g_agreed_through + dense span)
 *  - hash_confirm resolved_through (both simmed, cores matched → same snaps)
 *  - peer RESOLVED advertise (raise-safe when advertised from HC ADVANCE/HEAL;
 *    must NOT lower below HC-proven ticks — §51)
 *
 * §53/§53b: Live ADVANCE (HC-confirmed interval only) runs every pump.
 * choose_load may raise to max(agreed, hc_interval_floor, peer_resolved) —
 * never raw hc tip ticks (soak: raise 752→765 → follow NACK).
 *
 * Without the shared-frontier path, mismatch one tick after a commit re-loaded
 * a full interval + slack deep (798 → 768) — main-menu "resim back to title".
 * Peer eviction is still covered by follower NACK (abort, not hang).
 *
 * Fallback (no proven frontier below mismatch): interval ticks with one-
 * interval tip slack — ONLY when there is no shared frontier yet. Once
 * hash_confirm / a commit exists, never load an unconfirmed tip (those snaps
 * can fork dig_cpu while PC-cleared FRAME_COMMITs still matched). */
static uint32_t hc_provable_through(void)
{
    uint32_t rt;
    if (!g_b.hc)
        return 0u;
    rt = netplay_hc_resolved_through(g_b.hc);
    if (rt == 0u || !netplay_hc_confirm_through(g_b.hc, rt))
        return 0u;
    return rt;
}

/* Highest HC-confirmed *interval* snap in-ring at or below hc_floor.
 * Follower frontiers / RESOLVED adverts are ADVANCE'd interval ticks only —
 * raising choose_load to a raw tip (e.g. 765 with agreed=752) NACKs. */
static uint32_t hc_interval_floor(uint32_t hc_floor, uint32_t iv)
{
    uint32_t t;
    uint32_t oldest;
    uint32_t newest;
    if (hc_floor == 0u || !g_snaps || !g_b.hc)
        return 0u;
    if (iv < 1u)
        iv = 1u;
    oldest = netplay_snap_ring_oldest_tick(g_snaps);
    newest = netplay_snap_ring_newest_tick(g_snaps);
    t = hc_floor;
    if (t > newest)
        t = newest;
    for (;;) {
        if (t < oldest)
            return 0u;
        if (netplay_snap_ring_has(g_snaps, t) &&
            ((iv <= 1u) || ((t % iv) == 0u)) &&
            netplay_hc_confirm_through(g_b.hc, t))
            return t;
        if (t == 0u)
            return 0u;
        t--;
    }
}

static int choose_load_tick_inner(uint32_t mismatch, uint32_t *out_load)
{
    uint32_t t;
    uint32_t iv;
    uint32_t newest;
    uint32_t cap;
    uint32_t shared = 0;
    uint32_t hc_floor = 0;
    uint32_t hc_iv = 0;
    int have_shared = 0;
    if (!out_load || !g_snaps || netplay_snap_ring_count(g_snaps) == 0)
        return 0;
    iv = snap_interval();
    if (iv < 1u)
        iv = 1u;
    newest = netplay_snap_ring_newest_tick(g_snaps);

    if (g_b.hc)
        (void)netplay_hc_heal_stale_gap(g_b.hc);
    heal_agreed_watermark_if_aged_out();
    advance_agreed_watermark_from_hc();
    hc_floor = hc_provable_through();
    hc_iv = hc_interval_floor(hc_floor, iv);

    if (g_agreed_valid) {
        shared = g_agreed_through;
        have_shared = 1;
    } else if (hc_iv > 0u) {
        shared = hc_iv;
        have_shared = 1;
    } else if (hc_floor > 0u) {
        /* No interval snap yet — soft floor only when nothing else exists. */
        shared = hc_floor;
        have_shared = 1;
    }
    /* §53b: raise only to HC-confirmed interval snaps or peer-advertised
     * RESOLVED — never raw hc tip (752→765 NACK). Peer RESOLVED is raise-safe
     * (ADVANCE/HC-HEAL/commit advertise; never HEAL-FORCE). */
    if (!post_tip_hold_holdoff_active()) {
        uint32_t raised = have_shared ? shared : 0u;
        uint32_t hc_dense = 0u;
        if (hc_iv > raised)
            raised = hc_iv;
        /* §58/§61: dense tip snaps — raise to raw HC-proven tick only when
         * the peer has advertised RESOLVED at least that far. Otherwise both
         * peers keep the dense window, but open can still land above the
         * follower's frontier (soak: host 1180 vs peer_resolved 1168 →
         * REFUSED / NACK / demote storm). Missing peer_resolved stays on
         * interval floor (§53b); follower NACK remains the backstop. */
        if (tip_dense_window() > 0u && hc_floor > raised &&
            g_peer_resolved_through >= hc_floor &&
            netplay_snap_ring_has(g_snaps, hc_floor)) {
            raised = hc_floor;
            hc_dense = hc_floor;
        }
        if (g_peer_resolved_through > raised)
            raised = g_peer_resolved_through;
        if (raised > 0u && (!have_shared || raised > shared)) {
            if (have_shared && raised > shared) {
                static uint32_t s_raise_log;
                if (s_raise_log != raised) {
                    fprintf(stderr,
                            "psxrecomp: rb choose_load raise %u→%u "
                            "(agreed=%u hc_iv=%u hc_dense=%u peer_resolved=%u)\n",
                            (unsigned)shared, (unsigned)raised,
                            (unsigned)(g_agreed_valid ? g_agreed_through : 0u),
                            (unsigned)hc_iv, (unsigned)hc_dense,
                            (unsigned)g_peer_resolved_through);
                    fflush(stderr);
                    s_raise_log = raised;
                }
            }
            shared = raised;
            have_shared = 1;
        }
    }
    /* §38/§39: clamp ADVANCE→commit-frontier ONLY during post–tip-hold
     * holdoff. The always-on ≤runway+4 clamp (holdoff=0) undid healthy
     * ADVANCE 757→784 and forced SPAN CAP load=752 → tip-extend/verify
     * timeout hang (soak present_gap≈4s). */
    if (have_shared && g_rb && post_tip_hold_holdoff_active()) {
        uint32_t tip_rt = rnet_rb_resolved_through(g_rb);
        if (tip_rt > 0u && shared > tip_rt) {
            static uint32_t s_clamp_log_to;
            if (s_clamp_log_to != tip_rt) {
                fprintf(stderr,
                        "psxrecomp: rb choose_load clamp %u→%u "
                        "(commit frontier; tip-hold agreed holdoff)\n",
                        (unsigned)shared, (unsigned)tip_rt);
                fflush(stderr);
                s_clamp_log_to = tip_rt;
            }
            shared = tip_rt;
        }
    }
    /* §40/§41/§51/§53b: peer RESOLVED may limit unconfirmed tips above HC,
     * but must never lower the load below HC-proven *interval* ticks. */
    if (have_shared && peer_resolved_gate_blocks(shared)) {
        uint32_t clamped = g_peer_resolved_through;
        if (hc_iv > clamped)
            clamped = hc_iv;
        if (clamped < shared) {
            static uint32_t s_peer_clamp_log_to;
            if (s_peer_clamp_log_to != clamped) {
                fprintf(stderr,
                        "psxrecomp: rb choose_load clamp %u→%u "
                        "(peer RESOLVED; hc_iv=%u)\n",
                        (unsigned)shared, (unsigned)clamped,
                        (unsigned)hc_iv);
                fflush(stderr);
                s_peer_clamp_log_to = clamped;
            }
            shared = clamped;
        }
    }
    /* Snap at T = state after tick T; replaying the mismatch needs load < mismatch. */
    if (have_shared && mismatch > 0u) {
        uint32_t hi = shared;
        uint32_t lo;
        uint32_t oldest = netplay_snap_ring_oldest_tick(g_snaps);
        /* §99 MEDIA_KF: invent-through-media leaves HC/agreed on the last
         * pre-media interval snap (e.g. 432) while the ring holds FMV snaps
         * every fmv_media_snap_interval. Raise the walk to mismatch-1 so
         * Start-during-FMV loads ~tip; probe/transfer makes the pin identical. */
        int media_kf_near = 0;
        /* §97 media bout + §109 post-FMV heal: tip snaps are mutual under KF. */
        if (rb_want_heal_kf(mismatch, mismatch > 0u ? mismatch - 1u : 0u))
            media_kf_near = 1;
        if (hi > mismatch - 1u)
            hi = mismatch - 1u;
        if (media_kf_near && mismatch > 1u) {
            uint32_t media_hi = mismatch - 1u;
            if (media_hi > hi) {
                static uint32_t s_media_raise_log;
                if (s_media_raise_log != media_hi) {
                    fprintf(stderr,
                            "psxrecomp: rb choose_load MEDIA-KF raise walk "
                            "%u→%u (mismatch=%u agreed=%u hc_iv=%u)\n",
                            (unsigned)hi, (unsigned)media_hi,
                            (unsigned)mismatch,
                            (unsigned)(g_agreed_valid ? g_agreed_through : 0u),
                            (unsigned)hc_iv);
                    fflush(stderr);
                    s_media_raise_log = media_hi;
                }
                hi = media_hi;
            }
        }
        /* Watermark entirely below the ring: walk cannot succeed. Fall through
         * to tip-slack (BOOTSTRAP/HEAL should have raised agreed; if not,
         * refuse would stick forever with invent≠wire). */
        if (shared < oldest && !media_kf_near) {
            have_shared = 0;
        } else {
            /* Bound the walk to one ring depth of ticks. */
            lo = (hi > 255u) ? hi - 255u : 0u;
            if (oldest > lo)
                lo = oldest;
            for (t = hi;; --t) {
                int mutual = 0;
                if (g_agreed_valid) {
                    /* §51 Layer 2: inclusive dense span [span_lo, through] so
                     * the commit tip itself is preferred over %iv fallback
                     * (1189 tip with span_lo=tip used to miss → load 1184). */
                    if (t <= g_agreed_through && t >= g_agreed_span_lo)
                        mutual = 1;
                    else if ((iv <= 1u) || ((t % iv) == 0u)) {
                        /* §71: interval snaps ABOVE agreed are not mutual.
                         * follow hard-NACKs load > frontier (soak: host
                         * load=880 vs guest frontier=843 → NACK keep-live →
                         * promote-no-resim START fork). Require HC or peer
                         * RESOLVED before opening above the watermark. */
                        if (g_b.hc && netplay_hc_confirm_through(g_b.hc, t))
                            mutual = 1;
                        else if (g_peer_resolved_through >= t)
                            mutual = 1;
                    }
                } else if (g_b.hc && netplay_hc_confirm_through(g_b.hc, t)) {
                    mutual = 1;
                } else if ((iv <= 1u) || ((t % iv) == 0u)) {
                    mutual = 1;
                } else if (g_fmv_dense_through > 0u && t <= g_fmv_dense_through &&
                           t >= g_fmv_media_end_sim) {
                    mutual = 1;
                }
                /* HC-proven ticks are mutual even outside the commit span. */
                if (!mutual && hc_floor > 0u && t <= hc_floor &&
                    netplay_hc_confirm_through(g_b.hc, t))
                    mutual = 1;
                /* §99: media-interval (or dense) snaps are mutual under MEDIA_KF
                 * — CRC probe / transfer reconciles peer forks. */
                if (!mutual && media_kf_near) {
                    uint32_t miv = fmv_media_snap_interval();
                    if (miv < 1u)
                        miv = 1u;
                    if ((miv <= 1u) || ((t % miv) == 0u) ||
                        (g_fmv_dense_through > 0u && t <= g_fmv_dense_through &&
                         (g_fmv_media_lo == 0u || t >= g_fmv_media_lo)))
                        mutual = 1;
                }
                if (mutual && netplay_snap_ring_has(g_snaps, t)) {
                    *out_load = t;
                    return 1;
                }
                if (t <= lo)
                    break;
            }
            /* Shared frontier in range but no snap left — refuse rather than
             * load an unconfirmed tip above a live agreed watermark. MEDIA_KF
             * may still tip-slack on media-interval snaps below. */
            if (g_agreed_valid && !media_kf_near)
                return 0;
            have_shared = 0;
        }
    }
    /* §99/§100: tip-slack with FMV media interval when MEDIA_KF covers the
     * bout. Prefer ticks whose CRC we already cached (peer more likely to
     * hash-match and skip the 3.7 MB xfer). */
    if (rb_want_heal_kf(mismatch, mismatch > 0u ? mismatch - 1u : 0u) &&
        mismatch > 0u) {
        uint32_t miv = fmv_media_snap_interval();
        uint32_t mcap = mismatch - 1u;
        uint32_t fallback = 0u;
        int have_fallback = 0;
        if (miv < 1u)
            miv = 1u;
        if (miv > 1u)
            mcap -= (mcap % miv);
        for (;;) {
            if (netplay_snap_ring_has(g_snaps, mcap)) {
                uint32_t csz = 0, ccrc = 0;
                if (rb_media_crc_lookup(mcap, &csz, &ccrc) ||
                    (g_media_crc_peer_match_through > 0u &&
                     mcap <= g_media_crc_peer_match_through)) {
                    *out_load = mcap;
                    return 1;
                }
                if (!have_fallback) {
                    fallback = mcap;
                    have_fallback = 1;
                }
            }
            if (mcap == 0u)
                break;
            if (miv > 1u)
                mcap = (mcap < miv) ? 0u : (mcap - miv);
            else
                mcap--;
        }
        if (have_fallback) {
            *out_load = fallback;
            return 1;
        }
    }
    cap = mismatch;
    if (newest >= iv && cap > newest - iv)
        cap = newest - iv;
    if (iv > 1u)
        cap -= (cap % iv);

    for (;;) {
        if (netplay_snap_ring_has(g_snaps, cap)) {
            *out_load = cap;
            return 1;
        }
        if (cap == 0u)
            break;
        if (iv > 1u)
            cap = (cap < iv) ? 0u : (cap - iv);
        else
            cap--;
    }
    /* Last resort (boot / no frontier yet): any snap ≤ mismatch. */
    if (netplay_snap_ring_has(g_snaps, mismatch)) {
        *out_load = mismatch;
        return 1;
    }
    for (t = mismatch; t > 0; --t) {
        if (netplay_snap_ring_has(g_snaps, t - 1)) {
            *out_load = t - 1;
            return 1;
        }
    }
    if (netplay_snap_ring_has(g_snaps, 0)) {
        *out_load = 0;
        return 1;
    }
    return 0;
}

static int choose_load_tick(uint32_t mismatch, uint32_t *out_load)
{
    /* §55: after a baseline core mismatch at load L the fork provably predates
     * L — retrying the same snap can never heal (soak: 4 consecutive aborts at
     * 4176). Bisect: force the next load strictly below the failed one. */
    if (g_bl_fork_cap > 0u) {
        uint32_t m = mismatch;
        if (m > g_bl_fork_cap)
            m = g_bl_fork_cap; /* inner's shared walk returns load < m */
        if (!choose_load_tick_inner(m, out_load))
            return 0;
        /* §62: peer proved it cannot follow ≤ floor (snap evicted). If the
         * bisect window (must be < fork cap AND > peer floor) is empty, no
         * mutually-loadable snap covers the fork — refuse to open rather
         * than NACK-cycle into a deep demote. */
        if (g_peer_nack_floor > 0u && *out_load <= g_peer_nack_floor) {
            static uint32_t s_floor_log;
            if (s_floor_log != *out_load) {
                fprintf(stderr,
                        "psxrecomp: rb choose_load refuse load=%u ≤ peer NACK "
                        "floor %u (fork cap %u — no mutually-loadable snap)\n",
                        (unsigned)*out_load, (unsigned)g_peer_nack_floor,
                        (unsigned)g_bl_fork_cap);
                fflush(stderr);
                s_floor_log = *out_load;
            }
            return 0;
        }
        if (*out_load >= g_bl_fork_cap) {
            static uint32_t s_refuse_log;
            if (s_refuse_log != g_bl_fork_cap) {
                fprintf(stderr,
                        "psxrecomp: rb choose_load refuse load=%u ≥ fork cap %u "
                        "(no provably-safe snap below the mismatched baseline)\n",
                        (unsigned)*out_load, (unsigned)g_bl_fork_cap);
                fflush(stderr);
                s_refuse_log = g_bl_fork_cap;
            }
            return 0;
        }
        {
            static uint32_t s_bisect_log;
            if (s_bisect_log != *out_load) {
                fprintf(stderr,
                        "psxrecomp: rb choose_load bisect load=%u (< fork cap %u)\n",
                        (unsigned)*out_load, (unsigned)g_bl_fork_cap);
                fflush(stderr);
                s_bisect_log = *out_load;
            }
        }
        return 1;
    }
    if (!choose_load_tick_inner(mismatch, out_load))
        return 0;
    /* §62: same floor without a fork cap — do not reopen at a load the peer
     * just proved unfollowable. */
    if (g_peer_nack_floor > 0u && *out_load <= g_peer_nack_floor) {
        static uint32_t s_floor_log2;
        if (s_floor_log2 != *out_load) {
            fprintf(stderr,
                    "psxrecomp: rb choose_load refuse load=%u ≤ peer NACK "
                    "floor %u\n",
                    (unsigned)*out_load, (unsigned)g_peer_nack_floor);
            fflush(stderr);
            s_floor_log2 = *out_load;
        }
        return 0;
    }
    return 1;
}

/* Follower's confirmed frontier for the NACK wire hint: the deepest tick this
 * peer can prove agreement through (agreed watermark, else mutually
 * hash-confirmed). 0 = unknown. The initiator demotes to this instead of
 * guessing load-1, so both watermarks converge on a mutually provable tick. */
static uint32_t follower_frontier_hint(void)
{
    uint32_t oldest = g_snaps ? netplay_snap_ring_oldest_tick(g_snaps) : 0u;
    if (g_agreed_valid && g_agreed_through >= oldest)
        return g_agreed_through;
    if (g_b.hc) {
        uint32_t rt = netplay_hc_resolved_through(g_b.hc);
        if (rt >= oldest && netplay_hc_confirm_through(g_b.hc, rt))
            return rt;
    }
    return 0u;
}

/* §62: realigning Live to `demote` requires remote wire inputs for every tick
 * after it. A missing tick that is followed by a later PRESENT tick is a true
 * hole — the peer's tip-window retransmit has already moved past it and it
 * will never refill (2026-08-02 soak: demote 10359→10230, remote rows
 * 10231..10243 evicted, 10244+ present → permanent WIRE_HOLE stall). A
 * missing tail with nothing after it is just in flight and fine. */
static uint32_t remote_wire_hole_end_after(uint32_t demote, uint32_t live_sim)
{
    RNetSession *s = sess();
    int local, slot;
    uint32_t t;
    uint32_t first_missing = 0u;
    uint32_t hole_start = 0u;
    uint32_t hole_end = 0u;
    int have_missing = 0;
    if (!s || demote >= live_sim)
        return 0u;
    local = g_b.local_slot ? *g_b.local_slot : 0;
    slot = (local == 0) ? 1 : 0;
    if (g_b.slot_count && *g_b.slot_count < 2)
        return 0u;
    for (t = demote + 1u; t <= live_sim; ++t) {
        RNetInputSample sample;
        rnet_u32 wire = np_sched_wire_for_sim(t);
        int present = rnet_session_peek_remote_input(s, slot, wire, &sample) &&
                      sample.valid;
        if (!present) {
            if (!have_missing) {
                first_missing = t;
                have_missing = 1;
            }
        } else if (have_missing) {
            /* keep scanning — report the LAST closed hole */
            hole_start = first_missing;
            hole_end = t - 1u;
            have_missing = 0;
        }
    }
    if (hole_end > 0u) {
        fprintf(stderr,
                "psxrecomp: rb wire hole %u..%u after demote=%u — "
                "realign there would stall\n",
                (unsigned)hole_start, (unsigned)hole_end, (unsigned)demote);
        fflush(stderr);
    }
    return hole_end;
}

/* MotK convention: SYNC op=NACK = follower cannot open episode (missing load
 * snap / past frontier / zombie load). Initiator aborts SealInputs instead of
 * hanging. target field carries this follower's frontier (recomputed every
 * send). */
static void send_follow_nack(uint32_t epoch, uint32_t mismatch, uint32_t load,
                             uint32_t target, int slot, int log_line)
{
    RNetSession *s = sess();
    uint32_t frontier = follower_frontier_hint();
    (void)target; /* episode target is not useful to the initiator; send frontier */
    if (!s)
        return;
    (void)rnet_session_send_rb_sync(s, epoch, mismatch, load, frontier,
                                    (rnet_u8)(slot < 0 ? 0 : slot),
                                    RNET_RB_SYNC_OP_NACK, 0u);
    if (log_line) {
        fprintf(stderr,
                "psxrecomp: rb follow NACK epoch=%u load=%u frontier=%u\n",
                (unsigned)epoch, (unsigned)load, (unsigned)frontier);
        fflush(stderr);
    }
}

/* §101: refuse FOLLOW with NACK + rate-limited log. Zombie load/epoch used to
 * return silently → initiator sat in SealInputs until RB_SEAL_TIMEOUT_MS (4s)
 * while the follower spammed identical REFUSED lines (session-136 soak). */
static void follow_refuse_nack(uint32_t epoch, uint32_t mismatch, uint32_t load,
                               uint32_t target, int slot, const char *detail)
{
    static uint32_t s_ep;
    static uint32_t s_load;
    static uint32_t s_suppressed;
    int log_line = 1;
    int arm_fresh;

    if (epoch == s_ep && load == s_load) {
        s_suppressed++;
        log_line = 0;
        if ((s_suppressed % 32u) == 0u) {
            fprintf(stderr,
                    "psxrecomp: rb follow REFUSED (+%u similar epoch=%u "
                    "load=%u)%s%s\n",
                    (unsigned)s_suppressed, (unsigned)epoch, (unsigned)load,
                    detail && detail[0] ? " — " : "",
                    detail && detail[0] ? detail : "");
            fflush(stderr);
        }
    } else {
        if (s_suppressed > 0u) {
            fprintf(stderr,
                    "psxrecomp: rb follow REFUSED (+%u similar epoch=%u "
                    "load=%u)\n",
                    (unsigned)s_suppressed, (unsigned)s_ep, (unsigned)s_load);
            fflush(stderr);
            s_suppressed = 0u;
        }
        s_ep = epoch;
        s_load = load;
        if (detail && detail[0]) {
            fprintf(stderr,
                    "psxrecomp: rb follow REFUSED epoch=%u load=%u — %s\n",
                    (unsigned)epoch, (unsigned)load, detail);
            fflush(stderr);
        }
    }

    arm_fresh = !g_follow_nack_pending || g_follow_nack_epoch != epoch ||
                g_follow_nack_load != load;
    if (arm_fresh) {
        g_follow_nack_pending = 1;
        g_follow_nack_epoch = epoch;
        g_follow_nack_mismatch = mismatch;
        g_follow_nack_load = load;
        g_follow_nack_target = target;
        g_follow_nack_slot = slot;
        g_follow_nack_sends = 0;
        send_follow_nack(epoch, mismatch, load, target, slot, log_line);
        g_follow_nack_sends = 1;
    }
}

static void export_local_seals(void)
{
    RNetSession *s = sess();
    RNetRbFrame rows[24];
    uint32_t count = 0;
    uint32_t begin = 0;
    uint32_t valid = 0;
    uint32_t total = 0;
    int slot;
    int i;
    if (!g_rb || !s)
        return;
    /* Only this host's authority seat — exporting empty peer slots confused
     * receivers and is unnecessary (peer seals their own local_slot). */
    slot = g_b.local_slot ? *g_b.local_slot : 0;
    begin = 0;
    while (rnet_rb_export_seal_rows_chunk(g_rb, slot, begin, 24u, rows, &count) &&
           count > 0) {
        for (i = 0; i < (int)count; ++i) {
            total++;
            if (rows[i].is_valid)
                valid++;
        }
        /* Wire "mismatch" field carries seal_base (load_tick), not correction
         * mismatch — peers index the sealed table from that base. */
        (void)rnet_session_send_rb_seal_rows(
            s, rnet_rb_get_epoch_id(g_rb), rnet_rb_get_seal_base_tick(g_rb),
            rnet_rb_get_target_tick(g_rb), (rnet_u8)slot, begin, rows, (rnet_u16)count);
        begin += count;
        if (count < 24u)
            break;
    }
    if (!g_seal_export_logged && total > 0u) {
        fprintf(stderr,
                "psxrecomp: rb seal export slot=%d rows=%u valid=%u span=%u\n",
                slot, (unsigned)total, (unsigned)valid,
                (unsigned)rnet_rb_get_seal_span(g_rb));
        fflush(stderr);
        g_seal_export_logged = 1;
    }
}

/* §52: keep re-exporting our seal rows on the baseline-rexmit cadence until
 * the peer's POST proves it entered Replay (it can only do that once its
 * seal table is complete). Baseline/GO already had keepalive rexmit; seal
 * rows were a single unacknowledged burst per discrete event, so one WAN
 * drop of e.g. the post-tip-extend chunk deadlocked the follower in
 * AwaitingBaseline until verify timeout. Receiver side is idempotent
 * (rows OR into peer_seal_mask), and the export is at most a few 24-row
 * chunks, so the cost is negligible. */
static void maybe_rexmit_seals(void)
{
    uint64_t now;
    if (!g_rb || !rnet_rb_is_active(g_rb) || !rnet_rb_inputs_sealed(g_rb))
        return;
    if (g_peer_post_ok)
        return; /* peer verified with complete seals — nothing left to heal */
    now = rb_mono_ms();
    if (g_last_seal_rexmit_ms != 0ull && now >= g_last_seal_rexmit_ms &&
        (now - g_last_seal_rexmit_ms) < (uint64_t)RB_BASELINE_BURST_MS)
        return;
    /* First call per episode is the begin-time export itself; only log once
     * an actual re-send happens. */
    if (g_last_seal_rexmit_ms != 0ull && !g_seal_rexmit_logged) {
        fprintf(stderr, "psxrecomp: rb seal rexmit base=%u span=%u\n",
                (unsigned)rnet_rb_get_seal_base_tick(g_rb),
                (unsigned)rnet_rb_get_seal_span(g_rb));
        fflush(stderr);
        g_seal_rexmit_logged = 1;
    }
    g_last_seal_rexmit_ms = now;
    export_local_seals();
}

static void publish_sealed_sio(uint32_t tick)
{
    int slot;
    int n = g_b.slot_count ? *g_b.slot_count : 0;
    if (!g_rb || !g_b.apply_frame_slot)
        return;
    /* Hist first (covers any seat/tick the seal table missed), then sealed
     * authority overlays — never the reverse (hist would clobber seals). */
    if (g_b.publish_sio)
        g_b.publish_sio(tick);
    for (slot = 0; slot < n; ++slot) {
        RNetRbFrame row;
        if (!rnet_rb_get_sealed_frame(g_rb, slot, tick, &row) || !row.is_valid)
            continue;
        g_b.apply_frame_slot(slot, tick, row.buttons, row.stick_x, row.stick_y,
                             row.analog);
    }
}

static void maybe_send_baseline(void);
static void maybe_enter_replay(void);
static void accept_peer_baseline(uint32_t epoch, uint32_t load, uint32_t dig_m,
                                 uint32_t dig_a, uint32_t dig_b, uint32_t dig_c);
static void apply_stashed_baseline(void);
static void begin_follower(uint32_t epoch, uint32_t mismatch, uint32_t load,
                           uint32_t target, int slot, uint8_t wire_flags);
static void stash_peer_begin(uint32_t epoch, uint32_t mismatch, uint32_t load,
                             uint32_t target, int slot, uint8_t wire_flags);
static void apply_stashed_begin(void);
static void maybe_rexmit_begin(void);

static void enter_awaiting_baseline(void)
{
    uint32_t load = rnet_rb_get_load_tick(g_rb);
    rnet_rb_set_phase(g_rb, nRNetRbPhaseAwaitingBaseline);
    g_local_baseline_sent = 0;
    g_local_baseline_digest = 0;
    g_local_baseline_ready_sent = 0;
    /* Keep peer baseline / ready if packets arrived during Seal. */
    g_episode_snap_applied = 0;
    g_pending_load_tick = load;
    g_pending_load_valid = 1;
    /* Episode now owns pending_load. A leftover live_realign from abort/
     * NACK would make try_apply_pending_load early-return without
     * g_episode_snap_applied / maybe_send_baseline() — both peers stall
     * on rb_baseline (TM4 soak: epoch 49 load 15184). */
    g_live_realign_pending = 0;
    g_pending_resume_valid = 0;
    g_empty_span_verify_pending = 0;
    g_baseline_rexmit_logged = 0;
    g_wait_peer_bl_logged = 0;
    fprintf(stderr, "psxrecomp: rb episode load_tick=%u (snap pending)\n", (unsigned)load);
    fflush(stderr);
    apply_stashed_baseline();
}

static void accept_peer_baseline(uint32_t epoch, uint32_t load, uint32_t dig_m,
                                 uint32_t dig_a, uint32_t dig_b, uint32_t dig_c)
{
    /* Dedup vs the last *logged* copy of this exact message. RB_BASELINE_BURST
     * sends up to 8 redundant UDP copies per baseline (TURN loses one-shots),
     * from up to ~9 call sites across an episode's phases — soaks showed
     * ~25 identical "peer baseline" lines (each with its own fflush) per
     * episode, none carrying new information beyond the first. During a
     * dense episode chain that's real fflush/syscall overhead landing right
     * on the ticks already busy resimulating. The accepted state
     * (g_peer_baseline_ok/digest/av/aux/ready) is still updated
     * unconditionally on every copy — only the diagnostic print is gated on
     * the tuple actually changing (epoch bump / phase transition / a
     * genuinely different digest would all show up here). */
    static uint32_t s_log_epoch = 0xffffffffu;
    static uint32_t s_log_load;
    static uint32_t s_log_dig_m;
    static uint32_t s_log_dig_a;
    static uint32_t s_log_dig_b;
    static uint32_t s_log_dig_c;
    int changed;
    (void)load;
    g_peer_baseline_ok = 1;
    g_peer_baseline_digest = dig_m;
    g_peer_baseline_av = dig_b;
    g_peer_baseline_aux = dig_c;
    if (dig_a & RB_BL_FLAG_READY) {
        if (!g_peer_baseline_ready) {
            fprintf(stderr,
                    "psxrecomp: rb peer baseline ready-ACK received load=%u\n",
                    (unsigned)load);
            fflush(stderr);
        }
        g_peer_baseline_ready = 1;
    }
    changed = (epoch != s_log_epoch || load != s_log_load || dig_m != s_log_dig_m ||
              dig_a != s_log_dig_a || dig_b != s_log_dig_b || dig_c != s_log_dig_c);
    if (changed) {
        s_log_epoch = epoch;
        s_log_load = load;
        s_log_dig_m = dig_m;
        s_log_dig_a = dig_a;
        s_log_dig_b = dig_b;
        s_log_dig_c = dig_c;
        fprintf(stderr,
                "psxrecomp: rb peer baseline epoch=%u load=%u core=%08x av=%08x ext=%08x "
                "ready=%u (local_applied=%d phase=%d)\n",
                (unsigned)epoch, (unsigned)load, (unsigned)dig_m, (unsigned)dig_b,
                (unsigned)dig_c, (unsigned)(dig_a & RB_BL_FLAG_READY),
                g_episode_snap_applied, (int)rnet_rb_get_phase(g_rb));
        fflush(stderr);
    }
    maybe_send_baseline();
    maybe_enter_replay();
}

static void apply_stashed_baseline(void)
{
    if (!g_stash_bl_valid || !g_rb || !rnet_rb_is_active(g_rb))
        return;
    if (g_stash_bl_epoch != rnet_rb_get_epoch_id(g_rb))
        return;
    g_stash_bl_valid = 0;
    fprintf(stderr, "psxrecomp: rb baseline unstash epoch=%u dig=%08x ready=%u\n",
            (unsigned)g_stash_bl_epoch, (unsigned)g_stash_bl_dig_m,
            (unsigned)(g_stash_bl_dig_a & RB_BL_FLAG_READY));
    fflush(stderr);
    accept_peer_baseline(g_stash_bl_epoch, g_stash_bl_load, g_stash_bl_dig_m,
                         g_stash_bl_dig_a, g_stash_bl_dig_b, g_stash_bl_dig_c);
}

static void stash_or_accept_baseline(uint32_t epoch, uint32_t load, uint32_t dig_m,
                                     uint32_t dig_a, uint32_t dig_b, uint32_t dig_c)
{
    if (g_rb && rnet_rb_is_active(g_rb) && epoch == rnet_rb_get_epoch_id(g_rb)) {
        accept_peer_baseline(epoch, load, dig_m, dig_a, dig_b, dig_c);
        return;
    }
    /* Host often applies+sends before the follower has opened the episode —
     * dropping those packets left the guest forever without peer baseline. */
    g_stash_bl_valid = 1;
    g_stash_bl_epoch = epoch;
    g_stash_bl_load = load;
    g_stash_bl_dig_m = dig_m;
    g_stash_bl_dig_a = dig_a;
    g_stash_bl_dig_b = dig_b;
    g_stash_bl_dig_c = dig_c;
    if (!g_stash_bl_logged) {
        fprintf(stderr,
                "psxrecomp: rb baseline stashed epoch=%u load=%u dig=%08x "
                "(episode not active yet)\n",
                (unsigned)epoch, (unsigned)load, (unsigned)dig_m);
        fflush(stderr);
        g_stash_bl_logged = 1;
    }
}

/* Apply any SEAL_ROWS chunks stashed while the episode wasn't open yet.
 * Mirrors apply_stashed_baseline(); call at every point an episode becomes
 * active for a given epoch (begin_follower open, tip-extend absorb). */
static void apply_stashed_seal_rows(void)
{
    int i;
    if (!g_rb || !rnet_rb_is_active(g_rb))
        return;
    for (i = 0; i < RB_STASH_SEAL_ROWS_MAX; ++i) {
        if (!g_stash_seal_rows[i].valid)
            continue;
        if (g_stash_seal_rows[i].epoch != rnet_rb_get_epoch_id(g_rb)) {
            /* Stale — belongs to an epoch we've since moved past. */
            g_stash_seal_rows[i].valid = 0;
            continue;
        }
        g_stash_seal_rows[i].valid = 0;
        (void)rnet_rb_apply_peer_seal_rows(
            g_rb, g_stash_seal_rows[i].epoch, g_stash_seal_rows[i].mismatch,
            g_stash_seal_rows[i].target, (int32_t)g_stash_seal_rows[i].slot,
            g_stash_seal_rows[i].row_begin, g_stash_seal_rows[i].rows,
            g_stash_seal_rows[i].row_count);
        fprintf(stderr,
                "psxrecomp: rb seal_rows unstash epoch=%u slot=%u row_begin=%u "
                "n=%u\n",
                (unsigned)g_stash_seal_rows[i].epoch,
                (unsigned)g_stash_seal_rows[i].slot,
                (unsigned)g_stash_seal_rows[i].row_begin,
                (unsigned)g_stash_seal_rows[i].row_count);
        fflush(stderr);
    }
    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs &&
        rnet_rb_all_peer_seal_rows_complete(g_rb))
        enter_awaiting_baseline();
}

/* Stash a SEAL_ROWS chunk that arrived for an episode not open yet (or for a
 * different epoch than the active one). Overwrites the oldest/invalid slot
 * once full — losing a stale stash entry just means the sender's normal
 * chunk retransmit cadence re-delivers it, same as a plain drop today. */
static void stash_seal_rows_chunk(uint32_t epoch, uint32_t mismatch, uint32_t target,
                                  uint8_t slot, uint32_t row_begin,
                                  const RNetRbFrame *rows, uint16_t row_count)
{
    int i;
    int victim = 0;
    if (row_count > 24)
        row_count = 24;
    for (i = 0; i < RB_STASH_SEAL_ROWS_MAX; ++i) {
        if (!g_stash_seal_rows[i].valid) {
            victim = i;
            break;
        }
        victim = i; /* full: keep evicting forward, favors newest chunks */
    }
    g_stash_seal_rows[victim].valid = 1;
    g_stash_seal_rows[victim].epoch = epoch;
    g_stash_seal_rows[victim].mismatch = mismatch;
    g_stash_seal_rows[victim].target = target;
    g_stash_seal_rows[victim].slot = slot;
    g_stash_seal_rows[victim].row_begin = row_begin;
    memcpy(g_stash_seal_rows[victim].rows, rows, row_count * sizeof(RNetRbFrame));
    g_stash_seal_rows[victim].row_count = row_count;
    if (!g_stash_seal_rows_logged) {
        fprintf(stderr,
                "psxrecomp: rb seal_rows stashed epoch=%u slot=%u row_begin=%u "
                "n=%u (episode not active yet)\n",
                (unsigned)epoch, (unsigned)slot, (unsigned)row_begin,
                (unsigned)row_count);
        fflush(stderr);
        g_stash_seal_rows_logged = 1;
    }
}

/* §49: ready budget scales with measured RTT (TURN/hotspot). */
static uint32_t rb_ready_timeout_ms(void)
{
    uint32_t rtt = psx_netplay_rb_rtt_estimate_ms();
    uint32_t ms = RB_READY_TIMEOUT_MS;
    if (rtt >= 4u) {
        uint32_t scaled = rtt * 10u; /* ~5 RTT round-trips of headroom */
        if (scaled > ms)
            ms = scaled;
    }
    if (ms > RB_READY_TIMEOUT_MAX_MS)
        ms = RB_READY_TIMEOUT_MAX_MS;
    return ms;
}

static void send_baseline_packet(int ready, int rexmit_log)
{
    RNetSession *s = sess();
    if (!g_rb || !s || !g_local_baseline_sent)
        return;
    (void)rnet_session_send_rb_baseline(s, rnet_rb_get_epoch_id(g_rb),
                                        rnet_rb_get_load_tick(g_rb), g_local_baseline_digest,
                                        ready ? RB_BL_FLAG_READY : 0u, g_local_baseline_av,
                                        g_local_baseline_aux);
    if (ready)
        g_local_baseline_ready_sent = 1;
    if (rexmit_log && !g_baseline_rexmit_logged) {
        fprintf(stderr, "psxrecomp: rb baseline rexmit load=%u dig=%08x ready=%d\n",
                (unsigned)rnet_rb_get_load_tick(g_rb), (unsigned)g_local_baseline_digest, ready);
        fflush(stderr);
        g_baseline_rexmit_logged = 1;
    }
}

/* TURN loses one-shots — burst copies, paced so we don't flood the relay. */
static void send_baseline_burst(int ready, int n, int rexmit_log)
{
    int i;
    uint64_t now = rb_mono_ms();
    if (n < 1)
        n = 1;
    /* First ready=1 must not be swallowed by the rate limit after the initial
     * ready=0 burst — initiator was timing out waiting for an ACK that never
     * left (follower entered Replay with peer_ready=0 and no ready wire). */
    if (ready && !g_local_baseline_ready_sent)
        g_last_baseline_burst_ms = 0;
    if (g_last_baseline_burst_ms != 0ull && now >= g_last_baseline_burst_ms &&
        (now - g_last_baseline_burst_ms) < (uint64_t)RB_BASELINE_BURST_MS)
        return;
    g_last_baseline_burst_ms = now;
    if (ready && !g_local_baseline_ready_sent) {
        fprintf(stderr,
                "psxrecomp: rb baseline ready-ACK sent load=%u (burst=%d)\n",
                (unsigned)rnet_rb_get_load_tick(g_rb), n);
        fflush(stderr);
    }
    for (i = 0; i < n; ++i)
        send_baseline_packet(ready, rexmit_log && i == 0);
}

static void maybe_send_baseline(void)
{
    RNetSession *s = sess();
    CPUState *c = cpu();
    int follower;
    if (!g_rb || !s || !c)
        return;
    if (!g_episode_snap_applied || g_pending_load_valid)
        return; /* must restore before hashing / advertising baseline */
    /* §52: seal-row keepalive rides the same call cadence as baseline/GO
     * rexmit — runs through SealInputs/AwaitingBaseline/Replay/Verify until
     * the peer POSTs. */
    maybe_rexmit_seals();
    /* §60: BEGIN keepalive until the follower joins (peer baseline seen). */
    maybe_rexmit_begin();
    follower = rnet_rb_is_from_peer_notify(g_rb) ? 1 : 0;
    if (!g_local_baseline_sent) {
        NetplayCoreParts parts;
        netplay_core_digest_parts(c, &parts);
        g_local_baseline_digest = parts.core;
        g_local_baseline_av = netplay_av_digest();
        /* dig_c folds SPU+MDEC+CD+spad+DMA+SIO — matched core/av with
         * divergent bus/scratch was doomed Replay (pin zlib skew). */
        g_local_baseline_aux = netplay_baseline_ext_digest();
        g_local_baseline_sent = 1;
        g_baseline_handshake_ms = rb_mono_ms();
        g_last_baseline_burst_ms = 0; /* allow immediate burst */
        {
            int burst = rnet_rb_recommend_light_tip(g_rb) ? 1 : RB_BASELINE_BURST;
            NetplaySioParts sp;
            send_baseline_burst(0, burst, 0);
            netplay_sio_digest_parts(&sp);
            fprintf(stderr,
                    "psxrecomp: rb baseline sent load=%u core=%08x av=%08x ext=%08x cd=%08x "
                    "(burst=%d light=%u) parts cpu=%08x clk=%08x tim=%08x ram=%08x dirty=%08x "
                    "spu=%08x mdec=%08x aux=%08x spad=%08x dma=%08x sio=%08x "
                    "sioP=%08x/%08x/%08x/%08x/%08x\n",
                    (unsigned)rnet_rb_get_load_tick(g_rb),
                    (unsigned)g_local_baseline_digest,
                    (unsigned)g_local_baseline_av, (unsigned)g_local_baseline_aux,
                    (unsigned)netplay_cdrom_digest(), burst,
                    (unsigned)rnet_rb_recommend_light_tip(g_rb),
                    (unsigned)parts.cpu, (unsigned)parts.clock_irq,
                    (unsigned)parts.timers, (unsigned)parts.ram,
                    (unsigned)parts.dirty, (unsigned)netplay_spu_digest(),
                    (unsigned)netplay_mdec_digest(),
                    (unsigned)netplay_aux_digest(),
                    (unsigned)netplay_spad_digest(), (unsigned)netplay_dma_digest(),
                    (unsigned)netplay_sio_digest(),
                    (unsigned)sp.regs, (unsigned)sp.pads, (unsigned)sp.mc,
                    (unsigned)sp.pace, (unsigned)sp.meta);
        }
        fflush(stderr);
        return;
    }
    /* Keep advertising until handshake completes. Follower upgrades to ready=1
     * once it has the peer digest. */
    if (!g_peer_baseline_ok) {
        if (!g_wait_peer_bl_logged) {
            fprintf(stderr, "psxrecomp: rb waiting peer baseline (local core=%08x)\n",
                    (unsigned)g_local_baseline_digest);
            fflush(stderr);
            g_wait_peer_bl_logged = 1;
        }
        send_baseline_burst(0, RB_BASELINE_BURST, 1);
        return;
    }
    if (follower) {
        if (!g_local_baseline_ready_sent ||
            rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline ||
            rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay)
            send_baseline_burst(1, RB_BASELINE_BURST, 1);
    } else if (!g_peer_baseline_ready) {
        send_baseline_burst(0, RB_BASELINE_BURST, 1);
    } else {
        /* §49: initiator already saw follower ACK and emitted GO once in
         * maybe_enter_replay, then stopped advertising. TURN drops that
         * burst → follower ready-timeout while initiator is in Replay/Verify
         * waiting for peer POST. Keep GO alive until peer POST arrives. */
        RNetRbPhase ph = rnet_rb_get_phase(g_rb);
        if (!g_peer_post_ok &&
            (ph == nRNetRbPhaseAwaitingBaseline || ph == nRNetRbPhaseReplay ||
             ph == nRNetRbPhaseVerify))
            send_baseline_burst(1, RB_BASELINE_BURST, 1);
    }
}

/* Cheap per-replayed-tick audit: core digest only (registers + clock/IRQ +
 * timers + full RAM CRC — no VRAM/CD/aux/scratchpad/DMA/SIO). Used for both
 * "arm" and "fin" tags on every tick of a resim span.
 *
 * This used to also carry the *full* bus digest breakdown (av = full VRAM
 * CRC, plus cd/aux/spad/dma/sio/master) on every "fin" call — i.e. on every
 * replayed tick, not just the last one — purely to print it. During a dense
 * back-to-back episode chain (see 2026-08-01 episode-cost note) that meant
 * paying a full ~3 MiB CRC sweep per replayed tick, and then paying most of
 * it *again* moments later in enter_verify_at_tip() for the real POST
 * digest on the commit tick. The full breakdown now runs exactly once, in
 * enter_verify_at_tip(), right where its values are actually needed. */
/* §67 perf: resim audit digest cache. arm(N) state == fin(N-1) state unless a
 * snap load happened in between — reuse the fin digest instead of paying a
 * second full RAM CRC per resim tick (audit was 2×~2.2 MiB CRC per tick and
 * dominated admit during catchup windows). Storage lives near the pin globals
 * (§78: pin_baseline_from_cpu reads the cache). */
static void resim_audit_cache_invalidate(void)
{
    s_resim_audit_valid = 0;
}

static void log_resim_tick_audit(uint32_t sim, const char *tag)
{
    CPUState *c = cpu();
    int slot;
    int n = g_b.slot_count ? *g_b.slot_count : 0;
    uint32_t dig = 0u;
    int is_arm = (tag && tag[0] == 'a');
    NetplayCoreParts parts;
    memset(&parts, 0, sizeof(parts));
    if (is_arm && s_resim_audit_valid && sim == s_resim_audit_sim + 1u) {
        parts.core = s_resim_audit_core;
        dig = parts.core;
    } else if (c) {
        CPUState dig_cpu = *c;
        if (!rb_resume_pc_ok(dig_cpu.pc)) {
            uint32_t alt = g_last_good_bb_pc;
            if (rb_resume_pc_ok(alt))
                dig_cpu.pc = alt;
        }
        parts.core = netplay_core_digest(&dig_cpu);
        dig = parts.core;
    }
    if (!is_arm) {
        s_resim_audit_core = dig;
        s_resim_audit_sim = sim;
        s_resim_audit_valid = 1;
    }
    fprintf(stderr,
            "psxrecomp: rb audit %s sim=%u dig=%08x core=%08x av=00000000 "
            "cd=00000000 cyc=%llu",
            tag ? tag : "?", (unsigned)sim, (unsigned)dig, (unsigned)parts.core,
            (unsigned long long)psx_cycle_count);
    for (slot = 0; slot < n; ++slot) {
        RNetRbFrame row;
        if (rnet_rb_get_sealed_frame(g_rb, slot, sim, &row) && row.is_valid)
            fprintf(stderr, " s%d=%04x%s", slot, (unsigned)row.buttons,
                    row.analog ? "A" : "");
        else
            fprintf(stderr, " s%d=----", slot);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* §55: resim purity — a sealed-span tick may only be armed once every seat's
 * row is authoritative (local, peer-delivered, or wire-confirmed). The
 * 2026-08-02 soak forked when the host armed 4179 with the guest seat
 * unsealed (s1=----): publish fell back to live hist, RAM forked silently
 * (per-tick audit digest is CPU-only), and the FRAME_COMMIT abort at 4180
 * cascaded into an unrecoverable baseline-mismatch spiral. Stalling here is
 * safe: seal keepalive (§52) redelivers rows and the RB_REPLAY_STALL_MS
 * watchdog aborts a genuinely dead episode. */
static int rb_arm_rows_ready(uint32_t sim)
{
    static uint32_t s_wait_log_sim;
    int slot;
    int n = g_b.slot_count ? *g_b.slot_count : 0;
    if (!g_rb || !rnet_rb_inputs_sealed(g_rb))
        return 1;
    if (!rnet_rb_tick_in_sealed_span(g_rb, sim))
        return 1;
    for (slot = 0; slot < n; ++slot) {
        if (rnet_rb_seat_row_authoritative(g_rb, (int32_t)slot, sim))
            continue;
        if (s_wait_log_sim != sim) {
            fprintf(stderr,
                    "psxrecomp: rb arm wait rows sim=%u slot=%d (seat row not "
                    "authoritative — waiting for peer seals)\n",
                    (unsigned)sim, slot);
            fflush(stderr);
            s_wait_log_sim = sim;
        }
        return 0;
    }
    return 1;
}

static void arm_replay_tick(uint32_t sim)
{
    publish_sealed_sio(sim);
    g_stat_replay_ticks++;
    g_stat_replay_ticks_total++;
    g_needs_advance = 1;
    g_replay_progress_ms = rb_mono_ms();
    fprintf(stderr, "psxrecomp: rb arm sim=%u (target=%u)\n", (unsigned)sim,
            (unsigned)rnet_rb_get_target_tick(g_rb));
    fflush(stderr);
    log_resim_tick_audit(sim, "arm");
}

static void arm_rereplay_after_load(uint32_t reload_tick)
{
    RNetSession *s = sess();
    uint32_t target;
    uint32_t first;

    if (!g_rb || !s)
        return;
    resim_audit_cache_invalidate(); /* §67: snap load — fin cache stale */
    target = rnet_rb_get_target_tick(g_rb);
    first = reload_tick + 1u;
    /* Drop TipHold-Live invent FRAME_COMMITs; sealed resim rebuilds the span. */
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, reload_tick);
    g_tip_extend_prime_tick = reload_tick;
    g_episode_snap_applied = 1;
    g_pending_load_valid = 0;
    if (first > target) {
        rnet_session_set_sim_tick(s, reload_tick);
        g_needs_advance = 0;
        /* §112: if resume still pending, stay Replay until flush_resume. */
        if (g_pending_resume_valid) {
            g_empty_span_verify_pending = 1;
            fprintf(stderr,
                    "psxrecomp: rb rereplay empty span load=target=%u "
                    "(defer verify; resume_pending=1)\n",
                    (unsigned)reload_tick);
            fflush(stderr);
        } else {
            enter_verify_at_tip(reload_tick);
        }
        return;
    }
    /* Reset Live tip clock — TipHold invent may sit past the prior tip; arming
     * that tip skipped the sealed span (870 snap → arm 873) and compared
     * against stale peer invent digests. */
    rnet_session_set_sim_tick(s, first);
    /* §55: rereplay first tick must also carry authoritative rows; if the
     * peer's extend seals are still in flight, try_admit arms it later. */
    if (rb_arm_rows_ready(first))
        arm_replay_tick(first);
}

/* Enter Verify/POST at the restored tip (empty resim span, or finish_frame
 * reached target). */
static void enter_verify_at_tip(uint32_t done)
{
    RNetSession *s = sess();
    CPUState *c = cpu();
    uint32_t cd;
    if (!g_rb || !s)
        return;
    /* §77: freeze tip NOW — finish_frame already advanced sim to done+1 and
     * Verify may wait on peer POST/RESOLVED while Live invents past the tip
     * (soak: audit fin@2257=f4d9bd26, tip-hold pin after invent → SNAP-STALE
     * pin≠fin, flush fell back to episode pin). Pin+ring+auth here while CPU
     * still matches the sealed fin; tip-hold entry must not overwrite. */
    if (!pin_baseline_from_cpu(done)) {
        fprintf(stderr,
                "psxrecomp: rb verify tip pin FAILED tip=%u\n",
                (unsigned)done);
        fflush(stderr);
    }
    /* av/cd computed once here and reused by both prints below plus the wire
     * send. Previously av/aux/core were each computed a second time here
     * *in addition to* a full copy already computed moments earlier by the
     * per-tick "fin" audit on this same tick (see log_resim_tick_audit) —
     * i.e. two full ~3 MiB CRC sweeps per commit instead of one. */
    g_post_av = netplay_av_digest();
    cd = netplay_cdrom_digest();
    if (c) {
        CPUState dig_cpu;
        NetplayCoreParts parts;
        NetplaySioParts sp;
        uint32_t aux, spad, dma, sio, master, core;
        log_cpu_digest_split(c, "post");
        psx_netplay_rb_cpu_for_present_digest(&dig_cpu, c);
        netplay_core_digest_parts(&dig_cpu, &parts);
        aux = netplay_aux_digest();
        spad = netplay_spad_digest();
        dma = netplay_dma_digest();
        sio = netplay_sio_digest();
        netplay_sio_digest_parts(&sp);
        master = netplay_master_digest(&dig_cpu);
        core = parts.core;
        /* Wire dig_m = core only (§0 / hookup: POST agrees on core; CD/aux
         * stay audit). Folding aux aborted matched-core tips when SPU/MDEC
         * drifted (soak tip=940: core=9e3e1cb8 both, aux host≠guest). */
        g_post_digest = core;
        fprintf(stderr,
                "psxrecomp: rb post parts core=%08x aux=%08x (wire dig_m=%08x) "
                "dig=%08x | cpu=%08x clk=%08x tim=%08x ram=%08x dirty=%08x "
                "spad=%08x dma=%08x sio=%08x sioP=%08x/%08x/%08x/%08x/%08x\n",
                (unsigned)core, (unsigned)aux, (unsigned)g_post_digest,
                (unsigned)master, (unsigned)parts.cpu, (unsigned)parts.clock_irq,
                (unsigned)parts.timers, (unsigned)parts.ram,
                (unsigned)parts.dirty, (unsigned)spad, (unsigned)dma,
                (unsigned)sio, (unsigned)sp.regs, (unsigned)sp.pads,
                (unsigned)sp.mc, (unsigned)sp.pace, (unsigned)sp.meta);
    } else {
        g_post_digest = 0u;
    }
    g_post_target = done;
    rnet_rb_set_phase(g_rb, nRNetRbPhaseVerify);
    g_verify_wait_ms = rb_mono_ms();
    (void)rnet_session_send_rb_post(s, rnet_rb_get_epoch_id(g_rb), done, g_post_digest,
                                    g_post_av, 1u);
    g_local_post_sent = 1;
    g_post_rexmit_logged = 0;
    fprintf(stderr,
            "psxrecomp: rb post sent tip=%u dig_m=%08x av=%08x cd=%08x (peer_ok=%d)\n",
            (unsigned)g_post_target, (unsigned)g_post_digest, (unsigned)g_post_av,
            (unsigned)cd, g_peer_post_ok);
    fflush(stderr);
    if (g_peer_post_ok) {
        if (g_peer_post_digest == g_post_digest && g_peer_post_av == g_post_av)
            ownership_on_post_match(rnet_rb_get_target_tick(g_rb));
        else {
            char why[128];
            if (g_peer_post_digest != g_post_digest)
                snprintf(why, sizeof(why),
                         "post core diverge local=%08x peer=%08x",
                         (unsigned)g_post_digest, (unsigned)g_peer_post_digest);
            else
                snprintf(why, sizeof(why), "post av diverge local=%08x peer=%08x",
                         (unsigned)g_post_av, (unsigned)g_peer_post_av);
            rnet_rb_on_post_diverge(g_rb);
            abort_episode_realign(why);
        }
    }
}

static void maybe_enter_replay(void)
{
    RNetSession *s = sess();
    uint32_t load;
    int follower;
    if (!g_rb || !s)
        return;
    if (!g_episode_snap_applied || !g_local_baseline_sent || !g_peer_baseline_ok)
        return;
    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay)
        return; /* already armed */
    /* Never leave SealInputs with incomplete peer pads — hist invent would fork. */
    if (!rnet_rb_all_peer_seal_rows_complete(g_rb)) {
        /* §52: this was the only *silent* early return in the gate chain —
         * a lost seal chunk parked us here until the peer's verify timeout
         * with zero log evidence. Name the incomplete seats once we've been
         * waiting long enough that keepalive rexmit should have healed it. */
        if (!g_seal_wait_logged && g_baseline_handshake_ms != 0ull) {
            uint64_t now = rb_mono_ms();
            if (now >= g_baseline_handshake_ms &&
                (now - g_baseline_handshake_ms) >= 500ull) {
                char seats[64];
                int pos = 0;
                int slot;
                int n = g_b.slot_count ? *g_b.slot_count : 0;
                int local = g_b.local_slot ? *g_b.local_slot : 0;
                seats[0] = '\0';
                for (slot = 0; slot < n && pos < (int)sizeof(seats) - 12; ++slot) {
                    if (slot == local)
                        continue;
                    pos += snprintf(seats + pos, sizeof(seats) - (size_t)pos,
                                    " slot%d=%u", slot,
                                    (unsigned)rnet_rb_peer_seal_rows_complete(
                                        g_rb, (int32_t)slot));
                }
                fprintf(stderr,
                        "psxrecomp: rb waiting peer seal rows base=%u span=%u%s\n",
                        (unsigned)rnet_rb_get_seal_base_tick(g_rb),
                        (unsigned)rnet_rb_get_seal_span(g_rb), seats);
                fflush(stderr);
                g_seal_wait_logged = 1;
            }
        }
        return;
    }
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseAwaitingBaseline)
        return;
    if (g_peer_baseline_digest != g_local_baseline_digest) {
        char why[96];
        snprintf(why, sizeof(why), "baseline core mismatch local=%08x peer=%08x",
                 (unsigned)g_local_baseline_digest, (unsigned)g_peer_baseline_digest);
        abort_episode_realign(why);
        return;
    }
    if (g_peer_baseline_av != g_local_baseline_av) {
        char why[112];
        snprintf(why, sizeof(why), "baseline av mismatch local=%08x peer=%08x",
                 (unsigned)g_local_baseline_av, (unsigned)g_peer_baseline_av);
        abort_episode_realign(why);
        return;
    }
    if (g_peer_baseline_aux != g_local_baseline_aux) {
        char why[256];
        snprintf(why, sizeof(why),
                 "baseline ext mismatch local=%08x peer=%08x "
                 "(aux=%08x cd=%08x spad=%08x dma=%08x sio=%08x)",
                 (unsigned)g_local_baseline_aux, (unsigned)g_peer_baseline_aux,
                 (unsigned)netplay_aux_digest(), (unsigned)netplay_cdrom_digest(),
                 (unsigned)netplay_spad_digest(), (unsigned)netplay_dma_digest(),
                 (unsigned)netplay_sio_digest());
        abort_episode_realign(why);
        return;
    }
    g_episode_baseline_matched = 1;
    g_episode_load_tick = rnet_rb_get_load_tick(g_rb);
    follower = rnet_rb_is_from_peer_notify(g_rb) ? 1 : 0;
    /* Light tip: digests already match and load is tip-aligned — skip the
     * ready-ACK RTT (both peers enter Replay on digest match). Full episodes
     * keep symmetric ready (follower ACK → initiator GO). */
    if (!rnet_rb_recommend_light_tip(g_rb)) {
        uint32_t ready_ms = rb_ready_timeout_ms();
        if (follower) {
            send_baseline_burst(1, RB_BASELINE_BURST, 0);
            if (!g_peer_baseline_ready) {
                uint64_t now = rb_mono_ms();
                uint64_t t0 = g_baseline_handshake_ms ? g_baseline_handshake_ms : now;
                if (now >= t0 && (now - t0) >= (uint64_t)ready_ms) {
                    if (!g_ready_timeout_logged) {
                        fprintf(stderr,
                                "psxrecomp: rb ready timeout — abort (no initiator GO) "
                                "waited=%ums budget=%u\n",
                                (unsigned)(now - t0), (unsigned)ready_ms);
                        fflush(stderr);
                        g_ready_timeout_logged = 1;
                    }
                    abort_episode_realign("ready timeout (initiator never sent GO)");
                }
                return;
            }
        } else if (!g_peer_baseline_ready) {
            uint64_t now = rb_mono_ms();
            uint64_t t0 = g_baseline_handshake_ms ? g_baseline_handshake_ms : now;
            send_baseline_burst(0, RB_BASELINE_BURST, 1);
            if (now >= t0 && (now - t0) >= (uint64_t)ready_ms) {
                if (!g_ready_timeout_logged) {
                    fprintf(stderr,
                            "psxrecomp: rb ready timeout — abort (no peer ready-ACK) "
                            "waited=%ums budget=%u\n",
                            (unsigned)(now - t0), (unsigned)ready_ms);
                    fflush(stderr);
                    g_ready_timeout_logged = 1;
                }
                abort_episode_realign("ready timeout (peer never ACK'd baseline)");
            }
            return;
        } else {
            /* Initiator saw follower ACK — emit GO so follower can enter too. */
            send_baseline_burst(1, RB_BASELINE_BURST, 0);
        }
    } else {
        /* Digests match and load is tip-aligned on *this* peer — do not wait
         * for the ready-ACK RTT before entering Replay. Still *emit* ready/GO
         * so a peer that classified the same episode as non-light (its
         * resolved_through was ahead of load after a unilateral HC advance)
         * is not stranded in wait-for-GO until RB_READY_TIMEOUT_MS — that
         * asymmetry was the 4 fps cliff in the 2026-08-01 soak (initiator
         * light-skipped, follower full-path timed out). */
        g_peer_baseline_ready = 1;
        send_baseline_burst(1, RB_BASELINE_BURST, 0);
        fprintf(stderr,
                "psxrecomp: rb light-tip skip wait load=%u target=%u "
                "(still emitted ready/GO for asymmetric peer)\n",
                (unsigned)rnet_rb_get_load_tick(g_rb),
                (unsigned)rnet_rb_get_target_tick(g_rb));
        fflush(stderr);
    }
    load = rnet_rb_get_load_tick(g_rb);
    /* Snap at load_tick is post-present state (already finished that tip).
     * Re-arming load + flush_resume deferred-present used to phantom
     * finish_frame before the latched VBlank IRQ ran — MotK menu wait
     * (0x8006CD54↔0x8006CDA0) then presented on opposite ping-pong edges
     * (v0=slt 1 vs v0=countdown) with a ~5-cycle skew. Skip re-finishing
     * load; first resim quantum is load+1 after the latched IRQ + one
     * real VBlank period. */
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, load);
    rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay);
    g_local_post_sent = 0;
    g_peer_post_ok = 0;
    g_post_rexmit_logged = 0;
    {
        uint32_t target = rnet_rb_get_target_tick(g_rb);
        uint32_t first = load + 1u;
        if (first > target) {
            /* Seal span empty — restored tip is already the target.
             * §112: stay in Replay when resume is pending so flush_resume can
             * longjmp (and enter Verify) before Live. Immediate enter_verify
             * blocked flush then finalize cleared pending → PC=0 exit. */
            rnet_session_set_sim_tick(s, load);
            g_needs_advance = 0;
            fprintf(stderr,
                    "psxrecomp: rb replay empty span load=target=%u "
                    "(resume_pending=%d peer_ready=%d follower=%d)\n",
                    (unsigned)load, g_pending_resume_valid, g_peer_baseline_ready,
                    follower);
            fflush(stderr);
            if (g_pending_resume_valid) {
                g_empty_span_verify_pending = 1;
                fprintf(stderr,
                        "psxrecomp: rb empty span defer verify "
                        "(flush_resume will POST)\n");
                fflush(stderr);
            } else {
                enter_verify_at_tip(load);
            }
        } else {
            rnet_session_set_sim_tick(s, first);
            /* Arm before flush_resume: after longjmp the guest runs to the
             * next vblank, and finish_frame must see needs_advance. */
            arm_replay_tick(first);
            fprintf(stderr,
                    "psxrecomp: rb replay %u..%u (first_arm=%u resume_pending=%d "
                    "peer_ready=%d follower=%d hc_primed_after=%u)\n",
                    (unsigned)load, (unsigned)target, (unsigned)first,
                    g_pending_resume_valid, g_peer_baseline_ready, follower,
                    (unsigned)load);
            fflush(stderr);
        }
    }
}

static void clear_post_handshake(void)
{
    g_local_post_sent = 0;
    g_peer_post_ok = 0;
    g_peer_post_digest = 0;
    g_peer_post_av = 0;
    g_peer_post_match = 0;
    g_post_target = 0;
    g_post_rexmit_logged = 0;
    g_verify_wait_ms = 0;
}

static void clear_tip_extend_prime(void)
{
    g_tip_extend_prime_tick = 0;
}

/* Peek pad buttons from the delay ring at sim-keyed `tick` (0xFFFF if missing). */
static uint16_t tip_hold_peek_buttons(int slot, uint32_t tick)
{
    RNetSession *s = sess();
    RNetInputSample sample;
    rnet_u32 wire;
    int local;
    if (!s || slot < 0)
        return 0xFFFFu;
    wire = np_sched_wire_for_sim(tick);
    local = (g_b.local_slot && slot == *g_b.local_slot) ? 1 : 0;
    if (local) {
        if (!rnet_session_peek_input(s, slot, wire, &sample))
            return 0xFFFFu;
    } else if (!rnet_session_peek_remote_input(s, slot, wire, &sample)) {
        return 0xFFFFu;
    }
    if (!sample.valid || sample.size < 2u)
        return 0xFFFFu;
    return (uint16_t)sample.bytes[0] | ((uint16_t)sample.bytes[1] << 8);
}

/* TipHold SAFETY/quiet "held" split (§36/§37):
 * - Local: live physical pad (sim/staged stay frozen under invent-cap).
 * - Remote: sealed *tip* row only — do NOT walk tip+1..runway. A post-tip
 *   press while tip is idle must tip-extend / open a fresh episode after
 *   quiet (§35); treating latest-ahead as held forced ABSOLUTE 2000ms on the
 *   lagging peer (soak tip=1187: slot0 quiet, slot1 ABSOLUTE + lead cliff).
 *   §45: RACE CAP + REMOTE-HELD 500ms wall cut that park short when the peer
 *   tip has already left TipHold. */
static void tip_hold_held_split(uint32_t tip, int *held_local_out, int *held_remote_out)
{
    int slots = g_b.slot_count ? *g_b.slot_count : 0;
    int local = g_b.local_slot ? *g_b.local_slot : -1;
    int slot;
    int held_local = 0;
    int held_remote = 0;
    if (slots < 1)
        slots = 2;
    for (slot = 0; slot < slots; ++slot) {
        if (slot == local) {
            uint16_t live = 0xFFFFu;
            if (psx_netplay_live_pad_buttons(&live) && live != 0xFFFFu)
                held_local = 1;
            continue;
        }
        {
            uint16_t tip_btn = tip_hold_peek_buttons(slot, tip);
            if (g_b.ih) {
                RNetRbFrame tip_row;
                if (netplay_ih_get(g_b.ih, slot, tip, &tip_row) && tip_row.is_valid)
                    tip_btn = tip_row.buttons;
            }
            if (tip_btn != 0xFFFFu)
                held_remote = 1;
        }
    }
    if (held_local_out)
        *held_local_out = held_local;
    if (held_remote_out)
        *held_remote_out = held_remote;
}

/* 1 if wire tip+1..tip+runway has an unabsorbed RELEASE vs tip for a remote
 * seat. §35: do NOT treat a future press (tip already idle) as pending —
 * that over-blocked quiet for the full SAFETY window (soak: 21/30 SAFETY).
 * Future presses tip-extend or open a fresh episode after quiet commit. */
static int tip_hold_wire_pending_release(uint32_t tip)
{
    uint32_t runway;
    int slots;
    int slot;
    int local;
    if (!g_rb || tip == 0u)
        return 0;
    runway = rnet_rb_get_tip_runway(g_rb);
    if (runway == 0u)
        return 0;
    slots = g_b.slot_count ? *g_b.slot_count : 0;
    local = g_b.local_slot ? *g_b.local_slot : -1;
    if (slots < 1)
        slots = 2;
    for (slot = 0; slot < slots; ++slot) {
        uint16_t tip_btn;
        uint32_t t;
        if (slot == local)
            continue;
        tip_btn = tip_hold_peek_buttons(slot, tip);
        if (g_b.ih) {
            RNetRbFrame tip_row;
            if (netplay_ih_get(g_b.ih, slot, tip, &tip_row) && tip_row.is_valid)
                tip_btn = tip_row.buttons;
        }
        /* Tip already idle — nothing to coalesce-release. */
        if (tip_btn == 0xFFFFu)
            continue;
        for (t = tip + 1u; t <= tip + runway; ++t) {
            uint16_t b;
            RNetSession *s = sess();
            RNetInputSample sample;
            rnet_u32 wire = np_sched_wire_for_sim(t);
            if (!s || !rnet_session_peek_remote_input(s, slot, wire, &sample))
                break;
            b = tip_hold_peek_buttons(slot, t);
            /* Any delta while tip is held (usually release) needs coalesce. */
            if (b != tip_btn)
                return 1;
        }
    }
    return 0;
}

/* §47/§48: after chunk/final Verify POST match — stay in Replay ownership while
 * contiguous confirmed work remains; otherwise Final Verify → Live. */
static void ownership_chain_next_span(uint32_t tip, uint32_t frontier)
{
    int slot;
    int local;
    uint32_t mismatch;
    uint32_t prev_load;

    if (!g_rb)
        return;
    local = g_b.local_slot ? *g_b.local_slot : 0;
    slot = (local == 0) ? 1 : 0;
    if (g_b.slot_count && *g_b.slot_count < 2)
        slot = local;
    /* Preserve dense committed span for choose_load. Setting span_lo=tip
     * emptied (lo, through] so the next chain walked back to %iv snaps
     * (soak: tip=904 → load=896 → re-replay sticky Up). */
    prev_load = rnet_rb_get_load_tick(g_rb);

    /* Commit tip agreement without TipHold invent-cap (Verify barrier only). */
    g_episode_count++;
    g_agreed_through = tip;
    g_agreed_span_lo = (prev_load < tip) ? prev_load : tip;
    g_agreed_valid = 1;
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, tip);
    advertise_agreed_resolved(tip);
    g_last_commit_epoch = rnet_rb_get_epoch_id(g_rb); /* §42 P1 */
    if (g_stash_begin_valid && g_stash_begin_epoch == g_last_commit_epoch) {
        g_stash_begin_valid = 0;
        g_stash_begin_logged = 0;
    }
    send_commit_notice(g_last_commit_epoch, tip);

    /* §70/§77: tip was pinned at enter_verify_at_tip. Re-pinning after a
     * Verify wait lets Live invent poison the pin (same SNAP-STALE class as
     * tip-hold entry). Keep verify pin when present. */
    if (g_pin_valid && g_pin_tick == tip && g_pin_data && g_pin_size) {
        if (tip > g_auth_snap_through)
            g_auth_snap_through = tip;
        fprintf(stderr,
                "psxrecomp: rb ownership chain keep verify pin tip=%u "
                "bytes=%zu\n",
                (unsigned)tip, g_pin_size);
        fflush(stderr);
    } else if (!pin_baseline_from_cpu(tip)) {
        fprintf(stderr,
                "psxrecomp: rb ownership chain tip=%u — CPU pin FAILED "
                "(skip-snap disabled)\n",
                (unsigned)tip);
        fflush(stderr);
    }

    rnet_rb_session_reset(g_rb);
    g_tip_hold_until = 0;
    g_tip_hold_quiet_t0_ms = 0ull;
    g_tip_hold_block_quiet = 0;
    g_tip_hold_enter_ms = 0ull;
    g_tip_hold_coalesce_n = 0u;
    g_tip_extend_rereplay = 0;
    g_tip_extend_from_tick = 0;
    clear_tip_hold_rereplay_pending();
    clear_tip_extend_prime();
    g_needs_advance = 0;
    /* §51 Layer 3: continue from matched tip — restore pin on apply (§70). */
    g_ownership_skip_snap = g_pin_valid && g_pin_tick == tip ? 1 : 0;
    g_episode_snap_applied = 0;
    /* §112: do not clear g_pending_resume_valid — empty-span heal may still
     * owe flush_resume; next begin/apply re-arms or flush consumes it. */
    g_empty_span_verify_pending = 0;
    g_episode_baseline_matched = 0;
    clear_episode_wire_state();
    /* Keep g_pin — do not clear_baseline_pin(). */
    clear_post_handshake();
    g_bl_mismatch_streak = 0;
    g_fork_streak = 0u;
    g_fork_streak_tick = 0u;
    g_bl_fork_cap = 0u; /* §55: commit/reset clears the bisect cap */
    g_peer_nack_floor = 0u; /* §62 */

    /* §85: count this hop for both seats (follower waits for BEGIN). */
    if (g_ownership_chain_origin_tip == 0u)
        g_ownership_chain_origin_tip = tip;
    g_ownership_chain_hops++;

    /* §48: only the lower seat initiates chain BEGINs. Both peers calling
     * begin_rewind every chunk caused dual-init → tie-break abort → reload
     * storm (host WIN / guest YIELD loop around sticky invent). Follower
     * commits the tip watermark above and waits for peer SYNC BEGIN. */
    if (local != 0) {
        fprintf(stderr,
                "psxrecomp: rb ownership chain tip=%u frontier=%u — wait FOLLOW "
                "(slot %d; skip-snap pin=%d hop=%u)\n",
                (unsigned)tip, (unsigned)frontier, local, g_ownership_skip_snap,
                (unsigned)g_ownership_chain_hops);
        fflush(stderr);
        /* §60: initiator's next BEGIN often arrived (and was stashed) while we
         * were still in Verify — apply it now that we are idle. */
        apply_stashed_begin();
        return;
    }

    mismatch = tip + 1u;
    if (mismatch > frontier)
        mismatch = frontier;
    g_ownership_chain_frontier = frontier;
    g_ownership_chain = 1;
    fprintf(stderr,
            "psxrecomp: rb ownership chain tip=%u frontier=%u mismatch=%u "
            "(Replay owns catch-up — no TipHold; skip-snap pin=%d hop=%u)\n",
            (unsigned)tip, (unsigned)frontier, (unsigned)mismatch,
            g_ownership_skip_snap, (unsigned)g_ownership_chain_hops);
    fflush(stderr);
    if (!psx_netplay_rb_begin_rewind(mismatch, slot)) {
        fprintf(stderr,
                "psxrecomp: rb ownership chain FAILED tip=%u frontier=%u — "
                "tip-hold\n",
                (unsigned)tip, (unsigned)frontier);
        fflush(stderr);
        g_ownership_skip_snap = 0;
        ownership_clear_chain_budget();
        enter_tip_hold(tip);
    }
    g_ownership_chain = 0;
    g_ownership_chain_frontier = 0;
}

static void ownership_on_post_match(uint32_t tip)
{
    uint32_t frontier;
    uint32_t min_ahead;

    /* Contiguous frontier from the tick after the verified tip. */
    frontier = psx_netplay_rb_confirmed_frontier(tip + 1u);
    /* §102: peer-ahead light tip — one Verify then Live (no SPAN hops). */
    if (g_peer_ahead_light_episode) {
        fprintf(stderr,
                "psxrecomp: rb peer-ahead light tip=%u frontier=%u → Live "
                "(no ownership chain)\n",
                (unsigned)tip, (unsigned)frontier);
        fflush(stderr);
        g_peer_ahead_light_episode = 0;
        ownership_exit_to_live(tip, frontier, "peer-ahead light", 1);
        return;
    }
    /* §109: apply-only post-FMV heal KF — POST already matched at load.
     * Do NOT ownership-chain SPAN through the soft-fork window (soak session
     * 28: heal POST tip=826 matched, then chain hop target=850 hung guest
     * wait-FOLLOW → peer disconnect). */
    if (g_post_fmv_heal_kf) {
        fprintf(stderr,
                "psxrecomp: rb post-FMV heal KF tip=%u frontier=%u → Live "
                "(no ownership chain)\n",
                (unsigned)tip, (unsigned)frontier);
        fflush(stderr);
        ownership_exit_to_live(tip, frontier, "post-FMV heal KF", 1);
        return;
    }
    /* §48/§49/§73/§86/§87 B/§104: micro frontiers at/below min_ahead do not
     * chain. min_ahead = max(6, D-1) — delay cushion is not catch-up backlog,
     * but tip+D still chains once (D-1 headroom) instead of tip-hold every
     * POST. */
    min_ahead = ownership_chain_min_ahead();
    if (frontier > tip + min_ahead) {
        uint32_t max_hops = ownership_chain_max_hops();
        uint32_t max_ticks = ownership_chain_max_ticks();
        uint32_t origin = g_ownership_chain_origin_tip;
        uint32_t span = (origin != 0u && tip >= origin) ? (tip - origin) : 0u;
        /* §86 C: post-BUDGET suppress — log and Live; residual hitches show
         * here for diagnosis. */
        if (ownership_chain_suppressed(tip)) {
            fprintf(stderr,
                    "psxrecomp: rb ownership chain SUPPRESS tip=%u "
                    "frontier=%u min_ahead=%u (until_tip=%u) → Live\n",
                    (unsigned)tip, (unsigned)frontier, (unsigned)min_ahead,
                    (unsigned)g_chain_suppress_until_tip);
            fflush(stderr);
            ownership_exit_to_live(tip, frontier, "chain suppress", 0);
            return;
        }
        /* §85/§86 A: over budget → Live (not TipHold invent-cap). */
        if ((max_hops > 0u && g_ownership_chain_hops >= max_hops) ||
            (origin != 0u && span >= max_ticks)) {
            fprintf(stderr,
                    "psxrecomp: rb ownership chain BUDGET tip=%u "
                    "frontier=%u hops=%u span=%u (max_hops=%u "
                    "max_ticks=%u) → Live\n",
                    (unsigned)tip, (unsigned)frontier,
                    (unsigned)g_ownership_chain_hops, (unsigned)span,
                    (unsigned)max_hops, (unsigned)max_ticks);
            fflush(stderr);
            ownership_exit_to_live(tip, frontier, "chain budget", 1);
            return;
        }
        ownership_chain_next_span(tip, frontier);
        return;
    }
    /* §87: confirmed ticks still ahead (delay cushion) — Live, not TipHold
     * invent_slack park. Soak: tip-hold + coalesce-ahead → dual-init →
     * zombie epoch follow → rb_baseline / pcap_freeze hang. TipHold only
     * when the next tick is genuinely unavailable (frontier <= tip). */
    if (frontier > tip) {
        fprintf(stderr,
                "psxrecomp: rb ownership final tip=%u frontier=%u "
                "min_ahead=%u → Live (confirmed ahead)\n",
                (unsigned)tip, (unsigned)frontier, (unsigned)min_ahead);
        fflush(stderr);
        ownership_exit_to_live(tip, frontier, "final confirmed ahead", 1);
        return;
    }
    /* Exhausted contiguous confirmed work → tip-hold until pads quiet.
     * §63: immediate finalize_tip_hold() Live-walked the next dpad/Start
     * edges, then the following episode rewound and re-applied them
     * (pad-log DUP_SIO / felt doubled). Tip-hold invent-cap + quiet/coalesce
     * owns those edges instead. */
    fprintf(stderr,
            "psxrecomp: rb ownership final tip=%u frontier=%u "
            "min_ahead=%u → tip-hold\n",
            (unsigned)tip, (unsigned)frontier, (unsigned)min_ahead);
    fflush(stderr);
    ownership_clear_chain_budget();
    /* §109/§114: heal KF reached tip with no confirmed ahead — still
     * converged; keep invent off + KF stream (do not RELEASE). §115: if the
     * pin already advanced past the original keep, accept instead. */
    if (g_post_fmv_heal_kf) {
        uint32_t fork = g_last_begin_mismatch;
        if (fork == 0xffffffffu)
            fork = g_post_fmv_heal_fork_tick;
        g_post_fmv_heal_kf = 0;
        g_fmv_core_match_streak = 0;
        if (g_post_fmv_platform_nondet &&
            g_post_fmv_platform_keep_tick > 0u &&
            tip > g_post_fmv_platform_keep_tick) {
            rb_post_fmv_platform_accept(tip, fork,
                                       "heal tip-hold past platform keep");
        } else {
            rb_post_fmv_platform_nondet_enter(tip, fork, tip,
                                             "post-FMV heal KF tip-hold");
        }
    }
    /* §61: discard any stashed BEGIN for this episode (rexmit) so Live does
     * not reopen it. */
    if (g_stash_begin_valid && g_rb &&
        g_stash_begin_epoch == rnet_rb_get_epoch_id(g_rb)) {
        g_stash_begin_valid = 0;
        g_stash_begin_logged = 0;
    }
    enter_tip_hold(tip);
}

void psx_netplay_rb_ownership_step(void)
{
    RNetSession *s = sess();
    uint32_t sim;
    uint32_t target;
    uint32_t frontier;
    uint32_t want;
    uint32_t seal_base;
    uint32_t chunk_cap;
    int slot;
    int local;

    if (!g_rb || !s || !rnet_rb_is_active(g_rb))
        return;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return;

    sim = rnet_session_sim_tick(s);
    target = rnet_rb_get_target_tick(g_rb);
    frontier = psx_netplay_rb_confirmed_frontier(sim + 1u);

    /* Exhausted relative to cursor — finish_frame will Verify at tip. */
    if (frontier <= sim)
        return;
    /* More confirmed work than sealed tip — extend within SPAN chunk. */
    if (frontier <= target)
        return;

    seal_base = rnet_rb_get_seal_base_tick(g_rb);
    chunk_cap = seal_base + RB_MAX_RESIM_SPAN - 1u;
    want = frontier;
    if (want > chunk_cap)
        want = chunk_cap;
    /* §85: freeze at the episode's begin target — new wire during Replay is
     * TipHold/Live's job after this catch-up commits, not an unbounded
     * tip-extend treadmill (§74). */
    if (g_replay_catchup_cap > 0u && want > g_replay_catchup_cap)
        want = g_replay_catchup_cap;
    if (want <= target)
        return; /* span full for this chunk — Verify then chain on POST match */

    local = g_b.local_slot ? *g_b.local_slot : 0;
    slot = (local == 0) ? 1 : 0;
    if (g_b.slot_count && *g_b.slot_count < 2)
        slot = local;

    host_prefresh_seal_range(slot, target + 1u, want);
    if (local >= 0 && local != slot)
        host_prefresh_seal_range(local, target + 1u, want);

    if (!rnet_rb_can_extend_target(g_rb, want)) {
        fprintf(stderr,
                "psxrecomp: rb ownership extend blocked sim=%u target=%u "
                "want=%u frontier=%u (chunk Verify will chain)\n",
                (unsigned)sim, (unsigned)target, (unsigned)want,
                (unsigned)frontier);
        fflush(stderr);
        return;
    }
    fprintf(stderr,
            "psxrecomp: rb ownership extend sim=%u target %u→%u frontier=%u\n",
            (unsigned)sim, (unsigned)target, (unsigned)want, (unsigned)frontier);
    fflush(stderr);
    (void)psx_netplay_rb_tip_extend(want, slot);
}

static void enter_tip_hold(uint32_t target)
{
    RNetSession *s = sess();
    uint32_t runway;
    uint32_t sim;
    uint32_t base;
    int i;
    /* Capture POST digests before clear_post_handshake — we still owe the
     * peer a reliable delivery of our POST even after we leave Verify. */
    uint32_t post_dig = g_post_digest;
    uint32_t post_av = g_post_av;
    uint32_t post_tip = g_post_target ? g_post_target : target;
    int had_local_post = g_local_post_sent;
    /* §85: leaving Replay ownership — drop chain budget / catchup freeze. */
    ownership_clear_chain_budget();
    if (!g_rb || !rnet_rb_enter_tip_hold(g_rb))
        return;
    /* Episode boundary: don't let resim cost trip timesync adaptive-off. */
    psx_netplay_timesync_on_episode_boundary();
    g_agreed_through = target;
    g_agreed_span_lo = g_episode_load_tick;
    g_agreed_valid = 1;
    /* Drop invent-hold FRAME_COMMIT confirms past this tip. Without priming,
     * advance_agreed_watermark_from_hc immediately re-ADVANCE'd over the
     * hold-last ticks that both peers matched before the release arrived
     * (soak: release commit tip=1607 then agreed ADVANCE 1607→1632 from
     * stale HC — ghost second-release episodes / double menu inputs). */
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, target);
    rnet_rb_set_peer_convergence(g_rb, target);
    if (s) {
        /* Burst RESOLVED + POST: the peer that has not yet received our POST
         * is still in Verify and will time out unless these land. One-shot
         * UDP was the 2026-08-01 "peer POST missing" / tip-extend NACK storm. */
        for (i = 0; i < RB_POST_BURST; ++i) {
            (void)rnet_session_send_rb_resolved(s, target);
            if (had_local_post)
                (void)rnet_session_send_rb_post(s, rnet_rb_get_epoch_id(g_rb),
                                                post_tip, post_dig, post_av, 1u);
        }
        /* §41: tip-hold enter is a confirmed RESOLVED — track for heartbeat. */
        if (target > g_local_advertised_through)
            g_local_advertised_through = target;
        g_peer_resolved_hb_last_ms = rb_mono_ms();
        if (g_peer_resolved_hb_last_ms == 0ull)
            g_peer_resolved_hb_last_ms = 1ull;
    }
    runway = rnet_rb_get_tip_runway(g_rb);
    sim = s ? rnet_session_sim_tick(s) : target;
    /* Quiet window from the later of sealed tip and live tip — Live may already
     * sit past target when tip-extend rereplay returns to TipHold. */
    base = (sim > target) ? sim : target;
    g_tip_hold_until = base + runway;
    g_tip_hold_quiet_t0_ms = 0ull; /* arm on first stalled idle poll */
    g_tip_hold_block_quiet = 0;
    g_tip_hold_enter_ms = rb_mono_ms();
    if (g_tip_hold_enter_ms == 0ull)
        g_tip_hold_enter_ms = 1ull;
    g_tip_hold_coalesce_n = 0u;
    /* Fresh TipHold after Verify — deferred rereplay already flushed or N/A. */
    clear_tip_hold_rereplay_pending();
    g_needs_advance = 0;
    /* §77: tip was pinned at enter_verify_at_tip while CPU == sealed fin.
     * Re-pinning here after Verify wait / invent_slack walks poisons the pin
     * (soak: tip-hold pin≠audit fin → SNAP-STALE → episode-pin hitch). Keep
     * the verify pin; only capture if verify never pinned this tip. */
    if (g_pin_valid && g_pin_tick == target && g_pin_data && g_pin_size) {
        if (target > g_auth_snap_through)
            g_auth_snap_through = target;
        fprintf(stderr,
                "psxrecomp: rb tip-hold keep verify pin tip=%u bytes=%zu\n",
                (unsigned)target, g_pin_size);
        fflush(stderr);
    } else if (!pin_baseline_from_cpu(target)) {
        fprintf(stderr,
                "psxrecomp: rb tip-hold pin FAILED tip=%u — flush may "
                "SNAP-STALE\n",
                (unsigned)target);
        fflush(stderr);
    }
    /* If we never sampled RTT in the peer-POST path (common for the peer
     * that receives POST first / light-tip skips), take one here from
     * POST-send → tip-hold when the wait was meaningful. */
    if (had_local_post && g_verify_wait_ms != 0ull && g_rb_rtt_ema_ms < 4u) {
        uint64_t now = rb_mono_ms();
        if (now >= g_verify_wait_ms) {
            uint64_t sample = now - g_verify_wait_ms;
            if (sample >= 4ull && sample <= 2000ull)
                g_rb_rtt_ema_ms = (uint32_t)sample;
        }
    }
    clear_post_handshake();
    clear_tip_extend_prime();
    fprintf(stderr, "psxrecomp: rb tip-hold through=%u until=%u invent_slack=%u\n",
            (unsigned)target, (unsigned)g_tip_hold_until,
            (unsigned)rnet_rb_get_tip_seal_slack(g_rb));
    fflush(stderr);
}

static void clear_tip_hold_rereplay_pending(void)
{
    g_tip_hold_rereplay_pending = 0;
    g_tip_hold_rereplay_from = 0;
    g_tip_hold_rereplay_mismatch = 0;
}

/* §66: flush batched TipHold tip-extend rereplay. Returns 1 if Replay armed
 * (caller must not tip-hold-commit). */
/* §69: before deferred flush, raise tip through contiguous confirmed work so
 * one rereplay covers the ownership-chain remainder (soak: flush@4208→4213
 * then immediate ownership-chain begin@4215 for frontier 4239). */
static void tip_hold_absorb_frontier_before_flush(void)
{
    RNetSession *s = sess();
    uint32_t tip;
    uint32_t frontier;
    uint32_t want;
    uint32_t seal_base;
    int slot;
    int local;
    if (!g_rb || !s || !g_tip_hold_rereplay_pending)
        return;
    tip = rnet_rb_get_target_tick(g_rb);
    frontier = psx_netplay_rb_confirmed_frontier(tip + 1u);
    if (frontier <= tip)
        return;
    seal_base = rnet_rb_get_seal_base_tick(g_rb);
    want = frontier;
    /* §70: one deferred flush ≤ one SPAN chunk; leftover is ownership-chain
     * after POST (soak: absorb 2562→2607 = 45-tick hitch). */
    if (want > tip + RB_MAX_RESIM_SPAN)
        want = tip + RB_MAX_RESIM_SPAN;
    if (want > seal_base + RNET_RB_PEER_SEAL_MASK_BITS - 1u)
        want = seal_base + RNET_RB_PEER_SEAL_MASK_BITS - 1u;
    if (want <= tip)
        return;
    if (!rnet_rb_can_extend_target(g_rb, want))
        return;
    local = g_b.local_slot ? *g_b.local_slot : 0;
    slot = (int)rnet_rb_get_corrected_slot(g_rb);
    if (slot < 0)
        slot = (local == 0) ? 1 : 0;
    host_prefresh_seal_range(slot, tip + 1u, want);
    if (local != slot)
        host_prefresh_seal_range(local, tip + 1u, want);
    if (!rnet_rb_extend_target(g_rb, want))
        return;
    (void)rnet_rb_resign_slot_range(g_rb, slot, tip + 1u, want);
    /* §81: do NOT send REREPLAY SYNC here. Flush immediately follows and
     * sends one SYNC with the correct prefer_plus. Absorb used to emit
     * episode-original mismatch + REREPLAY, then flush emitted first_bad —
     * FOLLOW double-scheduled rereplay (soak prefer_plus=4990 then 5019). */
    export_local_seals();
    fprintf(stderr,
            "psxrecomp: rb tip-hold absorb frontier %u→%u before flush "
            "(frontier=%u)\n",
            (unsigned)tip, (unsigned)want, (unsigned)frontier);
    fflush(stderr);
    {
        uint32_t runway = rnet_rb_get_tip_runway(g_rb);
        uint32_t cap = want + runway;
        if (cap > g_tip_hold_until)
            g_tip_hold_until = cap;
    }
}

static int flush_tip_hold_deferred_rereplay(void)
{
    RNetSession *s;
    uint32_t from;
    uint32_t tip;
    uint32_t first_bad;
    uint32_t runway;
    uint32_t cap;
    uint32_t sim;
    uint32_t prefer_plus;
    int parked;
    int32_t slot;
    if (!g_tip_hold_rereplay_pending || !g_rb)
        return 0;
    tip_hold_absorb_frontier_before_flush();
    from = g_tip_hold_rereplay_from;
    first_bad = g_tip_hold_rereplay_mismatch;
    tip = rnet_rb_get_target_tick(g_rb);
    if (from == 0u)
        from = tip;
    /* §78: Live invent matched the seals on every tick before the earliest
     * coalesce mismatch, so the live ring snap at first_bad-1 sits on the
     * verified timeline. Resim only the truly-divergent suffix; §76/§77's
     * pin reload at `from` forked against the peer's raise-only continuation
     * (asymmetric reload: load(save(S)) re-derives device timing ≠ live S). */
    if (first_bad <= from || first_bad > tip)
        first_bad = from + 1u;
    /* Late-wire guard: a row landing below the recorded mismatch after the
     * last coalesce peek breaks the matched-prefix assumption. Hold-last
     * invent == sealed row at `from`; lower first_bad to the first sealed
     * remote row that differs (or is not yet valid). */
    if (first_bad > from + 1u) {
        int local = g_b.local_slot ? *g_b.local_slot : 0;
        int n = g_b.slot_count ? *g_b.slot_count : 0;
        int slot_i;
        for (slot_i = 0; slot_i < n; ++slot_i) {
            RNetRbFrame base;
            uint32_t t;
            if (slot_i == local)
                continue;
            if (!rnet_rb_get_sealed_frame(g_rb, slot_i, from, &base) ||
                !base.is_valid) {
                first_bad = from + 1u;
                break;
            }
            for (t = from + 1u; t < first_bad; ++t) {
                RNetRbFrame row;
                if (!rnet_rb_get_sealed_frame(g_rb, slot_i, t, &row) ||
                    !row.is_valid || row.buttons != base.buttons ||
                    row.analog != base.analog) {
                    first_bad = t;
                    break;
                }
            }
        }
    }
    s = sess();
    sim = s ? rnet_session_sim_tick(s) : 0u;
    /* §80/§81: Live parked at the POST tip → KEEP-LIVE / raise-only from
     * `from`. Invent walk past tip forced one-sided invent-snap reload while
     * FOLLOW stayed raise-only (soak FIRST_MISMATCH@4023). §81 parks admit
     * at `from` once deferred; also treat sealed tip audit as parked when
     * Live took one invent step before the first defer. */
    parked = (sim <= from) ? 1 : 0;
    if (!parked && s_resim_audit_valid && s_resim_audit_sim == from)
        parked = 1;
    prefer_plus = parked ? (from + 1u) : first_bad;
    fprintf(stderr,
            "psxrecomp: rb tip-hold deferred rereplay flush from=%u "
            "first_bad=%u tip=%u coalesce_n=%u parked=%d prefer_plus=%u\n",
            (unsigned)from, (unsigned)first_bad, (unsigned)tip,
            (unsigned)g_tip_hold_coalesce_n, parked, (unsigned)prefer_plus);
    fflush(stderr);
    clear_tip_hold_rereplay_pending();
    /* Abandon keys on the provably-matched tip below the reload. */
    g_tip_extend_from_tick = from;
    /* §80: notify FOLLOW — tip-hold flush used to be local-only, so FOLLOW
     * Verify/Replay raise-only never reloaded the same prefer. */
    if (s && !rnet_rb_is_from_peer_notify(g_rb)) {
        rnet_u8 wire_flags = (rnet_u8)RNET_RB_SYNC_FLAG_REREPLAY;
        if (rnet_rb_recommend_light_tip(g_rb))
            wire_flags |= RNET_RB_SYNC_FLAG_LIGHT_TIP;
        slot = rnet_rb_get_corrected_slot(g_rb);
        (void)rnet_session_send_rb_sync(
            s, rnet_rb_get_epoch_id(g_rb), prefer_plus,
            rnet_rb_get_load_tick(g_rb), tip,
            (rnet_u8)(slot < 0 ? 0 : slot), RNET_RB_SYNC_OP_BEGIN, wire_flags);
    }
    schedule_episode_rereplay(prefer_plus);
    runway = rnet_rb_get_tip_runway(g_rb);
    cap = tip + runway;
    if (cap > g_tip_hold_until)
        g_tip_hold_until = cap;
    g_tip_hold_quiet_t0_ms = 0ull;
    return 1;
}

/* TipHold commit wall: repair deferred invent first, then commit. */
static void tip_hold_try_finalize(void)
{
    if (flush_tip_hold_deferred_rereplay())
        return;
    finalize_tip_hold();
}

static void finalize_tip_hold(void)
{
    uint32_t target;
    if (!g_rb || !rnet_rb_is_tip_holding(g_rb))
        return;
    target = rnet_rb_get_target_tick(g_rb);
    /* §67: NEVER commit a deferred-extension tip that was not resimmed —
     * the raised span (from+1..tip) still holds invent-predicted state on
     * this side. Commit at the POST-matched tip we extended from; the
     * dropped edges are repaired by the successor episode (yield / FMV
     * callers). Wall paths flush first via tip_hold_try_finalize, so they
     * arrive here with pending already cleared. */
    if (g_tip_hold_rereplay_pending && g_tip_hold_rereplay_from != 0u &&
        g_tip_hold_rereplay_from < target) {
        fprintf(stderr,
                "psxrecomp: rb tip-hold commit clamp %u→%u "
                "(deferred extension dropped)\n",
                (unsigned)target, (unsigned)g_tip_hold_rereplay_from);
        fflush(stderr);
        target = g_tip_hold_rereplay_from;
    }
    g_episode_count++;
    g_agreed_through = target;
    g_agreed_span_lo = target;
    g_agreed_valid = 1;
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, target);
    /* Shared frontier: advertise the committed tip (§40/§41 heartbeat track). */
    advertise_agreed_resolved(target);
    fprintf(stderr, "psxrecomp: rb episode commit through=%u (tip-hold)\n", (unsigned)target);
    fflush(stderr);
    g_last_commit_epoch = rnet_rb_get_epoch_id(g_rb); /* §42 P1 */
    /* §61: drop same-epoch BEGIN stash so Live does not reopen it. */
    if (g_stash_begin_valid && g_stash_begin_epoch == g_last_commit_epoch) {
        g_stash_begin_valid = 0;
        g_stash_begin_logged = 0;
    }
    send_commit_notice(g_last_commit_epoch, target);  /* §42c */
    rnet_rb_session_reset(g_rb);
    g_tip_hold_until = 0;
    g_tip_hold_quiet_t0_ms = 0ull;
    g_tip_hold_block_quiet = 0;
    g_tip_hold_enter_ms = 0ull;
    g_tip_hold_coalesce_n = 0u;
    clear_tip_hold_rereplay_pending();
    g_tip_extend_rereplay = 0;
    g_tip_extend_from_tick = 0;
    clear_tip_extend_prime();
    g_needs_advance = 0;
    g_episode_snap_applied = 0;
    /* §112: keep pending_resume across tip-hold Live so apply-only / empty-span
     * heal can still flush_resume (live_realign gate once episode inactive).
     * Normal Replay already cleared it before POST. */
    g_empty_span_verify_pending = 0;
    g_episode_baseline_matched = 0;
    clear_episode_wire_state();
    clear_baseline_pin();
    /* Matched tip-hold is a clean commit — clear abort streak so a later
     * POST realign is not storm-cooled by a stale ready-timeout. */
    g_bl_mismatch_streak = 0;
    g_fork_streak = 0u; /* §42 P4: clean commit ends any fork suspicion */
    g_fork_streak_tick = 0u;
    g_resim_diverge_tick = 0u;
    g_resim_diverge_streak = 0u;
    g_bl_fork_cap = 0u; /* §55 */
    g_peer_nack_floor = 0u; /* §62 */
    /* §38: hold agreed at tip until peer tip-hold (SAFETY ~250ms) can exit. */
    g_post_tip_hold_holdoff_t0_ms = rb_mono_ms();
    if (g_post_tip_hold_holdoff_t0_ms == 0ull)
        g_post_tip_hold_holdoff_t0_ms = 1ull;
    /* Live invent must rebuild the delay cushion (see psx_netplay.c). */
    psx_netplay_timesync_on_episode_boundary();
}

static void poll_tip_hold_finalize(void)
{
    RNetSession *s = sess();
    RNetSessionStats st;
    uint32_t sim;
    uint32_t tip;
    uint32_t slack;
    uint32_t invent_cap;
    uint32_t runway;
    uint32_t remote_tip;
    uint64_t now;
    uint64_t need_ms;
    uint64_t absolute_ms;
    int held;
    int held_local;
    int held_remote;
    int pending;
    int raced;
    int peer_past_tip;
    if (!g_rb || !rnet_rb_is_tip_holding(g_rb) || !s)
        return;
    /* §91: peer may have COMMIT'd while we coalesce tip-extend — abandon
     * before SAFETY/quiet walls raise the tip further. */
    if (maybe_abandon_tip_extend())
        return;
    sim = rnet_session_sim_tick(s);
    tip = rnet_rb_get_target_tick(g_rb);
    slack = rnet_rb_get_tip_seal_slack(g_rb);
    invent_cap = tip + slack;
    runway = rnet_rb_get_tip_runway(g_rb);
    now = rb_mono_ms();
    tip_hold_held_split(tip, &held_local, &held_remote);
    held = held_local || held_remote;
    pending = tip_hold_wire_pending_release(tip) || g_tip_hold_block_quiet;
    /* Drop stale block_quiet once pads are idle and tip has no release ahead —
     * otherwise a failed tip-extend parked quiet for the whole SAFETY window. */
    if (g_tip_hold_block_quiet && !held && !tip_hold_wire_pending_release(tip))
        g_tip_hold_block_quiet = 0;
    pending = tip_hold_wire_pending_release(tip) || g_tip_hold_block_quiet;

    /* §45: peer tip vs sealed tip — race-ahead ends SAFETY defer early. */
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(s, &st);
    remote_tip = st.highest_remote_wire;
    raced = (remote_tip > tip + RB_MOTK_TIP_HOLD_RACE_MARGIN) ? 1 : 0;
    /* §65: peer wire past sealed tip (but not yet race-margin) means the
     * initiator is tip-extending / about to. Do not SAFETY/quiet/thrash
     * commit — wait for coalesce tip-extend FOLLOW. Soak: SAFETY CAP at
     * 4495 with remote_tip=4502 → host tip-extend ABANDON. */
    peer_past_tip = (remote_tip > tip) ? 1 : 0;

    /* §25/§35: thrash WALL only when coalesce is busy *and* idle (no held /
     * pending release). SAFETY never commits while digital held — unless
     * §45 race-ahead / remote-held wall says the peer already left. */
    if (g_tip_hold_enter_ms != 0ull) {
        uint64_t elapsed = now - g_tip_hold_enter_ms;
        if (!held && !pending && !peer_past_tip &&
            g_tip_hold_coalesce_n >= RB_MOTK_TIP_HOLD_THRASH_COUNT &&
            elapsed >= (uint64_t)RB_MOTK_TIP_HOLD_THRASH_WALL_MS) {
            fprintf(stderr,
                    "psxrecomp: rb tip-hold THRASH CAP %u ms "
                    "(coalesce_n=%u) — commit through=%u\n",
                    (unsigned)RB_MOTK_TIP_HOLD_THRASH_WALL_MS,
                    (unsigned)g_tip_hold_coalesce_n, (unsigned)tip);
            fflush(stderr);
            tip_hold_try_finalize();
            return;
        }
        /* held_remote-only: peer tip row still shows pressed, but we have
         * released. Cap Absolute far below the stuck-local-pad wall so a
         * stale sealed tip cannot park us for 2s while Live tip ages the
         * wire ring. Local-held keeps the full ABSOLUTE wall — unless the
         * peer is already far past tip (§82). */
        absolute_ms = (!held_local && held_remote)
                          ? (uint64_t)RB_MOTK_TIP_HOLD_REMOTE_HELD_WALL_MS
                          : (uint64_t)RB_MOTK_TIP_HOLD_ABSOLUTE_WALL_MS;
        if (peer_past_tip &&
            remote_tip >= tip + RB_MOTK_TIP_HOLD_PEER_AHEAD_FLUSH &&
            absolute_ms > (uint64_t)RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS)
            absolute_ms = (uint64_t)RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS;
        if (elapsed >= (uint64_t)RB_MOTK_TIP_HOLD_SAFETY_WALL_MS) {
            /* §69/§70: RACE with deferred coalesce — brief wait for more
             * tip-extends, then flush (do not sit until ABSOLUTE 2000ms).
             * Soak: RACE deferred @351ms → ABSOLUTE @2000ms mega-absorb.
             * §81/§82: peer_past_tip alone must not defer until ABSOLUTE
             * when remote lead < RACE_MARGIN (soak tip=6011 remote=+20).
             * Cap bare + deferred peer waits at RACE_PENDING, then flush. */
            int race_pending = (raced && g_tip_hold_rereplay_pending &&
                                elapsed < (uint64_t)
                                    RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS)
                                   ? 1
                                   : 0;
            int peer_wait = 0;
            if (peer_past_tip && !raced) {
                /* Brief wait for tip-extend FOLLOW — never until Absolute. */
                peer_wait = (elapsed < (uint64_t)
                                 RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS)
                                ? 1
                                : 0;
            }
            if (((held && !raced) || peer_wait || race_pending) &&
                elapsed < absolute_ms) {
                static uint32_t s_held_defer_tip;
                static uint32_t s_race_defer_tip;
                static uint32_t s_defer_peer_tip;
                if (race_pending && s_race_defer_tip != tip) {
                    fprintf(stderr,
                            "psxrecomp: rb tip-hold RACE deferred "
                            "(rereplay pending) through=%u elapsed=%u ms "
                            "remote_tip=%u — wait coalesce (cap %u ms)\n",
                            (unsigned)tip, (unsigned)elapsed,
                            (unsigned)remote_tip,
                            (unsigned)RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS);
                    fflush(stderr);
                    s_race_defer_tip = tip;
                } else if (peer_wait && g_tip_hold_rereplay_pending &&
                           s_defer_peer_tip != tip) {
                    fprintf(stderr,
                            "psxrecomp: rb tip-hold DEFER-PEER wait "
                            "through=%u elapsed=%u ms remote_tip=%u — "
                            "flush after %u ms coalesce\n",
                            (unsigned)tip, (unsigned)elapsed,
                            (unsigned)remote_tip,
                            (unsigned)RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS);
                    fflush(stderr);
                    s_defer_peer_tip = tip;
                } else if (peer_wait && !g_tip_hold_rereplay_pending &&
                           s_defer_peer_tip != tip) {
                    fprintf(stderr,
                            "psxrecomp: rb tip-hold DEFER-PEER wait "
                            "through=%u elapsed=%u ms remote_tip=%u — "
                            "flush after %u ms (bare peer-ahead; no "
                            "ABSOLUTE 2000)\n",
                            (unsigned)tip, (unsigned)elapsed,
                            (unsigned)remote_tip,
                            (unsigned)RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS);
                    fflush(stderr);
                    s_defer_peer_tip = tip;
                } else if (held && !race_pending && !peer_wait &&
                           s_held_defer_tip != tip) {
                    fprintf(stderr,
                            "psxrecomp: rb tip-hold SAFETY deferred "
                            "(digital held) through=%u elapsed=%u ms "
                            "held_local=%d held_remote=%d remote_tip=%u\n",
                            (unsigned)tip, (unsigned)elapsed,
                            held_local, held_remote, (unsigned)remote_tip);
                    fflush(stderr);
                    s_held_defer_tip = tip;
                }
            } else {
                const char *cap;
                unsigned cap_ms;
                if (peer_past_tip &&
                    elapsed >= (uint64_t)
                        RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS &&
                    elapsed < (uint64_t)RB_MOTK_TIP_HOLD_ABSOLUTE_WALL_MS) {
                    /* §81/§82: deferred repair or bare peer-ahead — flush
                     * without ABSOLUTE 2000. */
                    cap = "DEFER-PEER";
                    cap_ms = (unsigned)RB_MOTK_TIP_HOLD_RACE_PENDING_WALL_MS;
                } else if (raced && (held || peer_past_tip ||
                              g_tip_hold_rereplay_pending) &&
                    elapsed < absolute_ms) {
                    cap = "RACE";
                    cap_ms = (unsigned)RB_MOTK_TIP_HOLD_RACE_MARGIN;
                } else if (elapsed >= absolute_ms &&
                           absolute_ms <
                               (uint64_t)RB_MOTK_TIP_HOLD_ABSOLUTE_WALL_MS) {
                    cap = (peer_past_tip &&
                           remote_tip >=
                               tip + RB_MOTK_TIP_HOLD_PEER_AHEAD_FLUSH)
                              ? "PEER-AHEAD"
                              : "REMOTE-HELD";
                    cap_ms = (unsigned)absolute_ms;
                } else if (elapsed >=
                           (uint64_t)RB_MOTK_TIP_HOLD_ABSOLUTE_WALL_MS) {
                    cap = "ABSOLUTE";
                    cap_ms = (unsigned)RB_MOTK_TIP_HOLD_ABSOLUTE_WALL_MS;
                } else {
                    cap = "SAFETY";
                    cap_ms = (unsigned)RB_MOTK_TIP_HOLD_SAFETY_WALL_MS;
                }
                fprintf(stderr,
                        "psxrecomp: rb tip-hold %s CAP %u — commit "
                        "through=%u held=%d pending=%d "
                        "held_local=%d held_remote=%d "
                        "remote_tip=%u sim=%u\n",
                        cap, cap_ms, (unsigned)tip, held, pending,
                        held_local, held_remote, (unsigned)remote_tip,
                        (unsigned)sim);
                fflush(stderr);
                tip_hold_try_finalize();
                return;
            }
        }
    }
    /* Confirmed Live walked past the coalesce window (wire present during
     * tip-hold — see np_try_admit_rollback). Do not walk past while held —
     * same key-repeat class as SAFETY-while-held. */
    if (g_tip_hold_until > 0u && sim > g_tip_hold_until && !held &&
        !peer_past_tip) {
        tip_hold_try_finalize();
        return;
    }
    /* §34/§35/§65: quiet only while invent-capped, pads idle, no pending
     * release, and peer is not still wiring past tip (tip-extend in flight). */
    if (sim >= invent_cap && !held && !pending && !peer_past_tip) {
        if (g_tip_hold_quiet_t0_ms == 0ull)
            g_tip_hold_quiet_t0_ms = now;
        need_ms = (uint64_t)runway * 1000ull / 60ull;
        if (need_ms < (uint64_t)RB_MOTK_TIP_HOLD_QUIET_MIN_MS)
            need_ms = (uint64_t)RB_MOTK_TIP_HOLD_QUIET_MIN_MS;
        if (need_ms > (uint64_t)RB_MOTK_TIP_HOLD_QUIET_MAX_MS)
            need_ms = (uint64_t)RB_MOTK_TIP_HOLD_QUIET_MAX_MS;
        if (runway > 0u && now >= g_tip_hold_quiet_t0_ms + need_ms)
            tip_hold_try_finalize();
    } else {
        g_tip_hold_quiet_t0_ms = 0ull;
    }
}

/* §75: CPU still holds the sealed fin state at `prefer` — refresh the ring
 * from live and arm prefer+1 without reloading a possibly invent-poisoned
 * snap. Returns 1 if handled. */
static int tip_extend_keep_live(uint32_t prefer)
{
    CPUState *c = cpu();
    uint32_t live_core;
    uint32_t pc;
    RNetSession *s = sess();
    uint32_t sim;

    if (!c || !g_rb || !s)
        return 0;
    if (!s_resim_audit_valid || s_resim_audit_sim != prefer)
        return 0;
    live_core = netplay_core_digest(c);
    if (live_core != s_resim_audit_core)
        return 0;
    sim = rnet_session_sim_tick(s);
    /* finish_frame advances before ownership tip-extend (sim==prefer+1).
     * Allow sim==prefer only when that frame already finished (no pending
     * advance) — never skip a mid-frame rereplay. */
    if (sim == prefer + 1u) {
        /* ok — common FOLLOW path after finish_frame */
    } else if (sim == prefer && !g_needs_advance) {
        /* ok — parked on tip after verify/POST */
    } else {
        return 0;
    }
    pc = pick_snap_resume_pc(c, 0u);
    if (pc && g_snaps && g_b.bios_checksum && g_b.entry_pc) {
        CPUState snap = *c;
        snap.pc = pc;
        if (netplay_snap_ring_save(g_snaps, prefer, &snap, *g_b.bios_checksum,
                                   *g_b.entry_pc)) {
            note_good_bb_pc(pc);
            if (prefer > g_auth_snap_through)
                g_auth_snap_through = prefer;
        }
    }
    g_pending_load_valid = 0;
    g_tip_extend_rereplay = 0;
    g_pending_resume_valid = 0;
    g_episode_snap_applied = 1;
    clear_post_handshake();
    rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay);
    fprintf(stderr,
            "psxrecomp: rb tip-extend KEEP-LIVE tick=%u core=%08x target=%u "
            "(skip stale ring reload)\n",
            (unsigned)prefer, (unsigned)live_core,
            (unsigned)rnet_rb_get_target_tick(g_rb));
    fflush(stderr);
    arm_rereplay_after_load(prefer);
    return 1;
}

/* prefer_plus_one: pass prior_tip+1 so prefer==prior tip (initiator and
 * FOLLOW must agree). Walk down toward episode load before falling back to
 * the baseline pin — mismatch-1 missing used to pin-reload on initiator
 * while FOLLOW reloaded the prior tip → POST diverge. */
static void schedule_episode_rereplay(uint32_t prefer_plus_one)
{
    uint32_t load = rnet_rb_get_load_tick(g_rb);
    uint32_t reload = load;
    uint32_t prefer = (prefer_plus_one > 0u) ? prefer_plus_one - 1u : 0u;
    uint32_t t;

    if (prefer < load)
        prefer = load;

    /* §75: mid-Replay FOLLOW / tip-extend often asks to reload the tip we
     * just finished with sealed inputs. Live invent may still own that ring
     * slot — keep live CPU instead of SNAP-STALE fork. */
    if (tip_extend_keep_live(prefer))
        return;

    if (g_snaps && netplay_snap_ring_has(g_snaps, prefer)) {
        reload = prefer;
    } else if (g_snaps) {
        for (t = prefer; t > load; --t) {
            if (netplay_snap_ring_has(g_snaps, t)) {
                reload = t;
                break;
            }
        }
        if (reload == load && netplay_snap_ring_has(g_snaps, load))
            reload = load;
        else if (reload == load && g_pin_valid && g_pin_tick >= load)
            reload = g_pin_tick;
    } else if (g_pin_valid && g_pin_tick >= load) {
        reload = g_pin_tick;
    }
    g_pending_load_tick = reload;
    g_pending_load_valid = 1;
    g_tip_extend_rereplay = 1;
    g_pending_resume_valid = 0;
    g_episode_snap_applied = 0;
    clear_post_handshake();
    rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay);
    fprintf(stderr,
            "psxrecomp: rb tip-extend rereplay load=%u prefer=%u target=%u "
            "has_prefer=%d\n",
            (unsigned)reload, (unsigned)prefer,
            (unsigned)rnet_rb_get_target_tick(g_rb),
            g_snaps && netplay_snap_ring_has(g_snaps, prefer) ? 1 : 0);
    fflush(stderr);
}

static void commit_episode(void)
{
    RNetSession *s = sess();
    uint32_t target;
    uint32_t sim_after;
    if (!g_rb)
        return;
    target = rnet_rb_get_target_tick(g_rb);
    rnet_rb_on_post_match(g_rb);
    sim_after = target + 1u;
    if (s) {
        rnet_session_set_sim_tick(s, sim_after);
        (void)rnet_session_send_rb_resolved(s, target);
    }
    g_episode_count++;
    g_agreed_through = target;
    g_agreed_span_lo = g_episode_load_tick;
    g_agreed_valid = 1;
    g_episode_baseline_matched = 0;
    fprintf(stderr, "psxrecomp: rb episode commit through=%u (count=%u)\n", (unsigned)target,
            (unsigned)g_episode_count);
    fflush(stderr);
    g_last_commit_epoch = rnet_rb_get_epoch_id(g_rb); /* §42 P1 */
    if (g_stash_begin_valid && g_stash_begin_epoch == g_last_commit_epoch) {
        g_stash_begin_valid = 0;
        g_stash_begin_logged = 0;
    }
    send_commit_notice(g_last_commit_epoch, target);  /* §42c */
    rnet_rb_session_reset(g_rb);
    clear_tip_hold_rereplay_pending();
    g_tip_extend_rereplay = 0;
    g_tip_extend_from_tick = 0;
    clear_tip_extend_prime();
    g_needs_advance = 0;
    g_episode_snap_applied = 0;
    g_pending_resume_valid = 0;
    clear_episode_wire_state();
    g_bl_mismatch_streak = 0;
    g_fork_streak = 0u; /* §42 P4 */
    g_fork_streak_tick = 0u;
    g_resim_diverge_tick = 0u;
    g_resim_diverge_streak = 0u;
    g_bl_fork_cap = 0u; /* §55 */
    g_peer_nack_floor = 0u; /* §62 */
    /* No post-commit cooldown: promote-only after commit made char-select
     * D-pad feel rejected (hist updated, live sim not rolled). Abort/storm
     * cooldown still arms from live sim on failed episodes. */
}

void psx_netplay_rb_bind(const PsxNetplayRbBindings *b)
{
    if (!b) {
        memset(&g_b, 0, sizeof(g_b));
        g_bound = 0;
        return;
    }
    g_b = *b;
    g_bound = 1;
}

void psx_netplay_rb_start(void)
{
    RNetRbConfig cfg;
    RNetRollbackVTable vt;
    RNetSession *s = sess();
    int delay;

    psx_netplay_rb_shutdown(); /* also clears IRQ resume latches */
    if (!g_bound || !s)
        return;
    /* Rematch: belt-and-suspenders with interrupts_init (shutdown already
     * cleared; re-clear after recreate path in case bind raced). */
    psx_irq_clear_resume_latches();

    /* §96: dirty-VRAM tracking only while RB netplay is live. */
    gpu_vram_dirty_set_tracking(1);
    boot_state_vram_mirror_reset();

    g_snaps = netplay_snap_ring_create(NETPLAY_SNAP_RING_DEFAULT_DEPTH);
    tip_dense_reset();
    if (!g_snaps) {
        fprintf(stderr, "psxrecomp: rb snap ring create FAILED — rewind disabled\n");
        gpu_vram_dirty_set_tracking(0);
        boot_state_vram_mirror_reset();
        return;
    }
    delay = g_b.input_delay ? *g_b.input_delay : 2;
    if (delay < 0)
        delay = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.local_slot = (uint32_t)(g_b.local_slot ? *g_b.local_slot : 0);
    cfg.delay = (uint32_t)delay;
    cfg.tip_runway = RB_MOTK_TIP_RUNWAY;
    /* §27: 0 → RNET_RB_TIP_SEAL_SLACK_DEFAULT (2). Live invents past tip
     * during TipHold so presentation keeps moving; tip-extend repairs edges. */
    cfg.tip_seal_slack = 0u;
    /* Keep the light-tip depth ceiling matched to tip_runway. A coalesced
     * TipHold episode's eventual depth (load..target after tip-extend) can
     * approach tip_runway ticks — with the library default of 16 that
     * silently dropped ~19% of episodes out of the light-tip fast path in
     * soak testing purely because they coalesced past 16, paying a second
     * ready-ACK round trip they didn't need (see ROLLBACK_MOTK_HOOKUP.md,
     * "light-tip depth vs TipHold coalesce runway", 2026-08-01). */
    cfg.light_tip_max_depth = RB_MOTK_TIP_RUNWAY;
    {
        int slots = g_b.slot_count ? *g_b.slot_count : 2;
        if (slots < 2) slots = 2;
        if (slots > (int)RNET_RB_MAX_SLOTS) slots = (int)RNET_RB_MAX_SLOTS;
        cfg.slot_count = (uint32_t)slots;
    }

    memset(&vt, 0, sizeof(vt));
    vt.ctx = NULL;
    vt.save_state = host_save_state;
    vt.load_state = host_load_state;
    vt.advance_sim = host_advance_sim;
    vt.state_digest = host_state_digest;
    vt.hash_confirm_through = host_hash_confirm_through;
    vt.get_input_row = host_get_input_row;
    vt.stick_gates.hash_confirm_promote = host_hash_confirm_promote;
    /* stick_gates.ctx set per-decide if needed */

    g_rb = rnet_rb_create(&cfg, &vt);
    if (!g_rb) {
        fprintf(stderr, "psxrecomp: rb session create FAILED\n");
        netplay_snap_ring_destroy(g_snaps);
        g_snaps = NULL;
        return;
    }
    g_epoch = 0; /* counter — wire ids are (counter << slot_bits) | slot */
    g_peer_abort_last_epoch = 0xffffffffu;
    g_abort_wire_class = RNET_RB_ABORT_CLASS_ABORT;
    g_abort_wire_realign_tick = 0u;
    g_abort_from_peer = 0;
    g_episode_count = 0;
    g_pending_save_valid = 0;
    g_pending_load_valid = 0;
    g_auth_snap_through = 0; /* §75 */
    g_pending_resume_valid = 0;
    g_episode_snap_applied = 0;
    g_agreed_valid = 0;
    g_agreed_through = 0;
    g_agreed_span_lo = 0;
    g_peer_resolved_through = 0;
    g_peer_resolved_wait_t0_ms = 0ull;
    g_peer_resolved_gate_expired = 0;
    g_past_frontier_sticky = 0;
    g_local_advertised_through = 0;
    g_peer_resolved_hb_last_ms = 0ull;
    g_live_realign_pending = 0;
    g_episode_baseline_matched = 0;
    g_episode_load_tick = 0;
    g_tip_hold_until = 0;
    g_tip_hold_quiet_t0_ms = 0ull;
    g_tip_hold_block_quiet = 0;
    g_tip_hold_enter_ms = 0ull;
    g_post_tip_hold_holdoff_t0_ms = 0ull;
    g_tip_hold_coalesce_n = 0u;
    clear_tip_hold_rereplay_pending();
    g_tip_extend_rereplay = 0;
    g_tip_extend_prime_tick = 0;
    g_tip_extend_from_tick = 0;
    g_ownership_skip_snap = 0;
    g_stash_begin_valid = 0;
    g_stash_begin_logged = 0;
    g_last_commit_epoch = 0xffffffffu;
    g_peer_commit_epoch = 0xffffffffu;
    g_peer_commit_tick = 0;
    g_fork_streak = 0u;
    g_fork_streak_tick = 0u;
    g_bl_fork_cap = 0u; /* §55: commit/reset clears the bisect cap */
    g_peer_nack_floor = 0u; /* §62 */
    clear_baseline_pin();
    clear_episode_wire_state();
    s_snap_fail_logged = 0;
    s_snap_ok_logged = 0;
    s_snap_pc_reject_logged = 0;
    s_snap_stale_logged = 0;
    fprintf(stderr,
            "psxrecomp: rb snap ring ready depth=%u snap_interval=%u "
            "tip_dense=%u\n",
            (unsigned)netplay_snap_ring_depth(g_snaps),
            (unsigned)snap_interval(), (unsigned)tip_dense_window());
}

void psx_netplay_rb_cold_reset(void)
{
    /* Residue rb_shutdown historically left until the next rb_start — a
     * rematch host in lobby + cold-boot guest then forked dig0/sim~14. */
    tip_dense_reset();
    clear_episode_wire_state();
    clear_baseline_pin();
    g_boot_dig0_synced = 0;
    g_boot_dig0_mismatch_logged = 0;
    g_boot_dig0_local_core = 0;
    g_boot_dig0_local_valid = 0;
    g_boot_dig0_peer_core = 0;
    g_boot_dig0_peer_valid = 0;
    g_boot_dig0_last_rexmit_ms = 0;
    g_boot_dig0_wait_kind_logged = 0;
    g_follow_nack_pending = 0;
    g_follow_nack_epoch = 0;
    g_follow_nack_mismatch = 0;
    g_follow_nack_load = 0;
    g_follow_nack_target = 0;
    g_follow_nack_slot = 0;
    g_follow_nack_sends = 0;
    g_stash_bl_valid = 0;
    g_stash_bl_epoch = 0;
    g_stash_bl_load = 0;
    g_stash_bl_dig_m = 0;
    g_stash_bl_dig_a = 0;
    g_stash_bl_dig_b = 0;
    g_stash_bl_dig_c = 0;
    g_stash_bl_logged = 0;
    g_stat_replay_ticks = 0;
    g_stat_replay_ticks_total = 0;
    g_pending_save_valid = 0;
    g_pending_load_valid = 0;
    g_pending_resume_valid = 0;
    g_live_realign_pending = 0;
    g_was_in_fmv = 0;
    g_fmv_settle_until = 0;
    g_fmv_lockstep_until = 0;
    g_fmv_media_end_sim = 0;
    g_fmv_core_match_streak = 0;
    g_fmv_lockstep_released = 0;
    g_fmv_dense_through = 0;
    g_fmv_media_lo = 0;
    g_fmv_media_hi = 0;
    g_fmv_unmatched_desync = 0;
    g_fmv_unmatched_desync_arm_sim = 0;
    g_post_fmv_heal_sticky_until = 0u;
    g_post_fmv_heal_fork_tick = 0u;
    g_post_fmv_heal_loop_fork = 0u;
    g_post_fmv_heal_loop_count = 0u;
    g_request_post_fmv_heal = 0;
    g_post_fmv_heal_kf = 0;
    g_post_fmv_platform_nondet = 0;
    g_post_fmv_platform_keep_tick = 0u;
    g_platform_kf_next_sim = 0u;
    g_platform_kf_stream_count = 0u;
    g_platform_accepted_fork = 0u;
    g_last_good_bb_pc = 0;
}

void psx_netplay_rb_shutdown(void)
{
    if (g_rb) {
        rnet_rb_destroy(g_rb);
        g_rb = NULL;
    }
    if (g_snaps) {
        netplay_snap_ring_destroy(g_snaps);
        g_snaps = NULL;
    }
    /* BYE / soft-exit: drop resume latches before lobby rematch so dig0 cannot
     * see a prior-match game PC between shutdown and interrupts_init. */
    psx_irq_clear_resume_latches();
    psx_netplay_rb_cold_reset();
    gpu_vram_dirty_set_tracking(0);
    boot_state_vram_mirror_reset();
    clear_baseline_pin();
    g_pending_save_valid = 0;
    g_pending_load_valid = 0;
    g_auth_snap_through = 0; /* §75 */
    g_pending_resume_valid = 0;
    g_empty_span_verify_pending = 0;
    g_episode_snap_applied = 0;
    g_needs_advance = 0;
    g_live_realign_pending = 0;
    g_rewind_cooldown_until = 0;
    g_promote_sweep = 0;
    g_last_begin_mismatch = 0xffffffffu;
    g_was_in_fmv = 0;
    g_fmv_settle_until = 0;
    g_fmv_lockstep_until = 0;
    g_fmv_media_end_sim = 0;
    g_fmv_core_match_streak = 0;
    g_fmv_lockstep_released = 0;
    g_fmv_dense_through = 0;
    g_fmv_media_lo = 0;
    g_fmv_media_hi = 0;
    g_fmv_unmatched_desync = 0;
    g_fmv_unmatched_desync_arm_sim = 0;
    g_media_crc_head = 0;
    g_media_crc_n = 0;
    g_media_crc_peer_match_through = 0;
    memset(g_media_crc, 0, sizeof(g_media_crc));
    g_resim_diverge_tick = 0;
    g_resim_diverge_streak = 0;
    g_bl_mismatch_streak = 0;
    g_tip_hold_until = 0;
    g_tip_hold_quiet_t0_ms = 0ull;
    g_tip_hold_block_quiet = 0;
    g_tip_hold_enter_ms = 0ull;
    g_post_tip_hold_holdoff_t0_ms = 0ull;
    g_tip_hold_coalesce_n = 0u;
    clear_tip_hold_rereplay_pending();
    g_tip_extend_rereplay = 0;
    g_tip_extend_prime_tick = 0;
    g_tip_extend_from_tick = 0;
    g_ownership_skip_snap = 0;
    ownership_clear_chain_budget();
    g_chain_suppress_until_tip = 0u;
    g_chain_suppress_until_ms = 0ull;
    g_stash_begin_valid = 0;
    g_stash_begin_logged = 0;
    g_last_begin_rexmit_ms = 0;
    g_begin_rexmit_logged = 0;
    g_last_commit_epoch = 0xffffffffu;
    g_peer_commit_epoch = 0xffffffffu;
    g_peer_commit_tick = 0;
    g_fork_streak = 0u;
    g_fork_streak_tick = 0u;
    g_bl_fork_cap = 0u; /* §55: commit/reset clears the bisect cap */
    g_peer_nack_floor = 0u; /* §62 */
    g_last_good_bb_pc = 0;
    g_agreed_valid = 0;
    g_agreed_through = 0;
    g_agreed_span_lo = 0;
    g_peer_resolved_through = 0;
    g_peer_resolved_wait_t0_ms = 0ull;
    g_peer_resolved_gate_expired = 0;
    g_past_frontier_sticky = 0;
    g_local_advertised_through = 0;
    g_peer_resolved_hb_last_ms = 0ull;
    g_epoch = 0;
    g_peer_abort_last_epoch = 0xffffffffu;
    g_abort_wire_class = RNET_RB_ABORT_CLASS_ABORT;
    g_abort_wire_realign_tick = 0u;
    g_abort_from_peer = 0;
    g_rb_rtt_ema_ms = 0;
}

/* Dense tip snaps: settle + digest lockstep/+tip (§26) — first Cross/Start
 * after FMV still wants a near-tip load. Media uses fmv_media_snap_interval
 * instead (§96); episodes into media remain refused. */
static int rb_fmv_dense_snap_window(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    if (sim < g_fmv_settle_until)
        return 1;
    /* §114: platform nondet / DESYNC hold — every-tick snaps for KF stream. */
    if (g_post_fmv_platform_nondet || g_fmv_unmatched_desync)
        return 1;
    if (g_fmv_media_end_sim &&
        sim < g_fmv_lockstep_until + 32u &&
        sim < g_fmv_media_end_sim + RB_FMV_LOCKSTEP_MAX + 32u)
        return 1;
    return 0;
}

void psx_netplay_rb_request_snap(uint32_t tick)
{
    uint32_t iv;
    if (!g_snaps)
        return;
    /* Never overwrite the frozen episode baseline (or its ring slot). */
    if (g_pin_valid && tick == g_pin_tick)
        return;
    if (g_rb && rnet_rb_is_active(g_rb) && tick == rnet_rb_get_load_tick(g_rb))
        return;
    /* Resim must keep a snap at every replayed tick (post-verify + next
     * correction). Live path rate-limits — full boot_state snaps dominate FPS.
     * Exception: post-FMV settle/lockstep/+tip keep every-tick snaps so the
     * first invent-miss loads near the mismatch. FMV media (§96) uses a
     * coarser interval — episodes into media are refused anyway. */
    if (!(g_rb && rnet_rb_is_resimulating(g_rb))) {
        /* §75: sealed Replay snaps are authoritative — Live invent must not
         * poison them for tip-extend rereplay. */
        if (g_auth_snap_through > 0u && tick <= g_auth_snap_through)
            return;
        /* §79: TipHold Live invent/wire walk — save every tick into the dense
         * FIFO (protected above the sealed tip). Matched-prefix flush needs
         * first_bad-1 still in the ring after a 16–30 tick REMOTE-HELD walk. */
        if (g_rb && rnet_rb_is_tip_holding(g_rb) && tip_dense_window() > 0u) {
            tip_dense_push(tick);
        } else if (rb_fmv_media_active()) {
            iv = fmv_media_snap_interval();
            if (iv > 1u && (tick % iv) != 0u)
                return;
            if (tick >= g_fmv_dense_through)
                g_fmv_dense_through = tick;
        } else if (!rb_fmv_dense_snap_window()) {
            iv = snap_interval();
            if (iv > 1u && (tick % iv) != 0u) {
                /* §58: dense tip window — save every live tick and keep the
                 * last N non-interval snaps so a near-tip mispredict loads
                 * at the mismatch, not an interval boundary. */
                if (tip_dense_window() == 0u)
                    return;
                tip_dense_push(tick);
            }
        } else if (tick >= g_fmv_dense_through) {
            g_fmv_dense_through = tick;
        }
    }
    g_pending_save_tick = tick;
    g_pending_save_valid = 1;
}

/*
 * Apply pending baseline/realign snap without longjmp.
 *
 * NEVER call from mid-guest pump (psx_cycles watchdog → psx_netplay_pump):
 * that mutates RAM/CPU while native CPS continues and causes DISPATCH FATAL
 * (guest rb-diag: pc=0 / ra=0x00FFFFFF after snap apply, before Replay).
 *
 * Safe callers:
 *  - try_admit while parked in the admit wait loop (deferred resume OK)
 *  - rb_poll only when flush_resume will immediately longjmp (live realign
 *    or already in Replay)
 */
static int try_apply_pending_load(CPUState *cpu_in)
{
    int live_realign;
    if (!g_snaps || !cpu_in || !g_pending_load_valid)
        return 0;
    if (!g_b.bios_checksum || !g_b.entry_pc)
        return 0;
    live_realign = g_live_realign_pending;
    /* §75: tip-extend rereplay — if CPU still matches sealed fin at the
     * load tick, keep live (do not apply invent-poisoned ring snap). */
    if (!live_realign && g_tip_extend_rereplay &&
        tip_extend_keep_live(g_pending_load_tick))
        return 1;
    /* §51/§70: ownership SPAN continue at the POST-matched tip. Never hash
     * Live-drifted CPU after wait-FOLLOW invent — restore the pin captured
     * at ownership_chain_next_span (or ring refresh of that same state). */
    if (!live_realign && g_ownership_skip_snap &&
        g_agreed_valid && g_pending_load_tick == g_agreed_through) {
        uint32_t loaded_tick = g_pending_load_tick;
        RNetSession *s = sess();
        int loaded = 0;
        uint32_t pc;
        g_ownership_skip_snap = 0;
        if (g_pin_valid && g_pin_tick == loaded_tick && g_pin_data &&
            g_pin_size) {
            loaded = boot_state_load_buffer(g_pin_data, g_pin_size,
                                            *g_b.bios_checksum, *g_b.entry_pc,
                                            cpu_in);
        } else if (g_snaps) {
            loaded = netplay_snap_ring_load(g_snaps, loaded_tick, cpu_in,
                                            *g_b.bios_checksum, *g_b.entry_pc);
            if (loaded)
                pin_baseline_from_ring(loaded_tick);
        }
        if (!loaded) {
            fprintf(stderr,
                    "psxrecomp: rb ownership skip-snap FAILED load=%u — "
                    "fall through\n",
                    (unsigned)loaded_tick);
            fflush(stderr);
            /* Keep pending_load_valid; fall through to normal load. */
        } else {
            pc = rb_canonicalize_resume_pc(cpu_in->pc);
            if (!rb_resume_pc_ok(pc))
                pc = pick_snap_resume_pc(cpu_in, pc);
            if (!pc) {
                fprintf(stderr,
                        "psxrecomp: rb ownership skip-snap FAILED load=%u — "
                        "no safe resume pc\n",
                        (unsigned)loaded_tick);
                fflush(stderr);
                abort_episode("ownership skip-snap: no resume pc");
                return 0;
            }
            cpu_in->pc = pc;
            note_good_bb_pc(pc);
            psx_cycles_resync_after_restore(cpu_in);
            interrupts_resync_after_restore();
            cdrom_resync_deadlines_after_restore();
            if (!cdrom_xa_stream_active() && !cdrom_fmv_stream_pending())
                spu_cd_audio_reset();
            {
                extern void overlay_loader_clear_lazy_miss(void);
                overlay_loader_clear_lazy_miss();
            }
            psx_frontend_on_rb_snap_loaded();
            resim_audit_cache_invalidate();
            g_pending_load_valid = 0;
            /* §72: must arm resume like a normal snap apply. Clearing
             * resume_pending left the follower mid–Live/wait-FOLLOW with a
             * replaced CPU and no flush_resume longjmp → native return with
             * PC=0 ("execution completed") and peer disconnect (soak epoch
             * 48 @1976). flush_resume waits for Replay phase. */
            g_pending_resume_pc = pc;
            g_pending_resume_valid = 1;
            g_episode_snap_applied = 1;
            g_episode_load_tick = loaded_tick;
            if (s)
                rnet_session_set_sim_tick(s, loaded_tick + 1u);
            fprintf(stderr,
                    "psxrecomp: rb ownership continue skip-snap load=%u "
                    "pc=0x%08x (pin restore; sim=%u; resume deferred)\n",
                    (unsigned)loaded_tick, (unsigned)pc,
                    (unsigned)(loaded_tick + 1u));
            fflush(stderr);
            maybe_send_baseline();
            return 1;
        }
    }
    if (g_ownership_skip_snap &&
        !(g_agreed_valid && g_pending_load_tick == g_agreed_through)) {
        /* Stale continue flag vs a fresh deep begin — fall through to load. */
        g_ownership_skip_snap = 0;
    }
    {
        int loaded = 0;
        int loaded_from_pin = 0;
        uint32_t loaded_tick = g_pending_load_tick;
        /* §97: wait for host MEDIA-KF pin before applying into FMV media. */
        if (g_media_kf_episode && !g_media_kf_ready)
            return 0;
        /* Prefer pin when it matches the load tick: live realign and §76
         * tip-hold flush (sealed tip at tip-hold entry — ring may still be
         * invent-poisoned). §97 MEDIA-KF always prefers the sealed pin. */
        if (g_pin_valid && g_pin_tick == loaded_tick && g_pin_data &&
            g_pin_size &&
            (live_realign || g_tip_extend_rereplay || g_media_kf_episode)) {
            loaded = boot_state_load_buffer(g_pin_data, g_pin_size,
                                            *g_b.bios_checksum, *g_b.entry_pc,
                                            cpu_in);
            loaded_from_pin = loaded ? 1 : 0;
        } else {
            loaded = netplay_snap_ring_load(g_snaps, loaded_tick, cpu_in,
                                            *g_b.bios_checksum, *g_b.entry_pc);
        }
        if (!loaded) {
            fprintf(stderr,
                    "psxrecomp: rb snap load FAILED tick=%u (pin=%d has=%d count=%u "
                    "newest=%u)\n",
                    (unsigned)loaded_tick, g_pin_valid && g_pin_tick == loaded_tick,
                    g_snaps && netplay_snap_ring_has(g_snaps, loaded_tick) ? 1 : 0,
                    (unsigned)(g_snaps ? netplay_snap_ring_count(g_snaps) : 0u),
                    (unsigned)(g_snaps ? netplay_snap_ring_newest_tick(g_snaps) : 0u));
            fflush(stderr);
            g_live_realign_pending = 0;
            if (!live_realign)
                abort_episode("snap load failed");
            return 0;
        }

        {
            uint32_t snap_pc = rb_canonicalize_resume_pc(cpu_in->pc);
            uint32_t pc;
            /* §74 diag: if we JUST finished simulating loaded_tick (FOLLOW
             * rereplay / tip-extend commonly reload the tick they are parked
             * on), the audit-fin cache holds the freshly computed core digest
             * for it. A snap whose state differs is a stale/forked interval
             * snap — the exact fork source in the 2026-08-02 soak (guest fin
             * 3166=089b276e, loaded snap armed 3167 at f92756fb → POST
             * diverge). Capture before the invalidate below. */
            uint32_t stale_chk_core = 0u;
            int stale_chk = s_resim_audit_valid &&
                            s_resim_audit_sim == loaded_tick;
            if (stale_chk)
                stale_chk_core = s_resim_audit_core;
            g_pending_load_valid = 0;
            g_live_realign_pending = 0;
            resim_audit_cache_invalidate(); /* §67: guest state replaced */
            /* Prefer the snap's stored PC when already safe — host sticky /
             * IRQ hints can re-introduce MotK wait-loop edge forks.
             * Tick-0 must stay in BIOS even if a poisoned ring snap carried
             * prior-match main-RAM PC (rematch dig0 baseline mismatch). */
            if (loaded_tick == 0u && rb_resume_pc_ok(snap_pc) &&
                !rb_pc_is_bios(snap_pc)) {
                pc = pick_snap_resume_pc(cpu_in, snap_pc);
                if (!pc)
                    pc = 0xbfc00000u; /* reset vector — better than game PC */
                fprintf(stderr,
                        "psxrecomp: rb snap tick=0 rejected game pc=0x%08x "
                        "-> 0x%08x\n",
                        (unsigned)snap_pc, (unsigned)pc);
                fflush(stderr);
            } else if (rb_resume_pc_ok(snap_pc))
                pc = snap_pc;
            else
                pc = pick_snap_resume_pc(cpu_in, snap_pc);
            if (!pc) {
                char why[96];
                snprintf(why, sizeof(why),
                         "loaded snap tick=%u has no safe resume pc (snap=0x%08x)",
                         (unsigned)loaded_tick, (unsigned)snap_pc);
                if (live_realign) {
                    fprintf(stderr, "psxrecomp: rb realign FAILED — %s\n", why);
                    fflush(stderr);
                } else {
                    abort_episode(why);
                }
                return 0;
            }
            if (pc != cpu_in->pc) {
                fprintf(stderr,
                        "psxrecomp: rb snap pc rewrite tick=%u snap=0x%08x -> 0x%08x\n",
                        (unsigned)loaded_tick, (unsigned)cpu_in->pc, (unsigned)pc);
                fflush(stderr);
            }
            cpu_in->pc = pc;
            note_good_bb_pc(pc);
            psx_cycles_resync_after_restore(cpu_in);
            interrupts_resync_after_restore();
            /* Re-arm absolute CD deadlines from restored relative delays.
             * Do NOT cdrom_accelerate (delay-cap boost forks peers). */
            cdrom_resync_deadlines_after_restore();
            /* Wipe SPU CD FIFO only when XA is idle — clearing mid-stream
             * left MotK FMV silent/black after tip loads into media. */
            if (!cdrom_xa_stream_active() && !cdrom_fmv_stream_pending())
                spu_cd_audio_reset();
            {
                extern void overlay_loader_clear_lazy_miss(void);
                overlay_loader_clear_lazy_miss();
            }
            psx_frontend_on_rb_snap_loaded();
            /* §74/§75: tip-extend keep-live should have avoided this. If a
             * load still disagrees with fin, the ring slot was poison —
             * drop it and fall back to the episode pin (do not arm from
             * the forked state). */
            if (stale_chk) {
                uint32_t snap_core = netplay_core_digest(cpu_in);
                if (snap_core != stale_chk_core) {
                    fprintf(stderr,
                            "psxrecomp: rb SNAP-STALE tick=%u snap_core=%08x "
                            "fin_core=%08x pc=0x%08x snap_pc=0x%08x src=%s — "
                            "loaded snap differs from just-simulated state",
                            (unsigned)loaded_tick, (unsigned)snap_core,
                            (unsigned)stale_chk_core, (unsigned)pc,
                            (unsigned)snap_pc,
                            loaded_from_pin ? "pin" : "ring");
                    /* §78: §77's "trust verify pin" forked (one-sided reload
                     * vs peer raise-only continuation — epoch 200 diverge at
                     * 3082). Any load that disagrees with the just-simulated
                     * fin goes back to the peer-verified episode pin. */
                    if (g_tip_extend_rereplay && g_snaps) {
                        uint32_t fallback = g_episode_load_tick;
                        (void)netplay_snap_ring_drop_tick(g_snaps, loaded_tick);
                        if (g_auth_snap_through >= loaded_tick)
                            g_auth_snap_through =
                                loaded_tick > 0u ? loaded_tick - 1u : 0u;
                        if (fallback > 0u && fallback < loaded_tick &&
                            ((g_pin_valid && g_pin_tick == fallback) ||
                             netplay_snap_ring_has(g_snaps, fallback))) {
                            fprintf(stderr,
                                    " — drop poison; fallback load=%u\n",
                                    (unsigned)fallback);
                            fflush(stderr);
                            g_pending_load_tick = fallback;
                            g_pending_load_valid = 1;
                            /* Stay tip-extend so arm_rereplay keeps episode
                             * pin (do not re-baseline at fallback). */
                            g_tip_extend_rereplay = 1;
                            g_pending_resume_valid = 0;
                            g_episode_snap_applied = 0;
                            return try_apply_pending_load(cpu_in);
                        }
                        fprintf(stderr,
                                " — drop poison; no fallback (abort)\n");
                        fflush(stderr);
                        g_tip_extend_rereplay = 0;
                        abort_episode("tip-extend SNAP-STALE: no safe fallback");
                        return 0;
                    } else {
                        fprintf(stderr, "\n");
                        fflush(stderr);
                    }
                }
            }
            g_pending_resume_pc = pc;
            g_pending_resume_valid = 1;
            if (live_realign) {
                /* Do NOT clear remote tip rings here (§49b). Full clear left
                 * tip=0 → mutual pcap FREEZE (wire≫P, neither advances).
                 * Selective clear also fails after ring wrap (only far tips
                 * remain). Prior soak recovered without clear: peer
                 * post-realign production fills need; prepare_local_tip still
                 * emits. Keep absurd tip in the ring — cushion refuses invent
                 * on miss (§45) until real rows land. */
                realign_invalidate_evidence(loaded_tick);
                fprintf(stderr,
                        "psxrecomp: rb realign applied tick=%u pc=0x%08x core=%08x "
                        "cd=%08x\n",
                        (unsigned)loaded_tick, (unsigned)pc,
                        (unsigned)netplay_core_digest(cpu_in),
                        (unsigned)netplay_cdrom_digest());
                fflush(stderr);
                /* If this realign is also the active episode's load (NACK
                 * leftover flag, or poll_snap raced enter_awaiting_baseline),
                 * complete the baseline handshake. Live realign outside an
                 * episode, or at a different tick, stays apply-only. */
                if (g_rb && rnet_rb_is_active(g_rb) &&
                    rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline &&
                    loaded_tick == rnet_rb_get_load_tick(g_rb)) {
                    g_episode_snap_applied = 1;
                    g_episode_load_tick = loaded_tick;
                    pin_baseline_from_ring(loaded_tick);
                    fprintf(stderr,
                            "psxrecomp: rb realign also episode snap load=%u "
                            "(handshake)\n",
                            (unsigned)loaded_tick);
                    fflush(stderr);
                    maybe_send_baseline();
                }
                return 1;
            }
            if (g_tip_extend_rereplay) {
                g_tip_extend_rereplay = 0;
                fprintf(stderr,
                        "psxrecomp: rb tip-extend snap applied tick=%u pc=0x%08x "
                        "(episode pin kept load=%u)\n",
                        (unsigned)loaded_tick, (unsigned)pc,
                        (unsigned)g_episode_load_tick);
                fflush(stderr);
                /* poll_snap can apply while already in Replay — must arm here
                 * (hc_prime + sim=reload+1). try_admit's arm path is skipped
                 * once g_pending_load_valid is cleared. */
                arm_rereplay_after_load(loaded_tick);
                return 1;
            }
            g_episode_snap_applied = 1;
            g_episode_load_tick = loaded_tick;
            pin_baseline_from_ring(loaded_tick);
            fprintf(stderr,
                    "psxrecomp: rb snap applied tick=%u pc=0x%08x (resume deferred)\n",
                    (unsigned)loaded_tick, (unsigned)pc);
            fflush(stderr);
            /* §93: Live media detector can be cold while the load snap is mid-
             * FMV. Expand the media range. §97 MEDIA-KF episodes continue into
             * Replay with the host-sealed keyframe (no abort). */
            if (rb_fmv_media_active()) {
                if (!g_was_in_fmv) {
                    g_was_in_fmv = 1;
                    fprintf(stderr,
                            "psxrecomp: rb FMV rewind-defer ON "
                            "(depth24/mdec; media_kf=%d)\n",
                            g_media_kf_episode);
                    fflush(stderr);
                }
                if (g_fmv_media_lo == 0u || loaded_tick < g_fmv_media_lo)
                    g_fmv_media_lo = loaded_tick;
                if (loaded_tick >= g_fmv_media_hi)
                    g_fmv_media_hi = loaded_tick;
                /* Boot dig0 realign after rematch residue must not refuse
                 * load=0 as "FMV media" — BIOS has not reached a movie yet. */
                if (!g_media_kf_episode && !g_ownership_skip_snap &&
                    loaded_tick != 0u) {
                    char why[96];
                    snprintf(why, sizeof(why),
                             "episode load into FMV media (load=%u)",
                             (unsigned)loaded_tick);
                    abort_episode_realign(why);
                    return 0;
                }
            }
            maybe_send_baseline();
            return 1;
        }
    }
}

void psx_netplay_rb_flush_resume(void)
{
    uint32_t pc;
    int live_realign_resume;
    if (!g_pending_resume_valid)
        return;
    /* Episode: must not longjmp until Replay (both baselines). Live realign
     * resumes outside any episode after POST/baseline abort. */
    live_realign_resume = !g_rb || !rnet_rb_is_active(g_rb);
    if (!live_realign_resume && rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return;
    pc = g_pending_resume_pc;
    if (!rb_resume_pc_ok(pc)) {
        uint32_t alt = pick_snap_resume_pc(cpu(), pc);
        fprintf(stderr,
                "psxrecomp: rb flush_resume unsafe pc=0x%08x alt=0x%08x\n",
                (unsigned)pc, (unsigned)alt);
        fflush(stderr);
        if (!alt) {
            g_pending_resume_valid = 0;
            g_empty_span_verify_pending = 0;
            abort_episode("flush_resume: no safe resume pc");
            return;
        }
        pc = alt;
        g_pending_resume_pc = alt;
    }
    /* §112: empty-span POST while still on the flush path — enter_verify may
     * immediately ownership_exit_to_live/finalize (clears pending); we still
     * longjmp below with the captured pc so top-level resume is armed. */
    if (g_empty_span_verify_pending && g_rb &&
        rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay) {
        uint32_t tip = rnet_rb_get_target_tick(g_rb);
        g_empty_span_verify_pending = 0;
        fprintf(stderr,
                "psxrecomp: rb flush_resume empty-span verify tip=%u "
                "pc=0x%08x\n",
                (unsigned)tip, (unsigned)pc);
        fflush(stderr);
        enter_verify_at_tip(tip);
    }
    g_pending_resume_valid = 0;
    g_replay_progress_ms = rb_mono_ms();
    pc = rb_canonicalize_resume_pc(pc);
    note_good_bb_pc(pc); /* MotK wait sticky = CDA0 for both peers */
    {
        CPUState *c = cpu();
        CDROMDebugState cds;
        memset(&cds, 0, sizeof(cds));
        cdrom_debug_snapshot(&cds);
        fprintf(stderr,
                "psxrecomp: rb flush_resume pc=0x%08x depth24=%d mdec_recent=%d "
                "media=%d cd_reading=%d read_delay=%d xa=%d fmv_pending=%d "
                "i_stat=%08x sr=%08x\n",
                (unsigned)pc, gpu_display_is_depth24(),
                mdec_recently_active(RB_FMV_MDEC_HYSTERESIS), rb_fmv_media_active(),
                cds.reading, cds.read_delay, cdrom_xa_stream_active(),
                cdrom_fmv_stream_pending(), (unsigned)cds.i_stat,
                c ? (unsigned)c->cop0[12] : 0u);
        fflush(stderr);
        /* §93 P1: densify CD/MDEC crumbs after media pin load (160→192 class). */
        if (rb_fmv_media_active() || cds.reading ||
            mdec_recently_active(RB_FMV_MDEC_HYSTERESIS)) {
            RNetSession *s = sess();
            uint32_t sim = s ? (uint32_t)rnet_session_sim_tick(s) : 0u;
            psx_netplay_cd_bisect_arm(sim, 0u);
        }
        /* flush_resume is called from admit inside sdl_vblank_present, which
         * runs under gpu_vblank_flush_present with s_flushing_present=1.
         * longjmp abandons that frame — drop guard AND any leftover present
         * queue (mid-while decrement) so the first post-resume BB edge does
         * not phantom-finish_frame. Do not re-arm for latched I_STAT.VBlank. */
        gpu_vblank_clear_deferred_present();
        /* longjmp also skips generated bb_defer cleanup — zero the nest. */
        {
            extern int g_psx_cyc_bb_defer;
            extern uint32_t g_psx_cyc_batch;
            extern uint32_t *g_psx_cyc_local_acc;
            if (g_psx_cyc_local_acc) *g_psx_cyc_local_acc = 0;
            g_psx_cyc_local_acc = NULL;
            g_psx_cyc_bb_defer = 0;
            g_psx_cyc_batch = 0;
        }
    }
    /* Unwind like disk savestate load so mid-block CPS is abandoned.
     * resume_at arms shared top-level-resume for sentinel / null-pc recover. */
    psx_scheduler_resume_at(pc);
    /* unreachable on success */
    fprintf(stderr, "psxrecomp: rb flush_resume RETURNED (resume failed)\n");
    fflush(stderr);
    abort_episode("flush_resume returned (longjmp failed)");
}

void psx_netplay_rb_poll(struct CPUState *cpu_in, uint32_t resume_pc)
{
    uint32_t pc;

    if (!g_snaps || !cpu_in)
        return;

    if (g_pending_save_valid && g_b.bios_checksum && g_b.entry_pc) {
        RNetSession *s_pend = sess();
        uint32_t sim_now = s_pend ? rnet_session_sim_tick(s_pend) : 0u;
        /* Deferred tick-T snap must not capture sim>T state (rematch: tick-0
         * pending across boot_tip_wait / dig while IRQ PC latch was stale). */
        if (sim_now > g_pending_save_tick) {
            if (!s_snap_stale_logged) {
                fprintf(stderr,
                        "psxrecomp: rb snap save DROP tick=%u — sim advanced to "
                        "%u before a dispatchable PC (stale defer)\n",
                        (unsigned)g_pending_save_tick, (unsigned)sim_now);
                fflush(stderr);
                s_snap_stale_logged = 1;
            }
            g_pending_save_valid = 0;
        }
    }
    if (g_pending_save_valid && g_b.bios_checksum && g_b.entry_pc) {
        pc = pick_snap_resume_pc(cpu_in, resume_pc);
        if (!pc) {
            /* Keep pending — BB-edge poll usually has a real resume_pc. */
            if (!s_snap_pc_reject_logged) {
                fprintf(stderr,
                        "psxrecomp: rb snap save DEFERRED tick=%u — no dispatchable "
                        "PC (hint=0x%08x cpu=0x%08x irq=0x%08x check=0x%08x)\n",
                        (unsigned)g_pending_save_tick, (unsigned)resume_pc,
                        (unsigned)cpu_in->pc, (unsigned)psx_compiled_irq_resume_pc(),
                        (unsigned)psx_last_irq_check_pc());
                fflush(stderr);
                s_snap_pc_reject_logged = 1;
            }
        } else {
            CPUState snap = *cpu_in;
            snap.pc = pc;
            /* Tick-0 pin must not embed peer-local pad FSM / DualShock residue
             * (baseline core matched; ext forked on sio pads after rematch). */
            if (g_pending_save_tick == 0u) {
                int seats = g_b.slot_count ? *g_b.slot_count : 2;
                sio_netplay_canonicalize_session_pads(seats);
            }
            if (netplay_snap_ring_save(g_snaps, g_pending_save_tick, &snap,
                                       *g_b.bios_checksum, *g_b.entry_pc)) {
                note_good_bb_pc(pc);
                /* §75: Replay finishes seal the ring slot against Live invent. */
                if (g_rb && rnet_rb_is_resimulating(g_rb) &&
                    g_pending_save_tick > g_auth_snap_through)
                    g_auth_snap_through = g_pending_save_tick;
                /* §100: cache FMV media snap CRC for later MEDIA-KF probe. */
                if (rb_fmv_media_active()) {
                    size_t sz = 0;
                    const uint8_t *p = netplay_snap_ring_peek(
                        g_snaps, g_pending_save_tick, &sz);
                    if (p && sz)
                        rb_media_crc_note(g_pending_save_tick, (uint32_t)sz,
                                          rnet_checksum(p, sz));
                }
                g_pending_save_valid = 0;
                s_snap_fail_logged = 0;
                s_snap_pc_reject_logged = 0;
                if (!s_snap_ok_logged) {
                    fprintf(stderr,
                            "psxrecomp: rb snap save ok tick=%u pc=0x%08x "
                            "interval=%u (count=%u)\n",
                            (unsigned)g_pending_save_tick, (unsigned)pc,
                            (unsigned)snap_interval(),
                            (unsigned)netplay_snap_ring_count(g_snaps));
                    fflush(stderr);
                    s_snap_ok_logged = 1;
                }
            } else if (!s_snap_fail_logged) {
                fprintf(stderr,
                        "psxrecomp: rb snap save FAILED tick=%u (ring stays empty "
                        "until saves succeed)\n",
                        (unsigned)g_pending_save_tick);
                fflush(stderr);
                s_snap_fail_logged = 1;
            }
        }
    }

    /* Apply from BB/present poll ONLY for live realign (flush_resume runs in
     * the same present epilogue). Episode / tip-extend loads wait for
     * try_admit — applying mid-Replay poll mutated CPU under a live CPS and
     * forked MotK wait-loop resim (IRQ at CD54 vs CDA0 depending on which
     * BB edge the poll hit). */
    if (g_pending_load_valid && g_live_realign_pending)
        (void)try_apply_pending_load(cpu_in);
}

int psx_netplay_rb_rewind_suppressed(void)
{
    RNetSession *s = sess();
    uint32_t sim;
    if (!s)
        return g_rewind_cooldown_until > 0u;
    sim = rnet_session_sim_tick(s);
    return sim < g_rewind_cooldown_until;
}

int psx_netplay_rb_fmv_defer_rewind(void)
{
    return rb_in_fmv_defer_rewind_window();
}

int psx_netplay_rb_fmv_media_active(void)
{
    return rb_fmv_media_active();
}

int psx_netplay_rb_lockstep_no_invent(void)
{
    /* Media + post-FMV lockstep MIN (§26/§113). Invent after MIN/RELEASE.
     * §93/§94: also hold after MAX-unmatched DESYNC until rematch,
     * invent-hold expire, or netplay save complete. */
    return rb_in_fmv_lockstep_window();
}

int psx_netplay_rb_fmv_settle_active(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    rb_fmv_tick_settle();
    return (sim < g_fmv_settle_until) ? 1 : 0;
}

int psx_netplay_rb_fmv_desync_hold(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    rb_fmv_desync_invent_maybe_expire(sim);
    return g_fmv_unmatched_desync ? 1 : 0;
}

void psx_netplay_rb_clear_fmv_desync_hold(const char *why)
{
    rb_clear_fmv_desync_hold(why);
}

int psx_netplay_rb_post_fmv_heal_eligible(void)
{
    return rb_post_fmv_heal_eligible();
}

int psx_netplay_rb_post_fmv_heal_sticky(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    return rb_post_fmv_heal_sticky_active(sim);
}

void psx_netplay_rb_request_post_fmv_heal_kf(void)
{
    if (!rb_post_fmv_heal_eligible())
        return;
    /* §110b/§114: refuse further heal requests once this fork exhausted the
     * loop budget — unless §114 platform stream is armed (re-heal allowed). */
    if (g_post_fmv_heal_loop_count > RB_FMV_HEAL_LOOP_LIMIT &&
        g_post_fmv_heal_loop_fork > 0u && !g_post_fmv_platform_nondet)
        return;
    g_request_post_fmv_heal = 1;
    fprintf(stderr,
            "psxrecomp: rb post-FMV heal KF requested "
            "(hc-fork / silent core fork; sticky_until=%u loop=%u "
            "platform=%d)\n",
            (unsigned)g_post_fmv_heal_sticky_until,
            (unsigned)g_post_fmv_heal_loop_count,
            g_post_fmv_platform_nondet);
    fflush(stderr);
}

int psx_netplay_rb_fmv_episode_unsafe(uint32_t tick)
{
    (void)rb_in_fmv_defer_rewind_window();
    /* §97/§109: MEDIA_KF allows episodes into media-range and post-FMV /
     * DESYNC-hold silent-fork heals (begin arms keyframe). Without KF,
     * DESYNC invent-hold and media-range still block. */
    if (rb_media_kf_enabled())
        return 0;
    (void)tick;
    return rb_fmv_tick_unsafe_for_episode(tick);
}

int psx_netplay_rb_media_kf_probe_match(uint32_t size, uint32_t crc)
{
    uint32_t load;
    size_t sz = 0;
    const uint8_t *p;
    uint32_t got;
    if (!g_media_kf_episode || !g_rb || !g_snaps)
        return 0;
    load = rnet_rb_get_load_tick(g_rb);
    p = netplay_snap_ring_peek(g_snaps, load, &sz);
    if (!p || sz != (size_t)size)
        return 0;
    got = rnet_checksum(p, sz);
    if (got != crc)
        return 0;
    /* §100: refresh CRC ring + peer-match watermark (skip xfer). */
    rb_media_crc_note(load, size, got);
    if (!rb_media_kf_seal_local(load))
        return 0;
    if (load > g_media_crc_peer_match_through)
        g_media_crc_peer_match_through = load;
    return 1;
}

int psx_netplay_rb_media_kf_busy(void)
{
    return (g_media_kf_episode && !g_media_kf_ready) ? 1 : 0;
}

int psx_netplay_rb_media_kf_on_ready(const void *data, size_t size)
{
    uint32_t load;
    if (!g_media_kf_episode || !g_rb)
        return 0;
    load = rnet_rb_get_load_tick(g_rb);
    if (!g_media_kf_ready) {
        if (data && size > 0) {
            if (g_b.bios_checksum && g_b.entry_pc) {
                char reason[256];
                if (!boot_state_check_buffer((const uint8_t *)data, size,
                                             *g_b.bios_checksum, *g_b.entry_pc,
                                             reason, sizeof(reason))) {
                    fprintf(stderr,
                            "psxrecomp: rb MEDIA-KF install reject load=%u "
                            "bytes=%zu — %s\n",
                            (unsigned)load, size,
                            reason[0] ? reason : "unknown");
                    fflush(stderr);
                    g_media_kf_sending = 0;
                    abort_episode("MEDIA-KF install integrity reject");
                    return 1;
                }
            }
            if (!rb_install_media_kf_bytes(load, (const uint8_t *)data, size)) {
                fprintf(stderr,
                        "psxrecomp: rb MEDIA-KF install FAILED load=%u "
                        "bytes=%zu\n",
                        (unsigned)load, size);
                fflush(stderr);
                return 1;
            }
            fprintf(stderr,
                    "psxrecomp: rb MEDIA-KF installed load=%u bytes=%zu\n",
                    (unsigned)load, size);
            fflush(stderr);
        } else {
            (void)rb_media_kf_seal_local(load);
        }
    }
    g_media_kf_sending = 0;
    return 1;
}

void psx_netplay_rb_poll_replay_stall(void)
{
    uint64_t now;
    if (!g_rb || !rnet_rb_is_active(g_rb))
        return;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return;
    if (g_replay_progress_ms == 0ull)
        return;
    now = rb_mono_ms();
    if (now >= g_replay_progress_ms &&
        (now - g_replay_progress_ms) >= (uint64_t)RB_REPLAY_STALL_MS) {
        abort_episode("replay stall (no finish_frame after resume)");
    }
}

int psx_netplay_rb_take_promote_sweep(void)
{
    int v = g_promote_sweep;
    g_promote_sweep = 0;
    return v;
}

int psx_netplay_rb_fmv_unlock_grace_active(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    /* RELEASE arms dense_until = sim+UNLOCK_GRACE. §113: invent stays off
     * through lockstep_until; this flag still marks the post-RELEASE
     * reconcile / soft-promote window. */
    return g_fmv_lockstep_released && g_fmv_lockstep_until > 0u &&
           sim < g_fmv_lockstep_until;
}

int psx_netplay_rb_tip_extend(uint32_t mismatch_tick, int slot)
{
    RNetSession *s = sess();
    RNetRbPhase phase;
    RNetRbPhase phase_at_entry;
    uint32_t old_target;
    uint32_t new_target;
    uint32_t sim;
    uint32_t load;
    int32_t corr_slot;
    int need_rereplay;
    if (!g_rb || !s || !rnet_rb_is_active(g_rb))
        return 0;
    phase = rnet_rb_get_phase(g_rb);
    phase_at_entry = phase;
    if (phase != nRNetRbPhaseSealInputs && phase != nRNetRbPhaseAwaitingBaseline &&
        phase != nRNetRbPhaseReplay && phase != nRNetRbPhaseVerify &&
        phase != nRNetRbPhaseTipHold)
        return 0;
    /* §50: begin/follow already refuse NEW episodes in FMV lockstep, but
     * tip-extend on an open episode still raised the tip through media /
     * settle. Host rereplayed past guest ownership-final → Live, guest
     * FMV-refused the follow BEGIN, host ABANDON-realigned to bfc03c04 —
     * FIRST CORE + lasting CD fork. Same gate as begin: no tip raise. */
    (void)rb_in_fmv_defer_rewind_window();
    if (rb_in_fmv_lockstep_window()) {
        if (phase_at_entry == nRNetRbPhaseTipHold) {
            fprintf(stderr,
                    "psxrecomp: rb tip-extend REFUSED mismatch=%u — FMV "
                    "lockstep (tip-hold commit)\n",
                    (unsigned)mismatch_tick);
            fflush(stderr);
            /* §67: finalize clamps to the matched from-tip if a deferred
             * extension is pending — never flush a rereplay into media. */
            finalize_tip_hold();
            return 1; /* absorbed: episode closed at sealed tip */
        }
        {
            static uint32_t s_fmv_ext_refuse_sim;
            uint32_t sim_now = rnet_session_sim_tick(s);
            if (s_fmv_ext_refuse_sim != sim_now) {
                fprintf(stderr,
                        "psxrecomp: rb tip-extend REFUSED mismatch=%u — FMV "
                        "lockstep (keep target=%u; no raise through media)\n",
                        (unsigned)mismatch_tick,
                        (unsigned)rnet_rb_get_target_tick(g_rb));
                fflush(stderr);
                s_fmv_ext_refuse_sim = sim_now;
            }
        }
        return 0;
    }
    load = rnet_rb_get_load_tick(g_rb);
    if (mismatch_tick < load)
        return 0;
    corr_slot = rnet_rb_get_corrected_slot(g_rb);
    if (corr_slot >= 0 && slot >= 0 && corr_slot != slot)
        return 0; /* different seat — open a new episode after commit */
    old_target = rnet_rb_get_target_tick(g_rb);
    /* §91: peer already COMMIT'd this epoch at/below the sealed tip — further
     * tip-extend can never Verify (Live peer will not POST the raised tip).
     * Finalize TipHold / refuse Replay raise immediately. */
    if (g_peer_commit_epoch == rnet_rb_get_epoch_id(g_rb) &&
        g_peer_commit_tick > 0u && g_peer_commit_tick <= old_target) {
        fprintf(stderr,
                "psxrecomp: rb tip-extend REFUSED mismatch=%u — peer COMMIT "
                "epoch=%u tick=%u (tip=%u)\n",
                (unsigned)mismatch_tick, (unsigned)g_peer_commit_epoch,
                (unsigned)g_peer_commit_tick, (unsigned)old_target);
        fflush(stderr);
        if (phase_at_entry == nRNetRbPhaseTipHold) {
            if (g_tip_extend_from_tick == 0u && !g_tip_hold_rereplay_pending)
                g_tip_extend_from_tick = old_target;
            (void)maybe_abandon_tip_extend();
            return 1;
        }
        if (phase_at_entry == nRNetRbPhaseVerify ||
            phase_at_entry == nRNetRbPhaseReplay) {
            (void)maybe_abandon_tip_extend();
            return 1;
        }
        return 0;
    }
    sim = rnet_session_sim_tick(s);
    new_target = old_target;
    if (mismatch_tick > new_target)
        new_target = mismatch_tick;
    if (sim > new_target)
        new_target = sim;
    /* Rereplay only after the baseline/ready handshake (or TipHold/Verify).
     * Seal/AwaitingBaseline: resign+extend only — forcing Replay here made the
     * initiator solo-resim while the follower waited for GO (ready timeout).
     * TipHold: rereplay if Live invented past the prior tip *or* the mismatch
     * is past the sealed tip (peek-ahead release coalesce while Live is
     * stalled at invent-cap — sim may still equal old_target). */
    if (phase_at_entry == nRNetRbPhaseTipHold)
        need_rereplay = (sim > old_target || mismatch_tick > old_target) ? 1 : 0;
    else if (phase_at_entry == nRNetRbPhaseVerify)
        /* §76: raise tip into Replay without snap when the mismatch is a
         * pure extension past the POSTed tip. §80: late wire for a tick
         * already inside the sealed tip must rereplay (soak: arm 4012 with
         * s1=ffff, wire fdff → raise-only left the wrong fin sealed). */
        need_rereplay = (mismatch_tick <= old_target) ? 1 : 0;
    else if (phase_at_entry == nRNetRbPhaseReplay)
        /* §69: raise tip only while catching up above sim. §80: late wire
         * for an already-finished tick must rereplay that tick. */
        need_rereplay = (mismatch_tick < sim) ? 1 : 0;
    else
        need_rereplay = 0;
    /* Preflight span cap before mutating POST/seals — peer-seal bitmask is
     * uint64 (RNET_RB_PEER_SEAL_MASK_BITS). TipHold storm past load+63 must
     * commit and reopen, not sit in begin_refused until runway expires. */
    if (new_target > old_target && !rnet_rb_can_extend_target(g_rb, new_target)) {
        if (phase_at_entry == nRNetRbPhaseTipHold) {
            fprintf(stderr,
                    "psxrecomp: rb tip-extend SPAN CAP epoch=%u %u→%u "
                    "(seal span would exceed %u) — tip-hold commit\n",
                    (unsigned)rnet_rb_get_epoch_id(g_rb), (unsigned)old_target,
                    (unsigned)new_target, (unsigned)RNET_RB_PEER_SEAL_MASK_BITS);
            fflush(stderr);
            tip_hold_try_finalize();
        }
        return 0;
    }
    /* TipHold/Verify leave invalidates the prior POST pair. */
    if (phase_at_entry == nRNetRbPhaseTipHold || phase_at_entry == nRNetRbPhaseVerify) {
        clear_post_handshake();
        /* §42c: remember the matched/POSTed tip we are extending away from —
         * updated on EVERY TipHold/Verify extend so it always names the tip
         * the current rereplay reloads from (the one provably-preserved
         * matched snap). If the peer's OP_COMMIT lands there while we sit in
         * Verify at the extended tip, the extension is doomed — abandon and
         * realign there instead of verify-timeout forking.
         * §66: while TipHold rereplay is deferred/batched, keep the EARLIEST
         * tip so one flush reloads the whole coalesce span. */
        if (new_target > old_target) {
            if (phase_at_entry == nRNetRbPhaseTipHold &&
                g_tip_hold_rereplay_pending && g_tip_hold_rereplay_from != 0u)
                g_tip_extend_from_tick = g_tip_hold_rereplay_from;
            else
                g_tip_extend_from_tick = old_target;
        }
    }
    /* Prefill hist from the delay ring before resign/extend so local seat
     * (FOLLOW) and correction seat (initiator wire peek) seal auth pads —
     * not invent-idle — while Live is invent-cap stalled at the prior tip. */
    {
        uint32_t pref_lo = mismatch_tick;
        uint32_t pref_hi = (new_target > mismatch_tick) ? new_target : mismatch_tick;
        int local = g_b.local_slot ? *g_b.local_slot : -1;
        host_prefresh_seal_range(slot, pref_lo, pref_hi);
        if (local >= 0 && local != slot)
            host_prefresh_seal_range(local, pref_lo, pref_hi);
    }
    (void)rnet_rb_resign_slot_range(g_rb, slot, mismatch_tick, mismatch_tick);
    export_local_seals();
    if (new_target > old_target) {
        if (!rnet_rb_extend_target(g_rb, new_target))
            return 0;
        (void)rnet_rb_resign_slot_range(g_rb, slot, old_target + 1u, new_target);
        if (!rnet_rb_is_from_peer_notify(g_rb)) {
            /* Shared rereplay decision: the follower mirrors this flag instead
             * of re-deriving from its own (possibly different) live tip.
             * §80: mismatch field carries the prefer_plus arm point when
             * rereplaying a late-wire tick (FOLLOW schedule_episode_rereplay). */
            rnet_u8 wire_flags = (rnet_u8)(need_rereplay ? RNET_RB_SYNC_FLAG_REREPLAY : 0u);
            uint32_t wire_mismatch = need_rereplay ? mismatch_tick
                                                   : rnet_rb_get_mismatch_tick(g_rb);
            if (rnet_rb_recommend_light_tip(g_rb))
                wire_flags |= RNET_RB_SYNC_FLAG_LIGHT_TIP;
            (void)rnet_session_send_rb_sync(
                s, rnet_rb_get_epoch_id(g_rb), wire_mismatch, load, new_target,
                (rnet_u8)(slot < 0 ? 0 : slot), RNET_RB_SYNC_OP_BEGIN,
                wire_flags);
        }
        export_local_seals();
        fprintf(stderr,
                "psxrecomp: rb tip-extend epoch=%u %u→%u mismatch=%u slot=%d "
                "(light=%u rereplay=%d)\n",
                (unsigned)rnet_rb_get_epoch_id(g_rb), (unsigned)old_target,
                (unsigned)new_target, (unsigned)mismatch_tick, slot,
                (unsigned)rnet_rb_recommend_light_tip(g_rb), need_rereplay);
        fflush(stderr);
    } else if (need_rereplay) {
        (void)rnet_rb_extend_target(g_rb, old_target);
        /* §80: same-target late-wire rereplay must still notify FOLLOW. */
        if (!rnet_rb_is_from_peer_notify(g_rb)) {
            rnet_u8 wire_flags = (rnet_u8)RNET_RB_SYNC_FLAG_REREPLAY;
            if (rnet_rb_recommend_light_tip(g_rb))
                wire_flags |= RNET_RB_SYNC_FLAG_LIGHT_TIP;
            (void)rnet_session_send_rb_sync(
                s, rnet_rb_get_epoch_id(g_rb), mismatch_tick, load, old_target,
                (rnet_u8)(slot < 0 ? 0 : slot), RNET_RB_SYNC_OP_BEGIN,
                wire_flags);
        }
        export_local_seals();
        fprintf(stderr,
                "psxrecomp: rb tip-extend same-target rereplay epoch=%u "
                "mismatch=%u tip=%u slot=%d\n",
                (unsigned)rnet_rb_get_epoch_id(g_rb), (unsigned)mismatch_tick,
                (unsigned)old_target, slot);
        fflush(stderr);
    } else if (phase_at_entry == nRNetRbPhaseSealInputs ||
               phase_at_entry == nRNetRbPhaseAwaitingBaseline) {
        fprintf(stderr,
                "psxrecomp: rb tip-extend defer-rereplay epoch=%u mismatch=%u "
                "target=%u (handshake phase=%d)\n",
                (unsigned)rnet_rb_get_epoch_id(g_rb), (unsigned)mismatch_tick,
                (unsigned)new_target, (int)phase_at_entry);
        fflush(stderr);
    }
    if (need_rereplay) {
        /* §66: TipHold stays TipHold — raise tip + mark deferred rereplay so
         * coalesce-ahead can batch edges into one snap (soak early-match
         * cascade: tip-extend rereplay every 2–4 ticks → 10 fps). Quiet /
         * thrash / SAFETY flush via tip_hold_try_finalize. */
        if (phase_at_entry == nRNetRbPhaseTipHold) {
            if (!g_tip_hold_rereplay_pending) {
                g_tip_hold_rereplay_pending = 1;
                g_tip_hold_rereplay_from = old_target;
                g_tip_extend_from_tick = old_target;
            }
            /* §78: earliest divergent row across coalesced defers. */
            if (mismatch_tick > 0u &&
                (g_tip_hold_rereplay_mismatch == 0u ||
                 mismatch_tick < g_tip_hold_rereplay_mismatch))
                g_tip_hold_rereplay_mismatch = mismatch_tick;
            {
                uint32_t runway = rnet_rb_get_tip_runway(g_rb);
                uint32_t cap = new_target + runway;
                if (cap > g_tip_hold_until)
                    g_tip_hold_until = cap;
            }
            /* §24: do not reset quiet_t0 — coalesce window must still fire. */
            g_tip_hold_coalesce_n++;
            fprintf(stderr,
                    "psxrecomp: rb tip-extend rereplay DEFER epoch=%u "
                    "mismatch=%u from=%u tip=%u slot=%d coalesce_n=%u\n",
                    (unsigned)rnet_rb_get_epoch_id(g_rb),
                    (unsigned)mismatch_tick,
                    (unsigned)g_tip_hold_rereplay_from, (unsigned)new_target,
                    slot, (unsigned)g_tip_hold_coalesce_n);
            fflush(stderr);
        } else {
            /* §80: reload prefer = mismatch-1 (late-wire tick), not old tip.
             * FOLLOW gets the same prefer_plus via the wire mismatch field. */
            schedule_episode_rereplay(mismatch_tick);
            {
                uint32_t runway = rnet_rb_get_tip_runway(g_rb);
                uint32_t cap = new_target + runway;
                if (cap > g_tip_hold_until)
                    g_tip_hold_until = cap;
            }
            /* Rereplay needs a fresh quiet window after the resim settles. */
            g_tip_hold_quiet_t0_ms = 0ull;
            fprintf(stderr,
                    "psxrecomp: rb tip-extend rereplay epoch=%u mismatch=%u "
                    "prior_tip=%u slot=%d until=%u\n",
                    (unsigned)rnet_rb_get_epoch_id(g_rb),
                    (unsigned)mismatch_tick, (unsigned)old_target, slot,
                    (unsigned)g_tip_hold_until);
            fflush(stderr);
        }
    } else if (phase_at_entry == nRNetRbPhaseVerify && new_target > old_target) {
        /* §76: leave Verify into Replay for the raised span — no snap reload. */
        uint32_t first = old_target + 1u;
        g_tip_extend_from_tick = old_target;
        g_pending_load_valid = 0;
        g_tip_extend_rereplay = 0;
        g_pending_resume_valid = 0;
        g_episode_snap_applied = 1;
        rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay);
        if (first <= new_target) {
            rnet_session_set_sim_tick(s, first);
            if (rb_arm_rows_ready(first))
                arm_replay_tick(first);
            else
                g_needs_advance = 0;
        } else {
            rnet_session_set_sim_tick(s, old_target);
            g_needs_advance = 0;
            enter_verify_at_tip(old_target);
        }
        fprintf(stderr,
                "psxrecomp: rb tip-extend Verify→Replay raise-only "
                "epoch=%u %u→%u first=%u (no snap)\n",
                (unsigned)rnet_rb_get_epoch_id(g_rb), (unsigned)old_target,
                (unsigned)new_target, (unsigned)first);
        fflush(stderr);
    } else if (phase_at_entry == nRNetRbPhaseTipHold) {
        uint32_t runway = rnet_rb_get_tip_runway(g_rb);
        uint32_t cap = new_target + runway;
        if (cap > g_tip_hold_until)
            g_tip_hold_until = cap;
        /* §24: do NOT reset quiet_t0 on pure TipHold coalesce — extending
         * tip_hold_until still absorbs the edge into this episode, but the
         * wall-clock quiet / thrash deadline can still commit. */
        g_tip_hold_coalesce_n++;
    }
    g_tip_hold_block_quiet = 0;
    return 1;
}

int psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot)
{
    RNetRbCorrection corr;
    RNetSession *s = sess();
    uint32_t load = 0;
    uint32_t target;
    uint32_t sim;
    uint32_t snap_n;
    uint32_t snap_lo;
    uint32_t snap_hi;
    static uint32_t s_refuse_last_mismatch = 0xffffffffu;
    static uint32_t s_refuse_suppressed;

    if (!g_rb || !s)
        return 0;
    /* §115: tip+1 platform fork already accepted — do not re-heal it. */
    if (g_platform_accepted_fork != 0u &&
        mismatch_tick == g_platform_accepted_fork) {
        static uint32_t s_accept_refuse_log;
        if (s_accept_refuse_log != mismatch_tick) {
            fprintf(stderr,
                    "psxrecomp: rb begin REFUSED mismatch=%u — §115 platform "
                    "fork already accepted\n",
                    (unsigned)mismatch_tick);
            fflush(stderr);
            s_accept_refuse_log = mismatch_tick;
        }
        g_request_post_fmv_heal = 0;
        return 0;
    }
    /* Prefer tip-extend over refusing while an episode is already open.
     * Span-cap TipHold commit leaves the session inactive — fall through to
     * open a fresh episode for this mismatch (symmetric with peer). */
    if (rnet_rb_is_active(g_rb)) {
        if (psx_netplay_rb_tip_extend(mismatch_tick, slot))
            return 1;
        if (rnet_rb_is_tip_holding(g_rb)) {
            int32_t corr = rnet_rb_get_corrected_slot(g_rb);
            if (corr >= 0 && slot >= 0 && corr != (int32_t)slot) {
                /* Different seat cannot tip-extend — commit and reopen so the
                 * release/press is not begin_refused then quiet-dropped (§34).
                 * §67: commit at the matched from-tip (clamp inside) — the
                 * fresh episode below repairs any deferred extension span. */
                fprintf(stderr,
                        "psxrecomp: rb tip-hold yield different seat "
                        "corr=%d edge_slot=%d mismatch=%u — tip-hold commit\n",
                        (int)corr, slot, (unsigned)mismatch_tick);
                fflush(stderr);
                finalize_tip_hold();
            } else {
                g_tip_hold_block_quiet = 1;
                g_tip_hold_quiet_t0_ms = 0ull;
                return 0;
            }
        } else if (rnet_rb_is_active(g_rb)) {
            return 0;
        }
        fprintf(stderr,
                "psxrecomp: rb tip-hold yielded — begin fresh episode "
                "mismatch=%u slot=%d\n",
                (unsigned)mismatch_tick, slot);
        fflush(stderr);
    }

    sim = rnet_session_sim_tick(s);
    /* §102: drain peer RESOLVED before choose_load — a tip already in the
     * UDP queue must raise peer_resolved so we do not open a zombie load. */
    {
        rnet_u32 resolved = 0;
        while (rnet_session_take_rb_resolved(s, &resolved))
            apply_peer_resolved(resolved);
    }

    /* Tick settle tracker. Media/lockstep episodes resume into MotK wait/VLC
     * tips and hang — admit waits for wire instead.
     * §103: ownership SPAN after MEDIA-KF must continue through re-armed
     * settle (session 142: tip=776 frontier=835 refused → tip-hold hang). */
    (void)rb_in_fmv_defer_rewind_window();
    /* §109/§114: MEDIA_KF heal may open through media/settle/DESYNC hold. */
    if (rb_in_fmv_lockstep_window() && !g_ownership_chain &&
        !(rb_media_kf_enabled() &&
          (g_media_kf_episode || g_request_post_fmv_heal || g_post_fmv_heal_kf ||
           g_post_fmv_platform_nondet ||
           (g_fmv_unmatched_desync && g_request_post_fmv_heal)))) {
        static uint32_t s_fmv_refuse_sim;
        if (s_fmv_refuse_sim != sim) {
            fprintf(stderr,
                    "psxrecomp: rb begin REFUSED mismatch=%u — FMV lockstep "
                    "(no tip episode; admit waits for wire)%s\n",
                    (unsigned)mismatch_tick,
                    g_fmv_unmatched_desync ? " [DESYNC hold]" : "");
            fflush(stderr);
            s_fmv_refuse_sim = sim;
        }
        return 0;
    }
    /* §93 refuse / §97 MEDIA_KF: mismatch in prior media bout. */
    if (rb_fmv_tick_unsafe_for_episode(mismatch_tick) && !rb_media_kf_enabled()) {
        static uint32_t s_media_mm_refuse_sim;
        if (s_media_mm_refuse_sim != sim) {
            fprintf(stderr,
                    "psxrecomp: rb begin REFUSED mismatch=%u — FMV media-range "
                    "(lo=%u hi=%u settle_until=%u; no episode into media)\n",
                    (unsigned)mismatch_tick, (unsigned)g_fmv_media_lo,
                    (unsigned)g_fmv_media_hi, (unsigned)g_fmv_settle_until);
            fflush(stderr);
            s_media_mm_refuse_sim = sim;
        }
        return 0;
    }

    /* Coalesce: refuse reopen after abort/storm only; reconcile promotes wire.
     * until is armed from live sim at failure (not rewound tip) so catch-up
     * cannot burn the window before the next d-pad edge. Clean commit does
     * not arm this. */
    if (sim < g_rewind_cooldown_until && !g_ownership_chain &&
        !g_request_post_fmv_heal && !g_post_fmv_platform_nondet) {
        static uint32_t s_cd_log_sim;
        if (s_cd_log_sim != sim) {
            fprintf(stderr,
                    "psxrecomp: rb begin COOLDOWN mismatch=%u sim=%u until=%u "
                    "streak=%u\n",
                    (unsigned)mismatch_tick, (unsigned)sim,
                    (unsigned)g_rewind_cooldown_until,
                    (unsigned)g_bl_mismatch_streak);
            fflush(stderr);
            s_cd_log_sim = sim;
        }
        return 0;
    }
    snap_n = g_snaps ? netplay_snap_ring_count(g_snaps) : 0u;
    snap_lo = g_snaps ? netplay_snap_ring_oldest_tick(g_snaps) : 0u;
    snap_hi = g_snaps ? netplay_snap_ring_newest_tick(g_snaps) : 0u;
    if (!choose_load_tick(mismatch_tick, &load)) {
        /* One line per mismatch tick (+ suppressed count) — empty-ring thrash
         * previously wrote thousands of identical REFUSED lines per second. */
        if (mismatch_tick != s_refuse_last_mismatch) {
            if (s_refuse_suppressed > 0u) {
                fprintf(stderr,
                        "psxrecomp: rb begin REFUSED (+%u similar for mismatch=%u)\n",
                        (unsigned)s_refuse_suppressed,
                        (unsigned)s_refuse_last_mismatch);
                s_refuse_suppressed = 0;
            }
            fprintf(stderr,
                    "psxrecomp: rb begin REFUSED mismatch=%u slot=%d — no "
                    "confirmed snap (ring count=%u oldest=%u newest=%u "
                    "hc=%u agreed=%u)\n",
                    (unsigned)mismatch_tick, slot, (unsigned)snap_n,
                    (unsigned)snap_lo, (unsigned)snap_hi,
                    (unsigned)(g_b.hc ? netplay_hc_resolved_through(g_b.hc) : 0u),
                    (unsigned)(g_agreed_valid ? g_agreed_through : 0u));
            s_refuse_last_mismatch = mismatch_tick;
        } else {
            s_refuse_suppressed++;
        }
        return 0;
    }
    /* §101: never begin behind a peer tip we already know — that opens a
     * zombie-load episode the follower must NACK. Prefer peer_resolved (or
     * the newest mutual snap ≤ it) when choose_load landed lower. */
    if (!g_ownership_chain && g_peer_resolved_through > load &&
        mismatch_tick > 0u && g_peer_resolved_through < mismatch_tick &&
        g_snaps) {
        uint32_t want = g_peer_resolved_through;
        uint32_t t;
        uint32_t raised = 0u;
        if (want > mismatch_tick - 1u)
            want = mismatch_tick - 1u;
        for (t = want;; --t) {
            if (netplay_snap_ring_has(g_snaps, t) &&
                (t <= g_peer_resolved_through) &&
                (g_peer_nack_floor == 0u || t > g_peer_nack_floor)) {
                raised = t;
                break;
            }
            if (t == 0u || t <= load)
                break;
        }
        if (raised > load) {
            fprintf(stderr,
                    "psxrecomp: rb begin raise load %u→%u "
                    "(peer_resolved=%u — avoid zombie load)\n",
                    (unsigned)load, (unsigned)raised,
                    (unsigned)g_peer_resolved_through);
            fflush(stderr);
            load = raised;
        }
    }
    /* §102: peer-ahead NACK reopen — pin load at the follower's tip. */
    if (g_peer_ahead_force_load > 0u &&
        g_peer_ahead_force_load < mismatch_tick &&
        (g_peer_nack_floor == 0u || g_peer_ahead_force_load > g_peer_nack_floor) &&
        (netplay_snap_ring_has(g_snaps, g_peer_ahead_force_load) ||
         (g_agreed_valid && g_peer_ahead_force_load == g_agreed_through))) {
        if (load != g_peer_ahead_force_load) {
            fprintf(stderr,
                    "psxrecomp: rb begin peer-ahead force load %u→%u\n",
                    (unsigned)load, (unsigned)g_peer_ahead_force_load);
            fflush(stderr);
            load = g_peer_ahead_force_load;
        }
    }
    /* §51 Layer 3: chain continue must load exactly at the verified tip. */
    if (g_ownership_chain && mismatch_tick > 0u) {
        uint32_t tip = mismatch_tick - 1u;
        if (g_ownership_skip_snap ||
            (g_snaps && netplay_snap_ring_has(g_snaps, tip))) {
            if (load != tip) {
                fprintf(stderr,
                        "psxrecomp: rb ownership force load %u→%u "
                        "(chain tip; skip-snap=%d)\n",
                        (unsigned)load, (unsigned)tip, g_ownership_skip_snap);
                fflush(stderr);
                load = tip;
            }
        }
    }
    /* §38/§39: during tip-hold agreed holdoff, do not open load above the
     * commit frontier (peer may still tip-hold). Clamp in choose_load should
     * already pin load; this is belt-and-suspenders. */
    if (g_rb && post_tip_hold_holdoff_active()) {
        uint32_t tip_rt = rnet_rb_resolved_through(g_rb);
        if (tip_rt > 0u && load > tip_rt) {
            static uint32_t s_holdoff_refuse_sim;
            if (s_holdoff_refuse_sim != sim) {
                fprintf(stderr,
                        "psxrecomp: rb begin DEFER mismatch=%u load=%u "
                        "frontier=%u — tip-hold agreed holdoff\n",
                        (unsigned)mismatch_tick, (unsigned)load,
                        (unsigned)tip_rt);
                fflush(stderr);
                s_holdoff_refuse_sim = sim;
            }
            return 0;
        }
    }
    /* §40/§41/§51: defer above peer RESOLVED only while the gate holds — but
     * HC-proven loads are mutually safe (do not DEFER below hc_floor). */
    if (peer_resolved_gate_blocks(load)) {
        uint32_t floor = hc_provable_through();
        if (load > floor) {
            static uint32_t s_peer_defer_sim;
            if (s_peer_defer_sim != sim) {
                fprintf(stderr,
                        "psxrecomp: rb begin DEFER mismatch=%u load=%u "
                        "frontier=%u hc_floor=%u — peer RESOLVED\n",
                        (unsigned)mismatch_tick, (unsigned)load,
                        (unsigned)g_peer_resolved_through, (unsigned)floor);
                fflush(stderr);
                s_peer_defer_sim = sim;
            }
            return 0;
        }
    }
    if (s_refuse_suppressed > 0u) {
        fprintf(stderr,
                "psxrecomp: rb begin REFUSED (+%u similar for mismatch=%u)\n",
                (unsigned)s_refuse_suppressed, (unsigned)s_refuse_last_mismatch);
        s_refuse_suppressed = 0;
        s_refuse_last_mismatch = 0xffffffffu;
    }

    /* §93 refuse / §97 MEDIA_KF path for loads inside a prior FMV bout. */
    {
        int media_load = rb_fmv_tick_unsafe_for_episode(load) ||
                         rb_fmv_tick_unsafe_for_episode(mismatch_tick);
        if (media_load && !rb_media_kf_enabled()) {
            fprintf(stderr,
                    "psxrecomp: rb begin REFUSED mismatch=%u load=%u — FMV "
                    "media-range load (lo=%u hi=%u settle_until=%u)\n",
                    (unsigned)mismatch_tick, (unsigned)load,
                    (unsigned)g_fmv_media_lo, (unsigned)g_fmv_media_hi,
                    (unsigned)g_fmv_settle_until);
            fflush(stderr);
            return 0;
        }
        if (media_load && rb_media_kf_enabled()) {
            if (!g_snaps || !netplay_snap_ring_has(g_snaps, load)) {
                fprintf(stderr,
                        "psxrecomp: rb begin REFUSED mismatch=%u load=%u — "
                        "MEDIA-KF needs snap at load\n",
                        (unsigned)mismatch_tick, (unsigned)load);
                fflush(stderr);
                return 0;
            }
        }
    }

    target = rnet_rb_suggest_target(g_rb, mismatch_tick, sim);
    if (target < load)
        target = load;
    /* §47 ownership chain: aim at contiguous frontier, still SPAN-chunked. */
    if (g_ownership_chain && g_ownership_chain_frontier > target)
        target = g_ownership_chain_frontier;
    /* §102: peer-ahead light tip — short depth, no ownership SPAN chain. */
    if (g_peer_ahead_force_load > 0u) {
        uint32_t max_t = load + RB_PEER_AHEAD_LIGHT_DEPTH;
        if (target > max_t) {
            fprintf(stderr,
                    "psxrecomp: rb begin peer-ahead light CAP target %u→%u "
                    "(load=%u depth=%u)\n",
                    (unsigned)target, (unsigned)max_t, (unsigned)load,
                    (unsigned)RB_PEER_AHEAD_LIGHT_DEPTH);
            fflush(stderr);
            target = max_t;
        }
        g_ownership_chain = 0;
        g_ownership_chain_frontier = 0;
        ownership_clear_chain_budget();
        g_peer_ahead_light_episode = 1;
        g_peer_ahead_force_load = 0u;
    }
    /* Chunk deep catch-up (post-abort agreed tip << live) so Replay never
     * walks the whole cooldown window in one episode. */
    if (target > load + RB_MAX_RESIM_SPAN) {
        fprintf(stderr,
                "psxrecomp: rb begin SPAN CAP mismatch=%u load=%u "
                "target %u→%u (max_span=%u)%s\n",
                (unsigned)mismatch_tick, (unsigned)load, (unsigned)target,
                (unsigned)(load + RB_MAX_RESIM_SPAN),
                (unsigned)RB_MAX_RESIM_SPAN,
                g_ownership_chain ? " ownership-chain" : "");
        fflush(stderr);
        target = load + RB_MAX_RESIM_SPAN;
    }
    /* §109/§110b: hc-fork requested MEDIA_KF verify heal — resim a short
     * span past load so tip+1 is checked. Empty-span tip KF rematches tip
     * and storms (Win↔Linux soak). Cap loops at the same fork → DESYNC. */
    if (g_request_post_fmv_heal && rb_post_fmv_heal_eligible()) {
        if (mismatch_tick == g_post_fmv_heal_loop_fork &&
            g_post_fmv_heal_loop_fork > 0u)
            g_post_fmv_heal_loop_count++;
        else {
            g_post_fmv_heal_loop_fork = mismatch_tick;
            g_post_fmv_heal_loop_count = 1u;
        }
        if (g_post_fmv_heal_loop_count > RB_FMV_HEAL_LOOP_LIMIT) {
            RNetSession *s = sess();
            uint32_t live =
                s ? rnet_session_sim_tick(s) : (mismatch_tick > 0u ? mismatch_tick : load);
            fprintf(stderr,
                    "psxrecomp: rb DESYNC — post-FMV heal loop storm "
                    "fork=%u (%u×) — tip+1 platform nondet; §114 keep-live "
                    "KF stream, cooldown %u ticks\n",
                    (unsigned)mismatch_tick,
                    (unsigned)g_post_fmv_heal_loop_count,
                    (unsigned)RB_FORK_STORM_COOLDOWN_TICKS);
            fflush(stderr);
            g_request_post_fmv_heal = 0;
            g_post_fmv_heal_kf = 0;
            rb_post_fmv_platform_nondet_enter(load, mismatch_tick, live,
                                             "heal loop storm");
            arm_rewind_cooldown_ticks(live, RB_FORK_STORM_COOLDOWN_TICKS,
                                      "DESYNC post-FMV heal loop storm");
            /* Stream heals bypass cooldown via g_request_post_fmv_heal. */
            return 0;
        }
        {
            /* §114: apply-only. Verify-span resim of tip+1 fails matched-
             * baseline nondeterminism (Win↔Linux soak: +10 cyc @1036). */
            if (target != load) {
                fprintf(stderr,
                        "psxrecomp: rb begin post-FMV heal KF CAP target %u→%u "
                        "(apply-only; mismatch=%u load=%u loop=%u)\n",
                        (unsigned)target, (unsigned)load,
                        (unsigned)mismatch_tick, (unsigned)load,
                        (unsigned)g_post_fmv_heal_loop_count);
                fflush(stderr);
                target = load;
            } else {
                fprintf(stderr,
                        "psxrecomp: rb begin post-FMV heal KF apply-only "
                        "load=%u mismatch=%u loop=%u\n",
                        (unsigned)load, (unsigned)mismatch_tick,
                        (unsigned)g_post_fmv_heal_loop_count);
                fflush(stderr);
            }
        }
        g_post_fmv_heal_kf = 1;
        g_post_fmv_heal_fork_tick = mismatch_tick;
        g_request_post_fmv_heal = 0;
        g_ownership_chain = 0;
        g_ownership_chain_frontier = 0;
        ownership_clear_chain_budget();
        g_peer_ahead_light_episode = 0;
    }

    memset(&corr, 0, sizeof(corr));
    /* Slot-partitioned epoch id: (counter << bits) | initiator_slot. Dual
     * initiation can never collide, and the tie-break can derive any epoch's
     * initiator from the id alone. */
    g_epoch++;
    corr.epoch_id = (g_epoch << RB_EPOCH_SLOT_BITS) |
                    ((uint32_t)(g_b.local_slot ? *g_b.local_slot : 0) & RB_EPOCH_SLOT_MASK);
    corr.mismatch_tick = mismatch_tick;
    corr.load_tick = load;
    corr.target_tick = target;
    corr.slot = slot;
    corr.initiator = 1u;
    corr.from_peer_notify = 0u;
    /* Prefer MotK agreed tip when library watermark not yet set. Depth
     * ceiling comes from the session's configured light_tip_max_depth
     * (== RB_MOTK_TIP_RUNWAY), not the library default — see
     * psx_netplay_rb_start(). §93/§97: never light-tip into/near FMV media. */
    if (g_agreed_valid && !g_post_fmv_heal_kf &&
        !rb_fmv_tick_unsafe_for_episode(load) &&
        !rb_fmv_tick_unsafe_for_episode(target) &&
        rnet_rb_is_light_tip_candidate_ex(load, target, g_agreed_through,
                                          rnet_rb_get_light_tip_max_depth(g_rb)))
        corr.flags = RNET_RB_CORR_LIGHT_TIP;

    g_last_begin_mismatch = mismatch_tick;
    rnet_rb_begin_episode(g_rb, &corr);
    psx_netplay_timesync_on_episode_boundary();
    clear_episode_wire_state();
    /* §97/§109: arm MEDIA_KF after wire clear (clear resets media_kf_*). */
    {
        int post_heal = rb_post_fmv_heal_eligible() &&
                        !rb_fmv_tick_unsafe_for_episode(load) &&
                        !rb_fmv_tick_unsafe_for_episode(mismatch_tick);
        /* g_post_fmv_heal_kf may already be set when we CAP'd target=load. */
        if (g_post_fmv_heal_kf)
            post_heal = 1;
        if (rb_want_heal_kf(mismatch_tick, load) || post_heal) {
            g_media_kf_episode = 1;
            g_media_kf_ready = 0;
            g_media_kf_host_armed = 0;
            g_media_kf_sending = 0;
            g_post_fmv_heal_kf = post_heal ? 1 : 0;
        } else {
            g_post_fmv_heal_kf = 0;
        }
    }
    g_episode_snap_applied = 0;
    g_pending_resume_valid = 0;
    g_empty_span_verify_pending = 0;
    g_needs_advance = 0;
    g_seal_export_logged = 0;
    /* §85: freeze catch-up at this begin's target (before mid-Replay wire). */
    g_replay_catchup_cap = target;
    /* Seal from load_tick so every Replay quantum has authoritative pads. */
    rnet_rb_seal_inputs(g_rb, corr.load_tick, corr.target_tick, corr.slot);
    g_seal_wait_ms = rb_mono_ms();
    g_follow_nack_pending = 0;
    {
        /* Initiator-authoritative episode attributes: light-tip class rides
         * the wire so the follower classifies identically (burst symmetry). */
        rnet_u8 wire_flags = (rnet_u8)(
            (rnet_rb_get_corr_flags(g_rb) & RNET_RB_CORR_LIGHT_TIP)
                ? RNET_RB_SYNC_FLAG_LIGHT_TIP : 0u);
        if (g_media_kf_episode)
            wire_flags = (rnet_u8)(wire_flags | RNET_RB_SYNC_FLAG_MEDIA_KF);
        (void)rnet_session_send_rb_sync(s, corr.epoch_id, corr.mismatch_tick, corr.load_tick,
                                        corr.target_tick, (rnet_u8)(slot < 0 ? 0 : slot),
                                        RNET_RB_SYNC_OP_BEGIN, wire_flags);
    }
    export_local_seals();
    fprintf(stderr,
            "psxrecomp: rb begin epoch=%u mismatch=%u load=%u target=%u slot=%d "
            "light=%u media_kf=%d heal=%d (snaps=%u %u..%u local_slot=%d)\n",
            (unsigned)corr.epoch_id, (unsigned)mismatch_tick, (unsigned)load, (unsigned)target,
            slot, (unsigned)rnet_rb_recommend_light_tip(g_rb), g_media_kf_episode,
            g_post_fmv_heal_kf,
            (unsigned)snap_n, (unsigned)snap_lo, (unsigned)snap_hi,
            g_b.local_slot ? *g_b.local_slot : -1);
    fflush(stderr);

    if (rnet_rb_all_peer_seal_rows_complete(g_rb))
        enter_awaiting_baseline();
    else
        apply_stashed_baseline();
    return 1;
}

static void stash_peer_begin(uint32_t epoch, uint32_t mismatch, uint32_t load,
                             uint32_t target, int slot, uint8_t wire_flags)
{
    /* §61: never stash a BEGIN for the episode we are already in — that is a
     * keepalive rexmit. Stashing it and unstashing after Live re-opened the
     * same epoch (soak: follow 16 → final → unstash 16 → verify timeout). */
    if (g_rb && rnet_rb_is_active(g_rb) &&
        epoch == rnet_rb_get_epoch_id(g_rb))
        return;
    /* Also ignore BEGINs for an epoch we already committed. */
    if (epoch == g_last_commit_epoch)
        return;
    g_stash_begin_valid = 1;
    g_stash_begin_epoch = epoch;
    g_stash_begin_mismatch = mismatch;
    g_stash_begin_load = load;
    g_stash_begin_target = target;
    g_stash_begin_slot = slot;
    g_stash_begin_flags = wire_flags;
    if (!g_stash_begin_logged) {
        fprintf(stderr,
                "psxrecomp: rb BEGIN stashed epoch=%u load=%u target=%u "
                "(local episode still active — apply when idle)\n",
                (unsigned)epoch, (unsigned)load, (unsigned)target);
        fflush(stderr);
        g_stash_begin_logged = 1;
    }
}

static void apply_stashed_begin(void)
{
    uint32_t epoch, mismatch, load, target;
    int slot;
    uint8_t flags;
    if (!g_stash_begin_valid || !g_rb || rnet_rb_is_active(g_rb))
        return;
    /* §61: drop stashes for epochs already committed / completed. */
    if (g_stash_begin_epoch == g_last_commit_epoch) {
        g_stash_begin_valid = 0;
        g_stash_begin_logged = 0;
        return;
    }
    epoch = g_stash_begin_epoch;
    mismatch = g_stash_begin_mismatch;
    load = g_stash_begin_load;
    target = g_stash_begin_target;
    slot = g_stash_begin_slot;
    flags = g_stash_begin_flags;
    g_stash_begin_valid = 0;
    g_stash_begin_logged = 0;
    fprintf(stderr,
            "psxrecomp: rb BEGIN unstash epoch=%u load=%u target=%u\n",
            (unsigned)epoch, (unsigned)load, (unsigned)target);
    fflush(stderr);
    begin_follower(epoch, mismatch, load, target, slot, flags);
}

/* §60: BEGIN was fire-and-forget; on TURN a single drop left the follower
 * parked on "wait FOLLOW" while the initiator sat in rb_baseline forever.
 * Retransmit on the same cadence as seal/baseline keepalive. */
static void maybe_rexmit_begin(void)
{
    RNetSession *s = sess();
    RNetRbPhase ph;
    uint64_t now;
    rnet_u8 wire_flags;
    int32_t slot;
    if (!g_rb || !s || !rnet_rb_is_active(g_rb) || rnet_rb_is_from_peer_notify(g_rb))
        return;
    ph = rnet_rb_get_phase(g_rb);
    if (ph != nRNetRbPhaseSealInputs && ph != nRNetRbPhaseAwaitingBaseline)
        return;
    if (g_peer_baseline_ok)
        return; /* follower has joined the episode */
    now = rb_mono_ms();
    if (g_last_begin_rexmit_ms != 0ull && now >= g_last_begin_rexmit_ms &&
        (now - g_last_begin_rexmit_ms) < (uint64_t)RB_BASELINE_BURST_MS)
        return;
    if (g_last_begin_rexmit_ms != 0ull && !g_begin_rexmit_logged) {
        fprintf(stderr,
                "psxrecomp: rb BEGIN rexmit epoch=%u load=%u target=%u\n",
                (unsigned)rnet_rb_get_epoch_id(g_rb),
                (unsigned)rnet_rb_get_load_tick(g_rb),
                (unsigned)rnet_rb_get_target_tick(g_rb));
        fflush(stderr);
        g_begin_rexmit_logged = 1;
    }
    g_last_begin_rexmit_ms = now;
    wire_flags = (rnet_u8)(
        (rnet_rb_get_corr_flags(g_rb) & RNET_RB_CORR_LIGHT_TIP)
            ? RNET_RB_SYNC_FLAG_LIGHT_TIP : 0u);
    slot = rnet_rb_get_corrected_slot(g_rb);
    (void)rnet_session_send_rb_sync(
        s, rnet_rb_get_epoch_id(g_rb), rnet_rb_get_mismatch_tick(g_rb),
        rnet_rb_get_load_tick(g_rb), rnet_rb_get_target_tick(g_rb),
        (rnet_u8)(slot < 0 ? 0 : slot), RNET_RB_SYNC_OP_BEGIN, wire_flags);
}

static void begin_follower(uint32_t epoch, uint32_t mismatch, uint32_t load, uint32_t target,
                           int slot, uint8_t wire_flags)
{
    RNetRbCorrection corr;
    uint32_t snap_n;
    /* Tip-extend SYNC: same epoch, higher target — absorb, don't re-begin.
     * §80: also absorb same-target REREPLAY (late-wire / tip-hold flush).
     * Must mirror initiator tip_extend: clear POST + rereplay from prior tip
     * when TipHold/Verify (follower used to arm tip-only and abort on stale
     * pre-extend peer POST). */
    if (g_rb && rnet_rb_is_active(g_rb) && epoch == rnet_rb_get_epoch_id(g_rb) &&
        load == rnet_rb_get_load_tick(g_rb) &&
        (target > rnet_rb_get_target_tick(g_rb) ||
         ((wire_flags & RNET_RB_SYNC_FLAG_REREPLAY) &&
          target >= rnet_rb_get_target_tick(g_rb)))) {
        uint32_t old_t = rnet_rb_get_target_tick(g_rb);
        RNetRbPhase phase_at_entry = rnet_rb_get_phase(g_rb);
        RNetSession *fs = sess();
        uint32_t sim_now = fs ? rnet_session_sim_tick(fs) : 0u;
        int tip_raised = (target > old_t) ? 1 : 0;
        /* Shared rereplay decision: the initiator's flag is authoritative;
         * OR in the local invent check (our Live inventing past the prior tip
         * forks history regardless of what the initiator saw). */
        int need_rereplay = (wire_flags & RNET_RB_SYNC_FLAG_REREPLAY) ? 1 : 0;
        /* §50: mirror initiator — do not absorb tip raises through FMV. */
        (void)rb_in_fmv_defer_rewind_window();
        if (rb_in_fmv_lockstep_window()) {
            if (phase_at_entry == nRNetRbPhaseTipHold) {
                fprintf(stderr,
                        "psxrecomp: rb tip-extend FOLLOW REFUSED epoch=%u "
                        "%u→%u — FMV lockstep (tip-hold commit)\n",
                        (unsigned)epoch, (unsigned)old_t, (unsigned)target);
                fflush(stderr);
                finalize_tip_hold();
            } else {
                fprintf(stderr,
                        "psxrecomp: rb tip-extend FOLLOW REFUSED epoch=%u "
                        "%u→%u — FMV lockstep (keep target)\n",
                        (unsigned)epoch, (unsigned)old_t, (unsigned)target);
                fflush(stderr);
            }
            return;
        }
        if (phase_at_entry == nRNetRbPhaseTipHold)
            need_rereplay |= (sim_now > old_t) ? 1 : 0;
        else if (phase_at_entry == nRNetRbPhaseVerify ||
                 phase_at_entry == nRNetRbPhaseReplay) {
            /* §76/§69 raise-only when initiator did not request rereplay.
             * §80: honor wire REREPLAY (late-wire below tip / tip-hold flush). */
            if (!(wire_flags & RNET_RB_SYNC_FLAG_REREPLAY))
                need_rereplay = 0;
        }
        if (tip_raised && !rnet_rb_can_extend_target(g_rb, target)) {
            /* Initiator should have tip-hold-committed on span cap; if a stale
             * tip-extend SYNC still arrives, close TipHold so a later SYNC can
             * open a fresh episode. */
            if (phase_at_entry == nRNetRbPhaseTipHold) {
                fprintf(stderr,
                        "psxrecomp: rb tip-extend FOLLOW SPAN CAP epoch=%u "
                        "%u→%u — tip-hold commit\n",
                        (unsigned)epoch, (unsigned)old_t, (unsigned)target);
                fflush(stderr);
                tip_hold_try_finalize();
            }
            return;
        }
        clear_post_handshake();
        {
            int local = g_b.local_slot ? *g_b.local_slot : -1;
            if (tip_raised) {
                host_prefresh_seal_range(slot, old_t + 1u, target);
                if (local >= 0 && local != slot)
                    host_prefresh_seal_range(local, old_t + 1u, target);
            }
            host_prefresh_seal_range(slot, mismatch, mismatch);
        }
        if (!tip_raised || rnet_rb_extend_target(g_rb, target)) {
            if (tip_raised)
                (void)rnet_rb_resign_slot_range(g_rb, slot, old_t + 1u, target);
            (void)rnet_rb_resign_slot_range(g_rb, slot, mismatch, mismatch);
            export_local_seals();
            fprintf(stderr,
                    "psxrecomp: rb tip-extend FOLLOW epoch=%u %u→%u slot=%d "
                    "rereplay=%d\n",
                    (unsigned)epoch, (unsigned)old_t, (unsigned)target, slot,
                    need_rereplay);
            fflush(stderr);
            if (need_rereplay) {
                /* §66: TipHold FOLLOW mirrors initiator — defer snap while
                 * coalescing; flush on tip_hold_try_finalize.
                 * §80: Replay/Verify honor wire REREPLAY (mismatch =
                 * prefer_plus). TipHold flush SYNC also lands here when
                 * tip was already absorbed (same-target REREPLAY). */
                if (phase_at_entry == nRNetRbPhaseTipHold && tip_raised) {
                    if (!g_tip_hold_rereplay_pending) {
                        g_tip_hold_rereplay_pending = 1;
                        g_tip_hold_rereplay_from = old_t;
                        g_tip_extend_from_tick = old_t;
                    } else if (g_tip_hold_rereplay_from != 0u) {
                        g_tip_extend_from_tick = g_tip_hold_rereplay_from;
                    }
                    /* §78: mirror initiator's earliest divergent row. */
                    if (mismatch > 0u &&
                        (g_tip_hold_rereplay_mismatch == 0u ||
                         mismatch < g_tip_hold_rereplay_mismatch))
                        g_tip_hold_rereplay_mismatch = mismatch;
                    fprintf(stderr,
                            "psxrecomp: rb tip-extend FOLLOW rereplay DEFER "
                            "epoch=%u from=%u tip=%u slot=%d\n",
                            (unsigned)epoch,
                            (unsigned)g_tip_hold_rereplay_from,
                            (unsigned)target, slot);
                    fflush(stderr);
                } else if (phase_at_entry == nRNetRbPhaseTipHold && !tip_raised) {
                    /* §80: initiator tip-hold flush — mirror prefer_plus.
                     * §81: if we already flushed locally (both hit CAP), skip. */
                    if (g_tip_extend_rereplay &&
                        rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay) {
                        fprintf(stderr,
                                "psxrecomp: rb tip-extend FOLLOW flush-rereplay "
                                "SKIP epoch=%u prefer_plus=%u tip=%u "
                                "(already repairing)\n",
                                (unsigned)epoch, (unsigned)mismatch,
                                (unsigned)target);
                        fflush(stderr);
                    } else {
                        clear_tip_hold_rereplay_pending();
                        g_tip_extend_from_tick =
                            (mismatch > 0u) ? (mismatch - 1u) : old_t;
                        schedule_episode_rereplay(
                            mismatch > 0u ? mismatch : (old_t + 1u));
                        fprintf(stderr,
                                "psxrecomp: rb tip-extend FOLLOW flush-rereplay "
                                "epoch=%u prefer_plus=%u tip=%u slot=%d\n",
                                (unsigned)epoch, (unsigned)mismatch,
                                (unsigned)target, slot);
                        fflush(stderr);
                    }
                } else if (phase_at_entry == nRNetRbPhaseReplay ||
                           phase_at_entry == nRNetRbPhaseVerify) {
                    uint32_t prefer_plus =
                        (mismatch > 0u) ? mismatch : (old_t + 1u);
                    /* §81: same-target REREPLAY while already tip-extend
                     * repairing this tip — absorb+flush used to double-fire;
                     * both peers ABSOLUTE CAP also duplicate. */
                    if (!tip_raised && g_tip_extend_rereplay &&
                        phase_at_entry == nRNetRbPhaseReplay &&
                        rnet_rb_get_target_tick(g_rb) == target) {
                        fprintf(stderr,
                                "psxrecomp: rb tip-extend FOLLOW rereplay SKIP "
                                "epoch=%u prefer_plus=%u tip=%u "
                                "(already repairing)\n",
                                (unsigned)epoch, (unsigned)prefer_plus,
                                (unsigned)target);
                        fflush(stderr);
                    } else {
                        g_tip_extend_from_tick =
                            (prefer_plus > 0u) ? (prefer_plus - 1u) : old_t;
                        schedule_episode_rereplay(prefer_plus);
                        fprintf(stderr,
                                "psxrecomp: rb tip-extend FOLLOW rereplay "
                                "epoch=%u %u→%u prefer_plus=%u (phase=%d)\n",
                                (unsigned)epoch, (unsigned)old_t,
                                (unsigned)target, (unsigned)prefer_plus,
                                (int)phase_at_entry);
                        fflush(stderr);
                    }
                } else {
                    /* Reload at prior tip (old_t); mismatch field is episode-original. */
                    schedule_episode_rereplay(old_t + 1u);
                }
            }
            /* §76: Verify FOLLOW with raise-only must re-enter Replay for the
             * new span (same as initiator Verify→Replay). */
            if (!need_rereplay && phase_at_entry == nRNetRbPhaseVerify &&
                tip_raised) {
                uint32_t first = old_t + 1u;
                g_tip_extend_from_tick = old_t;
                g_pending_load_valid = 0;
                g_tip_extend_rereplay = 0;
                g_pending_resume_valid = 0;
                g_episode_snap_applied = 1;
                rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay);
                if (first <= target) {
                    if (fs)
                        rnet_session_set_sim_tick(fs, first);
                    if (rb_arm_rows_ready(first))
                        arm_replay_tick(first);
                    else
                        g_needs_advance = 0;
                }
                fprintf(stderr,
                        "psxrecomp: rb tip-extend FOLLOW Verify→Replay "
                        "epoch=%u %u→%u first=%u (no snap)\n",
                        (unsigned)epoch, (unsigned)old_t, (unsigned)target,
                        (unsigned)first);
                fflush(stderr);
            }
            if (phase_at_entry == nRNetRbPhaseTipHold || need_rereplay) {
                uint32_t runway = rnet_rb_get_tip_runway(g_rb);
                uint32_t cap = target + runway;
                if (cap > g_tip_hold_until)
                    g_tip_hold_until = cap;
                /* §24/§66: only immediate (non-TipHold) rereplay restarts quiet;
                 * TipHold defer/coalesce keeps the quiet/thrash deadline. */
                if (need_rereplay && phase_at_entry != nRNetRbPhaseTipHold)
                    g_tip_hold_quiet_t0_ms = 0ull;
                if (phase_at_entry == nRNetRbPhaseTipHold)
                    g_tip_hold_coalesce_n++;
                g_tip_hold_block_quiet = 0;
            }
        }
        return;
    }
    /* New epoch while TipHold: initiator span-cap committed and reopened —
     * yield tip-hold so we can follow (was silently dropping SYNC). */
    if (g_rb && rnet_rb_is_tip_holding(g_rb) &&
        epoch != rnet_rb_get_epoch_id(g_rb)) {
        fprintf(stderr,
                "psxrecomp: rb tip-hold yield FOLLOW new epoch=%u "
                "(was epoch=%u) — tip-hold commit\n",
                (unsigned)epoch, (unsigned)rnet_rb_get_epoch_id(g_rb));
        fflush(stderr);
        /* §67: peer already committed at the matched tip and moved on — do
         * NOT flush the deferred extension here (soak: flush kept the stale
         * epoch alive → tie-break WIN → POST diverge / ABANDON). Commit at
         * the matched from-tip; the new epoch repairs the extension span. */
        finalize_tip_hold();
    }
    /* Dual-initiation tie-break: both peers opened concurrent episodes and
     * each received the other's BEGIN while initiating. Deterministic winner:
     * lower initiator slot (derived from the partitioned epoch id). The loser
     * yields quietly — no cooldown, this is contention, not failure — and
     * falls through to follow the winner.
     * §68: yield through the whole Seal/AwaitingBaseline handshake, even
     * after local snap apply. Gating on !snap_applied left the loser
     * stashing the winner's BEGIN forever while both sides waited on
     * peer baseline (2026-08-02 soak: epoch 16 vs 17, 404× WIN drop).
     * Replay/Verify still refuse — abort/NACK owns those. */
    if (g_rb && rnet_rb_is_active(g_rb) && !rnet_rb_is_from_peer_notify(g_rb) &&
        epoch != rnet_rb_get_epoch_id(g_rb)) {
        uint32_t our_epoch = rnet_rb_get_epoch_id(g_rb);
        uint32_t our_slot = our_epoch & RB_EPOCH_SLOT_MASK;
        uint32_t their_slot = epoch & RB_EPOCH_SLOT_MASK;
        RNetRbPhase ph = rnet_rb_get_phase(g_rb);
        int handshake = (ph == nRNetRbPhaseSealInputs ||
                         ph == nRNetRbPhaseAwaitingBaseline);
        if (their_slot < our_slot && handshake) {
            fprintf(stderr,
                    "psxrecomp: rb tie-break YIELD our epoch=%u to peer "
                    "epoch=%u (slot %u beats %u%s)\n",
                    (unsigned)our_epoch, (unsigned)epoch,
                    (unsigned)their_slot, (unsigned)our_slot,
                    g_episode_snap_applied ? "; post-snap" : "");
            fflush(stderr);
            g_abort_wire_class = RNET_RB_ABORT_CLASS_REALIGN;
            abort_episode("tie-break yield (dual initiation)");
            clear_rewind_cooldown("tie-break yield");
            /* fall through: follow the winner's episode below */
        } else if (their_slot > our_slot) {
            static uint32_t s_win_log_our, s_win_log_peer, s_win_suppressed;
            if (s_win_log_our != our_epoch || s_win_log_peer != epoch) {
                if (s_win_suppressed) {
                    fprintf(stderr,
                            "psxrecomp: rb tie-break WIN "
                            "(suppressed %u repeats)\n",
                            (unsigned)s_win_suppressed);
                    s_win_suppressed = 0;
                }
                fprintf(stderr,
                        "psxrecomp: rb tie-break WIN our epoch=%u over peer "
                        "epoch=%u (slot %u beats %u) — drop\n",
                        (unsigned)our_epoch, (unsigned)epoch,
                        (unsigned)our_slot, (unsigned)their_slot);
                fflush(stderr);
                s_win_log_our = our_epoch;
                s_win_log_peer = epoch;
            } else {
                s_win_suppressed++;
            }
            return;
        }
    }
    if (!g_rb)
        return;
    /* §87/§101: never reopen an epoch we already committed (tip-hold /
     * ownership). NACK so the initiator aborts Seal instead of waiting 4s.
     * Soak: tie-break follow of zombie epoch=8 after tip-hold commit →
     * rb_baseline hang while peer already left → peer pcap_freeze. */
    if (!rnet_rb_is_active(g_rb) && epoch == g_last_commit_epoch) {
        char detail[96];
        snprintf(detail, sizeof(detail), "already committed (zombie epoch)");
        follow_refuse_nack(epoch, mismatch, load, target, slot, detail);
        return;
    }
    /* §87/§101: load strictly behind agreed tip rewinds into a dead timeline
     * (soak: follow load=1808 after commit tip=1852; session-136 load=3184
     * vs agreed tip=3200 → silent REFUSED ×100 + seal timeout). load==agreed
     * is the ownership-chain skip-snap continue and stays legal. */
    if (!rnet_rb_is_active(g_rb) && g_agreed_valid && load < g_agreed_through) {
        char detail[96];
        snprintf(detail, sizeof(detail),
                 "behind agreed tip=%u (zombie load)",
                 (unsigned)g_agreed_through);
        follow_refuse_nack(epoch, mismatch, load, target, slot, detail);
        return;
    }
    if (rnet_rb_is_active(g_rb)) {
        /* §60/§61: tip-extend same-epoch is handled above. Same-epoch BEGIN
         * rexmit must not be stashed (keepalive). Only stash a *new* epoch
         * that arrived while we are still Verify/Replay on the prior one. */
        if (epoch == rnet_rb_get_epoch_id(g_rb))
            return;
        stash_peer_begin(epoch, mismatch, load, target, slot, wire_flags);
        return;
    }
    (void)rb_in_fmv_defer_rewind_window();
    /* Same policy as begin (§109/§111): refuse tip episodes through
     * media/lockstep/sticky, but allow MEDIA_KF / post-FMV heal follows —
     * NACKing heal while sticky invent-held tip-starves both peers. */
    {
        int media_ep = (wire_flags & RNET_RB_SYNC_FLAG_MEDIA_KF) ||
                       rb_want_heal_kf(mismatch, load);
        if (rb_in_fmv_lockstep_window() &&
            !(rb_media_kf_enabled() && media_ep)) {
            fprintf(stderr,
                    "psxrecomp: rb follow REFUSED epoch=%u load=%u — FMV lockstep%s\n",
                    (unsigned)epoch, (unsigned)load,
                    g_fmv_unmatched_desync ? " [DESYNC hold]" : "");
            fflush(stderr);
            g_follow_nack_pending = 1;
            g_follow_nack_epoch = epoch;
            g_follow_nack_mismatch = mismatch;
            g_follow_nack_load = load;
            g_follow_nack_target = target;
            g_follow_nack_slot = slot;
            g_follow_nack_sends = 0;
            send_follow_nack(epoch, mismatch, load, target, slot, 1);
            g_follow_nack_sends = 1;
            return;
        }
    }
    /* §93 refuse / §97 MEDIA_KF follow into prior FMV bout. */
    {
        int media_ep = (wire_flags & RNET_RB_SYNC_FLAG_MEDIA_KF) ||
                       rb_want_heal_kf(mismatch, load);
        if (media_ep && !rb_media_kf_enabled() &&
            !(wire_flags & RNET_RB_SYNC_FLAG_MEDIA_KF)) {
            fprintf(stderr,
                    "psxrecomp: rb follow REFUSED epoch=%u load=%u mismatch=%u — "
                    "FMV media-range (lo=%u hi=%u settle_until=%u)\n",
                    (unsigned)epoch, (unsigned)load, (unsigned)mismatch,
                    (unsigned)g_fmv_media_lo, (unsigned)g_fmv_media_hi,
                    (unsigned)g_fmv_settle_until);
            fflush(stderr);
            g_follow_nack_pending = 1;
            g_follow_nack_epoch = epoch;
            g_follow_nack_mismatch = mismatch;
            g_follow_nack_load = load;
            g_follow_nack_target = target;
            g_follow_nack_slot = slot;
            g_follow_nack_sends = 0;
            send_follow_nack(epoch, mismatch, load, target, slot, 1);
            g_follow_nack_sends = 1;
            return;
        }
        if (media_ep && rb_media_kf_enabled()) {
            /* Follower may lack the snap — host will transfer the keyframe. */
            ;
        } else if (media_ep) {
            fprintf(stderr,
                    "psxrecomp: rb follow REFUSED epoch=%u — MEDIA-KF disabled "
                    "but peer requested media episode\n",
                    (unsigned)epoch);
            fflush(stderr);
            g_follow_nack_pending = 1;
            g_follow_nack_epoch = epoch;
            g_follow_nack_mismatch = mismatch;
            g_follow_nack_load = load;
            g_follow_nack_target = target;
            g_follow_nack_slot = slot;
            g_follow_nack_sends = 0;
            send_follow_nack(epoch, mismatch, load, target, slot, 1);
            g_follow_nack_sends = 1;
            return;
        }
    }
    snap_n = g_snaps ? netplay_snap_ring_count(g_snaps) : 0u;
    /* §60: ownership-chain continue uses skip-snap at the verified tip — the
     * initiator does not require a ring snap there; neither may the follower,
     * or we NACK a legitimate chain BEGIN and storm.
     * §97 MEDIA_KF: missing snap is OK — host transfers the keyframe. */
    if (!g_snaps || !netplay_snap_ring_has(g_snaps, load)) {
        int skip_ok = (g_ownership_skip_snap && g_agreed_valid &&
                       load == g_agreed_through);
        int media_kf_ok = rb_media_kf_enabled() &&
                          ((wire_flags & RNET_RB_SYNC_FLAG_MEDIA_KF) ||
                           rb_want_heal_kf(mismatch, load));
        if (!skip_ok && !media_kf_ok) {
            fprintf(stderr,
                    "psxrecomp: rb follow REFUSED epoch=%u load=%u — snap missing "
                    "(ring count=%u newest=%u)\n",
                    (unsigned)epoch, (unsigned)load, (unsigned)snap_n,
                    (unsigned)(g_snaps ? netplay_snap_ring_newest_tick(g_snaps)
                                       : 0u));
            fflush(stderr);
            g_follow_nack_pending = 1;
            g_follow_nack_epoch = epoch;
            g_follow_nack_mismatch = mismatch;
            g_follow_nack_load = load;
            g_follow_nack_target = target;
            g_follow_nack_slot = slot;
            g_follow_nack_sends = 0;
            send_follow_nack(epoch, mismatch, load, target, slot, 1);
            g_follow_nack_sends = 1;
            return;
        }
        fprintf(stderr,
                "psxrecomp: rb follow skip-snap load=%u (ownership chain tip)\n",
                (unsigned)load);
        fflush(stderr);
    }
    /* NACK loads past our shared frontier — initiator tip-slack beyond
     * hash_confirm is a doomed baseline (cpu-only tip snap fork). */
    if (g_b.hc)
        (void)netplay_hc_heal_stale_gap(g_b.hc);
    heal_agreed_watermark_if_aged_out();
    /* The initiator advances its agreed along hc-confirmed live play; do the
     * same here or its (valid) frontier loads get REFUSED as "unconfirmed". */
    advance_agreed_watermark_from_hc();
    {
        uint32_t frontier = 0u;
        int have_f = 0;
        uint32_t oldest = g_snaps ? netplay_snap_ring_oldest_tick(g_snaps) : 0u;
        if (g_agreed_valid) {
            frontier = g_agreed_through;
            have_f = 1;
        } else if (g_b.hc) {
            uint32_t rt = netplay_hc_resolved_through(g_b.hc);
            if (netplay_hc_confirm_through(g_b.hc, rt)) {
                frontier = rt;
                have_f = 1;
            }
        }
        /* hc/agreed below snap ring: do not NACK tip-slack loads (BOOTSTRAP
         * should have raised agreed; mirror choose_load fall-through). */
        if (have_f && g_snaps && frontier < oldest)
            have_f = 0;
        /* Shared frontier floor: the peer only advertises RESOLVED for ticks
         * it committed (tip-hold/commit), so a load at-or-below the library's
         * peer-advertised watermark is provably followable even when our own
         * agreed/hc frontier lags (lost RESOLVED was a NACK-cycle seed). */
        if (have_f && frontier < rnet_rb_resolved_through(g_rb))
            frontier = rnet_rb_resolved_through(g_rb);
        /* Mirror choose_load hard cap: once tip-hold/commit set agreed_through,
         * refuse loads above it even if local hash_confirm matched TipHold Live
         * (heal raises agreed when that tip aged out of the ring).
         * §99 MEDIA_KF: invent-through-media leaves HC/agreed lagging the tip
         * while choose_load raises to a near-tip media snap — same trust as
         * missing-snap OK (host KF probe/transfer). Do not NACK. */
        if (have_f && load > frontier) {
            int media_kf_ok = rb_media_kf_enabled() &&
                              ((wire_flags & RNET_RB_SYNC_FLAG_MEDIA_KF) ||
                               rb_fmv_tick_unsafe_for_episode(load) ||
                               rb_fmv_tick_unsafe_for_episode(mismatch));
            if (media_kf_ok) {
                fprintf(stderr,
                        "psxrecomp: rb follow MEDIA-KF allow load=%u past "
                        "frontier=%u (keyframe reconciles)\n",
                        (unsigned)load, (unsigned)frontier);
                fflush(stderr);
            } else {
                fprintf(stderr,
                        "psxrecomp: rb follow REFUSED epoch=%u load=%u — past "
                        "frontier=%u (unconfirmed tip)\n",
                        (unsigned)epoch, (unsigned)load, (unsigned)frontier);
                fflush(stderr);
                g_follow_nack_pending = 1;
                g_follow_nack_epoch = epoch;
                g_follow_nack_mismatch = mismatch;
                g_follow_nack_load = load;
                g_follow_nack_target = target;
                g_follow_nack_slot = slot;
                g_follow_nack_sends = 0;
                send_follow_nack(epoch, mismatch, load, target, slot, 1);
                g_follow_nack_sends = 1;
                return;
            }
        }
    }
    memset(&corr, 0, sizeof(corr));
    corr.epoch_id = epoch;
    corr.mismatch_tick = mismatch;
    corr.load_tick = load;
    corr.target_tick = target;
    corr.slot = slot;
    corr.initiator = 0u;
    corr.from_peer_notify = 1u;
    /* Light-tip is initiator-authoritative (wire flag) — never re-derived
     * from this peer's watermarks, so both sides classify identically. */
    if (wire_flags & RNET_RB_SYNC_FLAG_LIGHT_TIP)
        corr.flags = RNET_RB_CORR_LIGHT_TIP;
    /* Keep our epoch counter ahead of any seen counter (ids are
     * slot-partitioned; this only keeps counters monotonic for logging). */
    if ((epoch >> RB_EPOCH_SLOT_BITS) > g_epoch)
        g_epoch = epoch >> RB_EPOCH_SLOT_BITS;
    rnet_rb_begin_episode(g_rb, &corr);
    psx_netplay_timesync_on_episode_boundary();
    clear_episode_wire_state();
    if (rb_media_kf_enabled() &&
        ((wire_flags & RNET_RB_SYNC_FLAG_MEDIA_KF) ||
         rb_want_heal_kf(mismatch, load))) {
        g_media_kf_episode = 1;
        g_media_kf_ready = 0;
        g_media_kf_host_armed = 0;
        g_media_kf_sending = 0;
        /* §109/§114: heal when initiator MEDIA_KF + apply-only (target=load)
         * or legacy short verify span. Follower arms heal → §114 on success. */
        g_post_fmv_heal_kf =
            ((wire_flags & RNET_RB_SYNC_FLAG_MEDIA_KF) &&
             target >= load &&
             target <= load + RB_FMV_HEAL_VERIFY_SPAN &&
             mismatch > load && rb_post_fmv_heal_eligible())
                ? 1
                : 0;
        if (g_post_fmv_heal_kf)
            g_post_fmv_heal_fork_tick = mismatch;
    } else {
        g_post_fmv_heal_kf = 0;
    }
    g_episode_snap_applied = 0;
    g_pending_resume_valid = 0;
    g_empty_span_verify_pending = 0;
    g_needs_advance = 0;
    g_seal_export_logged = 0;
    g_follow_nack_pending = 0;
    /* §85: follower freezes at the initiator's wire target. */
    g_replay_catchup_cap = target;
    /* Consumed — don't re-apply a stashed copy of this BEGIN next pump. */
    if (g_stash_begin_valid && g_stash_begin_epoch == epoch)
        g_stash_begin_valid = 0;
    g_stash_begin_logged = 0;
    rnet_rb_seal_inputs(g_rb, load, target, slot);
    g_seal_wait_ms = rb_mono_ms();
    export_local_seals();
    fprintf(stderr,
            "psxrecomp: rb follow epoch=%u mismatch=%u load=%u target=%u "
            "media_kf=%d (snaps=%u local_slot=%d)\n",
            (unsigned)epoch, (unsigned)mismatch, (unsigned)load, (unsigned)target,
            g_media_kf_episode, (unsigned)snap_n,
            g_b.local_slot ? *g_b.local_slot : -1);
    fflush(stderr);
    /* Initiator's export_local_seals() burst can have beaten this follower's
     * own delayed SYNC through the relay — drain any stash for this epoch
     * before checking completeness. */
    apply_stashed_seal_rows();
    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs) {
        if (rnet_rb_all_peer_seal_rows_complete(g_rb))
            enter_awaiting_baseline();
        else
            apply_stashed_baseline();
    }
}

/* §114/§115: initiator re-opens apply-only MEDIA_KF while platform nondet is
 * armed and cores still disagree. Prefer host *current* tip when HC is stuck
 * on the known tip+1 fork so the pin advances; cap same-keep streams. */
static void rb_post_fmv_platform_kf_stream_poll(void)
{
    RNetSession *s = sess();
    uint32_t sim;
    uint32_t fork = 0u;
    uint32_t stream_mm;
    int local;
    int remote;
    int n;
    int stale_platform_fork;

    if (!g_post_fmv_platform_nondet && !g_fmv_unmatched_desync)
        return;
    if (!g_rb || !s || !rb_media_kf_enabled())
        return;
    if (rnet_rb_is_active(g_rb))
        return;
    if (rnet_session_state_busy(s))
        return;
    local = g_b.local_slot ? *g_b.local_slot : 0;
    if (local != 0)
        return; /* initiator-only */
    sim = rnet_session_sim_tick(s);
    if (g_platform_kf_next_sim != 0u && sim < g_platform_kf_next_sim)
        return;
    if (g_b.hc && !netplay_hc_peek_mismatch(g_b.hc, &fork, NULL, NULL)) {
        /* Digests agree for now — wait for CONFIRM streak to clear hold. */
        g_platform_kf_next_sim = sim + RB_FMV_PLATFORM_KF_IV;
        return;
    }
    if (fork == 0u)
        fork = (sim > 0u) ? sim : 1u;
    if (g_platform_accepted_fork != 0u && fork == g_platform_accepted_fork) {
        g_platform_kf_next_sim = sim + RB_FMV_PLATFORM_KF_IV;
        return;
    }

    stale_platform_fork =
        (g_post_fmv_heal_fork_tick != 0u && fork == g_post_fmv_heal_fork_tick) ||
        (g_post_fmv_platform_keep_tick != 0u &&
         fork == g_post_fmv_platform_keep_tick + 1u);

    /* §115: HC stays stuck at tip+1 forever (resolved=keep). Streaming that
     * fork only re-applies keep. Ride host sim so choose_load pins near tip. */
    stream_mm = fork;
    if (stale_platform_fork && sim > fork) {
        stream_mm = sim;
        fprintf(stderr,
                "psxrecomp: rb §115 KF stream ride host tip fork=%u→%u "
                "sim=%u (stale tip+1)\n",
                (unsigned)fork, (unsigned)stream_mm, (unsigned)sim);
        fflush(stderr);
    }

    if (stale_platform_fork) {
        g_platform_kf_stream_count++;
        if (g_platform_kf_stream_count > RB_FMV_PLATFORM_KF_STREAM_LIMIT) {
            uint32_t through = g_post_fmv_platform_keep_tick;
            if (sim > through)
                through = (sim > 0u) ? (sim - 1u) : fork;
            fprintf(stderr,
                    "psxrecomp: rb §115 KF stream CAP fork=%u count=%u "
                    "sim=%u — accept platform tip+1\n",
                    (unsigned)fork, (unsigned)g_platform_kf_stream_count,
                    (unsigned)sim);
            fflush(stderr);
            rb_post_fmv_platform_accept(through, fork,
                                       "KF stream limit at stale tip+1");
            return;
        }
    } else {
        g_platform_kf_stream_count = 0u;
    }

    n = g_b.slot_count ? *g_b.slot_count : 0;
    remote = (local == 0) ? 1 : 0;
    if (n < 2)
        remote = local;
    g_request_post_fmv_heal = 1;
    g_post_fmv_heal_loop_count = 0u;
    g_platform_kf_next_sim = sim + RB_FMV_PLATFORM_KF_IV;
    fprintf(stderr,
            "psxrecomp: rb §114 KF stream begin fork=%u stream_mm=%u sim=%u "
            "(platform nondet; stream=%u)\n",
            (unsigned)fork, (unsigned)stream_mm, (unsigned)sim,
            (unsigned)g_platform_kf_stream_count);
    fflush(stderr);
    (void)psx_netplay_rb_begin_rewind(stream_mm, remote);
}

void psx_netplay_rb_pump(void)
{
    RNetSession *s = sess();
    rnet_u32 epoch, mismatch, load, target, row_begin;
    rnet_u8 cslot, op, wire_flags, slot;
    rnet_u32 dig_m, dig_a, dig_b, dig_c, in_dig;
    rnet_u8 match;
    RNetRbFrame rows[24];
    rnet_u16 row_count;

    if (!g_rb || !s)
        return;

    /* §97: host probe/transfer media keyframe while awaiting baseline. */
    rb_media_kf_host_drive();
    /* §114: stream apply-only KF while platform nondet invent-hold is armed. */
    rb_post_fmv_platform_kf_stream_poll();

    /* §40: drain RESOLVED before SYNC so follow BEGIN in the same poll
     * sees an updated frontier (max(agreed, resolved_through)). */
    {
        rnet_u32 resolved = 0;
        while (rnet_session_take_rb_resolved(s, &resolved))
            apply_peer_resolved(resolved);
    }
    /* §53b: Live cadence — only HC-confirmed ADVANCE. Aged HEAL / HEAL-FORCE
     * stay at begin/follow: running them every pump under a live HC stall
     * invented unconfirmed agreed tips (752→816→…→1008 with hc stuck) and
     * baseline-forked the session. */ 
    if (g_b.hc)
        (void)netplay_hc_heal_stale_gap(g_b.hc);
    advance_agreed_watermark_from_hc();
    /* §41: retransmit confirmed RESOLVED while peer lagging. */
    maybe_heartbeat_agreed_resolved();

    while (rnet_session_take_rb_sync(s, &epoch, &mismatch, &load, &target, &cslot, &op,
                                     &wire_flags)) {
        if (op == RNET_RB_SYNC_OP_COMMIT) {
            /* §42c: peer committed epoch at tick (load field) and left it.
             * Record for the tip-extend abandon; also fold into the RESOLVED
             * watermark (a commit is by definition resolved through tick). */
            if (load > 0u &&
                (g_peer_commit_epoch != epoch || load > g_peer_commit_tick)) {
                g_peer_commit_epoch = epoch;
                g_peer_commit_tick = load;
                apply_peer_resolved(load);
                (void)maybe_abandon_tip_extend();
            }
            continue;
        }
        if (op == RNET_RB_SYNC_OP_ABORT) {
            /* Peer tore its episode down. Mirror teardown + the shared
             * cooldown class (mismatch field) so both peers re-arm on the
             * same schedule instead of drifting to skewed local timeouts. */
            uint32_t cls = mismatch;
            if (epoch == g_peer_abort_last_epoch)
                continue; /* rexmit burst dedupe */
            g_peer_abort_last_epoch = epoch;
            if (rnet_rb_is_active(g_rb) && epoch == rnet_rb_get_epoch_id(g_rb)) {
                char why[96];
                uint32_t live_sim = rnet_session_sim_tick(s);
                uint32_t tick = 0;
                int have;
                int snap_was_applied = g_episode_snap_applied;
                snprintf(why, sizeof(why), "peer abort epoch=%u class=%u",
                         (unsigned)epoch, (unsigned)cls);
                have = pick_realign_tip(&tick);
                g_abort_from_peer = 1;
                abort_episode(why);
                g_abort_from_peer = 0;
                /* §42 P1: prefer the sender's wire realign tick (load field)
                 * so both peers rewind to the SAME tick; fall back to the
                 * local pick only when the wire carried none / no snap. */
                if (!honor_peer_abort_realign(load, snap_was_applied, why) &&
                    snap_was_applied && have)
                    schedule_live_realign(tick, why);
                /* Shared cooldown class → same re-arm schedule as the peer. */
                if (cls == RNET_RB_ABORT_CLASS_REALIGN) {
                    g_bl_mismatch_streak = 0;
                } else {
                    g_bl_mismatch_streak++;
                    if (cls == RNET_RB_ABORT_CLASS_STORM &&
                        g_bl_mismatch_streak < 2u)
                        g_bl_mismatch_streak = 2u;
                }
                {
                    uint32_t cool_n = abort_class_cooldown_ticks(cls);
                    if (cool_n == 0u)
                        clear_rewind_cooldown(why);
                    else
                        arm_rewind_cooldown_ticks(live_sim, cool_n, why);
                }
            } else {
                /* Idle / wrong-epoch ABORT: still honor wire realign when
                 * Live is ahead (§84 NACK of a refused epoch we never joined;
                 * §42 P1 post-commit tip rollback). Hard cooldown classes
                 * still apply so we don't re-initiate into a storm. */
                char why[96];
                uint32_t live_sim = rnet_session_sim_tick(s);
                int honored = 0;
                snprintf(why, sizeof(why),
                         "peer abort (idle) epoch=%u class=%u",
                         (unsigned)epoch, (unsigned)cls);
                if (load > 0u)
                    honored = honor_peer_abort_realign(load, 0, why);
                if (honored) {
                    if (cls == RNET_RB_ABORT_CLASS_REALIGN)
                        g_bl_mismatch_streak = 0;
                    {
                        uint32_t cool_n = abort_class_cooldown_ticks(cls);
                        if (cool_n == 0u)
                            clear_rewind_cooldown(why);
                        else
                            arm_rewind_cooldown_ticks(live_sim, cool_n, why);
                    }
                } else if (cls == RNET_RB_ABORT_CLASS_STORM ||
                           cls == RNET_RB_ABORT_CLASS_NO_SNAP) {
                    arm_rewind_cooldown_ticks(live_sim,
                                              abort_class_cooldown_ticks(cls),
                                              why);
                }
            }
            continue;
        }
        if (op == RNET_RB_SYNC_OP_NACK) {
            /* MotK: op=NACK is follow-NACK (missing load snap / past frontier /
             * zombie load), not an echo. target field carries the follower's
             * frontier. */
            if (rnet_rb_is_active(g_rb) && epoch == rnet_rb_get_epoch_id(g_rb) &&
                !rnet_rb_is_from_peer_notify(g_rb)) {
                char why[96];
                RNetSession *ns = sess();
                uint32_t live_sim = ns ? rnet_session_sim_tick(ns) : 0u;
                uint32_t peer_frontier = target;
                uint32_t demote = (load > 0u) ? load - 1u : 0u;
                /* Capture before abort_episode clears it — decides whether Live
                 * must rewind (snap already applied) or can stay put. */
                int snap_was_applied = g_episode_snap_applied;
                uint32_t realign_to = 0u;
                int do_live_realign = 0;
                uint32_t cls;
                uint32_t cool_n;

                /* §101/§102: peer frontier ahead of refused load = zombie load
                 * (or §62 snap eviction). Raise peer_resolved + agreed, abort
                 * Seal immediately, clear hc-fork, and reopen one short light
                 * tip at the follower tip (no ownership SPAN cascade). */
                if (peer_frontier >= load && load > 0u) {
                    int local_slot = g_b.local_slot ? *g_b.local_slot : 0;
                    int remote_slot = (local_slot == 0) ? 1 : 0;
                    uint32_t light_mm;
                    if (g_b.slot_count && *g_b.slot_count < 2)
                        remote_slot = local_slot;
                    if (load > g_peer_nack_floor) {
                        g_peer_nack_floor = load;
                        fprintf(stderr,
                                "psxrecomp: rb peer NACK floor=%u "
                                "(peer frontier=%u ahead — zombie/evicted)\n",
                                (unsigned)g_peer_nack_floor,
                                (unsigned)peer_frontier);
                        fflush(stderr);
                    }
                    apply_peer_resolved(peer_frontier);
                    if (!g_agreed_valid || g_agreed_through < peer_frontier) {
                        g_agreed_through = peer_frontier;
                        g_agreed_span_lo = peer_frontier;
                        g_agreed_valid = 1;
                    }
                    advertise_agreed_resolved(peer_frontier);
                    snprintf(why, sizeof(why),
                             "peer follow NACK load=%u (peer tip=%u ahead)",
                             (unsigned)load, (unsigned)peer_frontier);
                    g_abort_wire_class = RNET_RB_ABORT_CLASS_ABORT;
                    g_abort_wire_realign_tick = 0u;
                    abort_episode(why);
                    if (snap_was_applied) {
                        uint32_t t = peer_frontier;
                        realign_to = 0u;
                        if (g_snaps) {
                            for (;;) {
                                if (netplay_snap_ring_has(g_snaps, t)) {
                                    realign_to = t;
                                    break;
                                }
                                if (t == 0u)
                                    break;
                                --t;
                            }
                        }
                        if (realign_to > 0u) {
                            fprintf(stderr,
                                    "psxrecomp: rb NACK peer-ahead realign "
                                    "load=%u → %u (frontier=%u)\n",
                                    (unsigned)load, (unsigned)realign_to,
                                    (unsigned)peer_frontier);
                            fflush(stderr);
                            schedule_live_realign(realign_to, why);
                        }
                    } else {
                        fprintf(stderr,
                                "psxrecomp: rb NACK keep-live peer-ahead "
                                "load=%u frontier=%u live=%u (no rewind)\n",
                                (unsigned)load, (unsigned)peer_frontier,
                                (unsigned)live_sim);
                        fflush(stderr);
                    }
                    clear_rewind_cooldown(why);
                    psx_netplay_hc_fork_recovery_clear();
                    /* Immediate short light tip at peer tip — not hc-fork SPAN. */
                    g_peer_ahead_force_load = peer_frontier;
                    light_mm = peer_frontier + 1u;
                    if (live_sim > light_mm)
                        light_mm = live_sim;
                    if (light_mm <= peer_frontier)
                        light_mm = peer_frontier + 1u;
                    fprintf(stderr,
                            "psxrecomp: rb peer-ahead light reopen "
                            "mismatch=%u load=%u depth≤%u\n",
                            (unsigned)light_mm, (unsigned)peer_frontier,
                            (unsigned)RB_PEER_AHEAD_LIGHT_DEPTH);
                    fflush(stderr);
                    if (!psx_netplay_rb_begin_rewind(light_mm, remote_slot)) {
                        g_peer_ahead_force_load = 0u;
                        g_peer_ahead_light_episode = 0;
                        fprintf(stderr,
                                "psxrecomp: rb peer-ahead light reopen "
                                "deferred (begin refused)\n");
                        fflush(stderr);
                    }
                    continue;
                }

                /* Shared frontier: demote to the follower's advertised
                 * frontier when it is deeper than load-1 — a tick the
                 * follower can actually prove, so the reopened episode's
                 * load is followable instead of NACK-cycling. */
                if (peer_frontier > 0u && peer_frontier < demote)
                    demote = peer_frontier;
                snprintf(why, sizeof(why), "peer follow NACK load=%u", (unsigned)load);
                /* Peer refused load as past its frontier — usually because we
                 * tip-held / advanced agreed_through without their POST ack
                 * (unilateral tip). Plain abort_episode left agreed_through
                 * at the refused tip, so the next begin reopened load=same
                 * tip → NACK → abort storm (epochs 7..11 in 2026-08-01 soak).
                 *
                 * 2026-08-01 cliff soak: always realigning to demote was wrong
                 * when the refused load snap was never applied (still in
                 * SealInputs) — that rewound ~25 ticks of good matched Live
                 * on only this peer, forked cores at demote+1, and armed
                 * cooldown so late wire became promote-no-resim poison. Only
                 * realign when we actually left the pre-episode tip. Always
                 * demote watermarks + prime HC so live hash_confirm cannot
                 * immediately re-ADVANCE past the refused tip (soak: NACK
                 * demote 3840 then `agreed ADVANCE 3840→3856` on the next
                 * pump from stale HC).
                 *
                 * §103 (session 142): deep demote Live 827→763 after a light
                 * tip NACK (snap never applied) forked cores @764 → forced
                 * MEDIA-KF ~3.7MB + ownership FMV refuse. When demote depth
                 * exceeds RB_NACK_DEEP_REALIGN_MAX and the snap was never
                 * applied: keep-live, soft-demote agreed to load-1 only,
                 * sticky-clamp peer_resolved to the follower frontier. */
                if (g_snaps && demote > 0u) {
                    uint32_t t;
                    for (t = demote;; --t) {
                        if (netplay_snap_ring_has(g_snaps, t)) {
                            demote = t;
                            break;
                        }
                        if (t == 0u)
                            break;
                    }
                }
                {
                    int deep_keep_live =
                        !snap_was_applied && live_sim > load && demote > 0u &&
                        live_sim > demote &&
                        (live_sim - demote) > RB_NACK_DEEP_REALIGN_MAX;
                    if (deep_keep_live) {
                        uint32_t soft =
                            (load > 0u) ? (load - 1u) : 0u;
                        if (g_snaps && soft > 0u) {
                            uint32_t t;
                            for (t = soft;; --t) {
                                if (netplay_snap_ring_has(g_snaps, t)) {
                                    soft = t;
                                    break;
                                }
                                if (t == 0u)
                                    break;
                            }
                        }
                        if (g_agreed_valid && g_agreed_through >= load) {
                            g_agreed_through = soft;
                            g_agreed_span_lo = soft;
                        }
                        /* Clamp peer tip to what the follower proved — do not
                         * drag local HC/agreed down to a stale frontier. */
                        if (peer_frontier > 0u) {
                            if (g_peer_resolved_through == 0u ||
                                g_peer_resolved_through > peer_frontier)
                                g_peer_resolved_through = peer_frontier;
                            g_peer_resolved_gate_expired = 0;
                            g_peer_resolved_wait_t0_ms = 0ull;
                            g_past_frontier_sticky = 1;
                            rnet_rb_demote_resolved_through(g_rb,
                                                            peer_frontier);
                        }
                        if (g_pin_valid && g_pin_tick >= load)
                            clear_baseline_pin();
                        g_episode_baseline_matched = 0;
                        g_abort_wire_class = RNET_RB_ABORT_CLASS_ABORT;
                        g_abort_wire_realign_tick = 0u;
                        abort_episode(why);
                        fprintf(stderr,
                                "psxrecomp: rb NACK keep-live deep "
                                "frontier=%u soft_agreed=%u live=%u load=%u "
                                "(no demote Live; sticky peer gate)\n",
                                (unsigned)peer_frontier, (unsigned)soft,
                                (unsigned)live_sim, (unsigned)load);
                        fflush(stderr);
                        clear_rewind_cooldown(why);
                        continue;
                    }
                }
                if (g_agreed_valid && g_agreed_through >= load) {
                    g_agreed_through = demote;
                    g_agreed_span_lo = demote;
                }
                rnet_rb_demote_resolved_through(g_rb, demote);
                if (g_b.hc)
                    netplay_hc_prime_after(g_b.hc, demote);
                if (g_pin_valid && g_pin_tick >= load)
                    clear_baseline_pin();
                g_episode_baseline_matched = 0;
                if (g_snaps && demote > 0u && netplay_snap_ring_has(g_snaps, demote)) {
                    /* Wire tip for the FOLLOW that never joined (§84): even
                     * keep-live here must advertise demote so a peer inventing
                     * past the frontier can honor_peer_abort_realign. */
                    realign_to = demote;
                    if (snap_was_applied || live_sim > load) {
                        /* §62: never realign below a remote-input wire hole the
                         * tip-window retransmit no longer covers — Live would
                         * stall on WIRE_HOLE forever. Raise the realign target
                         * to the first snap past the hole (watermarks stay
                         * demoted; only the Live rewind depth is capped).
                         * §71: also realign when the snap was NEVER applied but
                         * Live already walked past the refused load — keep-live
                         * + cooldown promote-no-resim forks START/menu presses
                         * (soak: NACK load=880 live=887 → cores diverge @896).
                         * §103: deep cases handled above (keep-live). */
                        uint32_t hole_end =
                            remote_wire_hole_end_after(demote, live_sim);
                        if (hole_end > 0u) {
                            uint32_t t;
                            uint32_t newest =
                                netplay_snap_ring_newest_tick(g_snaps);
                            realign_to = 0u;
                            for (t = hole_end; t <= newest; ++t) {
                                if (netplay_snap_ring_has(g_snaps, t)) {
                                    realign_to = t;
                                    break;
                                }
                            }
                            if (realign_to == 0u)
                                realign_to = newest;
                            fprintf(stderr,
                                    "psxrecomp: rb NACK hole-guard realign_to=%u "
                                    "(demote=%u hole_end=%u)\n",
                                    (unsigned)realign_to, (unsigned)demote,
                                    (unsigned)hole_end);
                            fflush(stderr);
                        }
                        do_live_realign = (realign_to > 0u);
                    }
                }
                /* §84: stage wire class/realign BEFORE abort_episode so OP_ABORT
                 * carries the shared tip. Prior path sent realign=0 → FOLLOW
                 * honor no-op → permanent invent fork after post-FMV NACK. */
                cls = (realign_to > 0u) ? RNET_RB_ABORT_CLASS_REALIGN
                                        : RNET_RB_ABORT_CLASS_ABORT;
                cool_n = abort_class_cooldown_ticks(cls);
                g_abort_wire_class = cls;
                g_abort_wire_realign_tick = realign_to;
                abort_episode(why);
                if (do_live_realign) {
                    fprintf(stderr,
                            "psxrecomp: rb NACK realign (%s) "
                            "demote=%u realign=%u live_was=%u load=%u\n",
                            snap_was_applied ? "snap was applied"
                                             : "live past refused load",
                            (unsigned)demote, (unsigned)realign_to,
                            (unsigned)live_sim, (unsigned)load);
                    fflush(stderr);
                    schedule_live_realign(realign_to, why);
                } else {
                    fprintf(stderr,
                            "psxrecomp: rb NACK keep-live demote=%u "
                            "realign_wire=%u live=%u (snap never applied — "
                            "no local rewind)\n",
                            (unsigned)demote, (unsigned)realign_to,
                            (unsigned)live_sim);
                    fflush(stderr);
                }
                if (cool_n == 0u)
                    clear_rewind_cooldown(why);
                else
                    arm_rewind_cooldown_ticks(live_sim, cool_n, why);
            }
            continue;
        }
        begin_follower(epoch, mismatch, load, target, (int)cslot, wire_flags);
    }

    while (rnet_session_take_rb_seal_rows(s, &epoch, &mismatch, &target, &slot, &row_begin, rows,
                                         &row_count)) {
        /* Same TURN/UDP reordering race as BASELINE: the initiator's
         * export_local_seals() burst can beat a delayed SYNC BEGIN through
         * the relay. Stash instead of dropping so the follower doesn't have
         * to wait out a full chunk retransmit before completing seal rows
         * (2026-08-01 gap review). */
        if (!rnet_rb_is_active(g_rb) || epoch != rnet_rb_get_epoch_id(g_rb)) {
            stash_seal_rows_chunk(epoch, mismatch, target, slot, row_begin, rows, row_count);
            continue;
        }
        (void)rnet_rb_apply_peer_seal_rows(g_rb, epoch, mismatch, target, (int32_t)slot,
                                           row_begin, rows, row_count);
        if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs &&
            rnet_rb_all_peer_seal_rows_complete(g_rb))
            enter_awaiting_baseline();
    }

    while (rnet_session_take_rb_baseline(s, &epoch, &load, &dig_m, &dig_a, &dig_b, &dig_c)) {
        stash_or_accept_baseline(epoch, load, dig_m, dig_a, dig_b, dig_c);
    }

    while (rnet_session_take_rb_post(s, &epoch, &target, &dig_m, &in_dig, &match)) {
        uint32_t tip;
        if (!rnet_rb_is_active(g_rb) || epoch != rnet_rb_get_epoch_id(g_rb))
            continue;
        /* TipHold cleared the POST handshake — ignore late/rexmit POSTs from
         * the prior Verify so tip-extend cannot compare against a stale peer
         * digest. Replay/Seal/Baseline also must not latch peer_ok early. */
        if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseVerify)
            continue;
        tip = rnet_rb_get_target_tick(g_rb);
        /* Tip-extend race: peer POST@old tip arrives during Verify@new tip.
         * Wire carries target_tick — drop mismatches (was false post diverge). */
        if (!netplay_rb_peer_post_tip_ok(target, tip)) {
            static uint32_t s_stale_log_tip;
            if (s_stale_log_tip != tip) {
                fprintf(stderr,
                        "psxrecomp: rb post ignore stale tip=%u (episode tip=%u "
                        "dig_m=%08x)\n",
                        (unsigned)target, (unsigned)tip, (unsigned)dig_m);
                fflush(stderr);
                s_stale_log_tip = tip;
            }
            continue;
        }
        g_peer_post_ok = 1;
        g_peer_post_digest = dig_m;
        g_peer_post_av = in_dig;
        g_peer_post_match = match;
        if (g_local_post_sent) {
            /* RTT sample: elapsed time since *we* sent our POST, now that the
             * peer's POST for this same episode has arrived. Both sides enter
             * Verify around the same real-world moment (triggered by the same
             * mismatch/rewind-request), so this is dominated by one-way
             * transit of the peer's packet, not local compute — a genuine
             * link-latency measurement. Sanity-bounded: a stale/backdated
             * g_verify_wait_ms (e.g. tip-hold reused an old episode's timer)
             * would otherwise poison the EMA with a bogus multi-second value. */
            if (g_verify_wait_ms != 0ull) {
                uint64_t now = rb_mono_ms();
                if (now >= g_verify_wait_ms) {
                    uint64_t sample = now - g_verify_wait_ms;
                    if (sample <= 2000ull) {
                        g_rb_rtt_ema_ms = g_rb_rtt_ema_ms
                                              ? (uint32_t)((3ull * g_rb_rtt_ema_ms + sample) / 4ull)
                                              : (uint32_t)sample;
                    }
                }
            }
            if (dig_m == g_post_digest && in_dig == g_post_av)
                ownership_on_post_match(tip);
            else {
                char why[128];
                if (dig_m != g_post_digest)
                    snprintf(why, sizeof(why),
                             "post core diverge local=%08x peer=%08x",
                             (unsigned)g_post_digest, (unsigned)dig_m);
                else
                    snprintf(why, sizeof(why), "post av diverge local=%08x peer=%08x",
                             (unsigned)g_post_av, (unsigned)in_dig);
                (void)g_peer_post_match;
                rnet_rb_on_post_diverge(g_rb);
                abort_episode_realign(why); /* lobby stays; Live rewinds to agreed tip */
            }
        }
    }

    /* Retransmit follow-NACK while initiator may still be sealing. */
    if (g_follow_nack_pending && g_follow_nack_sends < RB_FOLLOW_NACK_REXMIT) {
        send_follow_nack(g_follow_nack_epoch, g_follow_nack_mismatch, g_follow_nack_load,
                         g_follow_nack_target, g_follow_nack_slot, 0);
        g_follow_nack_sends++;
        if (g_follow_nack_sends >= RB_FOLLOW_NACK_REXMIT)
            g_follow_nack_pending = 0;
    }

    /* TURN drops one-shot SEAL_ROWS / BASELINE easily — retransmit while waiting. */
    if (rnet_rb_is_active(g_rb) && rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs) {
        uint64_t now = rb_mono_ms();
        export_local_seals();
        maybe_rexmit_begin();
        if (g_seal_wait_ms != 0ull && now >= g_seal_wait_ms &&
            (now - g_seal_wait_ms) >= (uint64_t)RB_SEAL_TIMEOUT_MS) {
            abort_episode("seal timeout (peer missing snap / NACK lost)");
        }
    }

    if (rnet_rb_is_active(g_rb) &&
        rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline) {
        /* Wire/handshake only — snap apply is admit-wait exclusive. Pump runs
         * under the cycle watchdog mid-guest; mutating here is lethal. */
        apply_stashed_baseline();
        maybe_send_baseline();
        maybe_enter_replay();
    }

    /* §60: idle follower with a stashed ownership-chain BEGIN (POST drain
     * ordered after SYNC in this pump, or BEGIN arrived while Verify-active). */
    if (!rnet_rb_is_active(g_rb))
        apply_stashed_begin();

    /* Keep BASELINE on the wire through Replay until the peer is clearly in. */
    if (rnet_rb_is_active(g_rb) && rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay) {
        if (g_local_baseline_sent && !g_local_post_sent) {
            if (rnet_rb_is_from_peer_notify(g_rb))
                send_baseline_burst(1, RB_BASELINE_BURST, 1);
            else if (!g_peer_baseline_ready)
                send_baseline_burst(0, RB_BASELINE_BURST, 1);
        }
        /* Guest crash/hang after flush_resume: no finish_frame progress.
         * Also polled from mid-guest truncated pump (poll_replay_stall). */
        psx_netplay_rb_poll_replay_stall();
    }

    /* Retransmit POST while Verify waits for peer — do not race into live. */
    if (rnet_rb_is_active(g_rb) && rnet_rb_get_phase(g_rb) == nRNetRbPhaseVerify &&
        g_local_post_sent && !g_peer_post_ok) {
        uint64_t now = rb_mono_ms();
        /* §42c: the peer's SAFETY-commit notice usually landed while we
         * were still in the tip-extend rereplay (Replay phase) — re-check
         * the recorded OP_COMMIT now that we are in Verify, or we ride the
         * full verify timeout for nothing. */
        if (maybe_abandon_tip_extend())
            return;
        if (!g_post_rexmit_logged) {
            fprintf(stderr, "psxrecomp: rb verify wait peer POST core=%08x\n",
                    (unsigned)g_post_digest);
            fflush(stderr);
            g_post_rexmit_logged = 1;
        }
        (void)rnet_session_send_rb_post(s, rnet_rb_get_epoch_id(g_rb),
                                        rnet_rb_get_target_tick(g_rb), g_post_digest, g_post_av,
                                        1u);
        if (g_verify_wait_ms == 0ull)
            g_verify_wait_ms = now;
        else if (now >= g_verify_wait_ms &&
                 (now - g_verify_wait_ms) >= (uint64_t)RB_VERIFY_TIMEOUT_MS) {
            abort_episode_realign("verify timeout (peer POST missing)");
        }
    }

    poll_tip_hold_finalize();
}

void psx_netplay_rb_boot_dig0_note_local(uint32_t core)
{
    g_boot_dig0_local_core = core;
    g_boot_dig0_local_valid = 1u;
}

void psx_netplay_rb_boot_dig0_note_peer(uint32_t core)
{
    g_boot_dig0_peer_core = core;
    g_boot_dig0_peer_valid = 1u;
}

static void boot_dig0_rexmit_local(void)
{
    RNetSession *s;
    uint32_t now;
    if (!g_boot_dig0_local_valid || g_boot_dig0_synced)
        return;
    s = (g_bound && g_b.session) ? *g_b.session : NULL;
    if (!s)
        return;
    now = (uint32_t)psx_host_mono_ms();
    if (g_boot_dig0_last_rexmit_ms != 0u &&
        (uint32_t)(now - g_boot_dig0_last_rexmit_ms) < 100u)
        return;
    g_boot_dig0_last_rexmit_ms = now ? now : 1u;
    (void)rnet_session_send_rb_frame_commit(s, 0u, g_boot_dig0_local_core);
}

int psx_netplay_rb_boot_dig0_gate(void)
{
    uint32_t ld = 0u;
    uint32_t pd = 0u;
    if (g_boot_dig0_synced)
        return 0;
    if (!g_bound || !g_b.hc || !g_snaps)
        return 0;
    /* Prefer sticky latches; fall back to HC while rematch FC is in flight. */
    if (g_boot_dig0_local_valid)
        ld = g_boot_dig0_local_core;
    else if (!netplay_hc_local_digest(g_b.hc, 0u, &ld)) {
        if (!g_boot_dig0_wait_kind_logged) {
            g_boot_dig0_wait_kind_logged = 1;
            fprintf(stderr,
                    "psxrecomp: rb boot dig0 wait — local dig0 not published\n");
            fflush(stderr);
        }
        return 1;
    } else {
        g_boot_dig0_local_core = ld;
        g_boot_dig0_local_valid = 1u;
    }
    if (g_boot_dig0_peer_valid)
        pd = g_boot_dig0_peer_core;
    else if (!netplay_hc_peer_digest(g_b.hc, 0u, &pd)) {
        if (!g_boot_dig0_wait_kind_logged) {
            g_boot_dig0_wait_kind_logged = 1;
            fprintf(stderr,
                    "psxrecomp: rb boot dig0 wait — peer dig0 missing "
                    "(rexmit local=%08x)\n",
                    (unsigned)ld);
            fflush(stderr);
        }
        boot_dig0_rexmit_local();
        return 1;
    } else {
        g_boot_dig0_peer_core = pd;
        g_boot_dig0_peer_valid = 1u;
    }
    if (ld != pd) {
        boot_dig0_rexmit_local();
        if (!g_boot_dig0_mismatch_logged) {
            g_boot_dig0_mismatch_logged = 1;
            fprintf(stderr,
                    "psxrecomp: rb boot dig0 MISMATCH local=%08x peer=%08x "
                    "(hold; rematch BIOS backend / HLE plan?)\n",
                    (unsigned)ld, (unsigned)pd);
            fflush(stderr);
        }
        return 1;
    }
    g_boot_dig0_mismatch_logged = 0;
    g_boot_dig0_wait_kind_logged = 0;
    /* One more send after we saw peer dig0 — the slower peer may still be
     * waiting (guest often syncs first; host then needs this rexmit). */
    g_boot_dig0_last_rexmit_ms = 0;
    boot_dig0_rexmit_local();
    g_boot_dig0_synced = 1;
    fprintf(stderr,
            "psxrecomp: rb boot dig0 synced core=%08x\n", (unsigned)ld);
    fflush(stderr);
    return 0;
}

int psx_netplay_rb_active(void)
{
    return g_rb && rnet_rb_is_active(g_rb);
}

int psx_netplay_rb_is_resimulating(void)
{
    return g_rb && rnet_rb_is_resimulating(g_rb);
}

int psx_netplay_rb_tip_holding(void)
{
    return g_rb && rnet_rb_is_tip_holding(g_rb);
}

uint32_t psx_netplay_rb_episode_target(void)
{
    if (!g_rb || !rnet_rb_is_active(g_rb))
        return 0;
    return rnet_rb_get_target_tick(g_rb);
}

uint32_t psx_netplay_rb_tip_hold_invent_slack(void)
{
    if (!g_rb)
        return 0;
    /* §80: park Live at tip while deferred tip-hold repair is pending. */
    if (g_tip_hold_rereplay_pending)
        return 0;
    return rnet_rb_get_tip_seal_slack(g_rb);
}

int psx_netplay_rb_tip_hold_rereplay_pending(void)
{
    return g_tip_hold_rereplay_pending ? 1 : 0;
}

uint32_t psx_netplay_rb_tip_hold_rereplay_from(void)
{
    return g_tip_hold_rereplay_pending ? g_tip_hold_rereplay_from : 0u;
}

uint32_t psx_netplay_rb_tip_runway(void)
{
    if (!g_rb)
        return 0;
    return rnet_rb_get_tip_runway(g_rb);
}

void psx_netplay_rb_tip_hold_block_quiet(int block)
{
    if (!g_rb || !rnet_rb_is_tip_holding(g_rb))
        return;
    if (block) {
        g_tip_hold_block_quiet = 1;
        g_tip_hold_quiet_t0_ms = 0ull;
    } else {
        g_tip_hold_block_quiet = 0;
    }
}

int psx_netplay_rb_ignore_peer_frame_commit(uint32_t tick, uint32_t hash)
{
    uint32_t ld = 0;
    if (!g_tip_extend_prime_tick || !g_b.hc)
        return 0;
    if (!g_rb || !rnet_rb_is_active(g_rb))
        return 0;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay &&
        rnet_rb_get_phase(g_rb) != nRNetRbPhaseVerify)
        return 0;
    if (tick <= g_tip_extend_prime_tick)
        return 0;
    /* Ahead of our sealed resim, or TipHold invent that disagrees with it. */
    if (!netplay_hc_local_digest(g_b.hc, tick, &ld))
        return 1;
    if (ld != hash)
        return 1;
    return 0;
}

int psx_netplay_rb_abort_resim_core_mismatch(uint32_t tick, uint32_t local_core,
                                             uint32_t peer_core)
{
    char why[128];
    uint32_t load;
    CPUState *c;
    if (!g_rb || !rnet_rb_is_active(g_rb))
        return 0;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay &&
        rnet_rb_get_phase(g_rb) != nRNetRbPhaseVerify)
        return 0;
    /* TipHold invent still in hc after tip-extend — drain should have dropped
     * it; never abort the sealed rereplay on that digest. */
    if (psx_netplay_rb_ignore_peer_frame_commit(tick, peer_core))
        return 0;
    /* Ignore stale live commits below the episode load (hc_prime_after should
     * already have cleared them; belt-and-suspenders). */
    load = rnet_rb_get_load_tick(g_rb);
    if (tick < load)
        return 0;
    snprintf(why, sizeof(why),
             "resim core diverge sim=%u local=%08x peer=%08x",
             (unsigned)tick, (unsigned)local_core, (unsigned)peer_core);
    fprintf(stderr, "psxrecomp: rb ABORT — %s (no false POST commit)\n", why);
    /* GPR-only forks with matched RAM: dump bus digests + cpu-split so logs
     * show whether scratchpad/DMA/SIO already diverged before dig_cpu. */
    c = cpu();
    if (c) {
        NetplayCoreParts parts;
        NetplaySioParts sp;
        CPUState dig_cpu;
        log_cpu_digest_split(c, "abort");
        psx_netplay_rb_cpu_for_present_digest(&dig_cpu, c);
        netplay_core_digest_parts(&dig_cpu, &parts);
        netplay_sio_digest_parts(&sp);
        fprintf(stderr,
                "psxrecomp: rb abort-parts sim=%u cpu=%08x clk=%08x tim=%08x "
                "ram=%08x dirty=%08x spad=%08x dma=%08x sio=%08x "
                "sioP=%08x/%08x/%08x/%08x/%08x aux=%08x cd=%08x av=%08x\n",
                (unsigned)tick, (unsigned)parts.cpu, (unsigned)parts.clock_irq,
                (unsigned)parts.timers, (unsigned)parts.ram,
                (unsigned)parts.dirty, (unsigned)netplay_spad_digest(),
                (unsigned)netplay_dma_digest(), (unsigned)netplay_sio_digest(),
                (unsigned)sp.regs, (unsigned)sp.pads, (unsigned)sp.mc,
                (unsigned)sp.pace, (unsigned)sp.meta,
                (unsigned)netplay_aux_digest(), (unsigned)netplay_cdrom_digest(),
                (unsigned)netplay_av_digest());
        fflush(stderr);
    }
    rnet_rb_on_post_diverge(g_rb);
    abort_episode_realign(why);
    return 1;
}

int psx_netplay_rb_load_pending(void)
{
    return g_pending_load_valid || g_pending_resume_valid || g_live_realign_pending;
}

uint32_t psx_netplay_rb_baseline_fork_cap(void)
{
    return g_bl_fork_cap;
}

int psx_netplay_rb_try_admit(void)
{
    RNetSession *s = sess();
    rnet_u32 sim;
    static int s_stall_logged;

    /* Live realign after abort: apply only while parked in admit wait, then
     * flush_resume longjmps (!episode active). Never leave this to pump. */
    if (g_live_realign_pending ||
        (g_pending_load_valid && (!g_rb || !rnet_rb_is_active(g_rb)))) {
        CPUState *c = cpu();
        if (c)
            (void)try_apply_pending_load(c);
        return 0;
    }

    if (!g_rb || !s || !rnet_rb_is_active(g_rb))
        return 0;

    /* Applied snap, waiting for Replay + flush_resume — do not run guest. */
    if (g_pending_resume_valid)
        return 0;

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs)
        return 0; /* wait for peer seal chunks */

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline) {
        CPUState *c = cpu();
        /* Guest is parked in admit wait — apply baseline snap here. */
        if (c)
            (void)try_apply_pending_load(c);
        maybe_send_baseline();
        maybe_enter_replay();
        /* Still waiting for flush_resume longjmp (pending) or peer baseline. */
        if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay && g_needs_advance &&
            !g_pending_resume_valid)
            return 1;
        return 0;
    }

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseVerify ||
        rnet_rb_get_phase(g_rb) == nRNetRbPhaseCommit) {
        return 0; /* wait for POST / commit */
    }

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseTipHold)
        return 0; /* Live runs; do not stall admit forever */

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay && g_pending_load_valid &&
        !g_episode_snap_applied) {
        CPUState *c = cpu();
        /* tip-extend apply arms inside try_apply_pending_load. */
        if (c)
            (void)try_apply_pending_load(c);
        return g_needs_advance ? 1 : 0;
    }

    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return 0;

    /* Same turn as snap apply: wait for flush_resume (admit loop / BB-edge). */
    if (g_pending_resume_valid)
        return 0;

    if (g_needs_advance) {
        s_stall_logged = 0;
        return 1;
    }

    sim = rnet_session_sim_tick(s);
    if (sim > rnet_rb_get_target_tick(g_rb)) {
        if (!s_stall_logged) {
            fprintf(stderr,
                    "psxrecomp: rb try_admit stall sim=%u > target=%u (phase=Replay)\n",
                    (unsigned)sim, (unsigned)rnet_rb_get_target_tick(g_rb));
            fflush(stderr);
            s_stall_logged = 1;
        }
        return 0;
    }
    if (!rb_arm_rows_ready(sim))
        return 0; /* §55: stall until every seat row is authoritative */
    arm_replay_tick(sim);
    s_stall_logged = 0;
    return 1;
}

void psx_netplay_rb_finish_frame(void)
{
    RNetSession *s = sess();
    CPUState *c = cpu();
    rnet_u32 done;
    uint32_t target_before;
    if (!g_rb || !s || !g_needs_advance)
        return;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return;

    /* First present after flush_resume — native chain is live again. */
    psx_scheduler_top_level_resume_clear();
    done = rnet_session_sim_tick(s);
    g_replay_progress_ms = rb_mono_ms();
    fprintf(stderr, "psxrecomp: rb finish_frame sim=%u target=%u\n", (unsigned)done,
            (unsigned)rnet_rb_get_target_tick(g_rb));
    fflush(stderr);
    log_resim_tick_audit(done, "fin");
    psx_netplay_rb_request_snap(done);
    if (c)
        psx_netplay_rb_poll(c, pick_snap_resume_pc(c, 0u));
    rnet_session_advance(s);
    g_needs_advance = 0;

    /* §47: deepen sealed tip from contiguous confirmed frontier before POST. */
    psx_netplay_rb_ownership_step();

    /* Host may have tip-extended (via reconcile / ownership) — if the
     * tip grew past done, keep Replay instead of POST. */
    target_before = rnet_rb_get_target_tick(g_rb);
    if (done >= target_before)
        enter_verify_at_tip(done);
    else if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay) {
        uint32_t next = done + 1u;
        if (next <= rnet_rb_get_target_tick(g_rb)) {
            rnet_session_set_sim_tick(s, next);
            /* §55: if the next tick's seat rows aren't authoritative yet,
             * leave it un-armed — try_admit re-checks and arms once the
             * peer's seal rows land (keepalive redelivers). */
            if (rb_arm_rows_ready(next))
                arm_replay_tick(next);
        }
    }
}

uint32_t psx_netplay_rb_episode_count(void)
{
    return g_episode_count;
}

uint64_t psx_netplay_rb_take_replay_ticks(void)
{
    uint64_t n = g_stat_replay_ticks;
    g_stat_replay_ticks = 0;
    return n;
}

uint64_t psx_netplay_rb_replay_ticks_total(void)
{
    return g_stat_replay_ticks_total;
}

/* Live POST-handshake latency EMA (ms), 0 = no episode has round-tripped
 * yet this session. See g_rb_rtt_ema_ms above. */
uint32_t psx_netplay_rb_rtt_estimate_ms(void)
{
    return g_rb_rtt_ema_ms;
}

int psx_netplay_rb_phase(void)
{
    return g_rb ? (int)rnet_rb_get_phase(g_rb) : 0;
}

uint32_t psx_netplay_rb_snap_count(void)
{
    return g_snaps ? netplay_snap_ring_count(g_snaps) : 0u;
}

#endif /* PSX_HAS_RECOMP_NET */
