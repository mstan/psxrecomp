#ifndef PSX_LOBBY_CLIENT_H
#define PSX_LOBBY_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_LOBBY_ID_LEN 40
#define PSX_LOBBY_NAME_LEN 64
#define PSX_LOBBY_ENDPOINT_LEN 64
#define PSX_LOBBY_MAX_LIST 32
#define PSX_LOBBY_MAX_MEMBERS 4
#define PSX_LOBBY_LANG_LEN 16

typedef struct PsxLobbyRow {
    char     lobby_id[PSX_LOBBY_ID_LEN];
    char     name[PSX_LOBBY_NAME_LEN];
    char     game_name[PSX_LOBBY_NAME_LEN];
    int      player_count;
    int      max_slots;
    int      has_password;
} PsxLobbyRow;

typedef struct PsxLobbyMember {
    int  slot;
    char player_id[PSX_LOBBY_ID_LEN];
    char display_name[PSX_LOBBY_NAME_LEN];
    int  ready;
} PsxLobbyMember;

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
    int  input_delay;      /* recomp-net delay frames */
    char language[PSX_LOBBY_LANG_LEN];
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

/* Default URL when PSX_NET_LOBBY_URL unset:
 * ws://netplay.technicallycomputers.ca:8765 */
const char *psx_lobby_default_url(void);

int  psx_lobby_connect(const char *ws_url); /* 0 ok */
void psx_lobby_disconnect(void);
int  psx_lobby_connected(void);

void psx_lobby_set_display_name(const char *name);
const char *psx_lobby_display_name(void);
const char *psx_lobby_player_id(void);

/* Non-blocking pump — call every frame from the launcher. */
void psx_lobby_pump(void);

void psx_lobby_request_list(void);
int  psx_lobby_list_count(void);
int  psx_lobby_list_get(int index, PsxLobbyRow *out);

/*
 * Create lobby. host_bind e.g. "0.0.0.0:7777". password may be NULL/empty.
 * match_caps may be NULL (legacy); when non-NULL and valid, sent to the server
 * so guests join with the host's sim settings.
 * Returns 0 if request sent; poll psx_lobby_join_info() / in_lobby().
 */
int  psx_lobby_create(const char *name, const char *game_name,
                      const char *password, const char *host_bind,
                      const PsxLobbyMatchCaps *match_caps);

int  psx_lobby_join(const char *lobby_id, const char *password,
                    const char *guest_bind);

int  psx_lobby_leave(void);

int  psx_lobby_in_lobby(void);
int  psx_lobby_is_host(void);
/* Filled after create/join/lobby_update; peer endpoints for PsxNetplayConfig. */
const PsxLobbyJoinInfo *psx_lobby_join_info(void);

/* Latest host match_caps (valid==0 until create/join/launch delivers one). */
const PsxLobbyMatchCaps *psx_lobby_match_caps(void);

/* Host: push updated caps while in lobby (clears ready via lobby_update). */
int  psx_lobby_set_match_caps(const PsxLobbyMatchCaps *caps);

/* Live member table from lobby_update (and create/join). */
int  psx_lobby_member_count(void);
int  psx_lobby_member_get(int index, PsxLobbyMember *out);

/* Local ready flag (from last lobby_update matching our player_id). */
int  psx_lobby_local_ready(void);
/* True when every seated player is ready and player_count >= 2. */
int  psx_lobby_all_ready(void);

/* Toggle ready in the current lobby. */
int  psx_lobby_set_ready(int ready);

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

#ifdef __cplusplus
}
#endif

#endif /* PSX_LOBBY_CLIENT_H */
