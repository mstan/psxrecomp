/* psx_netplay_sched.c — MotK thin glue over retcomm-rbengine.
 *
 * Policy lives in lib/retcomm-rbengine (rbe_sched_*). This TU binds MotK FMV /
 * dig0 / RTT gates and keeps the historical np_sched_* call surface.
 */

#if !defined(PSX_HAS_RECOMP_NET)

#include "psx_netplay_sched.h"

void np_sched_bind(const PsxNpSchedBridge *bridge) { (void)bridge; }
void np_sched_reset_session(void) {}
uint32_t np_sched_wire_for_sim(uint32_t sim_tick) { return sim_tick; }
int np_sched_real_delay_enabled(void) { return 1; }
void np_sched_sync_delay_from_session(void) {}
int np_sched_pre_admit(uint32_t sim, uint32_t wire, const RNetSessionStats *st)
{
    (void)sim; (void)wire; (void)st;
    return 0;
}
int np_sched_on_remote_miss(int slot, uint32_t sim, uint32_t wire,
                            const RNetSessionStats *st, int pred,
                            const char **reason_out)
{
    (void)slot; (void)sim; (void)wire; (void)st; (void)pred;
    if (reason_out) *reason_out = "stub";
    return 1;
}
void np_sched_note_remote_hit(void) {}
void np_sched_post_admit(int any_invent) { (void)any_invent; }
void np_sched_set_admit_stall(const char *tag) { (void)tag; }
void np_sched_clear_admit_stall(void) {}
const char *np_sched_admit_stall_tag(void) { return ""; }
void np_sched_note_mispredict(uint32_t age) { (void)age; }
void np_sched_note_episode_boundary(void) {}
void psx_netplay_timesync_on_episode_boundary(void) {}
void np_sched_arm_absurd_invent_catchup(void) {}

#else /* PSX_HAS_RECOMP_NET */

#include "psx_netplay_sched.h"

#include "psx_netplay_rb.h"
#include "retcomm_rbengine/mono_ms.h"
#include "retcomm_rbengine/sched.h"

#include <string.h>

static uint32_t np_gate_now_ms(void *ctx)
{
    (void)ctx;
    return rbe_mono_ms();
}

static uint32_t np_gate_rtt_ms(void *ctx)
{
    (void)ctx;
    return psx_netplay_rb_rtt_estimate_ms();
}

static uint8_t np_gate_episode_active(void *ctx)
{
    (void)ctx;
    return psx_netplay_rb_active() ? 1u : 0u;
}

static uint8_t np_gate_tip_holding(void *ctx)
{
    (void)ctx;
    return psx_netplay_rb_tip_holding() ? 1u : 0u;
}

static uint8_t np_gate_lockstep_no_invent(void *ctx)
{
    (void)ctx;
    return psx_netplay_rb_lockstep_no_invent() ? 1u : 0u;
}

static uint8_t np_gate_media_active(void *ctx)
{
    (void)ctx;
    return psx_netplay_rb_fmv_media_active() ? 1u : 0u;
}

static uint8_t np_gate_desync_hold(void *ctx)
{
    (void)ctx;
    return psx_netplay_rb_fmv_desync_hold() ? 1u : 0u;
}

static const char *np_gate_lockstep_stall_tag(void *ctx)
{
    (void)ctx;
    if (psx_netplay_rb_fmv_media_active())
        return "fmv_media";
    if (psx_netplay_rb_fmv_desync_hold())
        return "fmv_desync_hold";
    if (psx_netplay_rb_post_fmv_heal_sticky())
        return "heal_sticky";
    /* §113: invent hold runs through lockstep MIN (loading), not settle-only. */
    if (psx_netplay_rb_fmv_settle_active())
        return "fmv_settle";
    return "fmv_lockstep";
}

static uint32_t np_gate_episode_count(void *ctx)
{
    (void)ctx;
    return psx_netplay_rb_episode_count();
}

static uint64_t np_gate_replay_ticks_total(void *ctx)
{
    (void)ctx;
    return psx_netplay_rb_replay_ticks_total();
}

/* MotK rematch dig0 CRC gate — host-specific; kept out of portable sched. */
static uint8_t np_gate_pre_admit_hold(void *ctx, uint32_t sim, uint32_t wire,
                                     const char **tag_out)
{
    (void)ctx;
    (void)wire;
    if (sim == 1u && !psx_netplay_rb_active() &&
        psx_netplay_rb_boot_dig0_gate()) {
        if (tag_out)
            *tag_out = "boot_dig0_wait";
        return 1u;
    }
    return 0u;
}

void np_sched_bind(const PsxNpSchedBridge *bridge)
{
    RbeSchedBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (!bridge) {
        rbe_sched_bind(NULL);
        return;
    }
    rb.session = bridge->session;
    rb.input_delay = bridge->input_delay;
    rb.input_prediction = bridge->input_prediction;
    rb.local_slot = bridge->local_slot;
    rb.force_turn = bridge->force_turn;
    rb.gates.now_ms = np_gate_now_ms;
    rb.gates.rtt_ms = np_gate_rtt_ms;
    rb.gates.episode_active = np_gate_episode_active;
    rb.gates.tip_holding = np_gate_tip_holding;
    rb.gates.lockstep_no_invent = np_gate_lockstep_no_invent;
    rb.gates.lockstep_stall_tag = np_gate_lockstep_stall_tag;
    rb.gates.media_active = np_gate_media_active;
    rb.gates.desync_hold = np_gate_desync_hold;
    rb.gates.pre_admit_hold = np_gate_pre_admit_hold;
    rb.gates.episode_count = np_gate_episode_count;
    rb.gates.replay_ticks_total = np_gate_replay_ticks_total;
    rbe_sched_bind(&rb);
}

void np_sched_reset_session(void)
{
    rbe_sched_reset_session();
}

uint32_t np_sched_wire_for_sim(uint32_t sim_tick)
{
    return rbe_sched_wire_for_sim(sim_tick);
}

int np_sched_real_delay_enabled(void)
{
    return rbe_sched_real_delay_enabled();
}

void np_sched_sync_delay_from_session(void)
{
    rbe_sched_sync_delay_from_session();
}

int np_sched_pre_admit(uint32_t sim, uint32_t wire, const RNetSessionStats *st)
{
    return rbe_sched_pre_admit(sim, wire, st);
}

int np_sched_on_remote_miss(int slot, uint32_t sim, uint32_t wire,
                            const RNetSessionStats *st, int pred,
                            const char **reason_out)
{
    return rbe_sched_on_remote_miss(slot, sim, wire, st, pred, reason_out);
}

void np_sched_note_remote_hit(void)
{
    rbe_sched_note_remote_hit();
}

void np_sched_post_admit(int any_invent)
{
    rbe_sched_post_admit(any_invent);
}

void np_sched_set_admit_stall(const char *tag)
{
    rbe_sched_set_admit_stall(tag);
}

void np_sched_clear_admit_stall(void)
{
    rbe_sched_clear_admit_stall();
}

const char *np_sched_admit_stall_tag(void)
{
    return rbe_sched_admit_stall_tag();
}

void np_sched_note_mispredict(uint32_t age)
{
    rbe_sched_note_mispredict(age);
}

void np_sched_note_episode_boundary(void)
{
    rbe_sched_note_episode_boundary();
}

void psx_netplay_timesync_on_episode_boundary(void)
{
    np_sched_note_episode_boundary();
}

void np_sched_arm_absurd_invent_catchup(void)
{
    rbe_sched_arm_absurd_invent_catchup();
}

#endif /* PSX_HAS_RECOMP_NET */
