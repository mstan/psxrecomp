#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "psx_netplay.h"

#include "host_time.h"
#include "memcard.h"
#include "savestate.h"
#include "sio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(PSX_HAS_RECOMP_NET)
#include "recomp_net/recomp_net.h"
#include "recomp_net/input_contract.h"
#include "crc32.h"
#include "netplay_hash_confirm.h"
#include "netplay_input_hist.h"
#include "netplay_state_digest.h"
#include "psx_netplay_rb.h"
#include "psx_netplay_sched.h"
#include "cpu_state.h"
#include "cdrom.h"
#include "gpu.h"
#include "interrupts.h"
#include "mdec.h"
#include "overlay_loader.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"
#include "spu.h"
#if defined(PSX_HAS_LOBBY_CLIENT)
#include "psx_lobby_client.h"
#endif
#endif

#ifndef PSX_MAX_PLAYERS
#define PSX_MAX_PLAYERS 2
#endif

/* Session pad count mirrored for release_pads (available without recomp-net). */
static int g_np_slot_count = 2;

/* Persists across shutdown so starvation dumps still see last session topology. */
static char g_np_diag_arch[24] = "off";
static int  g_np_diag_max_players = 0;
static int  g_np_diag_player_count = 0;
static int  g_np_diag_configured = 0;

int psx_netplay_diag_snapshot(char *arch_out, size_t arch_cap,
                              int *max_players_out, int *player_count_out)
{
    if (arch_out && arch_cap)
        snprintf(arch_out, arch_cap, "%s", g_np_diag_arch);
    if (max_players_out) *max_players_out = g_np_diag_max_players;
    if (player_count_out) *player_count_out = g_np_diag_player_count;
    return g_np_diag_configured;
}

void psx_netplay_config_defaults(PsxNetplayConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->local_slot = 0;
    cfg->slot_count = 2;
    cfg->player_count = 0;
    cfg->occupied_mask = 0;
    cfg->input_player = -1;
    cfg->input_delay = 2;
    cfg->input_prediction = 4;
    cfg->force_input_relay = 0;
    cfg->force_turn = 0;
    cfg->transport = 0;
    cfg->session_id = 1;
    strncpy(cfg->bind_hostport, "0.0.0.0:7777", sizeof(cfg->bind_hostport) - 1);
    cfg->peer_hostport[0] = '\0';
}

static unsigned env_u(const char *name, unsigned def)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    return (unsigned)strtoul(v, NULL, 10);
}

void psx_netplay_apply_env(PsxNetplayConfig *cfg)
{
    const char *v;
    if (!cfg) return;
    v = getenv("PSX_NETPLAY");
    if (v && v[0] && v[0] != '0') cfg->enabled = 1;
    v = getenv("PSX_NET_SLOT");
    if (v && v[0]) cfg->local_slot = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_SLOTS");
    if (v && v[0]) cfg->slot_count = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_INPUT_PLAYER");
    if (v && v[0]) cfg->input_player = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_DELAY");
    if (v && v[0]) cfg->input_delay = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_PREDICTION");
    if (v && v[0]) cfg->input_prediction = (int)strtol(v, NULL, 10);
    cfg->session_id = env_u("PSX_NET_SESSION_ID", cfg->session_id);
    v = getenv("PSX_NET_BIND");
    if (v && v[0]) {
        strncpy(cfg->bind_hostport, v, sizeof(cfg->bind_hostport) - 1);
        cfg->bind_hostport[sizeof(cfg->bind_hostport) - 1] = '\0';
    }
    v = getenv("PSX_NET_PEER");
    if (v && v[0]) {
        strncpy(cfg->peer_hostport, v, sizeof(cfg->peer_hostport) - 1);
        cfg->peer_hostport[sizeof(cfg->peer_hostport) - 1] = '\0';
    }
    v = getenv("PSX_NET_TRANSPORT");
    if (v && v[0]) {
        if (strcmp(v, "ice") == 0 || strcmp(v, "ICE") == 0)
            cfg->transport = 1;
        else if (strcmp(v, "lan") == 0 || strcmp(v, "LAN") == 0)
            cfg->transport = 2;
    }
    v = getenv("PSX_NET_FORCE_TURN");
    if (v && v[0] && v[0] != '0')
        cfg->force_turn = 1;
    v = getenv("PSX_NET_MODE");
    if (v && v[0]) {
        if (strcmp(v, "rollback") == 0 || strcmp(v, "rb") == 0)
            cfg->rollback = 1;
        else if (strcmp(v, "delay") == 0 || strcmp(v, "delay-sync") == 0)
            cfg->rollback = 0;
    }
}

void psx_netplay_normalize_pad(PsxNetPad *pad)
{
    const int dead = 24; /* ~SDL-ish center deadzone in 0..255 space */
    if (!pad) return;
    pad->connected = 1;
    if (pad->lx > (uint8_t)(0x80 - dead) && pad->lx < (uint8_t)(0x80 + dead)) pad->lx = 0x80;
    if (pad->ly > (uint8_t)(0x80 - dead) && pad->ly < (uint8_t)(0x80 + dead)) pad->ly = 0x80;
    if (pad->rx > (uint8_t)(0x80 - dead) && pad->rx < (uint8_t)(0x80 + dead)) pad->rx = 0x80;
    if (pad->ry > (uint8_t)(0x80 - dead) && pad->ry < (uint8_t)(0x80 + dead)) pad->ry = 0x80;
    if (!pad->analog) {
        pad->lx = pad->ly = pad->rx = pad->ry = 0x80;
    }
}

static void force_session_pads_connected(int slot_count)
{
    int i;
    if (slot_count < 2) slot_count = 2;
    if (slot_count > PSX_MAX_PLAYERS) slot_count = PSX_MAX_PLAYERS;
    if (slot_count >= 3)
        sio_set_multitap(1);
    else
        sio_set_multitap(0);
    for (i = 0; i < slot_count; ++i) {
        sio_connect_pad(i);
        /* Tap seats are digital unless multitap_analog hack is armed. */
        const int force_dig =
            sio_pad_on_multitap(i) && !sio_get_multitap_analog();
        sio_set_pad_config_capable(i, force_dig ? 0 : 1);
    }
}

void psx_netplay_release_pads(void)
{
    int n = g_np_slot_count;
    if (n < 2) n = 2;
    if (n > PSX_MAX_PLAYERS) n = PSX_MAX_PLAYERS;
    /* Immediate digital + idle bus (not deferred type_req). Rematch dig0
     * baseline_ext was forking on pads when a DualShock host kept analog=1. */
    sio_netplay_canonicalize_session_pads(n);
}

/* ---- Start cadence bisect (always linked; PSX_START_BISECT=1) ---- */

static uint32_t psx_start_bisect_wall_ms(void)
{
    return (uint32_t)psx_host_mono_ms();
}

static int psx_start_bisect_env_flag(const char *name)
{
    const char *e = getenv(name);
    return (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
}

int psx_start_bisect_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        s_on = psx_start_bisect_env_flag("PSX_START_BISECT");
        if (s_on) {
            fprintf(stderr,
                    "psxrecomp: start-bisect ON (PSX_START_BISECT=1 — every "
                    "SDL/cap/stage/sio Start sample; hold Start 1s offline + "
                    "online; compare streams)\n");
            fflush(stderr);
        }
    }
    return s_on;
}

int psx_start_bisect_no_gc_update_in_admit(void)
{
    static int s = -1;
    if (s < 0) {
        s = psx_start_bisect_env_flag("PSX_START_BISECT_NO_GC_UPDATE_IN_ADMIT");
        if (s)
            fprintf(stderr,
                    "psxrecomp: start-bisect NO GameControllerUpdate in admit "
                    "spin\n");
    }
    return s;
}

int psx_start_bisect_no_tiphold_capture(void)
{
    static int s = -1;
    if (s < 0) {
        s = psx_start_bisect_env_flag("PSX_START_BISECT_NO_TIPHOLD_CAPTURE");
        if (s)
            fprintf(stderr,
                    "psxrecomp: start-bisect NO TipHold capture refresh\n");
    }
    return s;
}

int psx_start_bisect_no_catchup(void)
{
    static int s = -1;
    if (s < 0) {
        s = psx_start_bisect_env_flag("PSX_START_BISECT_NO_CATCHUP");
        if (s)
            fprintf(stderr, "psxrecomp: start-bisect NO catch-up budget\n");
    }
    return s;
}

int psx_start_bisect_no_replay_produce(void)
{
    static int s = -1;
    if (s < 0) {
        s = psx_start_bisect_env_flag("PSX_START_BISECT_NO_REPLAY_PRODUCE");
        if (s)
            fprintf(stderr,
                    "psxrecomp: start-bisect NO Replay tip produce\n");
    }
    return s;
}

int psx_start_bisect_spin_log(void)
{
    static int s = -1;
    if (s < 0)
        s = psx_start_bisect_env_flag("PSX_START_BISECT_SPIN");
    return s;
}

void psx_start_bisect_log(const char *path, uint32_t sim, int sdl_start,
                          int cap_start, int deb_start, int sio_start,
                          int latched, int tip_hold, int resim)
{
    static uint32_t s_n;
    const char *mode;
    char sdl_buf[8];
    char cap_buf[8];
    char deb_buf[8];
    char sio_buf[8];
    if (!psx_start_bisect_enabled())
        return;
    mode = psx_netplay_active() ? "online" : "offline";
    if (sdl_start < 0)
        snprintf(sdl_buf, sizeof(sdl_buf), "na");
    else
        snprintf(sdl_buf, sizeof(sdl_buf), "%d", sdl_start ? 1 : 0);
    if (cap_start < 0)
        snprintf(cap_buf, sizeof(cap_buf), "na");
    else
        snprintf(cap_buf, sizeof(cap_buf), "%d", cap_start ? 1 : 0);
    if (deb_start < 0)
        snprintf(deb_buf, sizeof(deb_buf), "na");
    else
        snprintf(deb_buf, sizeof(deb_buf), "%d", deb_start ? 1 : 0);
    if (sio_start < 0)
        snprintf(sio_buf, sizeof(sio_buf), "na");
    else
        snprintf(sio_buf, sizeof(sio_buf), "%d", sio_start ? 1 : 0);
    s_n++;
    fprintf(stderr,
            "psxrecomp: start-bisect n=%u mode=%s path=%s wall_ms=%u sim=%u "
            "sdl=%s cap=%s deb=%s sio=%s latch=%d tip_hold=%d resim=%d\n",
            (unsigned)s_n, mode, path ? path : "?",
            (unsigned)psx_start_bisect_wall_ms(), (unsigned)sim, sdl_buf,
            cap_buf, deb_buf, sio_buf, latched ? 1 : 0, tip_hold ? 1 : 0,
            resim ? 1 : 0);
    fflush(stderr);
}

/* ---- Start consumer bisect (PSX_START_CONSUMER=1) ----
 * What MotK's SIO reader sees each sim frame — not raw SDL. */

static uint32_t g_start_consumer_offline_frame;

int psx_start_consumer_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        s_on = psx_start_bisect_env_flag("PSX_START_CONSUMER");
        if (s_on) {
            fprintf(stderr,
                    "psxrecomp: start-consumer ON (PSX_START_CONSUMER=1 — "
                    "per-sim Start bit at SIO apply; hold Start offline + "
                    "online; compare game-visible timelines)\n");
            fflush(stderr);
        }
    }
    return s_on;
}

uint32_t psx_start_consumer_offline_frame(void)
{
    if (!psx_start_consumer_enabled())
        return 0u;
    g_start_consumer_offline_frame++;
    return g_start_consumer_offline_frame;
}

void psx_start_consumer_note(int slot, uint32_t sim, uint16_t buttons)
{
    static uint32_t s_n;
    static uint32_t s_last_sim[PSX_MAX_PLAYERS];
    static uint8_t s_last_start[PSX_MAX_PLAYERS];
    static uint8_t s_have[PSX_MAX_PLAYERS];
    int start;
    const char *edge;
    const char *mode;
    int resim;

    if (!psx_start_consumer_enabled())
        return;
    if (slot < 0 || slot >= PSX_MAX_PLAYERS)
        return;

    /* PSX digital Start = bit 3, active-low. */
    start = ((buttons & 0x0008u) == 0u) ? 1 : 0;

    /* Same sim + same Start: publish re-apply / multi-slot noise — skip. */
    if (s_have[slot] && s_last_sim[slot] == sim && s_last_start[slot] == (uint8_t)start)
        return;

    if (s_have[slot] && s_last_start[slot] != (uint8_t)start)
        edge = start ? "↓" : "↑";
    else
        edge = "-";

    mode = psx_netplay_active() ? "online" : "offline";
    resim = psx_netplay_is_resimulating();
    s_n++;
    fprintf(stderr,
            "psxrecomp: start-consumer n=%u mode=%s sim=%u slot=%d start=%d "
            "edge=%s buttons=%04x wall_ms=%u resim=%d\n",
            (unsigned)s_n, mode, (unsigned)sim, slot, start, edge,
            (unsigned)buttons, (unsigned)psx_start_bisect_wall_ms(),
            resim ? 1 : 0);
    fflush(stderr);

    s_last_sim[slot] = sim;
    s_last_start[slot] = (uint8_t)start;
    s_have[slot] = 1u;
}

#if !defined(PSX_HAS_RECOMP_NET)

int  psx_netplay_active(void) { return 0; }
int  psx_netplay_is_running(void) { return 0; }
const char *psx_netplay_transport_name(void) { return "none"; }
int  psx_netplay_ice_failed(void) { return 0; }
void psx_netplay_diag_tick(void) {}
int  psx_netplay_local_slot(void) { return -1; }
int  psx_netplay_input_player(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
int  psx_netplay_start(const PsxNetplayConfig *cfg)
{
    (void)cfg;
    return -1;
}
void psx_netplay_shutdown(void) {}
void psx_netplay_cold_reset(void) {}
void psx_netplay_stage_local(const PsxNetPad *pad) { (void)pad; }
void psx_netplay_pad_trace_dev(int card, int fallback, int sdl_start,
                               uint16_t buttons)
{
    (void)card;
    (void)fallback;
    (void)sdl_start;
    (void)buttons;
}
void psx_netplay_on_rb_snap_loaded(void) {}
void psx_netplay_hc_fork_recovery_restart(void) {}
void psx_netplay_hc_fork_recovery_clear(void) {}
void psx_netplay_cd_bisect_arm(uint32_t from_sim, uint32_t ticks)
{
    (void)from_sim;
    (void)ticks;
}
int psx_netplay_cd_bisect_active(void) { return 0; }
int  psx_netplay_needs_local_sample(void) { return 0; }
int  psx_netplay_live_pad_buttons(uint16_t *out)
{
    if (out)
        *out = 0xFFFFu;
    return 0;
}
int  psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    (void)tick;
    (void)local_hash;
    (void)remote_hash;
    return 0;
}
int  psx_netplay_peer_disconnected(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return 0;
}
void psx_netplay_touch_peer_liveness(void) {}
uint32_t psx_netplay_running_liveness_timeout_ms(void) { return 1500u; }
void psx_netplay_bind_guest_saves(void) {}
int  psx_netplay_is_host(void) { return 0; }
int  psx_netplay_request_save(int slot) { (void)slot; return 0; }
int  psx_netplay_request_load(int slot) { (void)slot; return 0; }
int  psx_netplay_in_load_barrier(void) { return 0; }
int  psx_netplay_consume_load_apply_failed(void) { return 0; }
void psx_netplay_pump(void) {}
int  psx_netplay_poll_admit(void) { return 1; }
void psx_netplay_finish_frame(void) {}
int  psx_netplay_remote_lead(void) { return 0; }
int  psx_netplay_input_delay(void) { return 2; }
int  psx_netplay_catchup_budget(void) { return 0; }
void psx_netplay_catchup_consume_frame(void) {}
void psx_netplay_wait_recv(int timeout_ms) { (void)timeout_ms; }
void psx_netplay_admit_wait_info(char *stall_out, size_t stall_cap,
                                 uint32_t *sim_tick_out, int *lead_out)
{
    if (stall_out && stall_cap) {
        stall_out[0] = '\0';
        if (stall_cap > 1)
            strncpy(stall_out, "off", stall_cap - 1);
    }
    if (sim_tick_out) *sim_tick_out = 0;
    if (lead_out) *lead_out = 0;
}
void psx_netplay_bind_cpu(struct CPUState *cpu) { (void)cpu; }
uint32_t psx_netplay_resolved_through(void) { return 0; }
int psx_netplay_hash_confirm_through(uint32_t tick) { (void)tick; return 0; }
int psx_netplay_rollback_mode(void) { return 0; }
void psx_netplay_poll_snap(struct CPUState *cpu, uint32_t resume_pc)
{
    (void)cpu;
    (void)resume_pc;
}
int psx_netplay_is_resimulating(void) { return 0; }

#else /* PSX_HAS_RECOMP_NET */

#define NP_SANDBOX_FALLBACK "saves/netplay"
#define NP_MC_BLOB_BYTES (4u + (size_t)MEMCARD_SIZE * 2u)
/* LOAD probe size==0 + this crc = post-load ready rendezvous (not SAVE coord). */
#define NP_LOAD_READY_CRC 0x4C4F4144u /* 'LOAD' */

typedef enum {
    NP_XFER_NONE = 0,
    NP_XFER_MC_PROBE,
    NP_XFER_MC_SEND,
    NP_XFER_SAVE_COORD,
    NP_XFER_SAVE_PROBE,
    NP_XFER_SAVE_SEND,
    NP_XFER_LOAD_PROBE,
    NP_XFER_LOAD_SEND,
    NP_XFER_LOAD_APPLYING, /* load staged; admit runs until savestate_poll fires */
    NP_XFER_LOAD_READY     /* local restore done; wait peer before lockstep */
} NpXferPhase;

typedef struct {
    RNetSession *session;
    PsxNetPad    staged;
    int          staged_valid;
    /* Physical pad refreshed every stage_local call (even when latched). */
    PsxNetPad    live;
    int          live_valid;
    int          active;
    int          slot_count;
    int          local_slot;
    int          input_player; /* resolved host PlayerInput index */
    int          needs_advance;
    int          latched_for_tick; /* 1 if staged pad frozen for current sim_tick */
    uint32_t     latched_sim_tick;
    /* Guest sandbox: personal roots restored on shutdown. */
    int          guest_sandbox;
    char         personal_save_dir[512];
    char         personal_mc0[512];
    char         personal_mc1[512];
    uint32_t     bios_checksum;
    uint32_t     entry_pc;
    /* Host-owned save/memcard sync. */
    NpXferPhase  xfer;
    int          xfer_slot;
    int          mc_sync_done;
    int          mc_sync_sent;
    int          local_save_staged;
    int          local_save_acked;   /* guest: coord reply already sent */
    uint32_t     save_target_tick;   /* both peers save during this sim_tick */
    int          load_applied_local;
    int          load_ready_replied; /* READY exchanged; synced; stay LOAD_READY until admit */
    int          load_sync_done;     /* hard_resync+prime once at mutual ready */
    int          load_apply_failed;  /* sticky: staged apply rejected — soft-exit */
    /* Transport / ICE / diag (MotK online path). */
    int          use_ice;
    int          ice_has_turn;
    int          force_input_relay;
    int          is_host;
    int          input_delay;
    int          input_prediction; /* invent lead cap (P); rollback only */
    uint32_t     session_id;
    uint32_t     frames_finished;
    uint32_t     diag_session;
    unsigned     ice_stun_port;
    unsigned     ice_turn_port;
    char         ice_stun_host[128];
    char         ice_turn_host[128];
    char         ice_turn_user[192];
    char         ice_turn_pass[128];
    char         ice_bind_addr[64];
    char         bind_hostport[64];
    char         peer_hostport[64];
    char         match_mode[32];
    char         lobby_server[256];
    char         lobby_id[64];
    /* Master digest / FRAME_COMMIT watermark (rollback hash_confirm). */
    CPUState*          cpu;
    NetplayHashConfirm hc;
    /* Rollback invent / stick-replace contract (PSX_NET_MODE=rollback). */
    int                rollback;
    NetplayInputHist   ih;
    /* Pending rewind from late wire (episode hookup is step 4). */
    int                pending_rewind;
    uint32_t           pending_rewind_tick;
    int                pending_rewind_slot;
} NetplayState;

static NetplayState g_np;

/* Local partition ring aligned with FRAME_COMMIT — explain first core fork. */
#define NP_PART_RING 128u
typedef struct NpPartSlot {
    uint32_t tick;
    NetplayCoreParts parts;
    uint32_t av;
    uint32_t cd;
    uint32_t spu;
    uint32_t mdec;
    uint32_t aux;
    uint8_t  valid;
} NpPartSlot;
static NpPartSlot s_part_ring[NP_PART_RING];
static int s_core_diverge_logged;
static uint32_t s_live_dig_last_tick = 0xffffffffu;
/* §54: HC-fork recovery — a live core fork with no pad mispredict never
 * opened an episode (FIRST CORE DIVERGE was log-only), so a swallowed
 * correction desynced the rest of the session silently at 60fps. Track how
 * long the hash_confirm peek mismatch has persisted and open a recovery
 * episode at the fork tick. */
static uint32_t s_fork_tick = 0xffffffffu;
static uint32_t s_fork_first_sim;
static uint32_t s_fork_last_attempt_sim;
/* §54: completed-tick mispredict promoted during cooldown/sweep — the state
 * already simmed with the wrong pad and nothing re-opens an episode after
 * cooldown expires. Defer the rewind instead of dropping it. */
static int s_deferred_rw_valid;
static uint32_t s_deferred_rw_tick;
static int s_deferred_rw_slot;
static int s_deferred_rw_logged;

static void np_part_ring_reset(void)
{
    memset(s_part_ring, 0, sizeof(s_part_ring));
    s_core_diverge_logged = 0;
    s_live_dig_last_tick = 0xffffffffu;
    s_fork_tick = 0xffffffffu;
    s_fork_first_sim = 0;
    s_fork_last_attempt_sim = 0;
    s_deferred_rw_valid = 0;
    s_deferred_rw_tick = 0;
    s_deferred_rw_slot = -1;
    s_deferred_rw_logged = 0;
}

static void np_part_ring_put(uint32_t tick, const NetplayCoreParts *parts,
                             uint32_t av, uint32_t cd, uint32_t spu,
                             uint32_t mdec, uint32_t aux)
{
    NpPartSlot *s = &s_part_ring[tick % NP_PART_RING];
    s->tick = tick;
    s->parts = *parts;
    s->av = av;
    s->cd = cd;
    s->spu = spu;
    s->mdec = mdec;
    s->aux = aux;
    s->valid = 1u;
}

static const NpPartSlot *np_part_ring_get(uint32_t tick)
{
    const NpPartSlot *s = &s_part_ring[tick % NP_PART_RING];
    if (!s->valid || s->tick != tick)
        return NULL;
    return s;
}

static void np_log_live_digest(uint32_t tick, const NetplayCoreParts *parts,
                               uint32_t av, uint32_t cd, uint32_t spu,
                               uint32_t mdec, uint32_t aux, const char *tag)
{
    fprintf(stderr,
            "psxrecomp: rb live dig %s sim=%u core=%08x cpu=%08x clk=%08x "
            "tim=%08x ram=%08x dirty=%08x av=%08x cd=%08x "
            "spu=%08x mdec=%08x aux=%08x\n",
            tag ? tag : "tick", (unsigned)tick, (unsigned)parts->core,
            (unsigned)parts->cpu, (unsigned)parts->clock_irq,
            (unsigned)parts->timers, (unsigned)parts->ram,
            (unsigned)parts->dirty, (unsigned)av, (unsigned)cd,
            (unsigned)spu, (unsigned)mdec, (unsigned)aux);
    {
        static int s_parts = -1;
        if (s_parts < 0) {
            const char *e = getenv("PSX_RB_SPU_PARTS");
            s_parts = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
            if (s_parts) {
                fprintf(stderr,
                        "psxrecomp: rb SPU part digests on "
                        "(PSX_RB_SPU_PARTS=1 — regs/voices/tail)\n");
            }
        }
        if (s_parts) {
            SpuSnapPartDigests pd;
            spu_snapshot_part_digests(&pd);
            fprintf(stderr,
                    "psxrecomp: rb spu parts sim=%u regs=%08x voices=%08x "
                    "tail=%08x\n",
                    (unsigned)tick, (unsigned)pd.regs, (unsigned)pd.voices,
                    (unsigned)pd.tail);
        }
    }
    fflush(stderr);
}

/* §54: persistent live core fork with no pad mispredict → open a recovery
 * episode at the fork tick. Only the slot-0 host initiates (avoids a dual
 * simultaneous-BEGIN collision); the guest follows via the wire SYNC. The
 * episode reloads from the newest provably-safe snap (≤ fork tick — HC is
 * confirmed through fork-1 by definition) and reseals both seats from
 * wire-authoritative history, converging the states. */
static void np_try_hc_fork_recovery(uint32_t fork_tick)
{
    uint32_t sim;
    int remote;
    uint32_t fork_cap;
    uint32_t need_retry;
    /* Fire before the 128-slot hc ring wraps and peek goes quiet. */
    const uint32_t persist_ticks = 16u;
    const uint32_t retry_ticks = 32u;
    /* §71: after baseline bisect failures, do not reopen every 32 ticks into
     * the same doomed ladder (soak: load 1808→1792→1776→… baseline mismatch). */
    const uint32_t retry_ticks_fork_cap = 256u;

    if (!g_np.rollback || !g_np.session)
        return;
    if (g_np.local_slot != 0)
        return; /* initiator side only */
    /* §115: tip+1 soft fork already accepted — stay Live. */
    if (psx_netplay_rb_platform_fork_accepted(fork_tick))
        return;
    /* §64: do not bookkeep persist while an episode / tip-hold / load is
     * in flight. Previously fork_tick advances during tip-extend rereplay
     * started the persist clock, so tip-extend abandon → Live opened a
     * second hc-fork recovery with zero Live gap (soak: epoch 8 → 16,
     * "persisted 39 ticks"). */
    /* §109/§110: allow hc-fork through DESYNC invent-hold or heal sticky when
     * MEDIA_KF can heal. Keep blocking live media+settle invent-hold. */
    if (psx_netplay_rb_active() || psx_netplay_rb_tip_holding() ||
        psx_netplay_rb_load_pending() || psx_netplay_rb_rewind_suppressed() ||
        (psx_netplay_rb_fmv_defer_rewind() && !psx_netplay_rb_fmv_desync_hold() &&
         !psx_netplay_rb_post_fmv_heal_eligible()) ||
        (psx_netplay_rb_lockstep_no_invent() && !psx_netplay_rb_fmv_desync_hold() &&
         !psx_netplay_rb_post_fmv_heal_eligible()) ||
        psx_netplay_rb_fmv_episode_unsafe(fork_tick))
        return;
    sim = rnet_session_sim_tick(g_np.session);
    fork_cap = psx_netplay_rb_baseline_fork_cap();
    if (fork_tick != s_fork_tick) {
        s_fork_tick = fork_tick;
        s_fork_first_sim = sim;
        return;
    }
    if (sim < s_fork_first_sim || (sim - s_fork_first_sim) < persist_ticks)
        return;
    need_retry = (fork_cap > 0u) ? retry_ticks_fork_cap : retry_ticks;
    if (s_fork_last_attempt_sim != 0u && sim >= s_fork_last_attempt_sim &&
        (sim - s_fork_last_attempt_sim) < need_retry)
        return;
    s_fork_last_attempt_sim = sim;
    remote = (g_np.local_slot == 0) ? 1 : 0;
    if (g_np.slot_count < 2)
        remote = g_np.local_slot;
    fprintf(stderr,
            "psxrecomp: rb hc-fork recovery begin t=%u (persisted %u ticks, "
            "no pad mispredict — state resync episode%s)\n",
            (unsigned)fork_tick, (unsigned)(sim - s_fork_first_sim),
            fork_cap ? "; fork_cap backoff" : "");
    fflush(stderr);
    /* §109/§114: silent fork → apply-only MEDIA_KF heal (target=load). */
    psx_netplay_rb_request_post_fmv_heal_kf();
    (void)psx_netplay_rb_begin_rewind(fork_tick, remote);
}

void psx_netplay_hc_fork_recovery_restart(void)
{
    uint32_t sim;
    if (!g_np.session)
        return;
    sim = rnet_session_sim_tick(g_np.session);
    /* Force a fresh Live persist window + retry gap after abandon/realign. */
    s_fork_first_sim = sim;
    s_fork_last_attempt_sim = sim;
    fprintf(stderr,
            "psxrecomp: rb hc-fork recovery restart (sim=%u — fresh persist)\n",
            (unsigned)sim);
    fflush(stderr);
}

void psx_netplay_hc_fork_recovery_clear(void)
{
    s_fork_tick = 0xffffffffu;
    s_fork_first_sim = 0u;
    s_fork_last_attempt_sim = 0u;
}

/* §93 P1: per-tick CD/MDEC/clk crumbs across BIOS→FMV (~160–192) and after
 * media realign. Enable with PSX_RB_CD_BISECT=1 or =LO-HI (default 150-250).
 * PSX_RB_CD_BISECT_ARM=N extends N ticks past each media flush_resume arm. */
static int s_cd_bisect_enabled;
static int s_cd_bisect_inited;
static uint32_t s_cd_bisect_lo = 150u;
static uint32_t s_cd_bisect_hi = 250u;
static uint32_t s_cd_bisect_arm_until; /* exclusive sim; 0 = inactive */
static uint32_t s_cd_bisect_arm_span = 48u;
static uint32_t s_cd_bisect_last_tick = 0xffffffffu;

static void np_cd_bisect_init(void)
{
    const char *e;
    const char *dash;
    unsigned lo = 0, hi = 0;
    if (s_cd_bisect_inited)
        return;
    s_cd_bisect_inited = 1;
    e = getenv("PSX_RB_CD_BISECT");
    if (!e || !e[0] || e[0] == '0')
        return;
    s_cd_bisect_enabled = 1;
    dash = strchr(e, '-');
    if (dash && dash > e &&
        sscanf(e, "%u-%u", &lo, &hi) == 2 && hi >= lo) {
        s_cd_bisect_lo = lo;
        s_cd_bisect_hi = hi;
    }
    e = getenv("PSX_RB_CD_BISECT_ARM");
    if (e && e[0]) {
        unsigned n = (unsigned)atoi(e);
        if (n > 0u && n < 4096u)
            s_cd_bisect_arm_span = n;
    }
    fprintf(stderr,
            "psxrecomp: rb CD bisect ON sim=%u..%u +arm=%u after media "
            "flush_resume (per-tick crumbs + cd-cmd ISSUE/QUEUE/DONE; "
            "diff peer lines by cyc, ignore resim=1)\n",
            (unsigned)s_cd_bisect_lo, (unsigned)s_cd_bisect_hi,
            (unsigned)s_cd_bisect_arm_span);
    fflush(stderr);
}

void psx_netplay_cd_bisect_arm(uint32_t from_sim, uint32_t ticks)
{
    uint32_t span;
    np_cd_bisect_init();
    if (!s_cd_bisect_enabled)
        return;
    span = ticks ? ticks : s_cd_bisect_arm_span;
    if (span == 0u)
        span = 48u;
    if (from_sim + span < from_sim)
        s_cd_bisect_arm_until = 0xffffffffu;
    else
        s_cd_bisect_arm_until = from_sim + span;
    fprintf(stderr,
            "psxrecomp: rb CD bisect arm sim=%u..%u (media flush_resume)\n",
            (unsigned)from_sim, (unsigned)(s_cd_bisect_arm_until - 1u));
    fflush(stderr);
}

int psx_netplay_cd_bisect_active(void)
{
    uint32_t sim;
    np_cd_bisect_init();
    if (!s_cd_bisect_enabled)
        return 0;
    sim = psx_netplay_sim_tick();
    if (sim >= s_cd_bisect_lo && sim <= s_cd_bisect_hi)
        return 1;
    if (s_cd_bisect_arm_until != 0u && sim < s_cd_bisect_arm_until)
        return 1;
    return 0;
}

static void np_cd_bisect_tick(uint32_t tick)
{
    CDROMDebugState cds;
    MDECDebugState mds;
    CPUState dig_cpu;
    uint32_t csv;
    uint32_t cd_crc;
    uint32_t clk_h;
    uint64_t mdec_age;
    int in_fixed;
    int in_arm;
    np_cd_bisect_init();
    if (!s_cd_bisect_enabled || !g_np.cpu)
        return;
    in_fixed = (tick >= s_cd_bisect_lo && tick <= s_cd_bisect_hi);
    in_arm = (s_cd_bisect_arm_until != 0u && tick < s_cd_bisect_arm_until);
    if (!in_fixed && !in_arm)
        return;
    if (s_cd_bisect_last_tick == tick)
        return;
    s_cd_bisect_last_tick = tick;

    memset(&cds, 0, sizeof(cds));
    memset(&mds, 0, sizeof(mds));
    cdrom_debug_snapshot(&cds);
    mdec_debug_get_state(&mds);
    psx_netplay_rb_cpu_for_present_digest(&dig_cpu, g_np.cpu);
    csv = interrupts_get_cycles_since_vblank();
    cd_crc = netplay_cdrom_digest();
    {
        NetplayCoreParts parts;
        netplay_core_digest_parts(&dig_cpu, &parts);
        clk_h = parts.clock_irq;
    }
    mdec_age = mdec_color_age_cycles();

    fprintf(stderr,
            "psxrecomp: rb cd-bisect sim=%u cyc=%llu csv=%u pc=%08x "
            "i_stat=%08x clk=%08x cd=%08x "
            "rd=%d reading=%d present_d=%d pend=%d/%d pend_cmd=%02x div=%d "
            "lba=%d rMSF=%02d:%02d:%02d mode=%02x "
            "irq_f=%02x irq_e=%02x sec_av=%d sec_pos=%d "
            "mdec_age=%llu mdec_busy=%u in=%u out=%u/%u depth24=%d "
            "xa=%d fmv_pend=%d\n",
            (unsigned)tick,
            (unsigned long long)psx_cycle_count,
            (unsigned)csv,
            (unsigned)dig_cpu.pc,
            (unsigned)cds.i_stat,
            (unsigned)clk_h,
            (unsigned)cd_crc,
            cds.read_delay,
            cds.reading,
            cds.irq_present_delay,
            cds.pending_pending,
            cds.pending_delay,
            (unsigned)cds.pending_cmd,
            cds.speed_divisor,
            cds.last_sector_lba,
            cds.read_min, cds.read_sec, cds.read_sect,
            (unsigned)cds.mode_reg,
            (unsigned)cds.irq_flag,
            (unsigned)cds.irq_enable,
            cds.sector_available,
            cds.sector_read_pos,
            (unsigned long long)mdec_age,
            (unsigned)mds.busy,
            (unsigned)mds.input_count,
            (unsigned)mds.output_pos,
            (unsigned)mds.output_size,
            gpu_display_is_depth24(),
            cdrom_xa_stream_active(),
            cdrom_fmv_stream_pending());
    fflush(stderr);
}

static void np_check_core_diverge(void)
{
    uint32_t tick = 0, local_d = 0, peer_d = 0;
    const NpPartSlot *slot;
    if (!netplay_hc_peek_mismatch(&g_np.hc, &tick, &local_d, &peer_d)) {
        s_fork_tick = 0xffffffffu; /* resolved / no complete pair pending */
        return;
    }
    /* Replay/Verify: never allow a false POST commit after mid-resim fork. */
    if (psx_netplay_rb_abort_resim_core_mismatch(tick, local_d, peer_d))
        return;
    np_try_hc_fork_recovery(tick);
    if (s_core_diverge_logged)
        return;
    s_core_diverge_logged = 1;
    slot = np_part_ring_get(tick);
    if (slot) {
        fprintf(stderr,
                "psxrecomp: rb FIRST CORE DIVERGE sim=%u local=%08x peer=%08x "
                "| local parts cpu=%08x clk=%08x tim=%08x ram=%08x dirty=%08x "
                "av=%08x cd=%08x spu=%08x mdec=%08x aux=%08x "
                "(compare peer rb live dig at same sim)\n",
                (unsigned)tick, (unsigned)local_d, (unsigned)peer_d,
                (unsigned)slot->parts.cpu, (unsigned)slot->parts.clock_irq,
                (unsigned)slot->parts.timers, (unsigned)slot->parts.ram,
                (unsigned)slot->parts.dirty, (unsigned)slot->av,
                (unsigned)slot->cd, (unsigned)slot->spu, (unsigned)slot->mdec,
                (unsigned)slot->aux);
    } else {
        fprintf(stderr,
                "psxrecomp: rb FIRST CORE DIVERGE sim=%u local=%08x peer=%08x "
                "(local parts aged out of ring — see prior rb live dig lines)\n",
                (unsigned)tick, (unsigned)local_d, (unsigned)peer_d);
    }
    fflush(stderr);
}

static void np_drain_peer_frame_commits(void)
{
    rnet_u32 through = 0, hash = 0;
    if (!g_np.session) return;
    while (rnet_session_take_rb_frame_commit(g_np.session, &through, &hash)) {
        /* Tip-extend rereplay: TipHold Live invent FCs may still be queued
         * after hc_prime — drop until peer's sealed resim matches. */
        if (g_np.rollback &&
            psx_netplay_rb_ignore_peer_frame_commit(through, hash))
            continue;
        if (through == 0u)
            psx_netplay_rb_boot_dig0_note_peer(hash);
        netplay_hc_note_peer(&g_np.hc, through, hash);
    }
    /* FIRST CORE can stick the watermark forever once that tick ages out of
     * the 128-slot ring — heal so choose_load_tick sees a real frontier. */
    if (netplay_hc_heal_stale_gap(&g_np.hc)) {
        static uint32_t s_heal_log;
        uint32_t rt = netplay_hc_resolved_through(&g_np.hc);
        if (s_heal_log != rt) {
            fprintf(stderr,
                    "psxrecomp: rb hash_confirm heal stale gap → resolved=%u\n",
                    (unsigned)rt);
            fflush(stderr);
            s_heal_log = rt;
        }
    }
    np_check_core_diverge();
}

static void np_emit_frame_commit(uint32_t tick)
{
    NetplayCoreParts parts;
    CPUState dig_cpu;
    uint32_t av = 0u;
    uint32_t cd = 0u;
    uint32_t spu = 0u;
    uint32_t mdec = 0u;
    uint32_t aux = 0u;
    int crumb;
    if (!g_np.cpu || !g_np.session) return;
    /* TipHold Live invents are not sealed-input truth — emitting them poisoned
     * tip-extend Replay (stale peer digests → false resim core diverge). */
    if (g_np.rollback && psx_netplay_rb_tip_holding() &&
        !psx_netplay_rb_is_resimulating())
        return;
    /* Live depth24 + hot MDEC: skip full-RAM FRAME_COMMIT (rewind already
     * deferred). Resim still commits every tick. MDEC-idle cutover still
     * commits so FMV1→FMV2 forks are visible (digs used to go dark for the
     * whole movie). Leaving FMV primes hash_confirm so the watermark is not
     * stuck on missing movie slots. */
    if (gpu_display_is_depth24() && mdec_recently_active(8) &&
        !(g_np.rollback && psx_netplay_rb_is_resimulating()))
        return;
    /* Present-edge: clear PC so parked-0 vs live-BB does not fork FRAME_COMMIT
     * while GPRs/RAM/clk match (was aborting good Replay on dig_cpu alone). */
    psx_netplay_rb_cpu_for_present_digest(&dig_cpu, g_np.cpu);
    netplay_core_digest_parts(&dig_cpu, &parts);
    /* av/cd/spu/mdec every 32 ticks — VRAM + SPU-RAM CRC every frame is heavy. */
    crumb = (tick == 0u || (tick % 32u) == 0u);
    if (crumb) {
        uint32_t aux_crc = 0xFFFFFFFFu;
        av = netplay_av_digest();
        cd = netplay_cdrom_digest();
        spu = netplay_spu_digest();
        mdec = netplay_mdec_digest();
        /* Same fold as netplay_aux_digest — avoid hashing SPU RAM twice. */
        aux_crc = crc32_update(aux_crc, (const uint8_t *)&spu, sizeof(spu));
        aux_crc = crc32_update(aux_crc, (const uint8_t *)&mdec, sizeof(mdec));
        aux = aux_crc ^ 0xFFFFFFFFu;
    }
    np_part_ring_put(tick, &parts, av, cd, spu, mdec, aux);
    netplay_hc_note_local(&g_np.hc, tick, parts.core);
    if (tick == 0u)
        psx_netplay_rb_boot_dig0_note_local(parts.core);
    (void)rnet_session_send_rb_frame_commit(g_np.session, tick, parts.core);
    (void)netplay_hc_heal_stale_gap(&g_np.hc);
    /* Breadcrumbs so both peers' logs line up by sim tick. Tag is local-only
     * (not peer agreement — that is hash_confirm resolved_through). */
    if (crumb && s_live_dig_last_tick != tick) {
        s_live_dig_last_tick = tick;
        np_log_live_digest(tick, &parts, av, cd, spu, mdec, aux, "local");
    }
    np_check_core_diverge();
}

static FILE *g_diag_file;
static uint32_t g_diag_file_session;
static int g_diag_summary_written;
static uint32_t g_diag_last_write_ms;
static int g_diag_mkdir_done;

static void np_sleep_ms(unsigned ms)
{
    psx_host_sleep_ms(ms);
}

static uint32_t np_mono_ms(void)
{
    return (uint32_t)psx_host_mono_ms();
}

static void np_enter_load_ready(int slot);
static void np_commit_load_sync(void);
static void np_begin_load_apply(int slot);
static void np_starv_reset(void);
static void np_maybe_stage_target_save(void);
static void np_note_save_complete(void);

static int np_file_crc(const uint8_t *data, size_t size, uint32_t *crc_out)
{
    if (!data || size == 0 || !crc_out) return 0;
    *crc_out = rnet_checksum(data, size);
    return 1;
}

static int np_slot_crc(int slot, uint32_t *size_out, uint32_t *crc_out)
{
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t crc;
    if (!savestate_read_slot(slot, &data, &size) || !data) return 0;
    if (!np_file_crc(data, size, &crc)) {
        free(data);
        return 0;
    }
    if (size_out) *size_out = (uint32_t)size;
    if (crc_out) *crc_out = crc;
    free(data);
    return 1;
}

static int np_build_mc_blob(uint8_t *out, size_t cap, size_t *out_size)
{
    uint8_t *p;
    if (!out || cap < NP_MC_BLOB_BYTES || !out_size) return -1;
    memset(out, 0, NP_MC_BLOB_BYTES);
    p = out;
    p[0] = memcard_is_present(0) ? 1u : 0u;
    p[1] = memcard_is_present(1) ? 1u : 0u;
    if (p[0] && memcard_export_raw(0, p + 4) != 0) return -1;
    if (p[1] && memcard_export_raw(1, p + 4 + MEMCARD_SIZE) != 0) return -1;
    *out_size = NP_MC_BLOB_BYTES;
    return 0;
}

static int np_apply_mc_blob(const uint8_t *data, size_t size)
{
    if (!data || size < NP_MC_BLOB_BYTES) return -1;
    if (data[0]) {
        if (memcard_import_raw(0, data + 4) != 0) return -1;
    }
    if (data[1]) {
        if (memcard_import_raw(1, data + 4 + MEMCARD_SIZE) != 0) return -1;
    }
    return 0;
}

static int np_mc_blob_crc(uint32_t *size_out, uint32_t *crc_out)
{
    uint8_t *blob = (uint8_t *)malloc(NP_MC_BLOB_BYTES);
    size_t sz = 0;
    uint32_t crc;
    if (!blob) return 0;
    if (np_build_mc_blob(blob, NP_MC_BLOB_BYTES, &sz) != 0) {
        free(blob);
        return 0;
    }
    if (!np_file_crc(blob, sz, &crc)) {
        free(blob);
        return 0;
    }
    if (size_out) *size_out = (uint32_t)sz;
    if (crc_out) *crc_out = crc;
    free(blob);
    return 1;
}

static int np_xfer_busy(void)
{
    if (!g_np.session) return 0;
    if (g_np.xfer != NP_XFER_NONE) return 1;
    return rnet_session_state_busy(g_np.session) ||
           rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL);
}

static void np_enter_guest_sandbox(void)
{
    /* Prefer memcard/save ROOT (pre-BIOS-token). savestate_dir() may already
     * be <root>/openbios|scph1001 — nesting netplay under that is wrong. */
    const char *root = savestate_root_dir();
    const char *dir = (root && root[0]) ? root : savestate_dir();
    const char *p0 = NULL;
    const char *p1 = NULL;
    uint32_t bios = 0, entry = 0;
    char sandbox[560];

    savestate_get_integrity(&bios, &entry);
    g_np.bios_checksum = bios;
    g_np.entry_pc = entry;
    if (dir && dir[0])
        strncpy(g_np.personal_save_dir, dir, sizeof(g_np.personal_save_dir) - 1);
    (void)memcard_debug_info(0, &p0, NULL, NULL, NULL);
    (void)memcard_debug_info(1, &p1, NULL, NULL, NULL);
    if (p0) strncpy(g_np.personal_mc0, p0, sizeof(g_np.personal_mc0) - 1);
    if (p1) strncpy(g_np.personal_mc1, p1, sizeof(g_np.personal_mc1) - 1);

    /* Prefer <memcard_dir>/netplay (absolute, next to the binary) so CWD does
     * not matter. Relative "saves/netplay" only as a last-resort fallback. */
    if (g_np.personal_save_dir[0]) {
        size_t n = strlen(g_np.personal_save_dir);
        while (n > 0 && (g_np.personal_save_dir[n - 1] == '/' ||
                         g_np.personal_save_dir[n - 1] == '\\')) {
            g_np.personal_save_dir[--n] = '\0';
        }
        snprintf(sandbox, sizeof(sandbox), "%s/netplay", g_np.personal_save_dir);
    } else {
        snprintf(sandbox, sizeof(sandbox), "%s", NP_SANDBOX_FALLBACK);
    }

    /* NULL bios_token: use sandbox path as-is (no further BIOS subdir). */
    savestate_configure(sandbox, bios, entry, NULL, 0);
    (void)memcard_rebind_dir(sandbox);
    g_np.guest_sandbox = 1;
    printf("psxrecomp: netplay guest sandbox -> %s\n", sandbox);
    fflush(stdout);
}

static void np_leave_guest_sandbox(void)
{
    if (!g_np.guest_sandbox) return;
    memcard_flush_all();
    (void)memcard_rebind_paths(
        g_np.personal_mc0[0] ? g_np.personal_mc0 : NULL,
        g_np.personal_mc1[0] ? g_np.personal_mc1 : NULL);
    (void)memcard_reload_bound();
    if (g_np.personal_save_dir[0]) {
        const char *token = savestate_bios_token();
        savestate_configure(g_np.personal_save_dir, g_np.bios_checksum,
                            g_np.entry_pc,
                            (token && token[0]) ? token : NULL,
                            savestate_openbios_wordsum());
    }
    g_np.guest_sandbox = 0;
}

static void np_apply_ready_state(void)
{
    rnet_u8 op = 0, slot = 0;
    const void *data = NULL;
    size_t size = 0;

    if (!rnet_session_state_take_ready(g_np.session, &op, &slot, &data, &size))
        return;
    if (!data || size == 0) {
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        if (g_np.local_slot != 0 && np_apply_mc_blob((const uint8_t *)data, size) != 0) {
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        g_np.mc_sync_done = 1;
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        g_np.needs_advance = 0;
        g_np.latched_for_tick = 0;
        return;
    }

    /* §97: FMV media keyframe — install into RB pin/ring (no disk slot). */
    if (op == RNET_STATE_OP_RB_KF) {
        (void)psx_netplay_rb_media_kf_on_ready(data, size);
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        return;
    }

    if (op == RNET_STATE_OP_SAVE) {
        if (g_np.local_slot != 0) {
            if (!savestate_write_slot((int)slot, data, size)) {
                printf("psxrecomp: netplay guest save slot=%u — write failed\n", (unsigned)slot);
                fflush(stdout);
                rnet_session_state_finish(g_np.session, 0);
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            /* Post-transfer hash verify against wire CRC. */
            {
                uint32_t got_sz = 0, got_crc = 0;
                if (!np_slot_crc((int)slot, &got_sz, &got_crc) ||
                    got_sz != (uint32_t)size ||
                    got_crc != rnet_checksum((const rnet_u8 *)data, size)) {
                    printf("psxrecomp: netplay guest save slot=%u — post-CRC mismatch\n",
                           (unsigned)slot);
                    fflush(stdout);
                    rnet_session_state_finish(g_np.session, 0);
                    g_np.xfer = NP_XFER_NONE;
                    return;
                }
            }
            printf("psxrecomp: netplay guest save slot=%u — synced (%zu bytes)\n",
                   (unsigned)slot, size);
            fflush(stdout);
        } else {
            printf("psxrecomp: netplay save slot=%u — transfer complete\n", (unsigned)slot);
            fflush(stdout);
        }
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        np_note_save_complete();
        return;
    }

    /* LOAD transfer (hash miss): guest stages the wire blob in memory (no disk
     * dependency — relative sandbox/CWD issues used to fail write_slot here).
     * Both peers request apply so host cannot restore before guest has bytes. */
    if (g_np.local_slot != 0) {
        if (!savestate_request_load_blob_protocol(data, size)) {
            printf("psxrecomp: netplay guest load slot=%u — blob stage failed "
                   "(%zu bytes, sandbox='%s')\n",
                   (unsigned)slot, size, savestate_dir());
            fflush(stdout);
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        /* Best-effort mirror to sandbox for hash-probe hits on rematch. */
        if (!savestate_write_slot((int)slot, data, size)) {
            printf("psxrecomp: netplay guest load slot=%u — sandbox mirror "
                   "failed (in-memory apply continues)\n",
                   (unsigned)slot);
            fflush(stdout);
        }
    } else {
        (void)savestate_request_load_protocol((int)slot);
    }
    rnet_session_state_finish(g_np.session, 0);
    np_begin_load_apply((int)slot);
    printf("psxrecomp: netplay load slot=%u — applying after transfer…\n", (unsigned)slot);
    fflush(stdout);
}

static void np_guest_handle_probe(void)
{
    rnet_u8 op = 0, slot = 0;
    rnet_u32 size = 0, crc = 0;
    int match = 0;

    if (g_np.local_slot == 0) return;
    if (!rnet_session_state_probe_pending(g_np.session, &op, &slot, &size, &crc))
        return;

    /* Post-load ready rendezvous (must be before SAVE size==0 coord). */
    if (op == RNET_STATE_OP_LOAD && size == 0 && crc == NP_LOAD_READY_CRC) {
        if (g_np.xfer == NP_XFER_LOAD_APPLYING) {
            if (savestate_pending())
                return; /* still need guest cycles to apply */
            if (savestate_take_load_completed()) {
                np_enter_load_ready((int)slot);
            } else if (!g_np.load_applied_local) {
                return; /* staged but not yet applied */
            }
        }
        if (g_np.xfer != NP_XFER_LOAD_READY && !g_np.load_applied_local) {
            return;
        }
        /* ACK host ready, commit resync+prime once, then stay in LOAD_READY
         * until try_admit succeeds (host must probe_finish first). */
        if (rnet_session_state_probe_reply(g_np.session, 1) != 0)
            return;
        np_commit_load_sync();
        if (!g_np.load_ready_replied) {
            g_np.load_ready_replied = 1;
            printf("psxrecomp: netplay load slot=%u — ready acked, waiting lockstep…\n",
                   (unsigned)slot);
            fflush(stdout);
        }
        return;
    }

    if (size == 0) {
        /* SAVE coord: crc carries the shared target sim_tick. Both peers
         * stage the write when sim reaches that tick (see
         * np_maybe_stage_target_save) so CRCs match and skip transfer. */
        if (g_np.xfer != NP_XFER_SAVE_COORD) {
            g_np.xfer = NP_XFER_SAVE_COORD;
            g_np.xfer_slot = (int)slot;
            g_np.save_target_tick = crc;
            g_np.local_save_staged = 0;
            g_np.local_save_acked = 0;
            printf("psxrecomp: netplay guest save slot=%u — armed target "
                   "sim=%u\n",
                   (unsigned)slot, (unsigned)crc);
            fflush(stdout);
        }
        if (savestate_pending()) return;
        if (savestate_take_save_failed()) {
            (void)rnet_session_state_probe_reply(g_np.session, 0);
            printf("psxrecomp: netplay guest save slot=%u — local write failed "
                   "(no safe resume PC) — aborting\n",
                   (unsigned)slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            g_np.local_save_staged = 0;
            g_np.local_save_acked = 0;
            return;
        }
        if (!g_np.local_save_staged || !savestate_slot_exists((int)slot)) return;
        if (!savestate_last_save_pc()) {
            /* Stale slot on disk from a prior match — wait for a real write. */
            return;
        }
        if (!g_np.local_save_acked) {
            g_np.local_save_acked = 1;
            (void)rnet_session_state_probe_reply(g_np.session, 1);
            printf("psxrecomp: netplay guest save slot=%u — local write done "
                   "@ target sim pc=0x%08X (frozen until hash probe)\n",
                   (unsigned)slot, (unsigned)savestate_last_save_pc());
            fflush(stdout);
        }
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        uint32_t local_sz = 0, local_crc = 0;
        match = np_mc_blob_crc(&local_sz, &local_crc) && local_sz == size && local_crc == crc;
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (match) g_np.mc_sync_done = 1;
        return;
    }

    /* §97: rollback FMV media keyframe — CRC of raw snap at episode load. */
    if (op == RNET_STATE_OP_RB_KF) {
        match = psx_netplay_rb_media_kf_probe_match(size, crc);
        (void)rnet_session_state_probe_reply(g_np.session, match);
        printf("psxrecomp: netplay MEDIA-KF probe — %s (size=%u crc=%08x)\n",
               match ? "match" : "miss", (unsigned)size, (unsigned)crc);
        fflush(stdout);
        return;
    }

    {
        uint32_t local_sz = 0, local_crc = 0;
        char reason[192];
        match = np_slot_crc((int)slot, &local_sz, &local_crc) && local_sz == size &&
                local_crc == crc;
        /* CRC match of a stale .pst (wrong codegen) is not loadable — ask the
         * host to transfer. Host also refuses probe start if its own slot is
         * stale, so this mainly covers guest-sandbox drift. */
        if (match && op == RNET_STATE_OP_LOAD &&
            !savestate_slot_compatible((int)slot, reason, sizeof(reason))) {
            printf("psxrecomp: netplay guest load slot=%u — hash matched but "
                   "unloadable (%s); requesting transfer\n",
                   (unsigned)slot, reason[0] ? reason : "incompatible");
            fflush(stdout);
            match = 0;
        }
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (op == RNET_STATE_OP_SAVE) {
            if (match) {
                g_np.xfer = NP_XFER_NONE;
                printf("psxrecomp: netplay guest save slot=%u — hashes match, "
                       "skip transfer\n",
                       (unsigned)slot);
                fflush(stdout);
                np_note_save_complete();
            } else {
                /* Host will chunk the authoritative .pst — stay parked. */
                g_np.xfer = NP_XFER_SAVE_SEND;
                g_np.xfer_slot = (int)slot;
            }
        } else if (op == RNET_STATE_OP_LOAD) {
            if (match) {
                if (g_np.xfer != NP_XFER_LOAD_APPLYING &&
                    g_np.xfer != NP_XFER_LOAD_READY) {
                    (void)savestate_request_load_protocol((int)slot);
                    np_begin_load_apply((int)slot);
                    printf("psxrecomp: netplay guest load slot=%u — hashes match, "
                           "applying…\n",
                           (unsigned)slot);
                    fflush(stdout);
                }
            } else {
                /* Must mark LOAD_SEND or guest keeps the 20s admit timeout and
                 * BYEs the host mid-TURN transfer. */
                g_np.xfer = NP_XFER_LOAD_SEND;
                g_np.xfer_slot = (int)slot;
                printf("psxrecomp: netplay guest load slot=%u — hash miss, "
                       "waiting for transfer…\n",
                       (unsigned)slot);
                fflush(stdout);
            }
        }
    }
}

static void np_host_drive_xfer(void)
{
    int match = 0;
    uint32_t size = 0, crc = 0;
    uint8_t *buf = NULL;
    size_t n = 0;

    if (g_np.local_slot != 0 || !g_np.session) return;

    switch (g_np.xfer) {
    case NP_XFER_MC_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            g_np.mc_sync_done = 1;
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        {
            uint8_t *blob = (uint8_t *)malloc(NP_MC_BLOB_BYTES);
            size_t sz = 0;
            if (!blob || np_build_mc_blob(blob, NP_MC_BLOB_BYTES, &sz) != 0 ||
                rnet_session_state_begin(g_np.session, RNET_STATE_OP_SRAM, 0, blob, sz) != 0) {
                free(blob);
                g_np.mc_sync_done = 1;
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            free(blob);
            g_np.xfer = NP_XFER_MC_SEND;
        }
        return;

    case NP_XFER_SAVE_COORD:
        /* Host + guest both stage at save_target_tick; wait for local write
         * and guest ACK before hashing. */
        if (savestate_pending()) return;
        if (savestate_take_save_failed()) {
            printf("psxrecomp: netplay save slot=%d — local write failed "
                   "(no safe resume PC) — aborting\n",
                   g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            g_np.local_save_staged = 0;
            return;
        }
        if (!g_np.local_save_staged || !savestate_slot_exists(g_np.xfer_slot))
            return;
        if (!savestate_last_save_pc())
            return;
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (!match) {
            /* Guest failed to save — still ship host blob if it has a PC. */
        }
        if (!np_slot_crc(g_np.xfer_slot, &size, &crc) ||
            rnet_session_state_probe(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)g_np.xfer_slot, size,
                                     crc) != 0) {
            printf("psxrecomp: netplay save slot=%d — hash probe failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        printf("psxrecomp: netplay save slot=%d — hash probe (%u bytes, pc=0x%08X)\n",
               g_np.xfer_slot, (unsigned)size, (unsigned)savestate_last_save_pc());
        fflush(stdout);
        g_np.xfer = NP_XFER_SAVE_PROBE;
        return;

    case NP_XFER_SAVE_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            printf("psxrecomp: netplay save slot=%d — hashes match, skip transfer\n",
                   g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            np_note_save_complete();
            return;
        }
        if (!savestate_last_save_pc()) {
            printf("psxrecomp: netplay save slot=%d — refusing null-PC transfer\n",
                   g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        if (!savestate_read_slot(g_np.xfer_slot, &buf, &n) || !buf ||
            rnet_session_state_begin(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)g_np.xfer_slot, buf,
                                     n) != 0) {
            free(buf);
            printf("psxrecomp: netplay save slot=%d — transfer begin failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        printf("psxrecomp: netplay save slot=%d — transferring %zu bytes to guest "
               "(pc=0x%08X)\n",
               g_np.xfer_slot, n, (unsigned)savestate_last_save_pc());
        fflush(stdout);
        free(buf);
        g_np.xfer = NP_XFER_SAVE_SEND;
        return;

    case NP_XFER_LOAD_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            (void)savestate_request_load_protocol(g_np.xfer_slot);
            np_begin_load_apply(g_np.xfer_slot);
            printf("psxrecomp: netplay load slot=%d — hashes match, applying…\n",
                   g_np.xfer_slot);
            fflush(stdout);
            return;
        }
        if (!savestate_read_slot(g_np.xfer_slot, &buf, &n) || !buf) {
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        /* Do not stage savestate_request_load here — host would apply during
         * SEND, enter LOAD_READY, and suppress INPUT before the guest can
         * admit frames for its own savestate_poll (deadlock). Both peers
         * stage in np_apply_ready_state when the transfer completes. */
        g_np.load_applied_local = 0;
        g_np.load_sync_done = 0;
        if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)g_np.xfer_slot, buf,
                                     n) != 0) {
            free(buf);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        free(buf);
        printf("psxrecomp: netplay load slot=%d — transferring %zu bytes\n", g_np.xfer_slot, n);
        fflush(stdout);
        g_np.xfer = NP_XFER_LOAD_SEND;
        return;

    case NP_XFER_MC_SEND:
    case NP_XFER_SAVE_SEND:
    case NP_XFER_LOAD_SEND:
        /* apply_ready runs first and clears take_ready (LOAD → LOAD_APPLYING). */
        if (rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL)) {
            int was_save = (g_np.xfer == NP_XFER_SAVE_SEND);
            if (g_np.xfer == NP_XFER_MC_SEND)
                g_np.mc_sync_done = 1;
            rnet_session_state_finish(g_np.session, 0);
            if (g_np.xfer == NP_XFER_LOAD_SEND) {
                np_begin_load_apply(g_np.xfer_slot);
            } else {
                g_np.xfer = NP_XFER_NONE;
                if (was_save)
                    np_note_save_complete();
            }
        }
        return;

    case NP_XFER_LOAD_READY:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        /* Mutual ready: drop probe stall first, then resync+prime. Stay in
         * LOAD_READY until try_admit (do not drop the app barrier early). */
        rnet_session_state_probe_finish(g_np.session);
        np_commit_load_sync();
        g_np.load_ready_replied = 1;
        printf("psxrecomp: netplay load slot=%d — mutual ready, waiting lockstep…\n",
               g_np.xfer_slot);
        fflush(stdout);
        return;

    default:
        return;
    }
}

static void np_prime_after_hard_resync(void)
{
    uint8_t bytes[PSX_NETPLAY_PAD_BYTES];
    PsxNetPad pad;

    /* Prime delay prefix with the current local hold (not forced neutral) so the
     * first D play frames continue what the player is already pressing. Each
     * peer only primes its own slot — lockstep stays valid. Tip latency for
     * *changes* remains D; we just avoid a post-load dead zone of released pads. */
    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0xFFFFu;
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
    pad.analog = 1;
    pad.connected = 1;
    if (g_np.staged_valid)
        pad = g_np.staged;
    pad.connected = 1;
    psx_netplay_normalize_pad(&pad);

    bytes[0] = (uint8_t)(pad.buttons & 0xFFu);
    bytes[1] = (uint8_t)((pad.buttons >> 8) & 0xFFu);
    bytes[2] = pad.lx;
    bytes[3] = pad.ly;
    bytes[4] = pad.rx;
    bytes[5] = pad.ry;
    bytes[6] = pad.analog ? 1u : 0u;
    bytes[7] = 1u;
    rnet_session_prime_delay_inputs(g_np.session, bytes, (rnet_u16)PSX_NETPLAY_PAD_BYTES);

    /* Keep staged matching the prime so the first tip sample is not a sudden
     * release while [0..D) still holds the live pad. */
    g_np.staged = pad;
    g_np.staged_valid = 1;
}

/* Stage restore. Keep INPUT flowing so try_admit can still run guest cycles
 * for savestate_poll — suppress only at mutual ready (np_commit_load_sync).
 * Ready probe must also leave INPUT unstalled (recomp-net size==0 LOAD). */
static void np_begin_load_apply(int slot)
{
    /* Transfer admit failures (state_xfer) often latch starvation; lead can sit
     * at D-1 after ICE xfer and would block the only frame savestate_poll needs. */
    np_starv_reset();
    g_np.xfer = NP_XFER_LOAD_APPLYING;
    g_np.load_applied_local = 0;
    g_np.load_sync_done = 0;
    g_np.load_ready_replied = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    g_np.live_valid = 0;
    g_np.xfer_slot = slot;
}

/* Once per load, at mutual ready (guest READY ACK / host take_reply). */
static void np_commit_load_sync(void)
{
    if (g_np.load_sync_done || !g_np.session)
        return;
    /* Suppress empty tips only for the hard_resync→prime window. */
    rnet_session_set_input_send_suppress(g_np.session, 1);
    rnet_session_hard_resync(g_np.session);
    np_prime_after_hard_resync(); /* clears suppress + emits fresh tip */
    netplay_hc_reset(&g_np.hc);
    np_part_ring_reset();
    g_np.load_sync_done = 1;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    /* staged_valid left set by prime — tip must match delay-prefix hold. */
    /* §95: load is a hard resync — drop leftover FMV DESYNC invent-hold. */
    if (g_np.rollback)
        psx_netplay_rb_clear_fmv_desync_hold("netplay load sync");
}

static void np_enter_load_ready(int slot)
{
    /* Do not hard_resync/prime here — the later-applying peer would clear the
     * earlier peer's tip and stall resume. Sync runs at mutual ready.
     * Do not suppress INPUT here either: the first peer to finish apply must
     * keep sending pads so the other can still admit frames for savestate_poll. */
    g_np.load_applied_local = 1;
    g_np.load_ready_replied = 0;
    g_np.load_sync_done = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    g_np.live_valid = 0;
    g_np.xfer = NP_XFER_LOAD_READY;
    g_np.xfer_slot = slot;
}

/* After both peers stage a load: run until restore completes, then rendezvous.
 * hard_resync+prime happens once at mutual ready (not at apply). */
static void np_drive_load_barrier(void)
{
    if (g_np.xfer != NP_XFER_LOAD_APPLYING)
        return;
    if (savestate_pending())
        return;
    if (savestate_take_load_failed()) {
        /* Stale/mismatched .pst: do not sit in load_apply_done forever. */
        printf("psxrecomp: netplay load slot=%d — apply failed "
               "(incompatible or missing .pst) — aborting barrier\n",
               g_np.xfer_slot);
        fflush(stdout);
        if (g_np.session)
            rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        g_np.load_applied_local = 0;
        g_np.load_ready_replied = 0;
        g_np.load_sync_done = 0;
        g_np.load_apply_failed = 1;
        if (g_np.session)
            rnet_session_set_input_send_suppress(g_np.session, 0);
        return;
    }
    if (!g_np.load_applied_local && !savestate_take_load_completed())
        return;

    np_enter_load_ready(g_np.xfer_slot);

    if (g_np.local_slot == 0) {
        if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)g_np.xfer_slot, 0,
                                     NP_LOAD_READY_CRC) != 0) {
            printf("psxrecomp: netplay load slot=%d — ready probe failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            g_np.load_applied_local = 0;
            if (g_np.session)
                rnet_session_set_input_send_suppress(g_np.session, 0);
            return;
        }
        printf("psxrecomp: netplay load slot=%d — applied, waiting for guest…\n",
               g_np.xfer_slot);
        fflush(stdout);
    } else {
        printf("psxrecomp: netplay guest load slot=%d — applied, waiting for host…\n",
               g_np.xfer_slot);
        fflush(stdout);
    }
}

static void np_maybe_start_mc_sync(void)
{
    uint32_t size = 0, crc = 0;
    if (g_np.local_slot != 0 || g_np.mc_sync_sent || g_np.mc_sync_done)
        return;
    if (!rnet_session_is_running(g_np.session)) return;
    if (np_xfer_busy()) return;
    if (!np_mc_blob_crc(&size, &crc)) {
        g_np.mc_sync_done = 1;
        return;
    }
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_SRAM, 0, size, crc) != 0) {
        g_np.mc_sync_done = 1;
        return;
    }
    g_np.mc_sync_sent = 1;
    g_np.xfer = NP_XFER_MC_PROBE;
}

static void encode_pad(const PsxNetPad *pad, RNetInputSample *out, rnet_u32 tick)
{
    PsxNetPad n = *pad;
    psx_netplay_normalize_pad(&n);
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->size = PSX_NETPLAY_PAD_BYTES;
    out->bytes[0] = (rnet_u8)(n.buttons & 0xFFu);
    out->bytes[1] = (rnet_u8)((n.buttons >> 8) & 0xFFu);
    out->bytes[2] = n.lx;
    out->bytes[3] = n.ly;
    out->bytes[4] = n.rx;
    out->bytes[5] = n.ry;
    out->bytes[6] = n.analog ? 1u : 0u;
    out->bytes[7] = 1u;
    out->valid = 1;
}

static void decode_pad(const RNetInputSample *in, PsxNetPad *pad)
{
    memset(pad, 0, sizeof(*pad));
    pad->buttons = 0xFFFFu;
    pad->lx = pad->ly = pad->rx = pad->ry = 0x80u;
    pad->analog = 1;
    pad->connected = 1;
    if (!in || !in->valid || in->size < PSX_NETPLAY_PAD_BYTES) return;
    pad->buttons = (uint16_t)in->bytes[0] | ((uint16_t)in->bytes[1] << 8);
    pad->lx = in->bytes[2];
    pad->ly = in->bytes[3];
    pad->rx = in->bytes[4];
    pad->ry = in->bytes[5];
    pad->analog = in->bytes[6] ? 1u : 0u;
    pad->connected = 1;
    psx_netplay_normalize_pad(pad);
}

/* PSX digital Start = bit 3 (active-low). Rising press: bit goes 1→0. */
#define NP_PAD_BTN_START 0x0008u
/* §34 dig / Start sticky removed — intentional taps must not be merged.
 * Post-match taunt double-pause is an in-game MotK bug (also DuckStation). */

static int np_pad_log_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        const char *e = getenv("PSX_RB_PAD_LOG");
        s_on = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        if (s_on)
            fprintf(stderr,
                    "psxrecomp: rb pad-log ON (PSX_RB_PAD_LOG=1 — local edges + "
                    "SIO apply; Start = bit3 → buttons=fff7)\n");
    }
    return s_on;
}

/* Pipeline + automatic Start verdicts. Implies edge logging when on. */
static int np_pad_trace_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        const char *e = getenv("PSX_RB_PAD_TRACE");
        s_on = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        if (s_on)
            fprintf(stderr,
                    "psxrecomp: rb pad-trace ON (PSX_RB_PAD_TRACE=1 — "
                    "dev/stage/tip/sio + VERDICT tags for Start doubles)\n");
    }
    return s_on;
}

static int np_pad_diag_enabled(void)
{
    return np_pad_log_enabled() || np_pad_trace_enabled();
}

static uint32_t np_pad_mono_ms(void)
{
    return (uint32_t)psx_host_mono_ms();
}

static int np_pad_start_down(uint16_t buttons)
{
    return ((uint16_t)(~buttons) & NP_PAD_BTN_START) != 0;
}

static const char *np_pad_trace_ctx(void)
{
    static char buf[96];
    int tip = 0, resim = 0, ep = 0;
    if (g_np.rollback) {
        tip = psx_netplay_rb_tip_holding();
        resim = psx_netplay_rb_is_resimulating();
        ep = psx_netplay_rb_active();
    }
    snprintf(buf, sizeof(buf), "tip_hold=%d resim=%d episode=%d D=%d",
             tip, resim, ep, g_np.input_delay);
    return buf;
}

/* Start gesture classifier (stage/dev edges). */
static uint32_t g_start_gest_down_sim;
static uint32_t g_start_gest_up_sim;
static uint8_t g_start_gest_down_valid;
static uint8_t g_start_gest_up_valid;
static uint32_t g_start_gest_last_stage_down_sim = 0xFFFFFFFFu;

static void np_pad_trace_start_down(const char *src, uint32_t sim)
{
    uint32_t gap;
    if (!np_pad_trace_enabled())
        return;
    if (g_start_gest_up_valid) {
        gap = sim - g_start_gest_up_sim;
        if (gap <= 2u)
            fprintf(stderr,
                    "psxrecomp: rb pad-trace VERDICT pulse src=%s sim=%u "
                    "gap_after_up=%u (bounce/debounce hole ≤2) %s\n",
                    src, (unsigned)sim, (unsigned)gap, np_pad_trace_ctx());
        else if (gap <= 60u)
            fprintf(stderr,
                    "psxrecomp: rb pad-trace VERDICT multipress src=%s sim=%u "
                    "gap_after_up=%u (capture saw multiple Start periods) %s\n",
                    src, (unsigned)sim, (unsigned)gap, np_pad_trace_ctx());
        fflush(stderr);
    }
    g_start_gest_down_sim = sim;
    g_start_gest_down_valid = 1;
    g_start_gest_up_valid = 0;
    if (src && strcmp(src, "stage") == 0)
        g_start_gest_last_stage_down_sim = sim;
}

static void np_pad_trace_start_up(const char *src, uint32_t sim)
{
    (void)src;
    if (!np_pad_trace_enabled())
        return;
    g_start_gest_up_sim = sim;
    g_start_gest_up_valid = 1;
    g_start_gest_down_valid = 0;
}

static void np_pad_log_edge(const char *tag, int slot, uint32_t sim, uint16_t prev,
                            uint16_t cur)
{
    uint16_t press;
    uint16_t release;
    if (prev == cur)
        return;
    press = (uint16_t)((uint16_t)(~cur) & prev);
    release = (uint16_t)((uint16_t)(~prev) & cur);
    if (!np_pad_diag_enabled())
        return;
    fprintf(stderr,
            "psxrecomp: rb pad-edge %s slot=%d sim=%u buttons=%04x→%04x "
            "press=%04x release=%04x%s%s\n",
            tag ? tag : "?", slot, (unsigned)sim, (unsigned)prev, (unsigned)cur,
            (unsigned)press, (unsigned)release,
            (press & NP_PAD_BTN_START) ? " START↓" : "",
            (release & NP_PAD_BTN_START) ? " START↑" : "");
    fflush(stderr);
    /* Gesture verdicts only on host sample path — not sio/invent (D-lag
     * would false-trigger multipress after a real release). */
    if (np_pad_trace_enabled() && slot == g_np.local_slot && tag &&
        (strcmp(tag, "local") == 0 || strcmp(tag, "stage") == 0 ||
         strcmp(tag, "dev") == 0)) {
        if (press & NP_PAD_BTN_START)
            np_pad_trace_start_down(tag, sim);
        if (release & NP_PAD_BTN_START)
            np_pad_trace_start_up(tag, sim);
    }
}

/* §63/§77/§79: snap load restores guest SIO from the savestate. Host-side
 * edge trackers and the local tip pipeline must preserve *logical* held
 * buttons across Replay/skip-snap — never synthesize idle→held for a button
 * the player kept down through the transition. */
static uint16_t g_sio_pad_prev[PSX_MAX_PLAYERS];
static uint8_t g_sio_pad_have[PSX_MAX_PLAYERS];
static uint16_t g_inv_pad_prev[PSX_MAX_PLAYERS];
static uint8_t g_inv_pad_have[PSX_MAX_PLAYERS];
static uint16_t g_local_pad_prev = 0xFFFFu;
static int g_local_pad_have;
static uint16_t g_live_trace_prev = 0xFFFFu;
static int g_live_trace_have;
static uint16_t g_dev_trace_prev = 0xFFFFu;
static int g_dev_trace_have;
static uint16_t g_tip_trace_prev = 0xFFFFu;
static int g_tip_trace_have;
static uint32_t g_sio_apply_sim[PSX_MAX_PLAYERS];
static uint16_t g_sio_apply_btn[PSX_MAX_PLAYERS];
static uint8_t g_sio_apply_n[PSX_MAX_PLAYERS];
static uint8_t g_sio_apply_have[PSX_MAX_PLAYERS];

void psx_netplay_on_rb_snap_loaded(void)
{
    int i;
    int local;
    uint16_t sample;
    uint16_t sdl_raw;
    uint16_t continuity;

    for (i = 0; i < PSX_MAX_PLAYERS; ++i) {
        g_sio_pad_prev[i] = sio_get_pad_buttons_slot(i);
        g_sio_pad_have[i] = 1u;
        g_inv_pad_prev[i] = g_sio_pad_prev[i];
        g_inv_pad_have[i] = 1u;
        g_sio_apply_have[i] = 0;
        g_sio_apply_n[i] = 0;
    }

    local = g_np.local_slot;
    /* Continuity prefers last staged (what tip produce will sample), else
     * live, else restored SIO — no debounce rewrite. */
    sdl_raw = g_np.live_valid ? g_np.live.buttons : 0xFFFFu;
    if (g_np.staged_valid)
        sample = g_np.staged.buttons;
    else if (g_np.live_valid)
        sample = g_np.live.buttons;
    else if (local >= 0 && local < PSX_MAX_PLAYERS)
        sample = g_sio_pad_prev[local];
    else
        sample = 0xFFFFu;
    continuity = sample;

    if (local >= 0 && local < PSX_MAX_PLAYERS) {
        uint16_t sio_b = g_sio_pad_prev[local];
        /* Active-low: bit clear = held. Merge so any bit held in continuity
         * stays held in guest SIO — next tip apply cannot idle→held. */
        uint16_t merged = (uint16_t)(sio_b & continuity);
        if (np_pad_log_enabled() || np_pad_trace_enabled()) {
            fprintf(stderr,
                    "psxrecomp: rb snap-load pad continuity slot=%d "
                    "sdl=%04x sample=%04x seed=%04x sio=%04x merged=%04x\n",
                    local, (unsigned)sdl_raw,
                    (unsigned)sample, (unsigned)continuity,
                    (unsigned)sio_b, (unsigned)merged);
            fflush(stderr);
        }
        if (merged != sio_b)
            sio_set_pad_state_slot(local, merged);
        if (psx_start_consumer_enabled()) {
            uint32_t sim =
                g_np.session ? rnet_session_sim_tick(g_np.session) : 0u;
            psx_start_consumer_note(local, sim, merged);
        }
        g_sio_pad_prev[local] = merged;
        g_local_pad_prev = continuity;
        g_local_pad_have = 1;
        g_live_trace_prev = continuity;
        g_live_trace_have = 1;
        g_dev_trace_prev = continuity;
        g_dev_trace_have = 1;
        g_tip_trace_prev = continuity;
        g_tip_trace_have = 1;
    } else {
        g_local_pad_prev = 0xFFFFu;
        g_local_pad_have = 0;
        g_live_trace_have = 0;
        g_dev_trace_have = 0;
        g_tip_trace_have = 0;
    }

    if (np_pad_start_down(continuity)) {
        /* Still held through the load — do not open a new Start gesture. */
        g_start_gest_down_valid = 1;
        g_start_gest_up_valid = 0;
    } else {
        g_start_gest_down_valid = 0;
        g_start_gest_up_valid = 0;
        g_start_gest_last_stage_down_sim = 0xFFFFFFFFu;
    }
}

void psx_netplay_pad_trace_dev(int card, int fallback, int sdl_start,
                               uint16_t buttons)
{
    uint32_t sim;
    uint16_t prev;
    int start_bit;
    if (!np_pad_trace_enabled() || !psx_netplay_active())
        return;
    sim = g_np.session ? rnet_session_sim_tick(g_np.session) : 0u;
    start_bit = np_pad_start_down(buttons);
    prev = g_dev_trace_have ? g_dev_trace_prev : 0xFFFFu;
    if (!g_dev_trace_have || prev != buttons ||
        start_bit != np_pad_start_down(prev)) {
        fprintf(stderr,
                "psxrecomp: rb pad-trace dev sim=%u ms=%u card=%d fallback=%d "
                "sdl_start=%d start_bit=%d buttons=%04x→%04x%s%s %s\n",
                (unsigned)sim, (unsigned)np_pad_mono_ms(), card, fallback,
                sdl_start ? 1 : 0, start_bit ? 1 : 0, (unsigned)prev,
                (unsigned)buttons,
                (!np_pad_start_down(prev) && start_bit) ? " START↓" : "",
                (np_pad_start_down(prev) && !start_bit) ? " START↑" : "",
                np_pad_trace_ctx());
        fflush(stderr);
        if (!np_pad_start_down(prev) && start_bit)
            np_pad_trace_start_down("dev", sim);
        if (np_pad_start_down(prev) && !start_bit)
            np_pad_trace_start_up("dev", sim);
        g_dev_trace_prev = buttons;
        g_dev_trace_have = 1;
    }
}

static void apply_pad_slot(int slot, const PsxNetPad *pad)
{
    if (slot < 0 || slot >= g_np.slot_count || slot >= PSX_MAX_PLAYERS || !pad) return;
    const int on_tap = sio_pad_on_multitap(slot);
    if (psx_start_bisect_enabled() && slot == g_np.local_slot) {
        uint32_t sim = g_np.session ? rnet_session_sim_tick(g_np.session) : 0u;
        psx_start_bisect_log("sio", sim, -1, -1, -1,
                             np_pad_start_down(pad->buttons) ? 1 : 0, 0,
                             psx_netplay_rb_tip_holding(),
                             psx_netplay_is_resimulating());
    }
    if (np_pad_diag_enabled()) {
        uint16_t prev = g_sio_pad_have[slot] ? g_sio_pad_prev[slot] : 0xFFFFu;
        uint32_t sim = g_np.session ? rnet_session_sim_tick(g_np.session) : 0u;
        uint16_t press = (uint16_t)((uint16_t)(~pad->buttons) & prev);
        uint8_t apply_n = 1;
        if (g_sio_apply_have[slot] && g_sio_apply_sim[slot] == sim &&
            g_sio_apply_btn[slot] == pad->buttons) {
            if (g_sio_apply_n[slot] < 255u)
                g_sio_apply_n[slot]++;
            apply_n = g_sio_apply_n[slot];
        } else {
            g_sio_apply_sim[slot] = sim;
            g_sio_apply_btn[slot] = pad->buttons;
            g_sio_apply_n[slot] = 1u;
            g_sio_apply_have[slot] = 1u;
            apply_n = 1u;
        }
        np_pad_log_edge("sio", slot, sim, prev, pad->buttons);
        if (np_pad_trace_enabled() &&
            (prev != pad->buttons || apply_n > 1u)) {
            fprintf(stderr,
                    "psxrecomp: rb pad-trace sio sim=%u slot=%d buttons=%04x→%04x "
                    "apply_n=%u%s%s %s\n",
                    (unsigned)sim, slot, (unsigned)prev, (unsigned)pad->buttons,
                    (unsigned)apply_n,
                    (press & NP_PAD_BTN_START) ? " START↓" : "",
                    (((uint16_t)(~prev) & pad->buttons) & NP_PAD_BTN_START)
                        ? " START↑"
                        : "",
                    np_pad_trace_ctx());
            if (apply_n >= 2u && (press & NP_PAD_BTN_START)) {
                fprintf(stderr,
                        "psxrecomp: rb pad-trace VERDICT dup_sio slot=%d sim=%u "
                        "apply_n=%u (same sim Start↓ re-applied — §63 class) %s\n",
                        slot, (unsigned)sim, (unsigned)apply_n, np_pad_trace_ctx());
            }
            if ((press & NP_PAD_BTN_START) &&
                g_start_gest_last_stage_down_sim != 0xFFFFFFFFu) {
                int d = g_np.input_delay;
                uint32_t stage_sim = g_start_gest_last_stage_down_sim;
                uint32_t delta = (sim >= stage_sim) ? (sim - stage_sim) : 0u;
                if (delta == (uint32_t)(d < 0 ? 0 : d) || sim == stage_sim) {
                    fprintf(stderr,
                            "psxrecomp: rb pad-trace VERDICT sample_vs_apply "
                            "stage_sim=%u sio_sim=%u delta=%u D=%d "
                            "(real-delay pair — not a double press) %s\n",
                            (unsigned)stage_sim, (unsigned)sim, (unsigned)delta,
                            d, np_pad_trace_ctx());
                }
            }
            fflush(stderr);
        }
        g_sio_pad_prev[slot] = pad->buttons;
        g_sio_pad_have[slot] = 1u;
    }
    const int force_dig = on_tap && !sio_get_multitap_analog();
    sio_set_pad_connected(slot, 1);
    sio_set_pad_config_capable(slot, force_dig ? 0 : 1);
    sio_set_pad_state_slot(slot, pad->buttons);
    if (force_dig)
        sio_set_pad_sticks(slot, 0x80, 0x80, 0x80, 0x80);
    else
        sio_set_pad_sticks(slot, pad->lx, pad->ly, pad->rx, pad->ry);
    sio_request_pad_type(slot, (!force_dig && pad->analog) ? 1 : 0);
    if (psx_start_consumer_enabled()) {
        uint32_t sim = g_np.session ? rnet_session_sim_tick(g_np.session) : 0u;
        psx_start_consumer_note(slot, sim, pad->buttons);
    }
}

static void host_sample_local(rnet_u32 tick, RNetInputSample *out, void *ctx)
{
    NetplayState *st = (NetplayState *)ctx;
    PsxNetPad pad;
    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0xFFFFu;
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
    pad.analog = 1;
    pad.connected = 1;
    if (st->staged_valid) pad = st->staged;
    pad.connected = 1;
    encode_pad(&pad, out, tick);
}

static void host_publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *ctx)
{
    int i;
    int n;
    (void)tick;
    (void)ctx;
    if (!by_slot || slots <= 0) return;
    n = g_np.slot_count;
    if (n > slots) n = slots;
    if (n > PSX_MAX_PLAYERS) n = PSX_MAX_PLAYERS;
    force_session_pads_connected(n);
    for (i = 0; i < n; ++i) {
        PsxNetPad pad;
        decode_pad(&by_slot[i], &pad);
        apply_pad_slot(i, &pad);
    }
}

typedef struct {
    NetplayHashConfirm *hc;
    uint32_t            tick;
} NpHcGateCtx;

static uint8_t np_hash_confirm_promote_gate(void *ctx)
{
    NpHcGateCtx *g = (NpHcGateCtx *)ctx;
    if (!g || !g->hc) return 0;
    return netplay_hc_confirm_through(g->hc, g->tick);
}

static void np_publish_hist_sio(uint32_t tick)
{
    int i;
    force_session_pads_connected(g_np.slot_count);
    for (i = 0; i < g_np.slot_count && i < PSX_MAX_PLAYERS; ++i) {
        RNetRbFrame row;
        PsxNetPad pad;
        if (!netplay_ih_get(&g_np.ih, i, tick, &row))
            continue;
        netplay_ih_frame_to_pad(&row, &pad);
        apply_pad_slot(i, &pad);
    }
}

static void np_rb_apply_frame_slot(int slot, uint32_t tick, uint16_t buttons,
                                   int8_t sx, int8_t sy, uint8_t analog)
{
    RNetRbFrame row;
    PsxNetPad pad;
    (void)tick;
    memset(&row, 0, sizeof(row));
    row.tick = tick;
    row.buttons = buttons;
    row.stick_x = sx;
    row.stick_y = sy;
    row.analog = analog ? 1u : 0u;
    row.is_valid = 1;
    netplay_ih_frame_to_pad(&row, &pad);
    force_session_pads_connected(g_np.slot_count);
    apply_pad_slot(slot, &pad);
}

static void np_rb_bind_and_start(void)
{
    PsxNetplayRbBindings b;
    memset(&b, 0, sizeof(b));
    b.session = &g_np.session;
    b.cpu = &g_np.cpu;
    b.ih = &g_np.ih;
    b.hc = &g_np.hc;
    b.bios_checksum = &g_np.bios_checksum;
    b.entry_pc = &g_np.entry_pc;
    b.slot_count = &g_np.slot_count;
    b.local_slot = &g_np.local_slot;
    b.input_delay = &g_np.input_delay;
    b.publish_sio = np_publish_hist_sio;
    b.apply_frame_slot = np_rb_apply_frame_slot;
    psx_netplay_rb_bind(&b);
    psx_netplay_rb_start();
}

/* MotK digital (active-low): wire only releases buttons vs invent; sticks equal.
 * Used for FMV soft-promote releases only (menu releases must rewind). */
static int np_digital_release_only(const RNetInputContractFrame *pub,
                                   const RNetInputContractFrame *wire)
{
    uint16_t newly_pressed;
    uint16_t newly_released;
    if (!pub || !wire)
        return 0;
    if (pub->stick_x != wire->stick_x || pub->stick_y != wire->stick_y)
        return 0;
    if (pub->buttons == wire->buttons)
        return 0;
    newly_pressed = (uint16_t)((uint16_t)(~wire->buttons) & pub->buttons);
    newly_released = (uint16_t)((uint16_t)(~pub->buttons) & wire->buttons);
    return newly_pressed == 0 && newly_released != 0;
}

/* Wire has at least one newly pressed button vs published invent (active-low).
 * FMV skip uses this — promote-without-resim left host mid-movie while peer
 * already cut over. */
static int np_digital_new_press(const RNetInputContractFrame *pub,
                                const RNetInputContractFrame *wire)
{
    uint16_t newly_pressed;
    if (!pub || !wire)
        return 0;
    newly_pressed = (uint16_t)((uint16_t)(~wire->buttons) & pub->buttons);
    return newly_pressed != 0;
}

/* After promoting a digital release at `release_tick`, rewrite predicted hist
 * rows release_tick+1..end to the released pad. Hold-last invent ahead of the
 * release tip left `pub` still held → ghost second-release episodes (soak:
 * RIGHT release@1607 then another `pub=ffdf wire=ffff` @1634).
 *
 * §34: TipHold stalls sim at tip while coalesce promotes release at tip+N —
 * end must reach tip+runway, not only sim (sim <= release_tick was a no-op).
 * §63: outside TipHold, also scrub sim..sim+P so gap1 invent poison past the
 * current tip cannot re-hold Start/dpad before wire catches up (soak: sio
 * Start↓@5861 then invent Start↓@5868 while auth was release). */
static void np_scrub_ahead_predicted(int slot, rnet_u32 release_tick,
                                     const RNetRbFrame *released)
{
    rnet_u32 sim;
    rnet_u32 end;
    rnet_u32 t;
    unsigned n = 0;
    if (!released || !g_np.session)
        return;
    sim = rnet_session_sim_tick(g_np.session);
    end = sim;
    if (psx_netplay_rb_tip_holding()) {
        uint32_t tip = psx_netplay_rb_episode_target();
        uint32_t runway = psx_netplay_rb_tip_runway();
        uint32_t tip_end = tip + runway;
        if (tip_end > end)
            end = tip_end;
    } else {
        /* Pred depth default matches sched P=9; scrub invent rows Live will
         * admit before the next remote burst. */
        rnet_u32 ahead = sim + 9u;
        if (ahead > end)
            end = ahead;
    }
    if (end <= release_tick)
        return;
    for (t = release_tick + 1u; t <= end; ++t) {
        RNetRbFrame row;
        RNetRbFrame scrub;
        if (!netplay_ih_get(&g_np.ih, slot, t, &row))
            continue;
        if (!row.is_predicted)
            continue;
        if (row.buttons == released->buttons &&
            row.stick_x == released->stick_x &&
            row.stick_y == released->stick_y)
            continue;
        scrub = *released;
        scrub.tick = t;
        scrub.is_predicted = 0u;
        scrub.is_valid = 1u;
        if (netplay_ih_promote(&g_np.ih, slot, &scrub))
            n++;
    }
    if (n) {
        fprintf(stderr,
                "psxrecomp: rb scrub-ahead release slot=%d from=%u end=%u "
                "sim=%u n=%u btn=%04x\n",
                slot, (unsigned)release_tick, (unsigned)end, (unsigned)sim, n,
                (unsigned)released->buttons);
        fflush(stderr);
    }
}

/* TipHold Live is stalled at invent-cap while digital is held — predicted hist
 * rows past tip may not exist yet, so ordinary reconcile never sees the
 * release. Peek wire tip+1..tip+runway; on the first pad delta vs tip hist,
 * promote the span and tip-extend (real coalesce). */
static void np_tip_hold_coalesce_ahead(void)
{
    uint32_t tip;
    uint32_t runway;
    int slot;

    if (!g_np.rollback || !g_np.session)
        return;
    if (!psx_netplay_rb_tip_holding())
        return;
    if (psx_netplay_rb_load_pending() || psx_netplay_rb_is_resimulating())
        return;

    tip = psx_netplay_rb_episode_target();
    runway = psx_netplay_rb_tip_runway();
    if (tip == 0u || runway == 0u)
        return;

    for (slot = 0; slot < g_np.slot_count; ++slot) {
        RNetRbFrame tip_row;
        rnet_u32 edge = 0;
        rnet_u32 t;
        if (slot == g_np.local_slot)
            continue;
        if (!netplay_ih_get(&g_np.ih, slot, tip, &tip_row) || !tip_row.is_valid)
            continue;

        for (t = tip + 1u; t <= tip + runway; ++t) {
            RNetInputSample sample;
            PsxNetPad pad;
            RNetRbFrame wire_frame;
            rnet_u32 wire = np_sched_wire_for_sim(t);
            if (!rnet_session_peek_remote_input(g_np.session, slot, wire, &sample))
                break;
            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, t, 0, &wire_frame);
            if (wire_frame.buttons == tip_row.buttons &&
                wire_frame.stick_x == tip_row.stick_x &&
                wire_frame.stick_y == tip_row.stick_y)
                continue;
            edge = t;
            break;
        }
        if (!edge)
            continue;

        for (t = tip + 1u; t <= edge; ++t) {
            RNetInputSample sample;
            PsxNetPad pad;
            RNetRbFrame wire_frame;
            rnet_u32 wire = np_sched_wire_for_sim(t);
            if (!rnet_session_peek_remote_input(g_np.session, slot, wire, &sample))
                break;
            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, t, 0, &wire_frame);
            (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
        }
        {
            RNetInputSample sample;
            PsxNetPad pad;
            RNetRbFrame edge_frame;
            RNetInputContractFrame tip_c, edge_c;
            rnet_u32 wire = np_sched_wire_for_sim(edge);
            if (rnet_session_peek_remote_input(g_np.session, slot, wire, &sample)) {
                decode_pad(&sample, &pad);
                netplay_ih_pad_to_frame(&pad, edge, 0, &edge_frame);
                netplay_ih_frame_to_contract(&tip_row, &tip_c);
                netplay_ih_frame_to_contract(&edge_frame, &edge_c);
                if (np_digital_release_only(&tip_c, &edge_c))
                    np_scrub_ahead_predicted(slot, edge, &edge_frame);
            }
        }
        {
            static uint32_t s_last_log_tip, s_last_log_edge;
            static uint32_t s_coalesce_log_suppressed;
            if (tip != s_last_log_tip || edge != s_last_log_edge) {
                if (s_coalesce_log_suppressed) {
                    fprintf(stderr,
                            "psxrecomp: rb tip-hold coalesce-ahead "
                            "(suppressed %u repeats)\n",
                            (unsigned)s_coalesce_log_suppressed);
                    s_coalesce_log_suppressed = 0;
                }
                fprintf(stderr,
                        "psxrecomp: rb tip-hold coalesce-ahead tip=%u edge=%u "
                        "slot=%d tip_btn=%04x\n",
                        (unsigned)tip, (unsigned)edge, slot,
                        (unsigned)tip_row.buttons);
                fflush(stderr);
                s_last_log_tip = tip;
                s_last_log_edge = edge;
            } else {
                s_coalesce_log_suppressed++;
            }
        }
        if (psx_netplay_rb_tip_extend(edge, slot))
            return;
        /* tip-extend refused (span cap / different seat) — open via begin. */
        if (psx_netplay_rb_begin_rewind(edge, slot))
            return;
        /* Still tip-holding with an unreabsorbed wire edge: block quiet
         * finalize so we do not commit and drop the release (§34 soak). */
        psx_netplay_rb_tip_hold_block_quiet(1);
    }
}

/* Late authoritative wire vs published predicted rows → promote or queue rewind.
 * Diag: promote-no-resim means hist took the late pad but guest sim did not
 * rewind — looks like "remote input rejected" when cadence already drifted.
 * §46: button mispredicts whose digests already match through t (HC watermark)
 * promote hist silently — no episode. Loading-screen clicks / ignored pads
 * stay corrected in history without a rubber-band resim. */
static int np_hc_silent_promote_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        const char *e = getenv("PSX_RB_HC_SILENT");
        s_on = (e && e[0]) ? (atoi(e) != 0) : 1;
        fprintf(stderr,
                "psxrecomp: rb hc-silent promote %s "
                "(button mispredict + hash_confirm through tick → hist only; "
                "PSX_RB_HC_SILENT=0 disables)\n",
                s_on ? "ON" : "OFF");
        fflush(stderr);
    }
    return s_on;
}

static void np_rollback_reconcile_wire(void)
{
    rnet_u32 sim;
    rnet_u32 t;
    int slot;
    int promote_sweep;
    int cooldown;
    int fmv_defer;
    int no_resim;
    /* Per-pump counters (one summary line when anything interesting happens). */
    unsigned n_no_resim = 0;
    unsigned n_soft_release = 0;
    unsigned n_hc_silent = 0;
    unsigned n_contract_promote = 0;
    unsigned n_episode_open = 0;
    unsigned n_begin_refused = 0;
    rnet_u32 first_no_resim_t = 0;
    int first_no_resim_slot = -1;
    uint16_t first_pub_btn = 0;
    uint16_t first_wire_btn = 0;
    rnet_u32 first_hc_t = 0;
    int first_hc_slot = -1;
    uint16_t first_hc_pub = 0;
    uint16_t first_hc_wire = 0;
    rnet_u32 first_rewind_t = 0;
    int first_rewind_slot = -1;
    const char *no_resim_why = NULL;
    RNetInputContractParams params;
    NpHcGateCtx gate_ctx;
    RNetInputContractHostGates gates;

    if (!g_np.rollback || !g_np.session) return;
    if (!rnet_session_is_running(g_np.session)) return;

    rnet_input_contract_params_init_defaults(&params);
    memset(&gates, 0, sizeof(gates));
    gate_ctx.hc = &g_np.hc;
    gates.ctx = &gate_ctx;
    gates.hash_confirm_promote = np_hash_confirm_promote_gate;

    promote_sweep = psx_netplay_rb_take_promote_sweep();
    cooldown = psx_netplay_rb_rewind_suppressed();
    fmv_defer = psx_netplay_rb_fmv_defer_rewind();
    {
        /* Media + short settle (§26/§100): promote-only; invent off during
         * live FMV even with MEDIA_KF (avoids GAP1→3.7MB KF xfer). Post-FMV
         * menus invent+resim. */
        int fmv_lock = psx_netplay_rb_lockstep_no_invent();
        no_resim = promote_sweep || cooldown || fmv_lock;
        if (promote_sweep)
            no_resim_why = "sweep";
        else if (cooldown)
            no_resim_why = "cooldown";
        else if (fmv_lock)
            no_resim_why = "fmv-lockstep";
    }
    sim = rnet_session_sim_tick(g_np.session);

    /* §54: deferred rewind for a completed-tick correction swallowed by
     * cooldown/sweep. HC confirm through the tick proves both peers matched
     * after it (mispredict was cosmetic / already healed) — drop. Otherwise
     * open the episode the cooldown suppressed. */
    if (s_deferred_rw_valid && !no_resim && !fmv_defer &&
        !g_np.pending_rewind && g_np.rollback) {
        if (netplay_hc_confirm_through(&g_np.hc, s_deferred_rw_tick)) {
            s_deferred_rw_valid = 0;
        } else if (!psx_netplay_rb_active() && !psx_netplay_rb_tip_holding() &&
                   !psx_netplay_rb_load_pending()) {
            if (!s_deferred_rw_logged) {
                fprintf(stderr,
                        "psxrecomp: rb deferred rewind t=%u slot=%d "
                        "(completed-tick correction was promote-only in "
                        "cooldown)\n",
                        (unsigned)s_deferred_rw_tick, s_deferred_rw_slot);
                fflush(stderr);
                s_deferred_rw_logged = 1;
            }
            if (psx_netplay_rb_begin_rewind(s_deferred_rw_tick,
                                            s_deferred_rw_slot)) {
                g_np.needs_advance = 0;
                s_deferred_rw_valid = 0;
            }
            /* refused (transient gate) — keep pending, retry next pump */
        }
    }
    for (slot = 0; slot < g_np.slot_count; ++slot) {
        if (slot == g_np.local_slot) continue;
        for (t = (sim > 64u) ? (sim - 64u) : 0u; t <= sim; ++t) {
            RNetRbFrame published;
            RNetInputSample sample;
            RNetRbFrame wire_frame;
            RNetInputContractFrame pub_c, wire_c;
            RNetInputContractDecision d;
            uint8_t completed;
            PsxNetPad pad;
            rnet_u32 wire;
            int pads_differ;
            int buttons_differ;

            if (!netplay_ih_get(&g_np.ih, slot, t, &published))
                continue;
            if (!published.is_predicted)
                continue;
            /* Hist is sim-keyed; §44 consumption mapping owns sim→wire. */
            wire = np_sched_wire_for_sim(t);
            if (!rnet_session_peek_remote_input(g_np.session, slot, wire, &sample))
                continue;

            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, t, 0, &wire_frame);
            netplay_ih_frame_to_contract(&published, &pub_c);
            netplay_ih_frame_to_contract(&wire_frame, &wire_c);
            buttons_differ = (pub_c.buttons != wire_c.buttons);
            pads_differ = buttons_differ ||
                          (pub_c.stick_x != wire_c.stick_x) ||
                          (pub_c.stick_y != wire_c.stick_y);
            completed = (sim > t) ? 1u : 0u;

            /* §46: digital mispredict whose FRAME_COMMIT digests already
             * matched through t — both peers agree the guest state after
             * this tick. Repair hist (and scrub sticky hold-last ahead on
             * release) without opening an episode. Distinct from the old
             * ungated menu soft-promote that forked RAM: HC fail-closed
             * means a real state-affecting press still rewinds. */
            if (buttons_differ && completed &&
                np_hc_silent_promote_enabled() &&
                netplay_hc_confirm_through(&g_np.hc, t)) {
                (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                if (np_digital_release_only(&pub_c, &wire_c))
                    np_scrub_ahead_predicted(slot, t, &wire_frame);
                if (n_hc_silent == 0) {
                    first_hc_t = t;
                    first_hc_slot = slot;
                    first_hc_pub = pub_c.buttons;
                    first_hc_wire = wire_c.buttons;
                }
                n_hc_silent++;
                continue;
            }

            /* Feed the timesync pacer ONLY on a genuine mispredict that
             * will actually need correction (not HC-silent above). */
            if (pads_differ)
                np_sched_note_mispredict((sim >= t) ? (sim - t) : 0u);
            /* After commit/realign: flush invent poison without another episode. */
            if (no_resim) {
                (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                if (pads_differ) {
                    /* §54: a COMPLETED tick was simmed with the wrong pad and
                     * this promote repairs only history — the live state is
                     * now forked. Remember the earliest such tick and re-open
                     * an episode once the cooldown/sweep window ends (unless
                     * HC proves both peers matched through it anyway). */
                    if (completed &&
                        (!s_deferred_rw_valid || t < s_deferred_rw_tick)) {
                        s_deferred_rw_valid = 1;
                        s_deferred_rw_tick = t;
                        s_deferred_rw_slot = slot;
                        s_deferred_rw_logged = 0;
                    }
                    if (n_no_resim == 0) {
                        first_no_resim_t = t;
                        first_no_resim_slot = slot;
                        first_pub_btn = pub_c.buttons;
                        first_wire_btn = wire_c.buttons;
                    }
                    n_no_resim++;
                }
                continue;
            }
            gate_ctx.tick = t;
            d = rnet_input_contract_stick_replace_decide(
                &pub_c, &wire_c, completed, &params, &gates);
            if (rnet_input_contract_decision_is_rewind(d)) {
                /* FMV/settle only: soft-promote releases (skip must rewind on
                 * press). Menu soft-promote + hold-last invent forked RAM
                 * (sticky Up skipped resim). Live invent is hold-last again —
                 * menu releases open a real episode unless §46 HC-silent. */
                if (fmv_defer && np_digital_release_only(&pub_c, &wire_c)) {
                    (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                    np_scrub_ahead_predicted(slot, t, &wire_frame);
                    if (n_soft_release == 0) {
                        first_no_resim_t = t;
                        first_no_resim_slot = slot;
                        first_pub_btn = pub_c.buttons;
                        first_wire_btn = wire_c.buttons;
                    }
                    n_soft_release++;
                    continue;
                }
                /* §48: post-FMV UNLOCK_GRACE — invent is on (§26) but sticky
                 * hold-last D-pad vs wire release must not open tip episodes
                 * into the title/menu (soak: pub=ffef wire=ffff @902). */
                if (published.is_predicted &&
                    np_digital_release_only(&pub_c, &wire_c) &&
                    psx_netplay_rb_fmv_unlock_grace_active()) {
                    (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                    np_scrub_ahead_predicted(slot, t, &wire_frame);
                    if (n_soft_release == 0) {
                        first_no_resim_t = t;
                        first_no_resim_slot = slot;
                        first_pub_btn = pub_c.buttons;
                        first_wire_btn = wire_c.buttons;
                        no_resim_why = "unlock-grace-release";
                    }
                    n_soft_release++;
                    continue;
                }
                /* FMV: non-press pad noise → promote only (avoid CD thrash). */
                if (fmv_defer && !np_digital_new_press(&pub_c, &wire_c)) {
                    (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                    if (pads_differ) {
                        if (n_no_resim == 0) {
                            first_no_resim_t = t;
                            first_no_resim_slot = slot;
                            first_pub_btn = pub_c.buttons;
                            first_wire_btn = wire_c.buttons;
                            no_resim_why = "fmv-nopress";
                        }
                        n_no_resim++;
                    }
                    continue;
                }
                if (!g_np.pending_rewind) {
                    g_np.pending_rewind = 1;
                    g_np.pending_rewind_tick = t;
                    g_np.pending_rewind_slot = slot;
                    g_np.ih.rewind_count++;
                    if (g_np.rollback) {
                        if (first_rewind_slot < 0) {
                            first_rewind_t = t;
                            first_rewind_slot = slot;
                            first_pub_btn = pub_c.buttons;
                            first_wire_btn = wire_c.buttons;
                        }
                        /* Promote BEFORE begin — seal_inputs reads hist for the
                         * local seat and publish_sio falls back to hist before
                         * peer SEAL_ROWS land. Skipping promote left invent-idle
                         * pads sealed/published across the skip press tick. */
                        (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                        if (np_digital_release_only(&pub_c, &wire_c))
                            np_scrub_ahead_predicted(slot, t, &wire_frame);
                        if (psx_netplay_rb_begin_rewind(t, slot)) {
                            g_np.needs_advance = 0;
                            n_episode_open++;
                        } else {
                            n_begin_refused++;
                            if (psx_netplay_rb_tip_holding())
                                psx_netplay_rb_tip_hold_block_quiet(1);
                        }
                        g_np.pending_rewind = 0;
                    }
                }
            } else if (pads_differ) {
                (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                if (np_digital_release_only(&pub_c, &wire_c))
                    np_scrub_ahead_predicted(slot, t, &wire_frame);
                n_contract_promote++;
            } else {
                (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
            }
        }
    }

    /* TipHold invent-cap stall: peek wire past tip for release/press edges that
     * have no predicted hist row yet (Live never invented them). */
    if (!g_np.pending_rewind)
        np_tip_hold_coalesce_ahead();

    if (n_no_resim || n_soft_release || n_hc_silent || n_episode_open ||
        n_begin_refused) {
        static uint32_t s_log_sim;
        static unsigned s_suppress;
        if (s_log_sim == sim && !n_episode_open && !n_hc_silent) {
            s_suppress++;
        } else {
            if (s_suppress) {
                fprintf(stderr,
                        "psxrecomp: rb wire diag (+%u similar pumps suppressed)\n",
                        s_suppress);
                s_suppress = 0;
            }
            s_log_sim = sim;
            if (n_hc_silent) {
                fprintf(stderr,
                        "psxrecomp: rb wire hc-silent-promote sim=%u n=%u "
                        "first_t=%u slot=%d pub=%04x wire=%04x "
                        "(digests matched through t — hist only, no resim)\n",
                        (unsigned)sim, n_hc_silent,
                        (unsigned)first_hc_t, first_hc_slot,
                        (unsigned)first_hc_pub, (unsigned)first_hc_wire);
            }
            if (n_no_resim) {
                fprintf(stderr,
                        "psxrecomp: rb wire promote-no-resim sim=%u n=%u reason=%s "
                        "first_t=%u slot=%d pub=%04x wire=%04x "
                        "(hist ok; sim NOT rolled back — remote feels rejected)\n",
                        (unsigned)sim, n_no_resim,
                        no_resim_why ? no_resim_why : "?",
                        (unsigned)first_no_resim_t, first_no_resim_slot,
                        (unsigned)first_pub_btn, (unsigned)first_wire_btn);
            }
            if (n_soft_release) {
                fprintf(stderr,
                        "psxrecomp: rb wire soft-promote release-only sim=%u n=%u "
                        "first_t=%u slot=%d pub=%04x wire=%04x%s%s\n",
                        (unsigned)sim, n_soft_release,
                        (unsigned)first_no_resim_t, first_no_resim_slot,
                        (unsigned)first_pub_btn, (unsigned)first_wire_btn,
                        no_resim_why ? " reason=" : "",
                        no_resim_why ? no_resim_why : "");
            }
            if (n_episode_open || n_begin_refused) {
                fprintf(stderr,
                        "psxrecomp: rb wire rewind-request sim=%u t=%u slot=%d "
                        "pub=%04x wire=%04x → episode_open=%u begin_refused=%u "
                        "contract_promote=%u\n",
                        (unsigned)sim, (unsigned)first_rewind_t, first_rewind_slot,
                        (unsigned)first_pub_btn, (unsigned)first_wire_btn,
                        n_episode_open, n_begin_refused, n_contract_promote);
            }
            fflush(stderr);
        }
    }
}


/* §47: Replay participates in the protocol like Live — produce local tip
 * from the live physical pad while consuming remote confirmed rows. */
static void np_rb_produce_local_tip_for_sim(rnet_u32 sim)
{
    if (!g_np.session || !psx_netplay_active())
        return;
    if (!rnet_session_is_running(g_np.session))
        return;
    if (sim != rnet_session_sim_tick(g_np.session))
        return;
    if (psx_start_bisect_no_replay_produce())
        return;
    if (g_np.live_valid) {
        uint16_t raw = g_np.live.buttons;
        g_np.staged = g_np.live;
        psx_netplay_normalize_pad(&g_np.staged);
        g_np.staged_valid = 1;
        if (psx_start_bisect_enabled()) {
            psx_start_bisect_log(
                "tip", (uint32_t)sim, -1,
                np_pad_start_down(raw) ? 1 : 0,
                np_pad_start_down(g_np.staged.buttons) ? 1 : 0, -1, 0,
                psx_netplay_rb_tip_holding(), 1);
        }
        if (np_pad_trace_enabled()) {
            uint16_t prev = g_tip_trace_have ? g_tip_trace_prev : 0xFFFFu;
            uint16_t cur = g_np.staged.buttons;
            uint32_t wire = np_sched_wire_for_sim((uint32_t)sim);
            if (!g_tip_trace_have || prev != cur) {
                fprintf(stderr,
                        "psxrecomp: rb pad-trace tip-produce sim=%u wire=%u "
                        "buttons=%04x→%04x%s%s path=replay/finish %s\n",
                        (unsigned)sim, (unsigned)wire, (unsigned)prev,
                        (unsigned)cur,
                        (!np_pad_start_down(prev) && np_pad_start_down(cur))
                            ? " START↓"
                            : "",
                        (np_pad_start_down(prev) && !np_pad_start_down(cur))
                            ? " START↑"
                            : "",
                        np_pad_trace_ctx());
                if (!np_pad_start_down(prev) && np_pad_start_down(cur) &&
                    !(g_local_pad_have && np_pad_start_down(g_local_pad_prev))) {
                    fprintf(stderr,
                            "psxrecomp: rb pad-trace VERDICT tip_without_stage "
                            "sim=%u (produce Start↓ with no staged Start — "
                            "§58-class bypass) %s\n",
                            (unsigned)sim, np_pad_trace_ctx());
                }
                fflush(stderr);
                g_tip_trace_prev = cur;
                g_tip_trace_have = 1;
            }
        }
    }
    g_np.latched_for_tick = 0;
    (void)rnet_session_prepare_local_tip(g_np.session, sim);
    g_np.latched_for_tick = 1;
    g_np.latched_sim_tick = (uint32_t)sim;
}

/* Rollback admit: tip + invent remotes within P of remote tip; stall outside.
 * BattleShip phase_lock: invent only when wire_need <= highest_remote + P. */
static int np_try_admit_rollback(void)
{
    rnet_u32 sim = rnet_session_sim_tick(g_np.session);
    RNetInputSample sample;
    RNetRbFrame row;
    PsxNetPad pad;
    RNetSessionStats st;
    rnet_u32 wire;
    int slot;
    int pred;
    int any_invent = 0;

    /* Tick FMV→settle tracker every admit (even when remotes are present). */
    (void)psx_netplay_rb_lockstep_no_invent();

    /* Mid-session DELAY_SYNC may have committed on the last advance. */
    np_sched_sync_delay_from_session();

    if (!rnet_session_prepare_local_tip(g_np.session, sim)) {
        np_sched_set_admit_stall("prepare_tip");
        return 0;
    }

    /* §44: consume at wire = sim (real delay); production runs at sim + D. */
    wire = np_sched_wire_for_sim(sim);
    pred = g_np.input_prediction;
    if (pred < 2) pred = 2;
    if (pred > 16) pred = 16;

    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);

    /* Scheduler gate: tip cadence, timesync pacing, cushion rebuild,
     * auto-delay resolution, runway/phase telemetry. */
    if (np_sched_pre_admit(sim, wire, &st))
        return 0;

    /* TipHold past invent-cap: never invent (that caused tip-extend rereplay
     * cliffs). Advance only when every remote wire row is present *and* all
     * pads (local + remote) are idle (0xFFFF). Live-walking held digital
     * through the runway fed MotK menu key-repeat (~24 extra navigations per
     * press) and burned the coalesce window before tip-extend could absorb
     * the release (soak: tip-extend=0, tip-hold→commit adjacent). Missing
     * remotes or any held button → stall; wall-clock quiet / coalesce-ahead
     * owns the runway. */
    if (psx_netplay_rb_tip_holding()) {
        uint32_t tip = psx_netplay_rb_episode_target();
        uint32_t slack = psx_netplay_rb_tip_hold_invent_slack();
        uint32_t from = psx_netplay_rb_tip_hold_rereplay_from();
        /* §81: park Live at the POST tip we deferred from — not the raised
         * coalesce tip. §80 parked at tip, so Live walked with tip-extends
         * and every flush logged parked=0 (invent-snap reload). */
        if (from > 0u && sim > from) {
            np_sched_set_admit_stall("tip_hold_deferred");
            return 0;
        }
        if (tip > 0u && sim > tip + slack) {
            int missing = 0;
            int held = 0;
            for (slot = 0; slot < g_np.slot_count; ++slot) {
                if (slot == g_np.local_slot) {
                    if (rnet_session_peek_input(g_np.session, slot, wire,
                                                &sample)) {
                        decode_pad(&sample, &pad);
                        if (pad.buttons != 0xFFFFu)
                            held = 1;
                    } else if (g_np.staged_valid &&
                               g_np.staged.buttons != 0xFFFFu) {
                        held = 1;
                    }
                    continue;
                }
                if (!rnet_session_peek_remote_input(g_np.session, slot, wire,
                                                    &sample)) {
                    missing = 1;
                    break;
                }
                decode_pad(&sample, &pad);
                if (pad.buttons != 0xFFFFu)
                    held = 1;
            }
            if (missing || held) {
                np_sched_set_admit_stall(missing ? "tip_hold_remote"
                                                 : "tip_hold_buttons");
                return 0;
            }
        }
    }

    for (slot = 0; slot < g_np.slot_count; ++slot) {
        if (slot == g_np.local_slot) {
            if (rnet_session_peek_input(g_np.session, slot, wire, &sample)) {
                decode_pad(&sample, &pad);
                netplay_ih_pad_to_frame(&pad, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            } else if (g_np.staged_valid) {
                netplay_ih_pad_to_frame(&g_np.staged, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            } else {
                memset(&pad, 0, sizeof(pad));
                pad.buttons = 0xFFFFu;
                pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
                pad.analog = 0; /* MotK digital default; never invent DualShock */
                pad.connected = 1;
                netplay_ih_pad_to_frame(&pad, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            }
            continue;
        }

        if (rnet_session_peek_remote_input(g_np.session, slot, wire, &sample)) {
            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, sim, 0, &row);
            (void)netplay_ih_put(&g_np.ih, slot, &row);
            /* Remote arrived — gap1 grace (if any) did its job. */
            np_sched_note_remote_hit();
        } else {
            const char *invent_reason = NULL;

            /* Scheduler decision: stall (grace / pcap freeze / cushion
             * rebuild / lockstep) or invent hold-last now. */
            if (np_sched_on_remote_miss(slot, sim, wire, &st, pred,
                                        &invent_reason))
                return 0;

            /* MotK digital: hold-last. Idle invent re-mismatched every held
             * D-pad tick after commit → episode storm / char-select freeze.
             * Menu release soft-promote is off (see reconcile) so sticky Up
             * cannot skip a needed resim. Seal gap-fill stays idle. */
            (void)invent_reason;
            any_invent = 1;
            (void)netplay_ih_invent_hold_last(&g_np.ih, slot, sim, &row);
            if (np_pad_diag_enabled() && row.is_valid) {
                uint16_t prev = g_inv_pad_have[slot] ? g_inv_pad_prev[slot] : 0xFFFFu;
                if (slot >= 0 && slot < PSX_MAX_PLAYERS) {
                    np_pad_log_edge("invent", slot, sim, prev, row.buttons);
                    g_inv_pad_prev[slot] = row.buttons;
                    g_inv_pad_have[slot] = 1u;
                }
            }
        }
    }

    np_sched_post_admit(any_invent);

    np_publish_hist_sio(sim);
    g_np.needs_advance = 1;
    return 1;
}

int psx_netplay_active(void)
{
    return g_np.active && g_np.session != NULL;
}

int psx_netplay_is_running(void)
{
    return psx_netplay_active() && rnet_session_is_running(g_np.session);
}

const char *psx_netplay_transport_name(void)
{
    if (!psx_netplay_active()) return "none";
    return g_np.use_ice ? "ice" : "lan";
}

int psx_netplay_ice_failed(void)
{
#if defined(RNET_ENABLE_ICE)
    if (!psx_netplay_active() || !g_np.use_ice)
        return 0;
    return rnet_session_ice_state(g_np.session) == RNET_ICE_STATE_FAILED;
#else
    return 0;
#endif
}

int psx_netplay_local_slot(void)
{
    return psx_netplay_active() ? g_np.local_slot : -1;
}

int psx_netplay_input_player(void)
{
    return psx_netplay_active() ? g_np.input_player : 0;
}

uint32_t psx_netplay_sim_tick(void)
{
    if (!psx_netplay_active()) return 0;
    return rnet_session_sim_tick(g_np.session);
}

void psx_netplay_stage_local(const PsxNetPad *pad)
{
    if (!pad) {
        g_np.staged_valid = 0;
        g_np.live_valid = 0;
        return;
    }
    /* Always refresh live physical snapshot first. TipHold invent-cap parks
     * sim (and therefore the latched staged sample); SAFETY/quiet must still
     * observe a real release (§36). */
    g_np.live = *pad;
    psx_netplay_normalize_pad(&g_np.live);
    g_np.live_valid = 1;
    /* Once running, freeze the first sample for the current sim tick so
     * re-admits / barrier retries cannot change the INPUT_CONFIRM hash. */
    if (psx_netplay_active() && rnet_session_is_running(g_np.session)) {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        if (g_np.latched_for_tick && g_np.latched_sim_tick == t) {
            /* Latched: still report live Start edges (SDL drop under TipHold
             * / barrier spin) — staged frozen so pad-edge local stays quiet. */
            if (np_pad_trace_enabled()) {
                uint16_t prev = g_live_trace_have ? g_live_trace_prev : 0xFFFFu;
                uint16_t cur = g_np.live.buttons;
                if (!g_live_trace_have || prev != cur) {
                    if ((np_pad_start_down(prev) != np_pad_start_down(cur)) ||
                        prev != cur) {
                        fprintf(stderr,
                                "psxrecomp: rb pad-trace live-only sim=%u "
                                "(latched) buttons=%04x→%04x%s%s %s\n",
                                (unsigned)t, (unsigned)prev, (unsigned)cur,
                                (!np_pad_start_down(prev) &&
                                 np_pad_start_down(cur))
                                    ? " START↓"
                                    : "",
                                (np_pad_start_down(prev) &&
                                 !np_pad_start_down(cur))
                                    ? " START↑"
                                    : "",
                                np_pad_trace_ctx());
                        fflush(stderr);
                        if (!np_pad_start_down(prev) && np_pad_start_down(cur))
                            np_pad_trace_start_down("live", t);
                        if (np_pad_start_down(prev) && !np_pad_start_down(cur))
                            np_pad_trace_start_up("live", t);
                    }
                    g_live_trace_prev = cur;
                    g_live_trace_have = 1;
                }
            }
            return;
        }
        {
            uint16_t raw;
            g_np.staged = *pad;
            psx_netplay_normalize_pad(&g_np.staged);
            raw = g_np.staged.buttons;
            if (psx_start_bisect_enabled()) {
                int th = psx_netplay_rb_tip_holding();
                int rs = psx_netplay_is_resimulating();
                int slot = g_np.local_slot;
                uint16_t sio_b = (slot >= 0 && slot < PSX_MAX_PLAYERS)
                                     ? sio_get_pad_buttons_slot(slot)
                                     : 0xFFFFu;
                psx_start_bisect_log(
                    "stage", t, -1,
                    np_pad_start_down(raw) ? 1 : 0,
                    np_pad_start_down(g_np.staged.buttons) ? 1 : 0,
                    np_pad_start_down(sio_b) ? 1 : 0, 1, th, rs);
            }
            if (np_pad_diag_enabled()) {
                uint16_t prev = g_local_pad_have ? g_local_pad_prev : 0xFFFFu;
                uint16_t cur = g_np.staged.buttons;
                np_pad_log_edge("local", g_np.local_slot, t, prev, cur);
                if (np_pad_trace_enabled() &&
                    (!g_local_pad_have || prev != cur)) {
                    fprintf(stderr,
                            "psxrecomp: rb pad-trace stage sim=%u "
                            "buttons=%04x→%04x latch=new%s%s %s\n",
                            (unsigned)t, (unsigned)prev, (unsigned)cur,
                            (!np_pad_start_down(prev) && np_pad_start_down(cur))
                                ? " START↓"
                                : "",
                            (np_pad_start_down(prev) && !np_pad_start_down(cur))
                                ? " START↑"
                                : "",
                            np_pad_trace_ctx());
                    fflush(stderr);
                }
                g_local_pad_prev = cur;
                g_local_pad_have = 1;
                g_live_trace_prev = g_np.live.buttons;
                g_live_trace_have = 1;
            }
            g_np.staged_valid = 1;
            g_np.latched_for_tick = 1;
            g_np.latched_sim_tick = t;
            return;
        }
    }
    /* Linking: keep refreshing released/local pads until START. */
    g_np.staged = *pad;
    psx_netplay_normalize_pad(&g_np.staged);
    g_np.staged_valid = 1;
}

int psx_netplay_needs_local_sample(void)
{
    if (!psx_netplay_active()) return 0;
    if (!rnet_session_is_running(g_np.session)) return 1; /* linking */
    {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        return !(g_np.latched_for_tick && g_np.latched_sim_tick == t);
    }
}

int psx_netplay_live_pad_buttons(uint16_t *out)
{
    if (!out)
        return 0;
    if (!g_np.live_valid) {
        *out = 0xFFFFu;
        return 0;
    }
    *out = g_np.live.buttons;
    return 1;
}

int psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    if (!psx_netplay_active()) return 0;
    return rnet_session_input_desync(g_np.session, tick, local_hash, remote_hash);
}

int psx_netplay_peer_disconnected(uint32_t timeout_ms)
{
    if (!psx_netplay_active()) return 0;
    /* timeout_ms == 0: BYE / peer_gone only (no silence timeout). Used during
     * load barriers where INPUT is suppressed for seconds. */
    return rnet_session_peer_disconnected(g_np.session, (rnet_u64)timeout_ms);
}

void psx_netplay_touch_peer_liveness(void)
{
    if (!psx_netplay_active() || !g_np.session) return;
    rnet_session_touch_peer_liveness(g_np.session);
}

uint32_t psx_netplay_running_liveness_timeout_ms(void)
{
    uint32_t sim;
    const char *tag;
    if (!psx_netplay_active() || !g_np.session)
        return 1500u;
    sim = rnet_session_sim_tick(g_np.session);
    tag = np_sched_admit_stall_tag();
    if (tag && tag[0] &&
        (strcmp(tag, "boot_tip_wait") == 0 ||
         strcmp(tag, "boot_dig0_wait") == 0 ||
         strcmp(tag, "pcap_freeze") == 0))
        return 0u;
    /* Free-run + tick-0 dig publish no INPUT; rematch dig can exceed 1.5s. */
    if (sim < 48u)
        return 0u;
    return 1500u;
}

static void np_diag_capture(const PsxNetplayConfig *cfg, int slots)
{
    const char *arch = "p2p";
    int players;
    if (!cfg) return;
    if (cfg->force_input_relay)
        arch = "server_relay";
    else if (slots >= 3)
        arch = "host_relay";
    players = cfg->player_count > 0 ? cfg->player_count : slots;
    if (players < 1) players = slots;
    snprintf(g_np_diag_arch, sizeof(g_np_diag_arch), "%s", arch);
    g_np_diag_max_players = slots;
    g_np_diag_player_count = players;
    g_np_diag_configured = 1;
}

#if defined(__linux__)
static int peer_is_loopback(const char *peer_hostport)
{
    if (!peer_hostport || !peer_hostport[0]) return 0;
    if (strncmp(peer_hostport, "127.", 4) == 0) return 1;
    if (strncmp(peer_hostport, "localhost:", 10) == 0) return 1;
    if (strncmp(peer_hostport, "::1:", 4) == 0) return 1;
    if (strcmp(peer_hostport, "::1") == 0) return 1;
    return 0;
}

/* Same-machine MotK FMV: lockstep syncs both peers' MDEC peaks; pinning each
 * slot to a disjoint CPU half cut headless FMV ~40 → ~45 in A/B. */
static void pin_localhost_peer_cpus(int local_slot)
{
    long ncpu;
    cpu_set_t set;
    int i, lo, hi;

    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 4) return;
    CPU_ZERO(&set);
    if (local_slot <= 0) {
        lo = 0;
        hi = (int)(ncpu / 2);
    } else {
        lo = (int)(ncpu / 2);
        hi = (int)ncpu;
    }
    for (i = lo; i < hi; i++)
        CPU_SET(i, &set);
    (void)sched_setaffinity(0, sizeof(set), &set);
}
#endif

#if defined(PSX_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
static void host_on_signal(const RNetSignal *msg, void *ctx)
{
    (void)ctx;
    if (!msg) return;
    (void)psx_lobby_send_signal((int)msg->type, (int)msg->flag, msg->text);
}

static void drain_lobby_signals(void)
{
    int type = 0, flag = 0;
    char text[2048];
    if (!g_np.session) return;
    while (psx_lobby_poll_signal(&type, &flag, text, sizeof(text))) {
        RNetSignal sig;
        memset(&sig, 0, sizeof(sig));
        /* Peers emit LOCAL_*; push_signal expects REMOTE_* for SDP/candidates. */
        if (type == (int)RNET_SIGNAL_LOCAL_SDP)
            type = (int)RNET_SIGNAL_REMOTE_SDP;
        else if (type == (int)RNET_SIGNAL_LOCAL_CANDIDATE)
            type = (int)RNET_SIGNAL_REMOTE_CANDIDATE;
        sig.type = (RNetSignalType)type;
        sig.flag = (rnet_u8)(flag & 0xFF);
        strncpy(sig.text, text, sizeof(sig.text) - 1);
        rnet_session_push_signal(g_np.session, &sig);
    }
}
#else
static void drain_lobby_signals(void) {}
#endif

static int resolve_use_ice(const PsxNetplayConfig *cfg)
{
    int in_motk_room = 0;

    if (cfg->transport == 2) return 0; /* force LAN */
#if defined(PSX_HAS_LOBBY_CLIENT)
    in_motk_room = psx_lobby_connected() && psx_lobby_in_lobby();
#endif

    /* §108: MotK/BPE online lobbies always use lobby UDP SFU — LAN dial to
     * relay_endpoint (or equal host/guest advertise). Match ICE / ice_p2p
     * selection was removed; waiting-room ICE RTT may still run for delay
     * hints only. Direct IP / LAN file lobby (no MotK seat) stays LAN UDP. */
    if (cfg->force_input_relay || in_motk_room) {
        if (!cfg->peer_hostport || !cfg->peer_hostport[0]) {
            fprintf(stderr,
                    "psx_netplay: MotK online / force_input_relay needs a "
                    "peer or SFU endpoint (empty peer)\n");
            fflush(stderr);
            return -1;
        }
        fprintf(stderr,
                "psx_netplay: server input relay — LAN transport to %s\n",
                cfg->peer_hostport);
        fflush(stderr);
        return 0;
    }

#if defined(RNET_ENABLE_ICE)
    /* Explicit ICE only outside MotK online (rare). */
    if (cfg->transport == 1)
        return 1;
#endif
    return 0;
}


int psx_netplay_start(const PsxNetplayConfig *cfg)
{
    RNetConfig rcfg;
    RNetHostVTable host;
    int in_player;
    int slots;
    int local;
    int use_ice;

    if (!cfg || !cfg->enabled) return -1;
    if (g_np.session) psx_netplay_shutdown();

    slots = cfg->slot_count;
    if (slots < 2) slots = 2;
    if (slots > PSX_MAX_PLAYERS) slots = PSX_MAX_PLAYERS;
    if (slots > RNET_MAX_SLOTS) slots = RNET_MAX_SLOTS;

    local = cfg->local_slot;
    if (local < 0) local = 0;
    if (local >= slots) local = slots - 1;

    rnet_config_init_defaults(&rcfg);
    rcfg.slot_count = (rnet_u8)slots;
    rcfg.local_slot = (rnet_u8)local;
    /* Max delay 20 matches RNET_MAX_BUNDLE 21 (neutral prefix + tip). */
    rcfg.input_delay = (rnet_u8)(cfg->input_delay < 0 ? 0
                               : (cfg->input_delay > 20 ? 20 : cfg->input_delay));
    /* §105: rollback floor D≥5 at session create (both peers). Lobby can
     * still seed 2–4 for delay-sync probe; Play clamps here. */
    if (cfg->rollback && rcfg.input_delay > 0u && rcfg.input_delay < 5u) {
        fprintf(stderr,
                "psxrecomp: rb session delay floor %u → 5 (rollback min)\n",
                (unsigned)rcfg.input_delay);
        fflush(stderr);
        rcfg.input_delay = 5u;
    }
    rcfg.session_id = cfg->session_id ? cfg->session_id : 1u;
    {
        uint32_t mask = cfg->occupied_mask;
        if (mask == 0u) {
            mask = (slots >= 32) ? 0xffffffffu : ((1u << slots) - 1u);
        } else {
            /* Always treat the local seat as occupied; clamp to session width. */
            mask |= (1u << (unsigned)local);
            if (slots < 32)
                mask &= (1u << slots) - 1u;
        }
        rcfg.occupied_mask = mask;
        if (mask != ((slots >= 32) ? 0xffffffffu : ((1u << slots) - 1u))) {
            fprintf(stderr,
                    "psx_netplay: occupied_mask=0x%x (sparse seats get "
                    "local neutral inputs)\n",
                    (unsigned)mask);
            fflush(stderr);
        }
    }

    /* Host resolves auto (-1) before start; accept 0..PSX_MAX_PLAYERS-1. */
    in_player = cfg->input_player;
    if (in_player < 0 || in_player >= PSX_MAX_PLAYERS) in_player = 0;

    use_ice = resolve_use_ice(cfg);
    if (use_ice < 0)
        return -4;

    memset(&host, 0, sizeof(host));
    host.sample_local = host_sample_local;
    host.publish = host_publish;
    host.ctx = &g_np;
#if defined(PSX_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
    if (use_ice)
        host.on_signal = host_on_signal;
#endif

    g_np.session = rnet_session_create(&rcfg, &host);
    if (!g_np.session) return -2;

    if (use_ice) {
#if defined(RNET_ENABLE_ICE)
        RNetIceConfig ice;
        RNetIpv4Address addrs[8];
        int naddr;
        const char *env_turn_host = getenv("PSX_NET_TURN_HOST");
        const char *env_turn_user = getenv("PSX_NET_TURN_USER");
        const char *env_turn_pass = getenv("PSX_NET_TURN_PASS");
        const char *env_stun = getenv("PSX_NET_STUN_HOST");

        g_np.ice_has_turn = 0;
        g_np.ice_stun_host[0] = '\0';
        g_np.ice_turn_host[0] = '\0';
        g_np.ice_turn_user[0] = '\0';
        g_np.ice_turn_pass[0] = '\0';
        g_np.ice_bind_addr[0] = '\0';

        rnet_ice_config_init_defaults(&ice);
        ice.controlling = (rcfg.local_slot == 0) ? 1u : 0u;

        naddr = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
        if (naddr > 0 && addrs[0].address[0]) {
            snprintf(g_np.ice_bind_addr, sizeof(g_np.ice_bind_addr), "%s",
                     addrs[0].address);
            ice.bind_address = g_np.ice_bind_addr;
        }

#if defined(PSX_HAS_LOBBY_CLIENT)
        /* Prefer TURN prefetched at WS welcome; re-request and wait if stale. */
        if (psx_lobby_connected()) {
            int i;
            const PsxLobbyTurnCredentials *tc = psx_lobby_turn_credentials();
            if (!tc || !tc->valid) {
                (void)psx_lobby_request_turn_credentials();
                for (i = 0; i < 200; ++i) { /* up to ~2s */
                    tc = psx_lobby_turn_credentials();
                    if (tc && tc->valid)
                        break;
                    psx_lobby_pump();
                    np_sleep_ms(10);
                }
            }
        }
        {
            const PsxLobbyTurnCredentials *tc = psx_lobby_turn_credentials();
            if (tc && tc->valid) {
                if (tc->stun_host[0]) {
                    snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host),
                             "%s", tc->stun_host);
                    ice.stun_host = g_np.ice_stun_host;
                    ice.stun_port = (rnet_u16)(tc->stun_port > 0 ? tc->stun_port
                                                                  : 3478);
                }
                snprintf(g_np.ice_turn_host, sizeof(g_np.ice_turn_host), "%s",
                         tc->turn_host);
                snprintf(g_np.ice_turn_user, sizeof(g_np.ice_turn_user), "%s",
                         tc->username);
                snprintf(g_np.ice_turn_pass, sizeof(g_np.ice_turn_pass), "%s",
                         tc->password);
                ice.turn_host = g_np.ice_turn_host;
                ice.turn_user = g_np.ice_turn_user;
                ice.turn_pass = g_np.ice_turn_pass;
                ice.turn_port = (rnet_u16)(tc->turn_port > 0 ? tc->turn_port
                                                              : 3478);
                g_np.ice_has_turn = 1;
            }
        }
#endif
        if (env_stun && env_stun[0]) {
            snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host), "%s",
                     env_stun);
            ice.stun_host = g_np.ice_stun_host;
            ice.stun_port = (rnet_u16)env_u("PSX_NET_STUN_PORT", ice.stun_port
                                                                     ? ice.stun_port
                                                                     : 3478);
        }
        if (env_turn_host && env_turn_host[0] && env_turn_user &&
            env_turn_user[0] && env_turn_pass && env_turn_pass[0]) {
            snprintf(g_np.ice_turn_host, sizeof(g_np.ice_turn_host), "%s",
                     env_turn_host);
            snprintf(g_np.ice_turn_user, sizeof(g_np.ice_turn_user), "%s",
                     env_turn_user);
            snprintf(g_np.ice_turn_pass, sizeof(g_np.ice_turn_pass), "%s",
                     env_turn_pass);
            ice.turn_host = g_np.ice_turn_host;
            ice.turn_user = g_np.ice_turn_user;
            ice.turn_pass = g_np.ice_turn_pass;
            ice.turn_port = (rnet_u16)env_u("PSX_NET_TURN_PORT", 3478);
            g_np.ice_has_turn = 1;
        }

        if (!g_np.ice_stun_host[0] && ice.stun_host && ice.stun_host[0]) {
            snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host), "%s",
                     ice.stun_host);
        }
        g_np.ice_stun_port = ice.stun_port ? (unsigned)ice.stun_port : 19302u;
        g_np.ice_turn_port = ice.turn_port ? (unsigned)ice.turn_port : 0u;

        if (g_np.ice_has_turn) {
            fprintf(stderr,
                    "psx_netplay: ICE stun=%s:%u turn=%s:%u user=%s bind=%s\n",
                    ice.stun_host ? ice.stun_host : "(default)",
                    (unsigned)ice.stun_port,
                    ice.turn_host, (unsigned)ice.turn_port, ice.turn_user,
                    ice.bind_address ? ice.bind_address : "(any)");
        } else {
            const char *allow_stun = getenv("PSX_NET_ALLOW_STUN_ONLY");
            fprintf(stderr,
                    "psx_netplay: ICE STUN-only (no TURN) stun=%s:%u "
                    "bind=%s — online MotK requires Coturn "
                    "(lobby get_turn_credentials or PSX_NET_TURN_*); set "
                    "PSX_NET_ALLOW_STUN_ONLY=1 to override\n",
                    ice.stun_host ? ice.stun_host : "(default)",
                    (unsigned)ice.stun_port,
                    ice.bind_address ? ice.bind_address : "(any)");
            /* BattleShip-style: refuse WAN ICE without TURN (CGNAT hangs). */
            if (!allow_stun || !allow_stun[0] || allow_stun[0] == '0') {
                rnet_session_destroy(g_np.session);
                g_np.session = NULL;
                return -4;
            }
        }

        {
            /* Online default is Force TURN (match_caps / UI); env overrides. */
            int force_turn = cfg->force_turn ? 1 : 0;
            const char *ft = getenv("PSX_NET_FORCE_TURN");
            if (ft && ft[0] && ft[0] != '0')
                force_turn = 1;
            else if (ft && ft[0] == '0')
                force_turn = 0;
            if (force_turn && !g_np.ice_has_turn) {
                fprintf(stderr,
                        "psx_netplay: FORCE_TURN requires Coturn credentials "
                        "(lobby get_turn_credentials or PSX_NET_TURN_*)\n");
                rnet_session_destroy(g_np.session);
                g_np.session = NULL;
                return -4;
            }
            if (force_turn) {
                ice.force_relay = 1;
                fprintf(stderr,
                        "psx_netplay: FORCE_TURN — ICE will use relay-only "
                        "candidates (host match_caps / all peers)\n");
            }
        }

        if (rnet_session_start_ice(g_np.session, &ice) != 0) {
            fprintf(stderr,
                    "psx_netplay: start_ice failed; refusing unsafe LAN "
                    "fallback for an online lobby\n");
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            return -4;
        }
#else
        fprintf(stderr, "psx_netplay: ICE requested but not built\n");
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
        return -4;
#endif
    }

    if (!use_ice) {
        /* Host-as-relay: slot 0 with 3+ seats and no dial peer. */
#if PSX_MAX_PLAYERS >= 3
        const int peer_empty =
            !cfg->peer_hostport || !cfg->peer_hostport[0];
        const int use_hub = (local == 0 && slots >= 3 && peer_empty);
        const int rc = use_hub
            ? rnet_session_start_lan_hub(g_np.session, cfg->bind_hostport)
            : rnet_session_start_lan(g_np.session, cfg->bind_hostport,
                                    cfg->peer_hostport);
#else
        const int rc = rnet_session_start_lan(g_np.session, cfg->bind_hostport,
                                              cfg->peer_hostport);
#endif
        if (rc != 0) {
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            return -3;
        }
    }
    np_diag_capture(cfg, slots);
    /* Before any snap/resim: CPU-authoritative VRAM. Must run before
     * g_np.active so a one-shot GL→CPU sync can still glReadPixels if we
     * were on FBO-auth OpenGL (ensure_cpu no-ops once netplay is active). */
    psx_frontend_netplay_force_sw_gpu();
    g_np.active = 1;
    netplay_hc_reset(&g_np.hc);
    np_part_ring_reset();
    g_np.rollback = cfg->rollback ? 1 : 0;
    netplay_ih_reset(&g_np.ih, (int)rcfg.slot_count);
    g_np.pending_rewind = 0;
    g_np.pending_rewind_tick = 0;
    g_np.pending_rewind_slot = 0;
    /* Seat / delay / integrity MUST be live before rb_start — RNetRbSession
     * freezes local_slot + slot_count at create. Starting with zeroed g_np made
     * every peer seal as slot 0 and export the wrong seat (VS-select hang). */
    g_np.use_ice = use_ice ? 1 : 0;
    g_np.slot_count = (int)rcfg.slot_count;
    g_np_slot_count = g_np.slot_count;
    g_np.local_slot = (int)rcfg.local_slot;
    g_np.input_player = in_player;
    g_np.input_delay = (int)rcfg.input_delay;
    g_np.input_prediction = cfg->input_prediction;
    if (g_np.input_prediction < 2) g_np.input_prediction = 2;
    if (g_np.input_prediction > 16) g_np.input_prediction = 16;
    {
        /* §44: scheduler policy lives in psx_netplay_sched.c. */
        PsxNpSchedBridge sb;
        sb.session = &g_np.session;
        sb.input_delay = &g_np.input_delay;
        sb.input_prediction = &g_np.input_prediction;
        sb.local_slot = &g_np.local_slot;
        /* Same Force TURN bit used for ICE relay-only (env may override). */
        {
            int ft = cfg->force_turn ? 1 : 0;
            const char *fte = getenv("PSX_NET_FORCE_TURN");
            if (fte && fte[0] == '1')
                ft = 1;
            else if (fte && fte[0] == '0')
                ft = 0;
            sb.force_turn = ft;
        }
        np_sched_bind(&sb);
    }
    {
        uint32_t bios = 0, entry = 0;
        savestate_get_integrity(&bios, &entry);
        g_np.bios_checksum = bios;
        g_np.entry_pc = entry;
    }
    if (g_np.rollback) {
        np_rb_bind_and_start();
        printf("psxrecomp: netplay mode=rollback (D=%d P=%d invent+contract)\n",
               g_np.input_delay, g_np.input_prediction);
        /* Peers MUST share one binary — mixed build-release vs packaged
         * motk-* left matched digests for hundreds of ticks then GPR/tim
         * forks in Replay, with pin zlib ~1.34M vs ~1.13M. */
        {
            char exe[512];
            long long sz = -1;
#if defined(_WIN32)
            DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
            if (n == 0 || n >= sizeof(exe))
                snprintf(exe, sizeof(exe), "(unknown)");
            {
                WIN32_FILE_ATTRIBUTE_DATA fad;
                if (GetFileAttributesExA(exe, GetFileExInfoStandard, &fad)) {
                    ULARGE_INTEGER u;
                    u.HighPart = fad.nFileSizeHigh;
                    u.LowPart = fad.nFileSizeLow;
                    sz = (long long)u.QuadPart;
                }
            }
#else
            ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            if (n < 0) {
                snprintf(exe, sizeof(exe), "(unknown)");
            } else {
                exe[n] = '\0';
                {
                    struct stat st;
                    if (stat(exe, &st) == 0)
                        sz = (long long)st.st_size;
                }
            }
#endif
            fprintf(stderr,
                    "psxrecomp: rb binary path=%s size=%lld "
                    "(same game version + digests; per-peer PGO OK)\n",
                    exe, sz);
            fflush(stderr);
        }
        fflush(stdout);
    }
    if (g_np.slot_count >= 3)
        sio_set_multitap(1);
    else
        sio_set_multitap(0);
    g_np.staged_valid = 0;
    g_np.live_valid = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.latched_sim_tick = 0;
    g_np.xfer = NP_XFER_NONE;
    g_np.xfer_slot = 0;
    g_np.mc_sync_done = 0;
    g_np.mc_sync_sent = 0;
    g_np.local_save_staged = 0;
    g_np.load_applied_local = 0;
    g_np.guest_sandbox = 0;
    g_np.force_input_relay = cfg->force_input_relay ? 1 : 0;
    g_np.session_id = rcfg.session_id;
    g_np.is_host = (g_np.local_slot == 0) ? 1 : 0;
    g_np.frames_finished = 0;
    g_np.diag_session++;
    g_diag_summary_written = 0;
    if (g_diag_file) {
        fclose(g_diag_file);
        g_diag_file = NULL;
    }
    g_diag_file_session = 0;
    g_diag_last_write_ms = 0;
    snprintf(g_np.bind_hostport, sizeof(g_np.bind_hostport), "%s",
             cfg->bind_hostport);
    snprintf(g_np.peer_hostport, sizeof(g_np.peer_hostport), "%s",
             cfg->peer_hostport);
    g_np.lobby_server[0] = '\0';
    g_np.lobby_id[0] = '\0';
#if defined(PSX_HAS_LOBBY_CLIENT)
    if (use_ice && psx_lobby_connected() && psx_lobby_in_lobby()) {
        const PsxLobbyJoinInfo *ji = psx_lobby_join_info();
        snprintf(g_np.match_mode, sizeof(g_np.match_mode), "hosted_lobby");
        snprintf(g_np.lobby_server, sizeof(g_np.lobby_server), "%s",
                 psx_lobby_default_url());
        if (ji && ji->lobby_id[0])
            snprintf(g_np.lobby_id, sizeof(g_np.lobby_id), "%s", ji->lobby_id);
        g_np.is_host = psx_lobby_is_host() ? 1 : 0;
    } else
#endif
    {
        snprintf(g_np.match_mode, sizeof(g_np.match_mode), "direct_ip");
    }

#if defined(__linux__)
    if (!use_ice && peer_is_loopback(cfg->peer_hostport))
        pin_localhost_peer_cpus(g_np.local_slot);
#endif

    psx_netplay_release_pads();
    fprintf(stderr,
            "psx_netplay: started transport=%s slot=%d input_player=%d session=%u "
            "delay=%u force_input_relay=%d force_turn=%d bind=%s peer=%s\n",
            use_ice ? "ice" : "lan", g_np.local_slot, g_np.input_player,
            (unsigned)rcfg.session_id, (unsigned)rcfg.input_delay,
            g_np.force_input_relay, cfg->force_turn ? 1 : 0, cfg->bind_hostport,
            use_ice ? "(ice)" : cfg->peer_hostport);
    return 0;
}

void psx_netplay_bind_guest_saves(void)
{
    uint32_t bios = 0, entry = 0;
    if (!psx_netplay_active())
        return;
    /* psx_netplay_start runs before savestate_configure, so g_np still held
     * bios=0/entry=0. Refresh for both seats: host RB snaps (MEDIA-KF) must
     * carry the real integrity key or the guest rejects the transfer. */
    savestate_get_integrity(&bios, &entry);
    g_np.bios_checksum = bios;
    g_np.entry_pc = entry;
    if (g_np.local_slot == 0 || g_np.guest_sandbox)
        return;
    np_enter_guest_sandbox();
}

/* Delay-sync starvation hold (lockstep-safe; mirrors snes_host_barrier_admit). */
#define PSX_STARVATION_ENTER_DEFAULT 4
#define PSX_STARVATION_EXIT_DEFAULT 3
#define PSX_STARVATION_EXIT_HR_LEAD_DEFAULT 0
#define PSX_STARVATION_GRACE_TICKS 60
/* Default 0: after starvation clears, resume ~1 sim/wall frame and let
 * remote_lead rebuild toward D instead of a turbo recovery burst.
 * Override: PSX_NET_STARVATION_RECOVERY_BURST / PSX_NET_CATCHUP_CAP. */
#define PSX_STARVATION_RECOVERY_BURST_DEFAULT 0
#define PSX_CATCHUP_CAP_DEFAULT 0

static struct {
    int latched;
    int enter_run;
    int exit_run;
    int recovery_amount;
    int latch_logged;
    int just_cleared;
} g_starv;

/* Defined below poll_admit; used by the starvation runway check. */
int psx_netplay_remote_lead(void);
int psx_netplay_input_delay(void);

static int np_starv_env_int(const char *name, int def)
{
    const char *v = getenv(name);
    long n;
    char *end;
    if (!v || !v[0])
        return def;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 0 || n > 64)
        return def;
    return (int)n;
}

static void np_starv_reset(void)
{
    memset(&g_starv, 0, sizeof(g_starv));
}

static int np_starv_runway_ok(void)
{
    int lead = psx_netplay_remote_lead();
    int delay = psx_netplay_input_delay();
    int hr_lead = np_starv_env_int("PSX_NET_STARVATION_EXIT_HR_LEAD",
                                   PSX_STARVATION_EXIT_HR_LEAD_DEFAULT);
    if (delay < 0)
        delay = 0;
    return lead >= delay + hr_lead;
}

void psx_netplay_cold_reset(void)
{
    /* Host-only statics that survive soft-return (BSS-zero on a cold peer).
     * Device *_init / rb_start still run on session_reboot — this is the
     * gap between BYE teardown and the next match. */
    {
        int i;
        for (i = 0; i < PSX_MAX_PLAYERS; ++i) {
            g_sio_pad_prev[i] = 0xFFFFu;
            g_sio_pad_have[i] = 0;
            g_inv_pad_prev[i] = 0xFFFFu;
            g_inv_pad_have[i] = 0;
        }
    }
    g_local_pad_prev = 0xFFFFu;
    g_local_pad_have = 0;
    g_live_trace_prev = 0xFFFFu;
    g_live_trace_have = 0;
    g_dev_trace_prev = 0xFFFFu;
    g_dev_trace_have = 0;
    g_tip_trace_prev = 0xFFFFu;
    g_tip_trace_have = 0;
    memset(g_sio_apply_sim, 0, sizeof(g_sio_apply_sim));
    memset(g_sio_apply_btn, 0, sizeof(g_sio_apply_btn));
    memset(g_sio_apply_n, 0, sizeof(g_sio_apply_n));
    memset(g_sio_apply_have, 0, sizeof(g_sio_apply_have));
    g_start_gest_down_sim = 0;
    g_start_gest_up_sim = 0;
    g_start_gest_down_valid = 0;
    g_start_gest_up_valid = 0;
    g_start_gest_last_stage_down_sim = 0xFFFFFFFFu;
    s_cd_bisect_arm_until = 0;
    s_cd_bisect_last_tick = 0xffffffffu;
    np_part_ring_reset();
    np_starv_reset();
    psx_netplay_rb_cold_reset();
    overlay_loader_clear_lazy_miss();
    psx_irq_clear_resume_latches();
}

void psx_netplay_shutdown(void)
{
    if (g_diag_file) {
        fclose(g_diag_file);
        g_diag_file = NULL;
    }
    g_diag_file_session = 0;
    g_diag_summary_written = 0;
    g_diag_last_write_ms = 0;
    if (g_np.session) {
        (void)rnet_session_send_bye(g_np.session);
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
    }
    psx_netplay_rb_shutdown();
    psx_netplay_rb_bind(NULL);
    /* Drop cushion/timesync/tip statics so soft-return rematch starts clean. */
    np_sched_bind(NULL);
    np_leave_guest_sandbox();
    {
        CPUState *saved_cpu = g_np.cpu;
        memset(&g_np, 0, sizeof(g_np));
        g_np.cpu = saved_cpu;
        netplay_hc_reset(&g_np.hc);
        np_part_ring_reset();
    }
    np_starv_reset();
    /* Lobby idle + rematch: wipe pad/CD/overlay residue a cold peer lacks. */
    psx_netplay_cold_reset();
}

int psx_netplay_is_host(void)
{
    return psx_netplay_active() && g_np.local_slot == 0;
}

int psx_netplay_request_save(int slot)
{
    uint32_t sim;
    uint32_t delay;
    uint32_t target;
    if (!psx_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0)
        return 1; /* guest: host-only; ignore */
    if (np_xfer_busy() || !g_np.mc_sync_done)
        return 1;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;

    /* Agree a future sim_tick so TURN/coord latency cannot make the host
     * write tick T while the guest still writes T+k (CRC miss → transfer).
     * crc field of size==0 probe carries the target tick. */
    sim = rnet_session_sim_tick(g_np.session);
    delay = (uint32_t)psx_netplay_input_delay();
    if (delay < 1u) delay = 1u;
    target = sim + delay + 2u;
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)slot, 0,
                                 target) != 0)
        return 1;
    g_np.xfer = NP_XFER_SAVE_COORD;
    g_np.xfer_slot = slot;
    g_np.save_target_tick = target;
    g_np.local_save_staged = 0;
    g_np.local_save_acked = 0;
    printf("psxrecomp: netplay save slot=%d — coordinating local writes "
           "(target sim=%u, now=%u)…\n",
           slot, (unsigned)target, (unsigned)sim);
    fflush(stdout);
    return 1;
}

int psx_netplay_request_load(int slot)
{
    uint32_t size = 0, crc = 0;
    char reason[192];
    if (!psx_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0)
        return 1;
    if (np_xfer_busy() || !g_np.mc_sync_done)
        return 1;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;
    if (!savestate_slot_compatible(slot, reason, sizeof(reason))) {
        printf("psxrecomp: netplay load slot=%d refused — %s "
               "(resave with this build: Shift+F%d)\n",
               slot, reason[0] ? reason : "incompatible", slot + 1);
        fflush(stdout);
        return 1;
    }
    if (!np_slot_crc(slot, &size, &crc))
        return 1;
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)slot, size, crc) != 0)
        return 1;
    g_np.xfer = NP_XFER_LOAD_PROBE;
    g_np.xfer_slot = slot;
    g_np.load_applied_local = 0;
    g_np.load_apply_failed = 0;
    printf("psxrecomp: netplay load slot=%d — hash probe (%u bytes)\n", slot, (unsigned)size);
    fflush(stdout);
    return 1;
}

int psx_netplay_in_load_barrier(void)
{
    if (!psx_netplay_active())
        return 0;
    /* Any save/load/memcard sync phase — TURN chunk xfers of ~1.4MB need the
     * 90s budget (20s admit stall was killing SAVE mid-transfer). */
    return (g_np.xfer != NP_XFER_NONE) ? 1 : 0;
}

int psx_netplay_consume_load_apply_failed(void)
{
    int v = g_np.load_apply_failed;
    g_np.load_apply_failed = 0;
    return v;
}


static int np_diag_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("PSX_NET_DIAG");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* Verbose delay-sync starvation latch/clear spam. Off by default — the latch
 * can toggle every few frames under jitter and floods stderr. Enable with
 * PSX_NET_DELAY_SYNC_DIAG=1 (alias: PSX_NET_STARVATION_DIAG=1). */
static int np_delay_sync_diag_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("PSX_NET_DELAY_SYNC_DIAG");
        if (!v || !v[0])
            v = getenv("PSX_NET_STARVATION_DIAG");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

static unsigned np_diag_interval_ms(void)
{
    static unsigned cached = 0;
    unsigned hz;
    if (cached)
        return cached;
    hz = env_u("PSX_NET_DIAG_HZ", 2);
    if (hz < 1) hz = 1;
    if (hz > 30) hz = 30;
    cached = 1000u / hz;
    if (cached < 1) cached = 1;
    return cached;
}

static void np_diag_escape(char *out, size_t out_len, const char *in)
{
    size_t oi = 0;
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;
    for (; *in && oi + 2 < out_len; ++in) {
        char c = *in;
        if (c == '"' || c == '\\') {
            if (oi + 3 >= out_len)
                break;
            out[oi++] = '\\';
            out[oi++] = c;
        } else if ((unsigned char)c < 0x20) {
            /* skip */
        } else {
            out[oi++] = c;
        }
    }
    out[oi] = '\0';
}

static const char *np_diag_ice_path(const RNetSessionStats *st)
{
    if (!g_np.use_ice)
        return "lan";
    if (!st)
        return "pending";
    if (st->ice_state == RNET_ICE_STATE_FAILED)
        return "failed";
    if (st->ice_path[0])
        return st->ice_path;
    if (st->ice_state == RNET_ICE_STATE_COMPLETED ||
        st->ice_state == RNET_ICE_STATE_CONNECTED)
        return "unknown";
    return "pending";
}

static const char *np_diag_ice_nat(const char *path)
{
    if (!g_np.use_ice)
        return "lan";
    if (!path || !path[0] || strcmp(path, "pending") == 0)
        return "pending";
    if (strcmp(path, "failed") == 0)
        return "failed";
    if (strcmp(path, "relay") == 0)
        return "turn";
    if (strcmp(path, "srflx") == 0 || strcmp(path, "prflx") == 0)
        return "stun";
    if (strcmp(path, "host") == 0)
        return "host";
    return "unknown";
}

static int np_diag_path_ready(const RNetSessionStats *st)
{
    if (!g_np.use_ice)
        return 1;
    if (!st)
        return 0;
    if (st->ice_state == RNET_ICE_STATE_FAILED)
        return 1;
    if (st->ice_path[0] && strcmp(st->ice_path, "pending") != 0 &&
        strcmp(st->ice_path, "unknown") != 0)
        return 1;
    if (st->ice_state == RNET_ICE_STATE_COMPLETED ||
        st->ice_state == RNET_ICE_STATE_CONNECTED)
        return 1;
    return 0;
}

static void np_diag_write_summary(FILE *f, const RNetSessionStats *st, uint32_t now)
{
    char server_esc[280];
    char lobby_esc[80];
    char bind_esc[80];
    char peer_esc[80];
    char stun_esc[140];
    char turn_esc[140];
    char ice_local_esc[120];
    char ice_remote_esc[120];
    const char *path = np_diag_ice_path(st);
    const char *nat = np_diag_ice_nat(path);
    const char *ice_state =
        st ? rnet_ice_state_name(st->ice_state) : "idle";

    np_diag_escape(server_esc, sizeof(server_esc), g_np.lobby_server);
    np_diag_escape(lobby_esc, sizeof(lobby_esc), g_np.lobby_id);
    np_diag_escape(bind_esc, sizeof(bind_esc), g_np.bind_hostport);
    np_diag_escape(peer_esc, sizeof(peer_esc), g_np.peer_hostport);
    np_diag_escape(stun_esc, sizeof(stun_esc), g_np.ice_stun_host);
    np_diag_escape(turn_esc, sizeof(turn_esc), g_np.ice_turn_host);
    np_diag_escape(ice_local_esc, sizeof(ice_local_esc),
                   st ? st->ice_local : "");
    np_diag_escape(ice_remote_esc, sizeof(ice_remote_esc),
                   st ? st->ice_remote : "");

    fprintf(f,
            "{\"type\":\"summary\",\"t_ms\":%u,\"match\":\"%s\","
            "\"lobby_server\":\"%s\",\"lobby_id\":\"%s\",\"is_host\":%d,"
            "\"slot\":%d,\"session_id\":%u,\"input_delay\":%d,"
            "\"force_input_relay\":%d,"
            "\"transport\":\"%s\",\"bind\":\"%s\",\"peer\":\"%s\","
            "\"turn_configured\":%d,\"stun_host\":\"%s\",\"stun_port\":%u,"
            "\"turn_host\":\"%s\",\"turn_port\":%u,\"ice_state\":\"%s\","
            "\"ice_path\":\"%s\",\"ice_nat\":\"%s\","
            "\"ice_local\":\"%s\",\"ice_remote\":\"%s\"}\n",
            (unsigned)now, g_np.match_mode[0] ? g_np.match_mode : "unknown",
            server_esc, lobby_esc, g_np.is_host, g_np.local_slot,
            (unsigned)g_np.session_id, g_np.input_delay, g_np.force_input_relay,
            g_np.use_ice ? "ice" : "lan", bind_esc, peer_esc,
            g_np.ice_has_turn ? 1 : 0, stun_esc, g_np.ice_stun_port, turn_esc,
            g_np.ice_turn_port, ice_state ? ice_state : "idle", path, nat,
            ice_local_esc, ice_remote_esc);
}

void psx_netplay_diag_tick(void)
{
    RNetSessionStats st;
    uint32_t now;
    const char *transport;
    const char *ice_state;
    const char *path;

    if (!np_diag_enabled() || !psx_netplay_active() || !g_np.session)
        return;

    rnet_session_get_stats(g_np.session, &st);

    if (!g_diag_summary_written && !np_diag_path_ready(&st))
        return;

    now = np_mono_ms();
    if (g_diag_last_write_ms &&
        (uint32_t)(now - g_diag_last_write_ms) < np_diag_interval_ms() &&
        g_diag_summary_written)
        return;
    g_diag_last_write_ms = now ? now : 1u;

    if (!g_diag_mkdir_done) {
        g_diag_mkdir_done = 1;
#ifdef _WIN32
        _mkdir("saves");
        _mkdir("saves\\netplay");
#else
        mkdir("saves", 0755);
        mkdir("saves/netplay", 0755);
#endif
    }

    if (!g_diag_file || g_diag_file_session != g_np.diag_session) {
        char pathbuf[64];
        if (g_diag_file) {
            fclose(g_diag_file);
            g_diag_file = NULL;
        }
        snprintf(pathbuf, sizeof(pathbuf), "saves/netplay/net_diag.jsonl");
        g_diag_file = fopen(pathbuf, "wb");
        if (!g_diag_file)
            return;
        setvbuf(g_diag_file, NULL, _IOLBF, 0);
        g_diag_file_session = g_np.diag_session;
        g_diag_summary_written = 0;
        fprintf(stderr, "psx_netplay: diag writing %s "
                        "(PSX_NET_DIAG_HZ interval %ums)\n",
                pathbuf, np_diag_interval_ms());
    }

    if (!g_diag_summary_written) {
        np_diag_write_summary(g_diag_file, &st, now);
        g_diag_summary_written = 1;
    }

    {
        char ice_local_esc[120];
        char ice_remote_esc[120];
        const char *stall = rnet_admit_stall_name(st.last_stall);
        int using_turn_path = (strcmp(np_diag_ice_path(&st), "relay") == 0) ? 1 : 0;

        transport = psx_netplay_transport_name();
        ice_state = rnet_ice_state_name(st.ice_state);
        path = np_diag_ice_path(&st);
        np_diag_escape(ice_local_esc, sizeof(ice_local_esc), st.ice_local);
        np_diag_escape(ice_remote_esc, sizeof(ice_remote_esc), st.ice_remote);

        fprintf(g_diag_file,
                "{\"t_ms\":%u,\"slot\":%d,\"transport\":\"%s\",\"ice_state\":\"%s\","
                "\"ice_path\":\"%s\",\"ice_nat\":\"%s\",\"turn\":%d,"
                "\"ice_local\":\"%s\",\"ice_remote\":\"%s\","
                "\"running\":%d,\"sim_tick\":%u,\"frames_finished\":%u,"
                "\"delay\":%u,\"stall\":\"%s\","
                "\"stall_ms\":%u,\"stall_max_ms\":%u,\"stall_streaks\":%u,"
                "\"consec_stalls\":%u,\"admit_ok\":%u,\"remote_lead\":%d,"
                "\"remote_wire\":%u,\"peer_rx_age_ms\":%llu,\"peer_gone\":%d,"
                "\"desync\":%d,\"desync_tick\":%u,\"state_busy\":%d,\"state_op\":%u,"
                "\"pkts_rx\":%u,\"input_sends\":%u}\n",
                (unsigned)now, g_np.local_slot, transport ? transport : "none",
                ice_state ? ice_state : "idle", path, np_diag_ice_nat(path),
                using_turn_path, ice_local_esc, ice_remote_esc, st.is_running,
                (unsigned)st.sim_tick, (unsigned)g_np.frames_finished,
                (unsigned)st.delay, stall ? stall : "unknown",
                (unsigned)st.last_admit_wait_ms, (unsigned)st.max_admit_wait_ms,
                (unsigned)st.stall_streaks, (unsigned)st.consecutive_stalls,
                (unsigned)st.admit_ok_count, st.remote_lead,
                (unsigned)st.highest_remote_wire,
                (unsigned long long)st.last_peer_rx_age_ms, st.peer_gone,
                st.input_desync, (unsigned)st.desync_tick, st.state_busy,
                (unsigned)st.state_op, (unsigned)st.packets_rx,
                (unsigned)st.input_bundle_sends);
    }
}

/* Stage the coord save once sim_tick reaches the agreed target. */
static void np_maybe_stage_target_save(void)
{
    uint32_t sim;
    if (g_np.xfer != NP_XFER_SAVE_COORD || g_np.local_save_staged)
        return;
    if (!g_np.session || !rnet_session_is_running(g_np.session))
        return;
    sim = rnet_session_sim_tick(g_np.session);
    if (sim < g_np.save_target_tick)
        return;
    if (!savestate_request_save_protocol(g_np.xfer_slot))
        return;
    g_np.local_save_staged = 1;
    printf("psxrecomp: netplay %s save slot=%d — staging @ sim=%u (target=%u)\n",
           g_np.local_slot == 0 ? "host" : "guest", g_np.xfer_slot,
           (unsigned)sim, (unsigned)g_np.save_target_tick);
    fflush(stdout);
}

/* §94: SAVE freeze opens a tip hole; invent-hold from FMV MAX-unmatched then
 * deadlocks admit (stall looked like fmv_settle). Clear invent-hold so gap1
 * invent can refill before the 20s watchdog. */
static void np_note_save_complete(void)
{
    if (!g_np.rollback)
        return;
    psx_netplay_rb_clear_fmv_desync_hold("netplay save complete");
}

static void np_pump_session(void)
{
#if defined(PSX_HAS_LOBBY_CLIENT)
    if (g_np.use_ice || psx_lobby_connected())
        psx_lobby_pump();
#endif
    drain_lobby_signals();
    rnet_session_pump(g_np.session);
    /* STATE_CHUNK AIMD still needs many pumps per wall-second on TURN; frame-
     * paced call sites alone leave the cwnd idle between vblanks. */
    if (rnet_session_state_busy(g_np.session)) {
        int burst;
        for (burst = 0; burst < 12; burst++)
            rnet_session_pump(g_np.session);
    }
    np_drain_peer_frame_commits();
    if (g_np.rollback) {
        np_rollback_reconcile_wire();
        psx_netplay_rb_pump();
    }
    np_guest_handle_probe();
    np_maybe_stage_target_save();
    np_apply_ready_state();
    np_drive_load_barrier();
    np_host_drive_xfer();
    if (rnet_session_is_running(g_np.session))
        np_maybe_start_mc_sync();
}

void psx_netplay_pump(void)
{
    if (!psx_netplay_active())
        return;
    /* Mid-guest cycle-watchdog pump during Replay must not run reconcile /
     * rb_pump (seal/baseline apply, hist promote) — that is host-asymmetric
     * work and was a candidate for SIO fsm forks with matched guest cycles.
     * Still drain transport + FRAME_COMMIT so mid-resim core aborts stay live.
     * Full pump resumes at admit / present edges. */
    if (g_np.rollback && psx_netplay_rb_is_resimulating()) {
#if defined(PSX_HAS_LOBBY_CLIENT)
        if (g_np.use_ice || psx_lobby_connected())
            psx_lobby_pump();
#endif
        drain_lobby_signals();
        rnet_session_pump(g_np.session);
        np_drain_peer_frame_commits();
        /* Stall abort only — full rb_pump stays off mid-guest (asymmetric). */
        psx_netplay_rb_poll_replay_stall();
        return;
    }
    np_pump_session();
    psx_netplay_diag_tick();
}

static int np_try_admit_gameplay(void)
{
    rnet_u32 sim = rnet_session_sim_tick(g_np.session);
    if (rnet_session_try_admit(g_np.session, sim)) {
        g_np.needs_advance = 1;
        return 1;
    }
    force_session_pads_connected(g_np.slot_count);
    return 0;
}

/*
 * §95: rollback LOAD barrier admit — tip + hold-last invent, no INPUT_CONFIRM.
 * Delay-sync try_admit wait_confirm hung the slower peer after the faster one
 * applied and froze (guest LOADED, host stuck load_applying+wait_confirm).
 * Load will hard_resync at mutual ready; pads here only need guest cycles for
 * savestate_poll / one resume tick.
 */
static int np_try_admit_load_barrier_rb(void)
{
    rnet_u32 sim = rnet_session_sim_tick(g_np.session);
    rnet_u32 wire;
    RNetInputSample sample;
    RNetRbFrame row;
    PsxNetPad pad;
    int slot;

    np_sched_set_admit_stall("load_barrier_rb");
    if (!rnet_session_prepare_local_tip(g_np.session, sim)) {
        np_sched_set_admit_stall("load_barrier_prepare_tip");
        force_session_pads_connected(g_np.slot_count);
        return 0;
    }
    wire = np_sched_wire_for_sim(sim);

    for (slot = 0; slot < g_np.slot_count; ++slot) {
        if (slot == g_np.local_slot) {
            if (rnet_session_peek_input(g_np.session, slot, wire, &sample)) {
                decode_pad(&sample, &pad);
                netplay_ih_pad_to_frame(&pad, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            } else if (g_np.staged_valid) {
                netplay_ih_pad_to_frame(&g_np.staged, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            } else {
                memset(&pad, 0, sizeof(pad));
                pad.buttons = 0xFFFFu;
                pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
                pad.analog = 0;
                pad.connected = 1;
                netplay_ih_pad_to_frame(&pad, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            }
            continue;
        }
        if (rnet_session_peek_remote_input(g_np.session, slot, wire, &sample)) {
            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, sim, 0, &row);
            (void)netplay_ih_put(&g_np.ih, slot, &row);
        } else {
            /* Never stall — peer may already be frozen in LOAD_READY. */
            (void)netplay_ih_invent_hold_last(&g_np.ih, slot, sim, &row);
        }
    }

    np_publish_hist_sio(sim);
    g_np.needs_advance = 1;
    np_sched_set_admit_stall("");
    return 1;
}

int psx_netplay_poll_admit(void)
{
    rnet_u32 sim;
    int enter_need;
    int exit_need;

    if (!psx_netplay_active()) return 1;

    np_pump_session();

    if (!rnet_session_is_running(g_np.session)) {
        psx_netplay_release_pads();
        np_starv_reset();
        psx_netplay_diag_tick();
        return 0;
    }

    /* Both peers stall until initial memcard hash-agree / transfer finishes. */
    if (!g_np.mc_sync_done)
        return 0;

    /* Post-load: allow admit only while savestate_poll still needs guest
     * cycles. After restore (or during ready rendezvous) freeze the sim clock
     * so peers cannot drift before hard_resync + prime. */
    if (g_np.xfer == NP_XFER_LOAD_APPLYING && !savestate_pending())
        return 0;

    /* Staged load must run guest cycles — bypass starvation latch. ICE xfer
     * often leaves lead=D-1 and would otherwise block try_admit forever.
     * §95: rollback must not use delay-sync confirm here. */
    if (g_np.xfer == NP_XFER_LOAD_APPLYING && savestate_pending()) {
        if (g_np.needs_advance)
            return 1;
        if (g_np.rollback)
            return np_try_admit_load_barrier_rb();
        return np_try_admit_gameplay();
    }

    /* Both peers: after mutual ready + sync, stay in LOAD_READY until admit
     * succeeds. Dropping the barrier early on the host let it spin on confirm
     * with FPS/present already "live". §95: rollback exits via tip invent. */
    if (g_np.xfer == NP_XFER_LOAD_READY) {
        if (g_np.load_sync_done && g_np.load_ready_replied && !g_np.needs_advance) {
            int admitted;
            if (g_np.rollback)
                admitted = np_try_admit_load_barrier_rb();
            else {
                sim = rnet_session_sim_tick(g_np.session);
                admitted = rnet_session_try_admit(g_np.session, sim);
                if (admitted)
                    g_np.needs_advance = 1;
                else
                    force_session_pads_connected(g_np.slot_count);
            }
            if (admitted) {
                g_np.xfer = NP_XFER_NONE;
                g_np.load_applied_local = 0;
                g_np.load_ready_replied = 0;
                g_np.load_sync_done = 0;
                printf("psxrecomp: netplay load slot=%d — peer ready, resuming lockstep\n",
                       g_np.xfer_slot);
                fflush(stdout);
                return 1;
            }
        }
        return 0;
    }

    /* Rollback episode OR pending Live realign: the live needs_advance latch
     * must not bypass rb_try_admit. Resim finish_frame never cleared
     * g_np.needs_advance, so a follower that entered an episode mid-tick spun
     * forever at uncapped FPS without ever arming rb finish_frame. Also stall
     * while a post-abort realign load is queued (episode already inactive). */
    if (g_np.rollback && g_np.xfer == NP_XFER_NONE &&
        (psx_netplay_rb_active() || psx_netplay_rb_load_pending())) {
        if (psx_netplay_rb_tip_holding() && !psx_netplay_rb_load_pending()) {
            /* TipHold: Live invent continues; episode stays open for tip-extend. */
        } else {
            int admit;
            g_np.needs_advance = 0;
            admit = psx_netplay_rb_try_admit();
            /* §47: each Replay admit produces local tip like Live. */
            if (admit)
                np_rb_produce_local_tip_for_sim(rnet_session_sim_tick(g_np.session));
            return admit;
        }
    }

    /* Already published this tick and waiting for finish_frame — do not
     * re-admit / re-sample (would desync the delay rings). */
    if (g_np.needs_advance) return 1;

    /* Rollback gameplay: invent missing remotes; skip delay-sync try_admit.
     * Save/load/memcard xfer paths above still use delay-sync admit. */
    if (g_np.rollback && g_np.xfer == NP_XFER_NONE)
        return np_try_admit_rollback();

    sim = rnet_session_sim_tick(g_np.session);
    enter_need = np_starv_env_int("PSX_NET_STARVATION_ENTER_FRAMES",
                                  PSX_STARVATION_ENTER_DEFAULT);
    exit_need = np_starv_env_int("PSX_NET_STARVATION_EXIT_FRAMES",
                                 PSX_STARVATION_EXIT_DEFAULT);

    /* SAVE coord: run admit until both reach save_target_tick and flush the
     * staged write. After the local .pst exists, freeze (host also freezes
     * unless the guest tip is behind and still needs catch-up admits). */
    if (g_np.xfer == NP_XFER_SAVE_COORD) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        np_maybe_stage_target_save();
        if (!g_np.local_save_staged || savestate_pending())
            return np_try_admit_gameplay();
        if (g_np.local_slot != 0)
            return 0; /* guest saved — wait for hash probe */
        /* Host saved: freeze for same-tick match. If guest is still behind
         * the target, keep admitting so it can catch up and write. */
        if (psx_netplay_remote_lead() < 0)
            return np_try_admit_gameplay();
        return 0;
    }

    if (g_np.xfer == NP_XFER_LOAD_PROBE || g_np.xfer == NP_XFER_LOAD_SEND ||
        g_np.xfer == NP_XFER_SAVE_PROBE || g_np.xfer == NP_XFER_SAVE_SEND ||
        g_np.xfer == NP_XFER_MC_PROBE || g_np.xfer == NP_XFER_MC_SEND) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        return np_try_admit_gameplay();
    }

    /* Startup grace: do not latch before the delay rings warm up. */
    if (sim < (rnet_u32)PSX_STARVATION_GRACE_TICKS) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        return np_try_admit_gameplay();
    }

    if (g_starv.latched) {
        /* Pump already ran; hold try_admit until remote tip refills. */
        if (np_starv_runway_ok()) {
            g_starv.exit_run++;
            if (g_starv.exit_run >= exit_need) {
                g_starv.latched = 0;
                g_starv.exit_run = 0;
                g_starv.latch_logged = 0;
                g_starv.just_cleared = 1;
            } else {
                return 0;
            }
        } else {
            g_starv.exit_run = 0;
            return 0;
        }
    }

    if (np_try_admit_gameplay()) {
        g_starv.enter_run = 0;
        if (g_starv.just_cleared) {
            int burst = np_starv_env_int("PSX_NET_STARVATION_RECOVERY_BURST",
                                         PSX_STARVATION_RECOVERY_BURST_DEFAULT);
            g_starv.just_cleared = 0;
            g_starv.recovery_amount = burst;
            if (np_delay_sync_diag_enabled()) {
                if (burst > 0) {
                    fprintf(stderr,
                            "psxrecomp: delay_sync_starvation cleared sim=%u lead=%d "
                            "D=%d — recovery burst %d\n",
                            (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                            psx_netplay_input_delay(), burst);
                } else {
                    fprintf(stderr,
                            "psxrecomp: delay_sync_starvation cleared sim=%u lead=%d "
                            "D=%d — resume 1:1 (rebuild input buffer)\n",
                            (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                            psx_netplay_input_delay());
                }
            }
        }
        return 1;
    }

    g_starv.just_cleared = 0;
    g_starv.enter_run++;
    if (g_starv.enter_run >= enter_need) {
        g_starv.latched = 1;
        g_starv.enter_run = 0;
        if (!g_starv.latch_logged) {
            if (np_delay_sync_diag_enabled()) {
                fprintf(stderr,
                        "psxrecomp: delay_sync_starvation latched sim=%u lead=%d "
                        "D=%d (enter=%d)\n",
                        (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                        psx_netplay_input_delay(), enter_need);
            }
            g_starv.latch_logged = 1;
        }
    }
    return 0;
}

void psx_netplay_finish_frame(void)
{
    rnet_u32 done;
    if (!psx_netplay_active()) return;

    if (g_np.rollback && psx_netplay_rb_is_resimulating()) {
        rnet_u32 done = rnet_session_sim_tick(g_np.session);
        /* Present-edge only: mid-guest pump skips reconcile. Coalesce late
         * wire into the active episode (tip-extend) before POST/Verify. */
        rnet_session_pump(g_np.session);
        np_rollback_reconcile_wire();
        /* §47: refill local tip while Replay consumes confirmed remotes. */
        np_rb_produce_local_tip_for_sim(done);
        psx_netplay_rb_pump();
        psx_netplay_rb_ownership_step();
        psx_netplay_rb_finish_frame();
        /* Exchange cores during Replay so mid-resim forks abort before POST. */
        np_emit_frame_commit(done);
        /* Outside emit: Live FMV skips FRAME_COMMIT; bisect still needs crumbs. */
        np_cd_bisect_tick(done);
        np_drain_peer_frame_commits();
        g_np.latched_for_tick = 0;
        g_np.needs_advance = 0; /* live latch must not outlive a resim vblank */
        g_np.frames_finished++;
        return;
    }

    if (!g_np.needs_advance) return;
    /* Digest the tick that just ran (sim_tick before advance). */
    done = rnet_session_sim_tick(g_np.session);
    np_emit_frame_commit(done);
    np_cd_bisect_tick(done);
    if (g_np.rollback) {
        uint32_t resume_hint = 0;
        /* TipHold Live: reconcile late wire so tip-extend can schedule rereplay. */
        if (psx_netplay_rb_tip_holding())
            np_rollback_reconcile_wire();
        psx_netplay_rb_pump();
        psx_netplay_rb_request_snap(done);
        /* Flush at vblank: MotK IRQ fast/mid paths used to skip poll_snap, so
         * deferred BB-edge saves never ran and the ring stayed empty. Prefer
         * IRQ BB-edge PCs — cpu->pc is often 0 during present/finish_frame. */
        if (g_np.cpu) {
            resume_hint = psx_compiled_irq_resume_pc();
            if (!psx_is_dispatchable(resume_hint))
                resume_hint = psx_last_irq_check_pc();
            if (!psx_is_dispatchable(resume_hint))
                resume_hint = g_np.cpu->pc;
            psx_netplay_rb_poll(g_np.cpu, resume_hint);
        }
    }
    rnet_session_advance(g_np.session);
    /* DELAY_SYNC may commit on this advance — keep g_np.input_delay aligned. */
    np_sched_sync_delay_from_session();
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.frames_finished++;
}

void psx_netplay_bind_cpu(struct CPUState *cpu)
{
    g_np.cpu = (CPUState*)cpu;
}

uint32_t psx_netplay_resolved_through(void)
{
    if (!psx_netplay_active()) return 0;
    return netplay_hc_resolved_through(&g_np.hc);
}

int psx_netplay_hash_confirm_through(uint32_t tick)
{
    if (!psx_netplay_active()) return 0;
    return netplay_hc_confirm_through(&g_np.hc, tick) ? 1 : 0;
}

int psx_netplay_rollback_mode(void)
{
    return psx_netplay_active() && g_np.rollback;
}

void psx_netplay_poll_snap(struct CPUState *cpu, uint32_t resume_pc)
{
    if (!psx_netplay_active() || !g_np.rollback)
        return;
    psx_netplay_rb_poll(cpu, resume_pc);
    /* BB-edge (interrupts.c): safe to longjmp like savestate_poll. */
    psx_netplay_rb_flush_resume();
}

int psx_netplay_is_resimulating(void)
{
    return psx_netplay_active() && g_np.rollback && psx_netplay_rb_is_resimulating();
}

int psx_netplay_remote_lead(void)
{
    RNetSessionStats st;
    if (!psx_netplay_active())
        return 0;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    return st.remote_lead;
}

int psx_netplay_input_delay(void)
{
    RNetSessionStats st;
    if (!psx_netplay_active())
        return 2;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    return st.delay > 0 ? (int)st.delay : 2;
}

int psx_netplay_catchup_budget(void)
{
    int lead;
    int delay;
    int extra;
    int budget;
    int cap;

    if (!psx_netplay_active())
        return 0;
    if (psx_start_bisect_no_catchup())
        return 0;
    /* §114: during post-FMV DESYNC / heal sticky, do not turbo catch-up —
     * absurd lead after heal abort + catchup widens platform tip+1 skew. */
    if (psx_netplay_rb_fmv_desync_hold() || psx_netplay_rb_post_fmv_heal_sticky())
        return 0;
    cap = np_starv_env_int("PSX_NET_CATCHUP_CAP", PSX_CATCHUP_CAP_DEFAULT);
    if (cap <= 0 && g_starv.recovery_amount <= 0)
        return 0;
    lead = psx_netplay_remote_lead();
    delay = psx_netplay_input_delay();
    if (delay < 0)
        delay = 0;
    /* Only spend surplus above D; keep the delay runway intact. */
    extra = lead - delay;
    if (extra < 0)
        extra = 0;
    budget = extra;
    if (g_starv.recovery_amount > budget)
        budget = g_starv.recovery_amount;
    if (budget > cap)
        budget = cap;
    return budget;
}

void psx_netplay_catchup_consume_frame(void)
{
    if (g_starv.recovery_amount > 0)
        g_starv.recovery_amount--;
}

void psx_netplay_wait_recv(int timeout_ms)
{
    if (!psx_netplay_active()) return;
    (void)rnet_session_wait_recv(g_np.session, timeout_ms);
}

void psx_netplay_admit_wait_info(char *stall_out, size_t stall_cap,
                                 uint32_t *sim_tick_out, int *lead_out)
{
    RNetSessionStats st;
    const char *name = "inactive";
    char phase[96];
    memset(&st, 0, sizeof(st));
    phase[0] = '\0';
    if (psx_netplay_active() && g_np.session) {
        rnet_session_get_stats(g_np.session, &st);
        name = rnet_admit_stall_name(st.last_stall);
        if (!name || !name[0])
            name = "unknown";
        /* LOAD_READY never calls try_admit, so last_stall stays "ok" — surface
         * the app barrier phase (+ transfer progress) instead. */
        switch (g_np.xfer) {
        case NP_XFER_SAVE_COORD:
            snprintf(phase, sizeof(phase), "save_coord");
            break;
        case NP_XFER_SAVE_PROBE:
            snprintf(phase, sizeof(phase), "save_probe");
            break;
        case NP_XFER_SAVE_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "save_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "save_xfer");
            break;
        case NP_XFER_MC_PROBE:
            snprintf(phase, sizeof(phase), "mc_probe");
            break;
        case NP_XFER_MC_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "mc_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "mc_xfer");
            break;
        case NP_XFER_LOAD_PROBE:
            snprintf(phase, sizeof(phase), "load_probe");
            break;
        case NP_XFER_LOAD_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "load_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "load_xfer");
            break;
        case NP_XFER_LOAD_APPLYING:
            if (savestate_pending()) {
                if (g_starv.latched)
                    snprintf(phase, sizeof(phase), "load_applying+starv_%s", name);
                else
                    snprintf(phase, sizeof(phase), "load_applying+%s", name);
            } else {
                snprintf(phase, sizeof(phase), "load_apply_done+%s", name);
            }
            break;
        case NP_XFER_LOAD_READY:
            if (g_np.load_ready_replied)
                snprintf(phase, sizeof(phase), "load_ready_admit+%s", name);
            else if (g_np.load_applied_local)
                snprintf(phase, sizeof(phase), "load_ready_wait_peer+%s", name);
            else
                snprintf(phase, sizeof(phase), "load_ready+%s", name);
            break;
        default:
            break;
        }
        /* Rollback episode: try_admit is skipped so last_stall stays "ok" —
         * surface the RB FSM phase instead. */
        if (!phase[0] && g_np.rollback && psx_netplay_rb_active()) {
            static const char *const k_rb_phase[] = {
                "rb_live", "rb_seal", "rb_baseline", "rb_replay",
                "rb_verify", "rb_commit", "rb_abort"
            };
            int ph = psx_netplay_rb_phase();
            if (ph >= 0 && ph < (int)(sizeof(k_rb_phase) / sizeof(k_rb_phase[0])))
                snprintf(phase, sizeof(phase), "%s", k_rb_phase[ph]);
            else
                snprintf(phase, sizeof(phase), "rb_phase_%d", ph);
        }
        /* Live rollback invent/lockstep never touches rnet try_admit — MotK
         * stall tag (wire_hole / fmv_settle / …) replaces the useless "ok". */
        if (!phase[0] && g_np.rollback && g_np.xfer == NP_XFER_NONE) {
            const char *motk = np_sched_admit_stall_tag();
            if (motk && motk[0])
                snprintf(phase, sizeof(phase), "%s", motk);
        }
    }
    if (stall_out && stall_cap) {
        if (phase[0])
            snprintf(stall_out, stall_cap, "%s", phase);
        else {
            strncpy(stall_out, name, stall_cap - 1);
            stall_out[stall_cap - 1] = '\0';
        }
    }
    if (sim_tick_out)
        *sim_tick_out = st.sim_tick;
    if (lead_out)
        *lead_out = st.remote_lead;
}

#endif /* PSX_HAS_RECOMP_NET */
