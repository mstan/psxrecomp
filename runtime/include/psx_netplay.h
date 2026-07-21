#ifndef PSX_NETPLAY_H
#define PSX_NETPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Delay-sync netplay facade over recomp-net (LAN peer UDP for now).
 * Lobby UI / ICE signaling are later work — this layer is CLI/env driven.
 *
 * Lockstep contract (matches recomp-net host_integration.md):
 *   wait_admit (publish pads for tick T) → guest runs frame T →
 *   finish_frame (advance) → wait_admit for T+1 → …
 * Guest must NOT run while linking or while try_admit fails.
 *
 * Input ownership:
 *   - Each peer stages one local device sample; recomp-net maps it onto
 *     that peer's local_slot (host 0 = sim P1, guest 1 = sim P2).
 *   - input_player selects which host PlayerInput (0/1) to sample; -1 = auto
 *     (prefer g_players[local_slot] if assigned, else player 0).
 *   - While active, publish / release_pads is the sole SIO writer.
 *   - Every session slot stays plugged for in-game 2P/VS detect.
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
    int         local_slot;    /* 0 or 1 */
    int         input_player;  /* 0/1 host device index; -1 = auto */
    int         input_delay;
    uint32_t    session_id;
    char        bind_hostport[64];
    char        peer_hostport[64];
} PsxNetplayConfig;

void psx_netplay_config_defaults(PsxNetplayConfig *cfg);
void psx_netplay_apply_env(PsxNetplayConfig *cfg);

int  psx_netplay_active(void);
int  psx_netplay_is_running(void);
int  psx_netplay_local_slot(void);
/* Resolved host player index (0/1) used for local capture. */
int  psx_netplay_input_player(void);
uint32_t psx_netplay_sim_tick(void);

int  psx_netplay_start(const PsxNetplayConfig *cfg);
void psx_netplay_shutdown(void);

/*
 * Guest only: after savestate_configure + memcard_init, redirect .pst/.mcd
 * writes to saves/netplay/ so host sync never touches personal saves.
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

/* 1 while post-load apply/ready barrier owns the clock (no FPS / no present). */
int  psx_netplay_in_load_barrier(void);

/* Stage local pad for the current sim tick. Ignored once that tick is latched. */
void psx_netplay_stage_local(const PsxNetPad *pad);

/* 1 while linking or before this sim tick's local pad is latched. */
int  psx_netplay_needs_local_sample(void);

/* 1 if INPUT_CONFIRM hash disagreement stalled the session. */
int  psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash);

/* 1 if peer sent BYE or went silent for ~timeout_ms (default 1500). */
int  psx_netplay_peer_disconnected(uint32_t timeout_ms);

/*
 * Pump + try_admit for the current sim tick. On success, publish has written
 * SIO and a finish_frame() is owed after the guest completes that tick.
 * Returns 1 if admitted, 0 if caller must keep polling (linking / wait).
 * Does NOT advance the session clock.
 */
int  psx_netplay_poll_admit(void);

/* Call after the guest finishes the admitted tick (vblank boundary). */
void psx_netplay_finish_frame(void);

/* Park the admit barrier until a peer datagram may be ready (or timeout). */
void psx_netplay_wait_recv(int timeout_ms);

/* Normalize sticks (deadzone → center) for stabler cross-device blobs. */
void psx_netplay_normalize_pad(PsxNetPad *pad);

void psx_netplay_release_pads(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_H */
