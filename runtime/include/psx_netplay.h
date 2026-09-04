#ifndef PSX_NETPLAY_H
#define PSX_NETPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Delay-sync netplay facade over recomp-net (LAN UDP or MotK ICE).
 * Online hosted lobbies use ICE + WS signaling; Direct IP / LAN stay on UDP.
 *
 * Lockstep contract (matches recomp-net host_integration.md):
 *   wait_admit (publish pads for tick T) → guest runs frame T →
 *   finish_frame (advance) → wait_admit for T+1 → …
 * Guest must NOT run while linking or while try_admit fails.
 *
 * Input ownership:
 *   - Each peer stages one local device sample; recomp-net maps it onto
 *     that peer's local_slot (slot 0 = sim P1, slot 1 = sim P2). Transport
 *     host/client role is determined by peer_hostport, not by local_slot.
 *   - input_player selects which host PlayerInput to sample; -1 = auto
 *     (prefer g_players[local_slot] if assigned, else player 0).
 *   - While active, publish / release_pads is the sole SIO writer.
 *   - Every session slot stays plugged for in-game N-player detect.
 *   - slot_count >= 3 enables SCPH-1070 multitap (sio_set_multitap).
 *
 * Pad blob (8 bytes):
 *   [0..1] buttons LE u16 (PSX active-low)
 *   [2] lx  [3] ly  [4] rx  [5] ry
 *   [6] analog (0/1)
 *   [7] connected (always 1)
 */

#define PSX_NETPLAY_PAD_BYTES 8

typedef struct PsxNetPad {
    uint16_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  analog;
    uint8_t  connected;
} PsxNetPad;

typedef struct PsxNetplayConfig {
    int         enabled;
    int         local_slot;    /* 0 .. slot_count-1 */
    int         slot_count;    /* 2 .. PSX_MAX_PLAYERS (session pad count) */
    int         player_count;  /* seated players at launch (0 = use slot_count) */
    /* Bit i = lobby seat i occupied. 0 = all seats occupied (legacy). Sparse
     * rooms (moved seats leaving a hole) must set this so recomp-net does not
     * wait forever on empty remotes. */
    uint32_t    occupied_mask;
    int         input_player;  /* host device index; -1 = auto */
    int         input_delay;
    /* Rollback invent runway (phase_lock / P). Clamped 2..16. Unused in delay-sync. */
    int         input_prediction;
    int         force_input_relay; /* 1 = lobby UDP SFU star (from launch) */
    int         force_turn;        /* 1 = ICE relay-only (Force TURN for UDP) */
    /* 0 = auto (MotK room → ICE, else LAN), 1 = force ICE, 2 = force LAN.
     * Env PSX_NET_TRANSPORT=lan|ice overrides. */
    int         transport;
    /* 0 = delay-sync, 1 = rollback invent/contract (lobby default on).
     * Env PSX_NET_MODE=delay|rollback overrides. */
    int         rollback;
    uint32_t    session_id;
    char        bind_hostport[64];
    char        peer_hostport[64];
} PsxNetplayConfig;

void psx_netplay_config_defaults(PsxNetplayConfig *cfg);
void psx_netplay_apply_env(PsxNetplayConfig *cfg);

int  psx_netplay_active(void);
int  psx_netplay_is_running(void);
/* "ice" | "lan" | "none" */
const char *psx_netplay_transport_name(void);
/* 1 when ICE agent reached FAILED (online path). */
int  psx_netplay_ice_failed(void);
/* Optional JSONL samples when PSX_NET_DIAG=1 (saves/netplay/net_diag.jsonl). */
void psx_netplay_diag_tick(void);
int  psx_netplay_local_slot(void);
/* Resolved host player index used for local capture. */
int  psx_netplay_input_player(void);
uint32_t psx_netplay_sim_tick(void);

/*
 * Snapshot for diagnostic dumps (starvation_dump.jsonl meta, etc.).
 * arch_out: "off" | "p2p" | "host_relay" | "server_relay" (never NULL when
 * arch_cap > 0). Returns 1 when netplay is/was configured this run.
 */
int  psx_netplay_diag_snapshot(char *arch_out, size_t arch_cap,
                               int *max_players_out, int *player_count_out);

int  psx_netplay_start(const PsxNetplayConfig *cfg);
void psx_netplay_shutdown(void);
/* Soft-return rematch / new lobby opponent: make host sim state match a cold
 * process peer. Call from session_reboot (rematch) and after BYE teardown.
 * Does not replace device *_init — orchestrates sticky host statics. */
void psx_netplay_cold_reset(void);

/*
 * After savestate_configure + memcard_init: refresh RB integrity keys from
 * savestate (host + guest — start() ran before configure), then guest-only
 * redirect .pst/.mcd writes to saves/netplay/ so host sync never touches
 * personal saves.
 */
void psx_netplay_bind_guest_saves(void);

/* 1 if this peer is sim authority (local_slot == 0). */
int  psx_netplay_is_host(void);

/*
 * Host-only save/load orchestration (hash probe → transfer on miss).
 * Returns 1 if the request was accepted/ignored-as-guest, 0 if netplay inactive.
 */
int  psx_netplay_request_save(int slot);
int  psx_netplay_request_load(int slot);

/* 1 while a save/load/memcard probe, chunk transfer, or post-load ready owns
 * the clock (long admit timeout, no peer-silence kick, FPS suppressed). */
int  psx_netplay_in_load_barrier(void);

/* 1 once after a staged netplay load apply failed (stale .pst / mismatch).
 * Clears. Caller should soft-exit to lobby — do not keep waiting on the barrier. */
int  psx_netplay_consume_load_apply_failed(void);

/* Stage local pad for the current sim tick. Ignored once that tick is latched.
 * Always refreshes the live physical snapshot (see live_pad_buttons). */
void psx_netplay_stage_local(const PsxNetPad *pad);

/* PSX_RB_PAD_TRACE=1: host capture checkpoint (card / SDL Start / fallback).
 * Call from capture_local_human_pad each sample; no-op when trace off. */
void psx_netplay_pad_trace_dev(int card, int fallback, int sdl_start,
                               uint16_t buttons);

/*
 * Start cadence bisect (PSX_START_BISECT=1): every-sample pipeline log for
 * offline vs online. Does not change debounce. Optional feature kills
 * (one at a time) for Stage-3 isolation — see ROLLBACK_MOTK_HOOKUP.md.
 *
 *   PSX_START_BISECT_NO_GC_UPDATE_IN_ADMIT=1
 *   PSX_START_BISECT_NO_TIPHOLD_CAPTURE=1
 *   PSX_START_BISECT_NO_CATCHUP=1
 *   PSX_START_BISECT_NO_REPLAY_PRODUCE=1
 *   PSX_START_BISECT_SPIN=1  — also log raw SDL on admit spins (dense)
 */
int  psx_start_bisect_enabled(void);
int  psx_start_bisect_no_gc_update_in_admit(void);
int  psx_start_bisect_no_tiphold_capture(void);
int  psx_start_bisect_no_catchup(void);
int  psx_start_bisect_no_replay_produce(void);
int  psx_start_bisect_spin_log(void);
/* path: SDL|cap|stage|tip|sio|spin  — start bits are 1=pressed (active-low decoded). */
void psx_start_bisect_log(const char *path, uint32_t sim, int sdl_start,
                          int cap_start, int deb_start, int sio_start,
                          int latched, int tip_hold, int resim);

/*
 * Start consumer bisect (PSX_START_CONSUMER=1): log the Start bit SIO presents
 * to the guest each sim frame (offline present sample / online publish apply).
 * Compare offline vs online timelines — not SDL. edge=↓|↑|- .
 */
int  psx_start_consumer_enabled(void);
/* Offline: bump once per sample_pad_into_sio; returned value is the frame id. */
uint32_t psx_start_consumer_offline_frame(void);
void psx_start_consumer_note(int slot, uint32_t sim, uint16_t buttons);

/* 1 while linking or before this sim tick's local pad is latched. */
int  psx_netplay_needs_local_sample(void);

/* Latest physical local buttons (0xFFFF idle). TipHold SAFETY/quiet must use
 * this — staged/delay-ring peeks stay frozen while sim is invent-capped. */
int  psx_netplay_live_pad_buttons(uint16_t *out);

/* 1 if INPUT_CONFIRM hash disagreement stalled the session. */
int  psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash);

/* 1 if peer sent BYE or went silent for ~timeout_ms (default 1500).
 * Pass 0 for BYE-only (load barrier / LINKING before HELLO). */
int  psx_netplay_peer_disconnected(uint32_t timeout_ms);

/* Re-arm RUNNING silence clock after a pump (admit barrier entry). */
void psx_netplay_touch_peer_liveness(void);

/* RUNNING peer_disconnected timeout. 0 = BYE-only (early sim / boot tip wait /
 * pcap_freeze — rematch dig0 can exceed 1.5s without INPUT). */
uint32_t psx_netplay_running_liveness_timeout_ms(void);

/*
 * Ingress / lobby / INPUT retransmit without try_admit. Used while the
 * delay-sync starvation latch holds for remote runway refill.
 */
void psx_netplay_pump(void);

/*
 * Pump + try_admit for the current sim tick. On success, publish has written
 * SIO and a finish_frame() is owed after the guest completes that tick.
 * Returns 1 if admitted, 0 if caller must keep polling (linking / wait).
 * Does NOT advance the session clock.
 *
 * After sustained admit misses, latches starvation (pump-only) until
 * remote_lead >= D for a few frames, then arms a recovery catch-up boost.
 * Env: PSX_NET_STARVATION_ENTER_FRAMES, EXIT_FRAMES, EXIT_HR_LEAD.
 */
int  psx_netplay_poll_admit(void);

/* Call after the guest finishes the admitted tick (vblank boundary). */
void psx_netplay_finish_frame(void);

/* highest_remote_wire - sim_tick (0 if inactive; can be negative). */
int  psx_netplay_remote_lead(void);
/* Session input delay frames (default 2 when inactive). */
int  psx_netplay_input_delay(void);

/* Clear timesync pegged-streak at episode/tip-hold boundaries so resim cost
 * cannot look like WAN transit and trip adaptive-off (see §22). */
void psx_netplay_timesync_on_episode_boundary(void);

/*
 * Extra headroom for post-starvation / behind-peer catch-up
 * (min(16, max(0, remote_lead - D, recovery_burst))).
 * Host should skip wall-clock pace while this is > 0, then call
 * psx_netplay_catchup_consume_frame() once per skipped pace.
 */
int  psx_netplay_catchup_budget(void);
void psx_netplay_catchup_consume_frame(void);

/* Park the admit barrier until a peer datagram may be ready (or timeout). */
void psx_netplay_wait_recv(int timeout_ms);

/* Diagnostics for a stuck admit barrier (stall name, sim tick, remote lead). */
void psx_netplay_admit_wait_info(char *stall_out, size_t stall_cap,
                                 uint32_t *sim_tick_out, int *lead_out);

/* Normalize sticks (deadzone → center) for stabler cross-device blobs. */
void psx_netplay_normalize_pad(PsxNetPad *pad);

void psx_netplay_release_pads(void);

/*
 * Bind the live CPUState for per-frame master digests (FRAME_COMMIT).
 * Call once after CPU init (alongside debug_server_set_cpu).
 */
struct CPUState;
void psx_netplay_bind_cpu(struct CPUState *cpu);

/* Highest sim tick whose master digest matched the peer (0 if none). */
uint32_t psx_netplay_resolved_through(void);
/* 1 if peer master-hash watermark covers tick (hash_confirm_promote). */
int  psx_netplay_hash_confirm_through(uint32_t tick);

/* 1 when PSX_NET_MODE=rollback (invent + input contract path). */
int  psx_netplay_rollback_mode(void);

/* Safe BB-edge snap save/load (call beside savestate_poll when !in_exception). */
void psx_netplay_poll_snap(struct CPUState *cpu, uint32_t resume_pc);
/* 1 while a rollback episode is resimulating (mute audio / skip wall pacer). */
int  psx_netplay_is_resimulating(void);

/* §63: reset host pad-edge trackers after snap load (guest SIO restored). */
void psx_netplay_on_rb_snap_loaded(void);

/* §64: restart hc-fork persist/retry clocks after tip-extend abandon so a
 * second recovery episode cannot open on the same Live tick. */
void psx_netplay_hc_fork_recovery_restart(void);
/* §102: drop pending hc-fork bookkeeping (peer-ahead NACK already scheduled
 * a light tip reopen — do not open a deep SPAN recovery). */
void psx_netplay_hc_fork_recovery_clear(void);

/* §93 P1 bisect: PSX_RB_CD_BISECT=1 (optional =LO-HI, default 150-250) logs
 * per-tick CD/MDEC/clk crumbs + CD cmd ISSUE/QUEUE/DONE timeline (cdrom.c;
 * includes stat/resp FIFO + GetTN tracks / GetTD td_lba).
 * Arm N more ticks after a media flush_resume. */
void psx_netplay_cd_bisect_arm(uint32_t from_sim, uint32_t ticks);
/* 1 when CD bisect env is on and sim is in the fixed or post-media arm window. */
int psx_netplay_cd_bisect_active(void);

/* Netplay GPU lock: CPU-authoritative VRAM (software raster) so snaps/digests
 * are peer-identical. If OpenGL was requested, keep the GL context for
 * present-only (CPU scanout → gl_renderer_present) — never glReadPixels.
 * Sim is forced to internal scale 1 (no SW SSAA hi-res mirror); present may
 * still upscale via GL/SDL filter. Vulkan still falls back to pure software
 * present. Implemented in main.cpp. */
void psx_frontend_netplay_force_sw_gpu(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_H */
