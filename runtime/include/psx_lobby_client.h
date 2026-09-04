#ifndef PSX_LOBBY_CLIENT_H
#define PSX_LOBBY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_LOBBY_ID_LEN 40
#define PSX_LOBBY_NAME_LEN 64
#define PSX_LOBBY_VERSION_LEN 32
#define PSX_LOBBY_ENDPOINT_LEN 64
#define PSX_LOBBY_MAX_LIST 32
#define PSX_LOBBY_MAX_MEMBERS 8
#define PSX_LOBBY_MAX_LAN_EPS 4
#define PSX_LOBBY_LANG_LEN 16

#ifndef PSX_GAME_VERSION
#define PSX_GAME_VERSION "dev"
#endif

typedef struct PsxLobbyRow {
    char     lobby_id[PSX_LOBBY_ID_LEN];
    char     name[PSX_LOBBY_NAME_LEN];
    char     game_name[PSX_LOBBY_NAME_LEN];
    char     game_version[PSX_LOBBY_VERSION_LEN];
    int      player_count;
    int      max_slots;
    int      has_password;
    /* Host UDP endpoint from the server list (for one-shot latency probes). */
    char     host_endpoint[PSX_LOBBY_ENDPOINT_LEN];
    /* Legacy hub lan_endpoints (compat). Prefer local UDP beacon by lobby_id. */
    char     lan_endpoints[PSX_LOBBY_MAX_LAN_EPS][PSX_LOBBY_ENDPOINT_LEN];
    int      lan_count;
    /* Round-trip ms to a reachable candidate; -1 unknown / timed out. */
    int      latency_ms;
} PsxLobbyRow;

typedef struct PsxLobbyMember {
    int  slot;
    char player_id[PSX_LOBBY_ID_LEN];
    char display_name[PSX_LOBBY_NAME_LEN];
    int  ready;
    /* Peer BIOS capability from set_ready bios_offer (0 if legacy/missing). */
    int  bios_offer_valid;
    int  bios_can_openbios;   /* linked OpenBIOS backend */
    int  bios_can_scph1001;   /* linked retail + validated dump available */
    int  bios_prefer_openbios; /* explicit OpenBIOS pick (not retail) */
} PsxLobbyMember;

/*
 * Local BIOS capability advertised on set_ready (see docs/BIOS_SELECTION.md
 * netplay settle rule). Updated by the host runtime via psx_lobby_set_bios_offer.
 */
typedef struct PsxLobbyBiosOffer {
    int  valid;
    int  can_openbios;
    int  can_scph1001;
    int  prefer_openbios; /* 1 = OpenBIOS selected; 0 = retail / willing SCPH */
} PsxLobbyBiosOffer;

/*
 * Host-authoritative sim settings negotiated over the lobby.
 * Guests apply these on launch so both peers boot with matching caps.
 */
typedef struct PsxLobbyMatchCaps {
    int  valid;            /* 1 when a host blob was received / set */
    int  aspect_num;       /* e.g. 4, 16, 21 */
    int  aspect_den;       /* e.g. 3, 9 */
    int  turbo_loads;      /* 0/1 */
    int  bios_hle;         /* 0/1 */
    int  fast_boot;        /* 0/1 */
    int  auto_skip_fmv;    /* 0/1 */
    int  input_delay;      /* recomp-net delay frames (D) */
    int  input_prediction; /* invent runway frames (P); rollback only */
    int  force_input_relay; /* 0/1 — server input relay (vs P2P) */
    int  force_turn;       /* 0/1 — ICE relay-only (Force TURN for UDP) */
    int  rollback;         /* 0/1 — invent/rollback netplay (default on) */
    /* DualShock-on-multitap-tap hack (0/1). Host-authoritative for the match. */
    int  multitap_analog;
    char language[PSX_LOBBY_LANG_LEN];
    /* Settled match BIOS: "openbios" | "scph1001" | "" (unset / legacy). */
    char session_bios[16];
} PsxLobbyMatchCaps;

typedef struct PsxLobbyJoinInfo {
    int      ok;
    char     lobby_id[PSX_LOBBY_ID_LEN];
    uint32_t session_id;
    int      local_slot;
    char     host_endpoint[PSX_LOBBY_ENDPOINT_LEN];
    char     guest_endpoint[PSX_LOBBY_ENDPOINT_LEN];
    char     bind_hostport[PSX_LOBBY_ENDPOINT_LEN];
    char     peer_hostport[PSX_LOBBY_ENDPOINT_LEN];
    int      player_count;
    int      max_slots;
    char     last_error[64]; /* need_password | bad_password | … */
} PsxLobbyJoinInfo;

/* Default URL when PSX_NET_LOBBY_URL unset. Order:
 *   1) env PSX_NET_LOBBY_URL
 *   2) compile-time PSX_NET_LOBBY_DEFAULT_URL (title CMake / scaffold)
 *   3) ws://netplay.retcomm.net:8765 */
const char *psx_lobby_default_url(void);

int  psx_lobby_connect(const char *ws_url); /* 0 = connected or connect started */
void psx_lobby_disconnect(void);
int  psx_lobby_connected(void);
/* 1 while DNS/TCP/WebSocket upgrade runs off the UI thread. */
int  psx_lobby_connecting(void);

void psx_lobby_set_display_name(const char *name);
const char *psx_lobby_display_name(void);
const char *psx_lobby_player_id(void);

/* Non-blocking pump — call every frame from the launcher. */
void psx_lobby_pump(void);

/*
 * Title + release pin used for create/join matching and list filters.
 * Defaults: game_name empty (no name filter), game_version PSX_GAME_VERSION.
 * List: Release pins filter by exact game_version; "dev" lists all versions
 * of the title (join still requires an exact version match).
 */
void psx_lobby_set_game_identity(const char *game_name, const char *game_version);
const char *psx_lobby_game_version(void);

/*
 * TOC fingerprint (lowercase hex SHA-256 from DiscIdentity::disc_fp).
 * Sent on create/join so the lobby server can reject mismatched mounts
 * (e.g. Track-01-only vs full multi-track cue). Empty disables the check
 * on the server for that peer (legacy clients); modern clients always set it
 * after a successful disc verify.
 */
void psx_lobby_set_disc_fp(const char *disc_fp);
const char *psx_lobby_disc_fp(void);

/* Default max_slots for create (clamped 2..8, default 2). */
void psx_lobby_set_max_slots(int max_slots);

void psx_lobby_request_list(void);
int  psx_lobby_list_count(void);
int  psx_lobby_list_get(int index, PsxLobbyRow *out);

/*
 * Create lobby. host_bind e.g. "0.0.0.0:7777". password may be NULL/empty.
 * game_version NULL/empty → identity / PSX_GAME_VERSION / "dev".
 * match_caps may be NULL (legacy); when non-NULL and valid, sent to the server
 * so guests join with the host's sim settings.
 * Returns 0 if request sent; poll psx_lobby_join_info() / in_lobby().
 */
int  psx_lobby_create(const char *name, const char *game_name,
                      const char *game_version,
                      const char *password, const char *host_bind,
                      const PsxLobbyMatchCaps *match_caps);

int  psx_lobby_join(const char *lobby_id, const char *password,
                    const char *guest_bind);

int  psx_lobby_leave(void);

/* Host-only: remove the player in `slot` (not the host / self). */
int  psx_lobby_kick(int slot);

/* Host-only: swap/move a seated player between slots (server broadcasts update). */
int  psx_lobby_move_member(int from_slot, int to_slot);

int  psx_lobby_in_lobby(void);
int  psx_lobby_is_host(void);
/* Host's player_id from the last create/lobby_update (empty if unknown). */
const char *psx_lobby_host_player_id(void);
/* Filled after create/join/lobby_update; peer endpoints for PsxNetplayConfig. */
const PsxLobbyJoinInfo *psx_lobby_join_info(void);

/* Latest host match_caps (valid==0 until create/join/launch delivers one). */
const PsxLobbyMatchCaps *psx_lobby_match_caps(void);

/* Host: push updated caps while in lobby (clears ready via lobby_update). */
int  psx_lobby_set_match_caps(const PsxLobbyMatchCaps *caps);

/* Live member table from lobby_update (and create/join). */
int  psx_lobby_member_count(void);
int  psx_lobby_member_get(int index, PsxLobbyMember *out);

/* Waiting-room RTT in ms *to* `slot`, or -1 if unknown / local seat.
 * Prefers ICE/TURN data-channel RTT when the lobby probe completes
 * (CGNAT / Force TURN); else direct UDP rnet_rtt_probe. Stored as
 * max(local, peer REPORT) so an optimistic guest sample cannot undercut
 * delay. Not the lobby WebSocket RTT. */
int  psx_lobby_member_latency_ms(int slot);

/* True when member.player_id matches psx_lobby_host_player_id().
 * Prefer this over `slot == 0` — seats can move. */
int  psx_lobby_member_is_host(const PsxLobbyMember *member);

/*
 * ICE signaling relay (MotK WS op:signal). text is SDP/candidate (max 2047).
 * send returns 0 if queued/written; poll returns 1 when an inbound signal was
 * copied out (LOCAL_* types as emitted by the peer — remap to REMOTE_* before
 * rnet_session_push_signal).
 */
int  psx_lobby_send_signal(int type, int flag, const char *text);
int  psx_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap);
/* Drop queued ICE SDP/candidates (soft-return / rematch hygiene). */
void psx_lobby_clear_signals(void);
/* When 0, inbound ICE op:signal is discarded (lobby / post-match). Launch
 * re-enables so early peer offers are kept until netplay drains them. */
void psx_lobby_set_ice_signal_accept(int accept);

/*
 * Coturn / ICE credentials minted by the WS lobby
 * (`get_turn_credentials` → `turn_credentials`). Valid until disconnect or TTL.
 * Strings are stable until the next successful mint or disconnect — safe to
 * pass into RNetIceConfig for juice_create.
 */
typedef struct PsxLobbyTurnCredentials {
    int      valid; /* 1 when ok mint cached and not expired */
    char     stun_host[128];
    int      stun_port;
    char     turn_host[128];
    int      turn_port;
    char     username[192];
    char     password[128];
    uint32_t ttl_secs;
} PsxLobbyTurnCredentials;

/* Queue WS get_turn_credentials. Returns 0 if sent/queued. */
int  psx_lobby_request_turn_credentials(void);
/* Non-NULL; valid==0 when unavailable / expired / STUN-only. */
const PsxLobbyTurnCredentials *psx_lobby_turn_credentials(void);

/* Local ready flag (from last lobby_update matching our player_id). */
int  psx_lobby_local_ready(void);
/* True when every seated player is ready and player_count >= 2. */
int  psx_lobby_all_ready(void);

/* Toggle ready in the current lobby (attaches current bios_offer). */
int  psx_lobby_set_ready(int ready);

/* Local BIOS offer used on the next set_ready (and included in settle). */
void psx_lobby_set_bios_offer(const PsxLobbyBiosOffer *offer);
const PsxLobbyBiosOffer *psx_lobby_bios_offer(void);

/*
 * Settle session BIOS from seated peers' bios_offer (+ local offer):
 *   OpenBIOS if anyone cannot run SCPH-1001 (missing offer ⇒ cannot);
 *   else SCPH-1001 when the host prefers retail and every peer can;
 *   else OpenBIOS if anyone prefers OpenBIOS; else SCPH-1001.
 * Host preference wins over guest OpenBIOS picks when all can SCPH.
 * Writes "openbios" or "scph1001" into out. Returns 0 on success.
 */
int  psx_lobby_settle_session_bios(char *out, size_t out_cap);

/*
 * Host: ask server to broadcast launch. When match_caps is non-NULL and valid,
 * it is attached to start so launch freezes the latest host settings.
 */
int  psx_lobby_request_start(const PsxLobbyMatchCaps *match_caps);

/*
 * Set when server sends op:launch. Both host and guests should boot netplay.
 * Cleared by psx_lobby_clear_launch_pending() after consuming.
 */
int  psx_lobby_launch_pending(void);
void psx_lobby_clear_launch_pending(void);

/* After soft-return / rematch: allow waiting-room ICE/UDP RTT probes again.
 * Launch suspends them so they cannot steal match ICE signaling. */
void psx_lobby_resume_waiting_room_rtt(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_LOBBY_CLIENT_H */
