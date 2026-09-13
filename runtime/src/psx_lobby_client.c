#include "psx_lobby_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(PSX_HAS_LOBBY_CLIENT)

const char *psx_lobby_default_url(void)
{
    const char *e = getenv("PSX_NET_LOBBY_URL");
    if (e && e[0])
        return e;
#if defined(PSX_NET_LOBBY_DEFAULT_URL)
    return PSX_NET_LOBBY_DEFAULT_URL;
#else
    return "ws://netplay.retcomm.net:8765";
#endif
}
int  psx_lobby_connect(const char *ws_url) { (void)ws_url; return -1; }
void psx_lobby_disconnect(void) {}
int  psx_lobby_connected(void) { return 0; }
int  psx_lobby_connecting(void) { return 0; }
void psx_lobby_set_display_name(const char *name) { (void)name; }
const char *psx_lobby_display_name(void) { return ""; }
const char *psx_lobby_player_id(void) { return ""; }
void psx_lobby_pump(void) {}
void psx_lobby_request_list(void) {}
int  psx_lobby_list_count(void) { return 0; }
int  psx_lobby_list_get(int index, PsxLobbyRow *out) { (void)index; (void)out; return 0; }
void psx_lobby_set_game_identity(const char *a, const char *b) { (void)a; (void)b; }
const char *psx_lobby_game_version(void) { return PSX_GAME_VERSION; }
void psx_lobby_set_disc_fp(const char *disc_fp) { (void)disc_fp; }
const char *psx_lobby_disc_fp(void) { return ""; }
void psx_lobby_set_max_slots(int max_slots) { (void)max_slots; }
int  psx_lobby_create(const char *a, const char *b, const char *c, const char *d,
                      const char *e, const PsxLobbyMatchCaps *f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return -1; }
int  psx_lobby_join(const char *a, const char *b, const char *c)
{ (void)a; (void)b; (void)c; return -1; }
int  psx_lobby_leave(void) { return -1; }
int  psx_lobby_kick(int slot) { (void)slot; return -1; }
void psx_lobby_set_allow_spectators(int allow) { (void)allow; }
int  psx_lobby_allow_spectators_pref(void) { return 0; }
int  psx_lobby_allow_spectators(void) { return 0; }
int  psx_lobby_max_spectators(void) { return 0; }
int  psx_lobby_spectator_count(void) { return 0; }
int  psx_lobby_local_is_spectator(void) { return 0; }
int  psx_lobby_spectator_slot_base(void) { return PSX_LOBBY_SPECTATOR_SLOT_BASE; }
int  psx_lobby_spectator_slot(int index) { (void)index; return -1; }
int  psx_lobby_local_wire_slot(void) { return -1; }
int  psx_lobby_seat_valid(int slot)
{ return slot >= 0 && slot < PSX_LOBBY_MAX_PLAYERS; }
int  psx_lobby_move_member(int from_slot, int to_slot)
{ (void)from_slot; (void)to_slot; return -1; }
int  psx_lobby_in_lobby(void) { return 0; }
int  psx_lobby_is_host(void) { return 0; }
const char *psx_lobby_host_player_id(void) { return ""; }
const PsxLobbyJoinInfo *psx_lobby_join_info(void)
{
    static PsxLobbyJoinInfo z;
    return &z;
}
const PsxLobbyMatchCaps *psx_lobby_match_caps(void)
{
    static PsxLobbyMatchCaps z;
    return &z;
}
int  psx_lobby_set_match_caps(const PsxLobbyMatchCaps *c) { (void)c; return -1; }
int  psx_lobby_member_count(void) { return 0; }
int  psx_lobby_member_get(int index, PsxLobbyMember *out) { (void)index; (void)out; return 0; }
int  psx_lobby_member_latency_ms(int slot) { (void)slot; return -1; }
int  psx_lobby_send_server_chat(const char *text) { (void)text; return -1; }
int  psx_lobby_server_chat_count(void) { return 0; }
int  psx_lobby_server_chat_get(int index, PsxLobbyChatMsg *out) { (void)index; (void)out; return 0; }
void psx_lobby_resume_waiting_room_rtt(void) {}
int  psx_lobby_member_is_host(const PsxLobbyMember *member)
{
    (void)member;
    return 0;
}
int  psx_lobby_send_signal(int type, int flag, const char *text)
{
    (void)type;
    (void)flag;
    (void)text;
    return -1;
}
int  psx_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap)
{
    (void)type;
    (void)flag;
    (void)text;
    (void)text_cap;
    return 0;
}
void psx_lobby_clear_signals(void) {}
void psx_lobby_set_ice_signal_accept(int accept) { (void)accept; }
int  psx_lobby_request_turn_credentials(void) { return -1; }
const PsxLobbyTurnCredentials *psx_lobby_turn_credentials(void)
{
    static PsxLobbyTurnCredentials z;
    return &z;
}
int  psx_lobby_local_ready(void) { return 0; }
int  psx_lobby_all_ready(void) { return 0; }
int  psx_lobby_set_ready(int ready) { (void)ready; return -1; }
void psx_lobby_set_bios_offer(const PsxLobbyBiosOffer *offer) { (void)offer; }
const PsxLobbyBiosOffer *psx_lobby_bios_offer(void)
{
    static PsxLobbyBiosOffer z;
    return &z;
}
void psx_lobby_set_memcard_offer(const PsxLobbyMemcardOffer *offer) { (void)offer; }
const PsxLobbyMemcardOffer *psx_lobby_memcard_offer(void)
{
    static PsxLobbyMemcardOffer z;
    return &z;
}
int  psx_lobby_send_chat(const char *text) { (void)text; return -1; }
int  psx_lobby_report_chat(const char *const *m, int c, const char *r,
                           const char *n)
{ (void)m; (void)c; (void)r; (void)n; return -1; }
int  psx_lobby_set_blocks(const char *accounts) { (void)accounts; return -1; }
int  psx_lobby_automatch_request_rulesets(void) { return -1; }
int  psx_lobby_automatch_available(void) { return 0; }
int  psx_lobby_automatch_ruleset_count(void) { return 0; }
int  psx_lobby_automatch_ruleset_get(int i, PsxLobbyRuleset *o) { (void)i; (void)o; return 0; }
int  psx_lobby_automatch_queue(const char *r, int m, const char *e)
{ (void)r; (void)m; (void)e; return -1; }
int  psx_lobby_automatch_cancel(void) { return -1; }
int  psx_lobby_automatch_state(void) { return PSX_LOBBY_AUTOMATCH_IDLE; }
int  psx_lobby_automatch_queued_secs(void) { return 0; }
int  psx_lobby_automatch_pool(void) { return 0; }
int  psx_lobby_automatch_found_get(PsxLobbyAutomatchFound *o) { (void)o; return 0; }
int  psx_lobby_automatch_accept(int a) { (void)a; return -1; }
int  psx_lobby_automatch_room(void) { return 0; }
void psx_lobby_automatch_refuse_local(const char *w) { (void)w; }
const char *psx_lobby_automatch_error(void) { return ""; }
int  psx_lobby_seat_move_self(int to_slot) { (void)to_slot; return -1; }
int  psx_lobby_seat_swap_request(int target_slot) { (void)target_slot; return -1; }
int  psx_lobby_seat_swap_incoming(char *who, size_t who_cap, int *from_slot)
{ (void)who; (void)who_cap; (void)from_slot; return 0; }
int  psx_lobby_seat_swap_respond(int accept) { (void)accept; return -1; }
int  psx_lobby_seat_swap_outgoing(void) { return 0; }
void psx_lobby_seat_swap_clear(void) {}
int  psx_lobby_chat_count(void) { return 0; }
int  psx_lobby_chat_get(int index, PsxLobbyChatMsg *out) { (void)index; (void)out; return 0; }
void psx_lobby_chat_clear(void) {}
int  psx_lobby_settle_session_bios(char *out, size_t out_cap)
{
    if (!out || out_cap < 9) return -1;
    strncpy(out, "openbios", out_cap - 1);
    out[out_cap - 1] = '\0';
    return 0;
}
int  psx_lobby_request_start(const PsxLobbyMatchCaps *c) { (void)c; return -1; }
int  psx_lobby_launch_pending(void) { return 0; }
void psx_lobby_clear_launch_pending(void) {}

#else /* PSX_HAS_LOBBY_CLIENT */

#include "rnet_ws.h"
#include "rnet_sha1.h"
#include "recomp_net/address.h"
#include "recomp_net/ice.h"
#include "recomp_net/ice_rtt.h"
#include "recomp_net/lan_beacon.h"
#include "recomp_net/rtt_probe.h"
#include "recomp_net/chat_filter.h"
#include "recomp_net/chat_report.h"
#include "recomp_net/auth.h"   /* optional Discord session for `hello` */
#include "host_time.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#define close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* Every free-text value in an outbound frame goes through json_escape, and
 * JSON_ESC_CAP is the buffer its result needs. Escaping at most doubles a
 * value (only " \\ \n \r \t expand, each to two bytes; anything else below
 * 0x20 is dropped), so 2x + 8 never truncates -- which matters most for a
 * password, where a dropped character would put a secret on the wire that is
 * not the one the host typed. Declared up here because the first frame that
 * needs it is built well above the definition. */
#define JSON_ESC_CAP(n) ((n) * 2 + 8)
static size_t json_escape(const char *in, char *out, size_t cap);

/* Winsock sets WSAGetLastError(), not errno — bare errno checks drop the
 * non-blocking WS handshake on Windows (list/create look permanently dead). */
static int socket_would_block(void)
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

typedef struct {
    int fd;
    int connected;
    int handshake_done;
    char player_id[PSX_LOBBY_ID_LEN];
    char display_name[PSX_LOBBY_NAME_LEN];
    char host[128];
    int port;
    /* Actual TCP peer after connect (split-horizon / hairpin-safe for SFU). */
    char connected_peer_ip[64];
    char path[128];
    char rx_http[4096];
    size_t rx_http_len;
    /* Bytes that arrived with the HTTP 101 response after the header end. */
    uint8_t ws_pending[4096];
    size_t ws_pending_len;
    PsxLobbyRow list[PSX_LOBBY_MAX_LIST];
    int list_count;
    PsxLobbyOnlinePlayer online[PSX_LOBBY_MAX_ONLINE];
    int online_count;
    int in_lobby;
    int is_host;
    char host_player_id[PSX_LOBBY_ID_LEN];
    char my_bind[PSX_LOBBY_ENDPOINT_LEN];
    char filter_game_name[PSX_LOBBY_NAME_LEN];
    char filter_game_version[PSX_LOBBY_VERSION_LEN];
    char disc_fp[65]; /* lowercase hex SHA-256 TOC fingerprint; "" = unset */
    PsxLobbyJoinInfo join;
    PsxLobbyMember members[PSX_LOBBY_MAX_MEMBERS];
    int member_count;
    int local_ready;
    int all_ready;
    int launch_pending;
    PsxLobbyMatchCaps match_caps;
    PsxLobbyBiosOffer bios_offer;
    PsxLobbyMemcardOffer memcard_offer;
    /* Seat swap: one pending ask aimed at us, one outgoing result. */
    int  swap_in_valid;
    char swap_in_asker_id[PSX_LOBBY_ID_LEN];
    char swap_in_asker_name[PSX_LOBBY_NAME_LEN];
    int  swap_in_from_slot;
    int  swap_out; /* 0 idle, 1 waiting, 2 accepted, -1 declined */
    /* Lobby chat ring (oldest at chat_head). */
    PsxLobbyChatMsg chat[PSX_LOBBY_CHAT_RING];
    int chat_head;
    int chat_count;
    uint32_t chat_seq;
    /* Server (per-game) chat: a second ring with its own sequence. */
    PsxLobbyChatMsg schat[PSX_LOBBY_CHAT_RING];
    int schat_head;
    int schat_count;
    uint32_t schat_seq;
    char pending_tx[8][2048];
    int pending_n;
    /* Inbound ICE signals (WS op:signal). */
    struct {
        int type;
        int flag;
        char text[2048];
    } sig_q[32];
    int sig_head;
    int sig_tail;
    int sig_count;
    /* 0 when idle / after leave — drop stale ICE. Set while lobby ICE RTT
     * probe is active (2+ seated) and again on launch for match ICE. */
    int ice_signal_accept;
    /* 1 from op:launch until rematch/leave — blocks waiting-room ICE/UDP RTT
     * so clear_launch_pending cannot revive a probe that steals match signals. */
    int ice_rtt_suspended;
    /* Coturn mint from WS get_turn_credentials. */
    PsxLobbyTurnCredentials turn;
    time_t turn_received_at;
    int turn_request_pending;
    /* Waiting-room latency (ms) keyed by pad slot; -1 = unknown. */
    int member_rtt_ms[PSX_LOBBY_MAX_MEMBERS];
    uint64_t rtt_next_ping_ms;
} LobbyClient;

static LobbyClient g_lc = {
    .fd = -1,
    .filter_game_version = PSX_GAME_VERSION,
};

enum {
    PSX_LOBBY_SIG_RTT_PING = 100,
    PSX_LOBBY_SIG_RTT_PONG = 101,
    PSX_LOBBY_SIG_RTT_REPORT = 102
};

static uint64_t lobby_mono_ms(void)
{
    /* QPC / CLOCK_MONOTONIC — not GetTickCount64 (coarse Win↔Linux lobby RTT). */
    return psx_host_mono_ms();
}

/* Defined later; used by waiting-room RTT signal handling. */
int psx_lobby_send_signal(int type, int flag, const char *text);
static int endpoint_port_is_zero(const char *ep);
static int using_server_input_relay(const PsxLobbyJoinInfo *j);
static void queue_send(const char *msg);
static void flush_pending(void);
static void lobby_rtt_close(void);
static void lobby_ice_rtt_close(void);
static int  lobby_ice_rtt_peer_id(char *out, size_t cap);
static void lobby_rtt_store_for_peer(const char *peer_id, int ms);

static RNetRttProbe *g_rtt_probe;

/* Waiting-room ICE/TURN RTT (CGNAT-safe). Prefer over direct UDP probe. */
static RNetIceRttProbe *g_ice_rtt;
static char g_ice_rtt_peer_id[PSX_LOBBY_ID_LEN];
static int g_ice_rtt_force_relay;
static uint64_t g_ice_rtt_last_log_ms;
/* Last path_report sent to the lobby server (direct|relay|fail). */
static char g_ice_path_reported[16];
static uint64_t g_ice_path_report_ms;

/* One-shot list latency: burst-ping LAN + public candidates after lobby_list. */
#define PSX_LOBBY_MAX_PROBE_PEND (PSX_LOBBY_MAX_LIST * (PSX_LOBBY_MAX_LAN_EPS + 1))
static RNetRttProbe *g_list_rtt_probe;
static int g_list_rtt_active;
static int g_list_rtt_on_next_list; /* set by request_list / Refresh */
static uint64_t g_list_rtt_deadline_ms;
static unsigned long long g_list_rtt_sent_ts[PSX_LOBBY_MAX_PROBE_PEND];
static int g_list_rtt_lobby_idx[PSX_LOBBY_MAX_PROBE_PEND];
static int g_list_rtt_pend_active[PSX_LOBBY_MAX_PROBE_PEND];
static int g_list_rtt_pend_count;

/* Local UDP broadcast discovery — LAN endpoint never goes to the hub. */
static RNetLanBeacon *g_lan_beacon_pub;
static RNetLanBeacon *g_lan_beacon_listen;

/* Host STUN advertise → set_host_endpoint for list / pre-join RTT. */
enum {
    HOST_ADV_IDLE = 0,
    HOST_ADV_WAIT_TURN,
    HOST_ADV_DONE
};
static int g_host_adv_state;
static uint64_t g_host_adv_deadline_ms;

static void lobby_host_advertise_reset(void)
{
    g_host_adv_state = HOST_ADV_IDLE;
    g_host_adv_deadline_ms = 0;
}

static int host_endpoint_is_loopback(const char *ep)
{
    if (!ep || !ep[0])
        return 0;
    if (strncmp(ep, "127.", 4) == 0)
        return 1;
    if (strncmp(ep, "::1:", 4) == 0 || strcmp(ep, "::1") == 0)
        return 1;
    if (strncmp(ep, "localhost:", 10) == 0 || strcmp(ep, "localhost") == 0)
        return 1;
    return 0;
}

static int endpoint_host_port(const char *ep, char *host, size_t host_cap, int *port_out)
{
    const char *colon;
    size_t n;
    if (!ep || !ep[0] || !host || host_cap == 0 || !port_out)
        return 0;
    colon = strrchr(ep, ':');
    if (!colon || colon == ep || !colon[1])
        return 0;
    n = (size_t)(colon - ep);
    if (n + 1 > host_cap)
        n = host_cap - 1;
    memcpy(host, ep, n);
    host[n] = '\0';
    *port_out = (int)strtoul(colon + 1, NULL, 10);
    return *port_out > 0 && *port_out <= 65535;
}

static void lobby_store_connected_peer_ip(int fd)
{
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);
    g_lc.connected_peer_ip[0] = '\0';
    if (fd < 0)
        return;
    memset(&ss, 0, sizeof(ss));
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0)
        return;
    if (ss.ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)&ss;
        if (!inet_ntop(AF_INET, &in->sin_addr, g_lc.connected_peer_ip,
                       sizeof(g_lc.connected_peer_ip)))
            g_lc.connected_peer_ip[0] = '\0';
    }
#if defined(AF_INET6)
    else if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&ss;
        if (!inet_ntop(AF_INET6, &in6->sin6_addr, g_lc.connected_peer_ip,
                       sizeof(g_lc.connected_peer_ip)))
            g_lc.connected_peer_ip[0] = '\0';
    }
#endif
}

static int parse_ipv4_dotted(const char *host, unsigned *o)
{
    unsigned a, b, c, d;
    char extra;
    if (!host || !o)
        return 0;
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    o[0] = a;
    o[1] = b;
    o[2] = c;
    o[3] = d;
    return 1;
}

static int ipv4_is_rfc1918(const unsigned o[4])
{
    if (!o)
        return 0;
    if (o[0] == 10)
        return 1;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31)
        return 1;
    if (o[0] == 192 && o[1] == 168)
        return 1;
    return 0;
}

static int host_is_loopback_name(const char *host)
{
    if (!host || !host[0])
        return 0;
    if (strncmp(host, "127.", 4) == 0)
        return 1;
    if (strcmp(host, "::1") == 0)
        return 1;
    if (strcmp(host, "localhost") == 0)
        return 1;
    return 0;
}

static int host_is_rfc1918_name(const char *host)
{
    unsigned o[4];
    if (!host || !host[0])
        return 0;
    if (!parse_ipv4_dotted(host, o))
        return 0;
    return ipv4_is_rfc1918(o);
}

/* Lobby WebSocket and UDP input relay are the same recomp-net-server process.
 * Rewrite only when it improves reachability:
 *   - keep an already-RFC1918 advertise (server LAN pick / split-horizon)
 *   - rewrite to WS peer IP only when that peer is private/loopback and
 *     advertise is public, loopback, or a hostname (never clobber LAN with WAN)
 *   - else fall back to WS URL host string for loopback advertise only */
static int rewrite_relay_endpoint_to_lobby_host(char *ep, size_t cap)
{
    char rh[128];
    const char *use_host = NULL;
    int rport = 0;
    int n;
    int adv_lan, adv_loop, peer_local;
    if (!ep || !cap)
        return 0;
    if (!endpoint_host_port(ep, rh, sizeof(rh), &rport))
        return 0;
    adv_lan = host_is_rfc1918_name(rh);
    adv_loop = host_is_loopback_name(rh);
    if (adv_lan)
        return 0; /* server already gave a same-LAN dial target */
    peer_local = g_lc.connected_peer_ip[0] &&
                 (host_is_rfc1918_name(g_lc.connected_peer_ip) ||
                  host_is_loopback_name(g_lc.connected_peer_ip));
    if (peer_local)
        use_host = g_lc.connected_peer_ip;
    else if (adv_loop && g_lc.host[0] && !host_is_loopback_name(g_lc.host))
        use_host = g_lc.host;
    if (!use_host || !use_host[0])
        return 0;
    if (strcmp(rh, use_host) == 0)
        return 0;
    n = snprintf(ep, cap, "%s:%d", use_host, rport);
    if (n <= 0 || (size_t)n >= cap)
        return -1;
    return 1;
}

static int ipv4_is_link_local(const unsigned o[4])
{
    return o && o[0] == 169 && o[1] == 254;
}

/* Public WAN IPv4 suitable for lobby-list host_endpoint (not LAN/loopback). */
static int endpoint_is_public_ipv4(const char *ep)
{
    char host[128];
    unsigned o[4];
    int port = 0;
    if (!endpoint_host_port(ep, host, sizeof(host), &port))
        return 0;
    if (!parse_ipv4_dotted(host, o))
        return 0;
    if (o[0] == 0 || o[0] == 127 || o[0] >= 224)
        return 0;
    if (ipv4_is_rfc1918(o) || ipv4_is_link_local(o))
        return 0;
    return 1;
}

static void lobby_rtt_ensure(void);
static void lobby_list_rtt_start(int force_all);

/* /24 heuristic — good enough for typical home/small office LANs. */
static int ipv4_same_lan24(const unsigned a[4], const unsigned b[4])
{
    return a && b && a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static int my_bind_port(void)
{
    char host[128];
    int port = 7777;
    if (g_lc.my_bind[0] && endpoint_host_port(g_lc.my_bind, host, sizeof(host), &port))
        return port;
    return 7777;
}

/* Single LAN advertise candidate: the Host Lobby "Advertised IP" / my_bind NIC.
 * Do not enumerate every local interface — only the menu selection. */
static int collect_host_lan_endpoints(char out[][PSX_LOBBY_ENDPOINT_LEN], int max_out)
{
    char bind_host[128];
    unsigned bind_o[4];
    int port = my_bind_port();

    if (!out || max_out <= 0)
        return 0;
    bind_host[0] = '\0';
    if (!g_lc.my_bind[0] ||
        !endpoint_host_port(g_lc.my_bind, bind_host, sizeof(bind_host), &port) ||
        !parse_ipv4_dotted(bind_host, bind_o) || !ipv4_is_rfc1918(bind_o) ||
        strcmp(bind_host, "0.0.0.0") == 0)
        return 0;
    snprintf(out[0], PSX_LOBBY_ENDPOINT_LEN, "%s:%d", bind_host, port);
    return 1;
}

/* Discover a public UDP mapping for the game port. Coturn on the same LAN can
 * return RFC1918 — reject those and fall back to any:/port + public STUN. */
static int lobby_host_stun_public(char *out, size_t out_len)
{
    RNetExternalIpv4Config stun;
    char any_bind[64];
    char endpoint[RNET_ENDPOINT_TEXT_MAX];
    int port = my_bind_port();
    int attempt;
    int last_rc = RNET_EXTERNAL_IPV4_ERR_ARGUMENT;

    if (!out || out_len == 0)
        return -1;
    out[0] = '\0';
    snprintf(any_bind, sizeof(any_bind), "0.0.0.0:%d", port);

    for (attempt = 0; attempt < 4; ++attempt) {
        const char *bind_hp;
        endpoint[0] = '\0';
        rnet_external_ipv4_config_init(&stun);
        if (attempt < 2 && g_lc.turn.valid && g_lc.turn.stun_host[0]) {
            stun.stun_host = g_lc.turn.stun_host;
            stun.stun_port = (unsigned short)g_lc.turn.stun_port;
        }
        /* attempt 0: coturn + my_bind, 1: coturn + any, 2: default + my_bind,
         * 3: default + any */
        bind_hp = (attempt & 1) ? any_bind
                                : (g_lc.my_bind[0] ? g_lc.my_bind : any_bind);
        last_rc = rnet_external_udp_endpoint_discover(&stun, bind_hp, endpoint,
                                                      sizeof(endpoint));
        if (last_rc != RNET_EXTERNAL_IPV4_OK || !endpoint[0])
            continue;
        if (!endpoint_is_public_ipv4(endpoint)) {
            fprintf(stderr,
                    "psx_lobby: STUN mapped private %s (bind=%s stun=%s) — "
                    "retrying\n",
                    endpoint, bind_hp,
                    stun.stun_host ? stun.stun_host : "(default)");
            continue;
        }
        strncpy(out, endpoint, out_len - 1);
        out[out_len - 1] = '\0';
        return 0;
    }
    return last_rc != 0 ? last_rc : -1;
}

static void lobby_lan_beacon_close_all(void)
{
    rnet_lan_beacon_close(&g_lan_beacon_pub);
    rnet_lan_beacon_close(&g_lan_beacon_listen);
}

static void lobby_lan_beacon_publish_update(void)
{
    char lan[PSX_LOBBY_MAX_LAN_EPS][PSX_LOBBY_ENDPOINT_LEN];
    int lan_n;
    if (!g_lc.is_host || !g_lc.in_lobby || g_lc.launch_pending)
        return;
    if (!g_lc.join.lobby_id[0])
        return;
    lan_n = collect_host_lan_endpoints(lan, PSX_LOBBY_MAX_LAN_EPS);
    if (lan_n <= 0) {
        rnet_lan_beacon_close(&g_lan_beacon_pub);
        return;
    }
    if (!g_lan_beacon_pub &&
        rnet_lan_beacon_publish_open(&g_lan_beacon_pub, 0) != 0)
        return;
    if (rnet_lan_beacon_publish_set(g_lan_beacon_pub, g_lc.join.lobby_id, lan[0],
                                    g_lc.filter_game_name) != 0) {
        rnet_lan_beacon_close(&g_lan_beacon_pub);
        return;
    }
    fprintf(stderr, "psx_lobby: LAN beacon publish %s → %s\n",
            g_lc.join.lobby_id, lan[0]);
}

static void lobby_lan_beacon_tick(void)
{
    if (g_lc.is_host && g_lc.in_lobby && !g_lc.launch_pending) {
        if (!g_lan_beacon_pub)
            lobby_lan_beacon_publish_update();
        if (g_lan_beacon_pub)
            (void)rnet_lan_beacon_publish_tick(g_lan_beacon_pub);
    } else if (g_lan_beacon_pub) {
        rnet_lan_beacon_close(&g_lan_beacon_pub);
    }

    /* Guests (and hosts browsing after leave) listen for same-LAN announces. */
    if (!g_lc.is_host || !g_lc.in_lobby) {
        int updated = 0;
        if (!g_lan_beacon_listen)
            (void)rnet_lan_beacon_listen_open(&g_lan_beacon_listen, 0);
        if (g_lan_beacon_listen)
            updated = rnet_lan_beacon_listen_pump(g_lan_beacon_listen);
        /* New beacon → re-probe list rows still missing latency. */
        if (updated > 0 && g_lc.list_count > 0 && !g_list_rtt_active) {
            int i;
            int need = 0;
            for (i = 0; i < g_lc.list_count; ++i) {
                if (g_lc.list[i].latency_ms < 0) {
                    need = 1;
                    break;
                }
            }
            if (need)
                lobby_list_rtt_start(0);
        }
    }
}

static void lobby_host_advertise_tick(void)
{
    char endpoint[RNET_ENDPOINT_TEXT_MAX];
    char msg[384];
    int rc;

    if (g_host_adv_state != HOST_ADV_WAIT_TURN)
        return;
    if (!g_lc.is_host || !g_lc.in_lobby || g_lc.launch_pending) {
        lobby_host_advertise_reset();
        return;
    }
    if (using_server_input_relay(&g_lc.join)) {
        g_host_adv_state = HOST_ADV_DONE;
        return;
    }
    /* Prefer Coturn STUN from turn_credentials; don't block forever. */
    if (!g_lc.turn.valid && lobby_mono_ms() < g_host_adv_deadline_ms)
        return;
    if (!g_lc.my_bind[0]) {
        g_host_adv_state = HOST_ADV_DONE;
        return;
    }

    /* Free the game UDP port for an exclusive STUN bind (skip on loopback hub). */
    if (!host_endpoint_is_loopback(g_lc.join.host_endpoint)) {
        lobby_rtt_close();
        endpoint[0] = '\0';
        rc = lobby_host_stun_public(endpoint, sizeof(endpoint));
        if (rc == 0 && endpoint[0]) {
            strncpy(g_lc.join.host_endpoint, endpoint,
                    sizeof(g_lc.join.host_endpoint) - 1);
            g_lc.join.host_endpoint[sizeof(g_lc.join.host_endpoint) - 1] = '\0';
        } else {
            fprintf(stderr,
                    "psx_lobby: STUN advertise failed (%d) — keeping %s%s\n", rc,
                    g_lc.join.host_endpoint[0] ? g_lc.join.host_endpoint
                                               : "(none)",
                    endpoint_is_public_ipv4(g_lc.join.host_endpoint)
                        ? ""
                        : " (use LAN beacon for local list RTT)");
        }
        /* Answer list/waiting-room PINGs again as soon as STUN frees the port. */
        lobby_rtt_ensure();
    }

    /* Private IPs stay on the LAN beacon only — never on the hub list. */
    lobby_lan_beacon_publish_update();

    g_host_adv_state = HOST_ADV_DONE;
    if (!g_lc.join.host_endpoint[0])
        return;
    {
        char ep_esc[JSON_ESC_CAP(PSX_LOBBY_ENDPOINT_LEN)];
        json_escape(g_lc.join.host_endpoint, ep_esc, sizeof(ep_esc));
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"set_host_endpoint\",\"host_endpoint\":\"%s\"}",
                 ep_esc);
    }
    queue_send(msg);
    flush_pending();
    fprintf(stderr, "psx_lobby: advertised host_endpoint=%s (LAN via local beacon)\n",
            g_lc.join.host_endpoint);
}

static void lobby_rtt_close(void)
{
    rnet_rtt_probe_close(&g_rtt_probe);
}

static void lobby_list_rtt_close(void)
{
    rnet_rtt_probe_close(&g_list_rtt_probe);
    g_list_rtt_active = 0;
    g_list_rtt_pend_count = 0;
    memset(g_list_rtt_pend_active, 0, sizeof(g_list_rtt_pend_active));
    memset(g_list_rtt_sent_ts, 0, sizeof(g_list_rtt_sent_ts));
    memset(g_list_rtt_lobby_idx, 0, sizeof(g_list_rtt_lobby_idx));
}

static int collect_local_rfc1918(unsigned out[][4], int max_out)
{
    RNetIpv4Address addrs[16];
    int n;
    int i;
    int count = 0;

    if (!out || max_out <= 0)
        return 0;
    n = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
    if (n < 0)
        n = 0;
    if (n > (int)(sizeof(addrs) / sizeof(addrs[0])))
        n = (int)(sizeof(addrs) / sizeof(addrs[0]));
    for (i = 0; i < n && count < max_out; ++i) {
        unsigned o[4];
        if (!parse_ipv4_dotted(addrs[i].address, o) || !ipv4_is_rfc1918(o))
            continue;
        memcpy(out[count], o, sizeof(o));
        ++count;
    }
    return count;
}

static int cand_already(char cands[][PSX_LOBBY_ENDPOINT_LEN], int n, const char *ep)
{
    int i;
    for (i = 0; i < n; ++i) {
        if (strcmp(cands[i], ep) == 0)
            return 1;
    }
    return 0;
}

/* Prefer local beacon LAN, then legacy server lan_endpoints, then public. */
static int lobby_row_build_candidates(const PsxLobbyRow *row,
                                      char cands[][PSX_LOBBY_ENDPOINT_LEN],
                                      int max_cands)
{
    unsigned local[8][4];
    int local_n;
    int n = 0;
    int i;
    int pass;
    char beacon_ep[PSX_LOBBY_ENDPOINT_LEN];

    if (!row || !cands || max_cands <= 0)
        return 0;

    beacon_ep[0] = '\0';
    if (g_lan_beacon_listen && row->lobby_id[0] &&
        rnet_lan_beacon_lookup(g_lan_beacon_listen, row->lobby_id, beacon_ep,
                               sizeof(beacon_ep)) &&
        beacon_ep[0] && !endpoint_port_is_zero(beacon_ep)) {
        strncpy(cands[n], beacon_ep, PSX_LOBBY_ENDPOINT_LEN - 1);
        cands[n][PSX_LOBBY_ENDPOINT_LEN - 1] = '\0';
        ++n;
    }

    local_n = collect_local_rfc1918(local, 8);

    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < row->lan_count && n < max_cands; ++i) {
            char host[64];
            int port = 0;
            unsigned o[4];
            int same = 0;
            int j;
            if (!row->lan_endpoints[i][0] ||
                endpoint_port_is_zero(row->lan_endpoints[i]))
                continue;
            if (!endpoint_host_port(row->lan_endpoints[i], host, sizeof(host), &port) ||
                !parse_ipv4_dotted(host, o) || !ipv4_is_rfc1918(o))
                continue;
            for (j = 0; j < local_n; ++j) {
                if (ipv4_same_lan24(local[j], o)) {
                    same = 1;
                    break;
                }
            }
            if (pass == 0 && !same)
                continue;
            if (pass == 1 && same)
                continue; /* already added */
            if (cand_already(cands, n, row->lan_endpoints[i]))
                continue;
            strncpy(cands[n], row->lan_endpoints[i], PSX_LOBBY_ENDPOINT_LEN - 1);
            cands[n][PSX_LOBBY_ENDPOINT_LEN - 1] = '\0';
            ++n;
        }
    }
    if (n < max_cands && row->host_endpoint[0] &&
        !endpoint_port_is_zero(row->host_endpoint) &&
        !cand_already(cands, n, row->host_endpoint)) {
        strncpy(cands[n], row->host_endpoint, PSX_LOBBY_ENDPOINT_LEN - 1);
        cands[n][PSX_LOBBY_ENDPOINT_LEN - 1] = '\0';
        ++n;
    }
    return n;
}

/* force_all: Refresh — re-probe every row. Otherwise only rows with unknown RTT. */
static void lobby_list_rtt_start(int force_all)
{
    int i;

    lobby_list_rtt_close();
    if (g_lc.list_count <= 0)
        return;
    /* Drain local beacons before building candidates (may beat WS list). */
    if (!g_lan_beacon_listen)
        (void)rnet_lan_beacon_listen_open(&g_lan_beacon_listen, 0);
    if (g_lan_beacon_listen)
        (void)rnet_lan_beacon_listen_pump(g_lan_beacon_listen);
    if (rnet_rtt_probe_open(&g_list_rtt_probe, NULL) != 0)
        return;

    for (i = 0; i < g_lc.list_count; ++i) {
        char cands[PSX_LOBBY_MAX_LAN_EPS + 1][PSX_LOBBY_ENDPOINT_LEN];
        int cn;
        int c;
        if (force_all)
            g_lc.list[i].latency_ms = -1;
        if (g_lc.list[i].latency_ms >= 0)
            continue;
        cn = lobby_row_build_candidates(&g_lc.list[i], cands,
                                        PSX_LOBBY_MAX_LAN_EPS + 1);
        for (c = 0; c < cn && g_list_rtt_pend_count < PSX_LOBBY_MAX_PROBE_PEND; ++c) {
            unsigned long long sent = 0;
            int slot = g_list_rtt_pend_count;
            if (rnet_rtt_probe_set_peer(g_list_rtt_probe, cands[c]) != 0)
                continue;
            if (rnet_rtt_probe_ping_ts(g_list_rtt_probe, &sent) != 0)
                continue;
            g_list_rtt_sent_ts[slot] = sent;
            g_list_rtt_lobby_idx[slot] = i;
            g_list_rtt_pend_active[slot] = 1;
            ++g_list_rtt_pend_count;
        }
    }

    if (g_list_rtt_pend_count <= 0) {
        lobby_list_rtt_close();
        return;
    }
    g_list_rtt_active = 1;
    /* STUN advertise briefly drops the host answer sock; give guests time. */
    g_list_rtt_deadline_ms = lobby_mono_ms() + 1500ull;
}

static void lobby_list_rtt_tick(void)
{
    int remaining;

    if (!g_list_rtt_active || !g_list_rtt_probe)
        return;

    for (;;) {
        int ms = 0;
        unsigned long long echo = 0;
        int p;
        int got = rnet_rtt_probe_pump_ex(g_list_rtt_probe, &ms, &echo);
        if (got != 1)
            break;
        for (p = 0; p < g_list_rtt_pend_count; ++p) {
            int li;
            int q;
            if (!g_list_rtt_pend_active[p] || g_list_rtt_sent_ts[p] != echo)
                continue;
            li = g_list_rtt_lobby_idx[p];
            if (li >= 0 && li < g_lc.list_count && g_lc.list[li].latency_ms < 0)
                g_lc.list[li].latency_ms = ms;
            /* Drop remaining candidates for this lobby. */
            for (q = 0; q < g_list_rtt_pend_count; ++q) {
                if (g_list_rtt_lobby_idx[q] == li)
                    g_list_rtt_pend_active[q] = 0;
            }
            break;
        }
    }

    remaining = 0;
    {
        int p;
        for (p = 0; p < g_list_rtt_pend_count; ++p) {
            if (g_list_rtt_pend_active[p])
                ++remaining;
        }
    }
    if (remaining <= 0 || lobby_mono_ms() >= g_list_rtt_deadline_ms)
        lobby_list_rtt_close();
}

static void member_rtt_clear(void)
{
    int i;
    for (i = 0; i < PSX_LOBBY_MAX_MEMBERS; ++i)
        g_lc.member_rtt_ms[i] = -1;
    g_lc.rtt_next_ping_ms = 0;
}

/* member_rtt_ms has a cell per seat in BOTH tables, but a gallery seat is
 * numbered from the server's spectator base (64+), not from the array. Map
 * a seat to its cell; -1 for a seat neither table has. Without this every
 * spectator's report was dropped as out of range and the gallery showed no
 * latency at all. */
static int rtt_index_for_slot(int slot)
{
    int base;
    if (slot < 0) return -1;
    if (slot < PSX_LOBBY_MAX_PLAYERS) return slot;
    base = psx_lobby_spectator_slot_base();
    if (base > 0 && slot >= base && slot < base + PSX_LOBBY_MAX_SPECTATORS)
        return PSX_LOBBY_MAX_PLAYERS + (slot - base);
    return -1;
}

static int member_slot_for_player(const char *player_id)
{
    int i;
    if (!player_id || !player_id[0])
        return -1;
    for (i = 0; i < g_lc.member_count; ++i) {
        if (strcmp(g_lc.members[i].player_id, player_id) == 0)
            return g_lc.members[i].slot;
    }
    return -1;
}

static int local_member_slot(void)
{
    return member_slot_for_player(g_lc.player_id);
}

/* Default max_slots for create (clamped 2..8). */
static int g_lobby_max_slots = 2;
/* Host preference for the next create, beside the seat ceiling it travels
 * with. File scope so a setting made before a reconnect is still there when
 * the create goes out. */
static int g_allow_spectators_pref;

void psx_lobby_set_max_slots(int max_slots)
{
    if (max_slots < 2) max_slots = 2;
    if (max_slots > 8) max_slots = 8;
    g_lobby_max_slots = max_slots;
}

static const char *effective_game_version(const char *override_ver)
{
    if (override_ver && override_ver[0]) {
        return override_ver;
    }
    if (g_lc.filter_game_version[0]) {
        return g_lc.filter_game_version;
    }
    return PSX_GAME_VERSION;
}

/* Release builds pin the lobby browser to our exact game_version.
 * Non-release ("dev") shows all versions of our title so testers can see
 * unofficial / mismatched hosts; join still requires an exact version match. */
static int list_filter_version_strict(void)
{
    const char *gv = effective_game_version(NULL);
    return gv && gv[0] && strcmp(gv, "dev") != 0;
}

static void queue_send(const char *json);
static void clear_turn_credentials(void);
static int queue_turn_credentials_request(void);

static void clear_turn_credentials(void)
{
    memset(&g_lc.turn, 0, sizeof(g_lc.turn));
    g_lc.turn_received_at = 0;
    g_lc.turn_request_pending = 0;
}

static int queue_turn_credentials_request(void)
{
    if (!psx_lobby_connected())
        return -1;
    queue_send("{\"op\":\"get_turn_credentials\"}");
    g_lc.turn_request_pending = 1;
    return 0;
}

static void queue_list_request(void)
{
    char msg[384];
    char gn_esc[JSON_ESC_CAP(PSX_LOBBY_NAME_LEN)];
    char gv_esc[JSON_ESC_CAP(PSX_LOBBY_VERSION_LEN)];
    const char *gn = g_lc.filter_game_name;
    const char *gv = effective_game_version(NULL);
    json_escape(gn, gn_esc, sizeof(gn_esc));
    json_escape(gv ? gv : "dev", gv_esc, sizeof(gv_esc));
    if (list_filter_version_strict() && (gn[0] || (gv && gv[0]))) {
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"list\",\"game_name\":\"%s\",\"game_version\":\"%s\"}",
                 gn_esc, gv_esc);
        queue_send(msg);
    } else if (gn[0]) {
        snprintf(msg, sizeof(msg), "{\"op\":\"list\",\"game_name\":\"%s\"}", gn_esc);
        queue_send(msg);
    } else {
        queue_send("{\"op\":\"list\"}");
    }
}

static void match_caps_clear(PsxLobbyMatchCaps *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->aspect_num = 4;
    c->aspect_den = 3;
    c->input_delay = 6;
    c->input_prediction = 10;
    c->guest_memcard = 1;
}

static int json_extract_object(const char *json, const char *key, char *out, size_t out_cap);
static void parse_match_caps_object(const char *obj, PsxLobbyMatchCaps *out);
static void ingest_match_caps_from_json(const char *json);
static int append_match_caps_json(char *dst, size_t dst_cap, const PsxLobbyMatchCaps *caps);

const char *psx_lobby_default_url(void)
{
    const char *e = getenv("PSX_NET_LOBBY_URL");
    if (e && e[0])
        return e;
#if defined(PSX_NET_LOBBY_DEFAULT_URL)
    return PSX_NET_LOBBY_DEFAULT_URL;
#else
    return "ws://netplay.retcomm.net:8765";
#endif
}

static int parse_ws_url(const char *url, char *host, size_t hcap, int *port, char *path, size_t pcap)
{
    const char *p = url;
    const char *slash;
    char hostport[192];
    char *colon;
    if (!url) {
        return -1;
    }
    if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        return -1; /* TLS not in this phase */
    }
    slash = strchr(p, '/');
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= sizeof(hostport)) {
            n = sizeof(hostport) - 1;
        }
        memcpy(hostport, p, n);
        hostport[n] = '\0';
        strncpy(path, slash, pcap - 1);
        path[pcap - 1] = '\0';
    } else {
        strncpy(hostport, p, sizeof(hostport) - 1);
        hostport[sizeof(hostport) - 1] = '\0';
        strncpy(path, "/", pcap - 1);
    }
    colon = strrchr(hostport, ':');
    if (colon && strchr(hostport, ']') == NULL) {
        *colon = '\0';
        *port = atoi(colon + 1);
        strncpy(host, hostport, hcap - 1);
    } else {
        strncpy(host, hostport, hcap - 1);
        *port = 8765;
    }
    host[hcap - 1] = '\0';
    return 0;
}

static const char *json_get_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[80];
    const char *p;
    size_t o = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        if (out && cap) {
            out[0] = '\0';
        }
        return NULL;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return NULL;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '"') {
        return NULL;
    }
    ++p;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case '"': out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/'; break;
            default: out[o++] = *p; break;
            }
            ++p;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
}

/* Parse JSON string array values for key into out[0..max_out). Returns count. */
static int json_parse_str_array(const char *json, const char *key,
                                char out[][PSX_LOBBY_ENDPOINT_LEN], int max_out)
{
    char pat[80];
    const char *p;
    int n = 0;

    if (!json || !key || !out || max_out <= 0)
        return 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return 0;
    p = strchr(p + strlen(pat), '[');
    if (!p)
        return 0;
    ++p;
    while (*p && n < max_out) {
        size_t o = 0;
        while (*p && (isspace((unsigned char)*p) || *p == ','))
            ++p;
        if (*p == ']')
            break;
        if (*p != '"')
            break;
        ++p;
        while (*p && *p != '"' && o + 1 < PSX_LOBBY_ENDPOINT_LEN)
            out[n][o++] = *p++;
        out[n][o] = '\0';
        if (*p == '"')
            ++p;
        if (out[n][0])
            ++n;
    }
    return n;
}

static void lobby_row_lan_fingerprint(const PsxLobbyRow *row, char *out, size_t cap)
{
    size_t o = 0;
    int i;
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!row)
        return;
    for (i = 0; i < row->lan_count; ++i) {
        int wrote;
        if (!row->lan_endpoints[i][0])
            continue;
        wrote = snprintf(out + o, cap > o ? cap - o : 0, "%s%s", o ? "|" : "",
                         row->lan_endpoints[i]);
        if (wrote < 0 || (size_t)wrote >= (cap > o ? cap - o : 0))
            break;
        o += (size_t)wrote;
    }
}

static size_t json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!in || !out || cap == 0) return 0;
    while (*in && o + 2 < cap) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\r') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (c == '\t') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 't';
        } else if (c < 0x20) {
            continue;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

static void enqueue_signal(int type, int flag, const char *text)
{
    int i;
    if (!g_lc.ice_signal_accept)
        return;
    if (g_lc.sig_count >= (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]))) {
        g_lc.sig_head = (g_lc.sig_head + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
        g_lc.sig_count--;
    }
    i = g_lc.sig_tail;
    g_lc.sig_q[i].type = type;
    g_lc.sig_q[i].flag = flag;
    g_lc.sig_q[i].text[0] = '\0';
    if (text)
        strncpy(g_lc.sig_q[i].text, text, sizeof(g_lc.sig_q[i].text) - 1);
    g_lc.sig_tail = (g_lc.sig_tail + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
    g_lc.sig_count++;
}

void psx_lobby_clear_signals(void)
{
    g_lc.sig_head = 0;
    g_lc.sig_tail = 0;
    g_lc.sig_count = 0;
}

void psx_lobby_set_ice_signal_accept(int accept)
{
    g_lc.ice_signal_accept = accept ? 1 : 0;
}

static int json_get_int(const char *json, const char *key, int def)
{
    char pat[80];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        return def;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return def;
    }
    return (int)strtol(p + 1, NULL, 10);
}

static int json_get_bool(const char *json, const char *key, int def);

/* The `players` array of a lobby_list: everyone on the hub. Flat objects,
 * so each one is cut out by brace depth and read with the key getters. An
 * older server has no such array and the count simply goes to zero. */
static void lobby_list_parse_players(const char *json)
{
    const char *p = strstr(json, "\"players\"");
    int n = 0;
    g_lc.online_count = 0;
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    ++p;
    while (*p && n < PSX_LOBBY_MAX_ONLINE) {
        const char *obj, *end;
        int depth = 0;
        char chunk[512];
        size_t len;
        while (*p && *p != '{' && *p != ']') ++p;
        if (*p != '{') break;
        obj = end = p;
        do {
            if (*end == '{') ++depth;
            else if (*end == '}') --depth;
            ++end;
        } while (*end && depth > 0);
        len = (size_t)(end - obj);
        if (len >= sizeof(chunk)) len = sizeof(chunk) - 1;
        memcpy(chunk, obj, len);
        chunk[len] = '\0';
        memset(&g_lc.online[n], 0, sizeof(g_lc.online[n]));
        json_get_str(chunk, "display_name", g_lc.online[n].display_name,
                     sizeof(g_lc.online[n].display_name));
        json_get_str(chunk, "country", g_lc.online[n].country,
                     sizeof(g_lc.online[n].country));
        json_get_str(chunk, "lobby_id", g_lc.online[n].lobby_id,
                     sizeof(g_lc.online[n].lobby_id));
        json_get_str(chunk, "lobby_name", g_lc.online[n].lobby_name,
                     sizeof(g_lc.online[n].lobby_name));
        g_lc.online[n].hosting = json_get_bool(chunk, "hosting", 0);
        json_get_str(chunk, "tag", g_lc.online[n].tag, sizeof(g_lc.online[n].tag));
        json_get_str(chunk, "account", g_lc.online[n].account,
                     sizeof(g_lc.online[n].account));
        json_get_str(chunk, "game_name", g_lc.online[n].game_name,
                     sizeof(g_lc.online[n].game_name));
        /* Players of another title are not "online" for this one. A row
         * with no title yet (a client that has not listed) is kept. */
        if (g_lc.filter_game_name[0] && g_lc.online[n].game_name[0] &&
            strcmp(g_lc.online[n].game_name, g_lc.filter_game_name) != 0)
            { p = end; continue; }
        if (g_lc.online[n].display_name[0]) ++n;
        p = end;
    }
    g_lc.online_count = n;
}

int psx_lobby_online_count(void)
{
    return g_lc.online_count;
}

int psx_lobby_online_get(int index, PsxLobbyOnlinePlayer *out)
{
    if (!out || index < 0 || index >= g_lc.online_count) return 0;
    *out = g_lc.online[index];
    return 1;
}

static int json_get_bool(const char *json, const char *key, int def)
{
    char pat[80];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        return def;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return def;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (strncmp(p, "true", 4) == 0) {
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        return 0;
    }
    return def;
}

static int json_extract_object(const char *json, const char *key, char *out, size_t out_cap)
{
    char pat[80];
    const char *p;
    int depth;
    size_t n;
    if (!json || !key || !out || out_cap < 3) return 0;
    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '{') return 0;
    depth = 0;
    n = 0;
    do {
        if (*p == '{') ++depth;
        else if (*p == '}') --depth;
        if (n + 1 >= out_cap) return 0;
        out[n++] = *p++;
    } while (*p && depth > 0);
    out[n] = '\0';
    return depth == 0 && n > 1;
}

static void parse_match_caps_object(const char *obj, PsxLobbyMatchCaps *out)
{
    if (!obj || !out || obj[0] != '{') return;
    match_caps_clear(out);
    out->aspect_num = json_get_int(obj, "aspect_num", 4);
    out->aspect_den = json_get_int(obj, "aspect_den", 3);
    out->turbo_loads = json_get_bool(obj, "turbo_loads", 0);
    out->bios_hle = json_get_bool(obj, "bios_hle", 1);
    out->fast_boot = json_get_bool(obj, "fast_boot", 0);
    out->auto_skip_fmv = json_get_bool(obj, "auto_skip_fmv", 0);
    out->input_delay = json_get_int(obj, "input_delay", 6);
    if (out->input_delay < 0) out->input_delay = 0;
    if (out->input_delay > 20) out->input_delay = 20;
    out->input_prediction = json_get_int(obj, "input_prediction", 10);
    if (out->input_prediction < 2) out->input_prediction = 2;
    if (out->input_prediction > 16) out->input_prediction = 16;
    out->force_input_relay = json_get_bool(obj, "force_input_relay", 0);
    out->force_turn = json_get_bool(obj, "force_turn", 0);
    /* Absent field → delay-sync (older hosts). New hosts always publish explicit. */
    out->rollback = json_get_bool(obj, "rollback", 0);
    out->multitap_analog = json_get_bool(obj, "multitap_analog", 0);
    /* Absent (older host) = allowed; the seat-1 offer still gates it. */
    out->guest_memcard = json_get_bool(obj, "guest_memcard", 1);
    out->guest_memcard_active = json_get_bool(obj, "guest_memcard_active", 0);
    json_get_str(obj, "language", out->language, sizeof(out->language));
    json_get_str(obj, "session_bios", out->session_bios, sizeof(out->session_bios));
    /* Normalize settled BIOS id. */
    if (out->session_bios[0] &&
        strcmp(out->session_bios, "openbios") != 0 &&
        strcmp(out->session_bios, "scph1001") != 0)
        out->session_bios[0] = '\0';
    out->valid = 1;
}

static void ingest_match_caps_from_json(const char *json)
{
    char obj[1024];
    if (json_extract_object(json, "match_caps", obj, sizeof(obj)))
        parse_match_caps_object(obj, &g_lc.match_caps);
}

static int append_match_caps_json(char *dst, size_t dst_cap, const PsxLobbyMatchCaps *caps)
{
    char lang[PSX_LOBBY_LANG_LEN];
    size_t i, o = 0;
    if (!dst || dst_cap < 8 || !caps || !caps->valid) return 0;
    /* Sanitize language for JSON string (alnum / _ / - only). */
    for (i = 0; caps->language[i] && o + 1 < sizeof(lang); ++i) {
        char ch = caps->language[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
            lang[o++] = ch;
    }
    lang[o] = '\0';
    if (!lang[0]) strncpy(lang, "en", sizeof(lang) - 1);
    {
        const char *sb = caps->session_bios;
        if (!sb[0] || (strcmp(sb, "openbios") != 0 && strcmp(sb, "scph1001") != 0))
            sb = "";
        return snprintf(dst, dst_cap,
                        ",\"match_caps\":{\"v\":1,\"aspect_num\":%d,\"aspect_den\":%d,"
                        "\"turbo_loads\":%s,\"bios_hle\":%s,\"fast_boot\":%s,"
                        "\"auto_skip_fmv\":%s,\"input_delay\":%d,\"input_prediction\":%d,"
                        "\"force_input_relay\":%s,\"force_turn\":%s,\"rollback\":%s,"
                        "\"multitap_analog\":%s,\"guest_memcard\":%s,"
                        "\"guest_memcard_active\":%s,"
                        "\"language\":\"%s\",\"session_bios\":\"%s\"}",
                        caps->aspect_num, caps->aspect_den,
                        caps->turbo_loads ? "true" : "false",
                        caps->bios_hle ? "true" : "false",
                        caps->fast_boot ? "true" : "false",
                        caps->auto_skip_fmv ? "true" : "false",
                        caps->input_delay,
                        caps->input_prediction,
                        caps->force_input_relay ? "true" : "false",
                        caps->force_turn ? "true" : "false",
                        caps->rollback ? "true" : "false",
                        caps->multitap_analog ? "true" : "false",
                        caps->guest_memcard ? "true" : "false",
                        caps->guest_memcard_active ? "true" : "false",
                        lang, sb);
    }
}

static void queue_send(const char *json)
{
    if (g_lc.pending_n >= 8) {
        return;
    }
    strncpy(g_lc.pending_tx[g_lc.pending_n], json, sizeof(g_lc.pending_tx[0]) - 1);
    g_lc.pending_tx[g_lc.pending_n][sizeof(g_lc.pending_tx[0]) - 1] = '\0';
    g_lc.pending_n++;
}

static void flush_pending(void)
{
    int i;
    if (!g_lc.handshake_done) {
        return;
    }
    for (i = 0; i < g_lc.pending_n; ++i) {
        rnet_ws_write_text(g_lc.fd, g_lc.pending_tx[i], 1);
    }
    g_lc.pending_n = 0;
}

static int endpoint_port_is_zero(const char *ep)
{
    const char *colon;
    if (!ep || !ep[0]) return 1;
    colon = strrchr(ep, ':');
    if (!colon || !colon[1]) return 1;
    return (int)strtoul(colon + 1, NULL, 10) == 0;
}

/* Prefer a usable host:port among candidates (skip empty / :0). */
static void copy_first_usable_endpoint(char *dst, size_t dst_len, const char *a,
                                       const char *b, const char *c)
{
    const char *cands[3];
    int i;
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    cands[0] = a;
    cands[1] = b;
    cands[2] = c;
    for (i = 0; i < 3; ++i) {
        if (cands[i] && cands[i][0] && !endpoint_port_is_zero(cands[i])) {
            strncpy(dst, cands[i], dst_len - 1);
            dst[dst_len - 1] = '\0';
            return;
        }
    }
}

static int using_server_input_relay(const PsxLobbyJoinInfo *j)
{
    if (g_lc.match_caps.valid && g_lc.match_caps.force_input_relay)
        return 1;
    /* Server rewrote both endpoints to the same relay advertise address. */
    if (j && j->host_endpoint[0] && j->guest_endpoint[0] &&
        !endpoint_port_is_zero(j->host_endpoint) &&
        !endpoint_port_is_zero(j->guest_endpoint) &&
        strcmp(j->host_endpoint, j->guest_endpoint) == 0 &&
        (!g_lc.my_bind[0] || strcmp(j->host_endpoint, g_lc.my_bind) != 0))
        return 1;
    return 0;
}

static void fill_peer_bind_from_join(void)
{
    PsxLobbyJoinInfo *j = &g_lc.join;
    const int force_relay = using_server_input_relay(j);
    const int seats = j->player_count >= 2 ? j->player_count : j->max_slots;
    const int host_hub = (g_lc.is_host && seats >= 3 && !force_relay) ? 1 : 0;
    memset(j->bind_hostport, 0, sizeof(j->bind_hostport));
    memset(j->peer_hostport, 0, sizeof(j->peer_hostport));
    if (force_relay) {
        /* Everyone dials the lobby-server UDP relay — ephemeral local bind
         * (same as LAN guests) so same-PC multi-instance doesn't collide. */
        strncpy(j->bind_hostport, "0.0.0.0:0", sizeof(j->bind_hostport) - 1);
        copy_first_usable_endpoint(j->peer_hostport, sizeof(j->peer_hostport),
                                   j->host_endpoint, j->guest_endpoint, NULL);
    } else if (g_lc.is_host) {
        strncpy(j->bind_hostport, g_lc.my_bind, sizeof(j->bind_hostport) - 1);
        if (!host_hub) {
            /* 2P P2P: dial guest when they advertised a fixed port. Online
             * guests often join with :0 — leave peer empty (accept-first). */
            if (j->guest_endpoint[0] && !endpoint_port_is_zero(j->guest_endpoint))
                strncpy(j->peer_hostport, j->guest_endpoint, sizeof(j->peer_hostport) - 1);
        }
        /* host_hub: peer stays empty → rnet_session_start_lan_hub */
    } else {
        /* Guests dialing 3+ host hub: ephemeral local UDP (join only probes
         * 7778+ and does not hold the socket). 2P P2P keeps the advertised
         * fixed guest_bind so the host can dial. */
        if (seats >= 3) {
            strncpy(j->bind_hostport, "0.0.0.0:0", sizeof(j->bind_hostport) - 1);
        } else {
            strncpy(j->bind_hostport, g_lc.my_bind, sizeof(j->bind_hostport) - 1);
        }
        strncpy(j->peer_hostport, j->host_endpoint, sizeof(j->peer_hostport) - 1);
    }
    j->bind_hostport[sizeof(j->bind_hostport) - 1] = '\0';
    j->peer_hostport[sizeof(j->peer_hostport) - 1] = '\0';
}

/* Read one seat array into the membership table, appending from `n`.
 *
 * Players and spectators arrive as two arrays of identical rows and land in
 * one table tagged by role, because every consumer -- the UI's two tables
 * included -- wants "who is here and what are they" rather than two parallel
 * lists to keep in step. Returns the new row count.
 *
 * `key` absent is not an error: a server that predates spectators sends no
 * "spectators", and the right result there is a lobby with an empty gallery. */
static int parse_seat_array(const char *json, const char *key, int is_spectator,
                            int n)
{
    char keybuf[32];
    const char *p;
    snprintf(keybuf, sizeof(keybuf), "\"%s\"", key);
    p = strstr(json, keybuf);
    if (!p) {
        return n;
    }
    p = strchr(p, '[');
    if (!p) {
        return n;
    }
    ++p;
    while (*p && n < PSX_LOBBY_MAX_MEMBERS) {
        const char *obj;
        while (*p && *p != '{') {
            if (*p == ']') {
                return n;
            }
            ++p;
        }
        if (*p != '{') {
            break;
        }
        obj = p;
        {
            int depth = 0;
            const char *end = p;
            do {
                if (*end == '{') {
                    ++depth;
                } else if (*end == '}') {
                    --depth;
                }
                ++end;
            } while (*end && depth > 0);
            {
                char chunk[768];
                char offer[512];
                size_t len = (size_t)(end - obj);
                if (len >= sizeof(chunk)) {
                    len = sizeof(chunk) - 1;
                }
                memcpy(chunk, obj, len);
                chunk[len] = '\0';
                memset(&g_lc.members[n], 0, sizeof(g_lc.members[n]));
                g_lc.members[n].slot = json_get_int(chunk, "slot", n);
                json_get_str(chunk, "player_id", g_lc.members[n].player_id,
                             sizeof(g_lc.members[n].player_id));
                json_get_str(chunk, "display_name", g_lc.members[n].display_name,
                             sizeof(g_lc.members[n].display_name));
                g_lc.members[n].ready = json_get_bool(chunk, "ready", 0);
                g_lc.members[n].is_spectator = is_spectator;
                if (json_extract_object(chunk, "bios_offer", offer, sizeof(offer))) {
                    char prefer[24];
                    prefer[0] = '\0';
                    g_lc.members[n].bios_offer_valid = 1;
                    g_lc.members[n].bios_can_openbios =
                        json_get_bool(offer, "can_openbios", 1);
                    g_lc.members[n].bios_can_scph1001 =
                        json_get_bool(offer, "can_scph1001", 0);
                    json_get_str(offer, "prefer", prefer, sizeof(prefer));
                    g_lc.members[n].bios_prefer_openbios =
                        (strcmp(prefer, "openbios") == 0) ? 1 : 0;
                }
                json_get_str(chunk, "country", g_lc.members[n].country,
                             sizeof(g_lc.members[n].country));
                json_get_str(chunk, "account", g_lc.members[n].account,
                             sizeof(g_lc.members[n].account));
                if (json_extract_object(chunk, "memcard_offer", offer, sizeof(offer))) {
                    g_lc.members[n].memcard_offer_valid = 1;
                    g_lc.members[n].memcard_has_card =
                        json_get_bool(offer, "has_card", 0);
                    g_lc.members[n].memcard_share =
                        json_get_bool(offer, "share", 0);
                }
                if (g_lc.player_id[0] &&
                    strcmp(g_lc.members[n].player_id, g_lc.player_id) == 0) {
                    g_lc.local_ready = g_lc.members[n].ready;
                    g_lc.join.local_slot = g_lc.members[n].slot;
                    /* And the role, which a host move can change under us at
                     * any moment. Everything downstream -- whether this build
                     * contributes input at all -- reads it, so it has to be
                     * refreshed from the same update that moved the seat. */
                    g_lc.join.local_is_spectator = is_spectator;
                }
                ++n;
                p = end;
            }
        }
    }
    return n;
}

static void parse_slots_array(const char *json)
{
    int n;
    g_lc.member_count = 0;
    g_lc.local_ready = 0;
    g_lc.join.local_is_spectator = 0;
    /* Default to what we already knew, not to zero. This also runs for
     * `launch`, which carries no allow_spectators -- it has no reason to --
     * and zeroing there would erase the gallery at the exact moment the client
     * decides whether it is in it. */
    g_lc.join.allow_spectators =
        json_get_bool(json, "allow_spectators", g_lc.join.allow_spectators);
    g_lc.join.max_spectators =
        json_get_int(json, "max_spectators", g_lc.join.max_spectators);
    g_lc.join.spectator_count =
        json_get_int(json, "spectator_count", g_lc.join.spectator_count);
    g_lc.join.spectator_relay_base =
        json_get_int(json, "spectator_relay_base",
                     g_lc.join.spectator_relay_base);
    g_lc.join.host_spectates = json_get_bool(json, "host_spectates", 0);
    g_lc.join.spectator_slot_base =
        json_get_int(json, "spectator_slot_base",
                     g_lc.join.spectator_slot_base > 0
                         ? g_lc.join.spectator_slot_base
                         : PSX_LOBBY_SPECTATOR_SLOT_BASE);
    n = parse_seat_array(json, "slots", 0, 0);
    n = parse_seat_array(json, "spectators", 1, n);
    g_lc.member_count = n;
}

static void ingest_host_player_id(const char *json)
{
    char host_id[PSX_LOBBY_ID_LEN];
    host_id[0] = '\0';
    json_get_str(json, "host_player_id", host_id, sizeof(host_id));
    if (host_id[0]) {
        strncpy(g_lc.host_player_id, host_id, sizeof(g_lc.host_player_id) - 1);
        g_lc.host_player_id[sizeof(g_lc.host_player_id) - 1] = '\0';
    }
}


/* ── Block list ────────────────────────────────────────────────────────────── */

int psx_lobby_set_blocks(const char *accounts)
{
    /* Room for the server's cap (256 ids) at 40 characters each, plus the
     * separators and the envelope. A list that would not fit is truncated at
     * a separator rather than sent malformed -- a half-written id at the end
     * would name nobody, and a malformed op would leave the server enforcing
     * nothing at all. */
    static char msg[256 * 41 + 64];
    static char list[256 * 41];
    size_t n = 0;
    int first = 1;
    const char *p = accounts ? accounts : "";

    if (!g_lc.connected) return -1;
    list[0] = '\0';
    while (*p) {
        const char *sep = strchr(p, ';');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len && len < 40 && n + len + 8 < sizeof(list)) {
            if (!first) { list[n++] = ','; }
            list[n++] = '"';
            memcpy(list + n, p, len);
            n += len;
            list[n++] = '"';
            list[n] = '\0';
            first = 0;
        }
        if (!sep) break;
        p = sep + 1;
    }
    snprintf(msg, sizeof(msg), "{\"op\":\"set_blocks\",\"accounts\":[%s]}", list);
    queue_send(msg);
    flush_pending();
    return 0;
}

/* ── Automatch ─────────────────────────────────────────────────────────────
 *
 * State machine and wire for recomp-net-server docs/AUTOMATCH.md. The room a
 * pairing produces arrives as an ordinary `joined` and is handled by the
 * existing code; this owns the ticket, the accept gate, and the latency
 * probe the server uses to pick an input delay.
 */
#define AUTOMATCH_PROBE_LEN 14

static int set_nonblock(int fd);   /* defined with the socket code below */

typedef struct {
    int  have_rulesets;          /* the server has answered once this connection */
    int  rulesets_in_flight;
    int  ruleset_count;
    PsxLobbyRuleset rulesets[PSX_LOBBY_MAX_RULESETS];

    int  state;                  /* PSX_LOBBY_AUTOMATCH_* */
    char ticket_id[PSX_LOBBY_ID_LEN];
    char match_id[PSX_LOBBY_ID_LEN];
    int  queued_secs;
    int  pool;
    char error[160];

    PsxLobbyAutomatchFound found;
    /* When the offer lapses, as a monotonic timestamp rather than the count
     * the server sent. The server states the deadline once; a client that
     * stored the number and showed it unchanged would display "15s" for the
     * whole fifteen seconds, which reads as a frozen dialog. */
    uint64_t found_deadline_ms;

    /* Where to send the latency probe, published on rulesets_ok and again on
     * automatch_queued. Kept from whichever arrived last. */
    char probe_host[128];
    int  probe_port;
    unsigned probe_magic;
    int  probe_type;
    /* The measurement, and the nonce that identifies our outstanding probe. */
    int      rtt_ms;             /* <0 = not measured yet */
    uint32_t probe_nonce;
    uint64_t probe_sent_ms;      /* 0 = none outstanding */
    int      probe_socket;       /* -1 = not open */
    int      rtt_reported;       /* the server has our number */
    /* A queue op is out and its answer -- automatch_queued, or an error --
     * has not arrived. Without it a refusal of the FIRST queue attempt
     * (need_account is the common one) would arrive while state is still
     * IDLE and be filed as somebody else's error. */
    int      queue_in_flight;
    /* This seat came from a pairing, not from a room somebody hosts. */
    int      in_automatch_room;
} LobbyAutomatch;

static LobbyAutomatch g_am = { .rtt_ms = -1, .probe_socket = -1 };

static void automatch_reset_queue_state(void)
{
    g_am.state = PSX_LOBBY_AUTOMATCH_IDLE;
    g_am.queue_in_flight = 0;
    g_am.ticket_id[0] = '\0';
    g_am.match_id[0] = '\0';
    g_am.queued_secs = 0;
    g_am.pool = 0;
    g_am.rtt_reported = 0;
    g_am.found_deadline_ms = 0;
    memset(&g_am.found, 0, sizeof(g_am.found));
}

static void automatch_fail(const char *why)
{
    g_am.state = PSX_LOBBY_AUTOMATCH_FAILED;
    g_am.queue_in_flight = 0;
    snprintf(g_am.error, sizeof(g_am.error), "%s", why ? why : "automatch failed");
    fprintf(stderr, "psx_lobby: automatch failed: %s\n", g_am.error);
}

static void automatch_probe_close(void)
{
    if (g_am.probe_socket >= 0) {
        close(g_am.probe_socket);
        g_am.probe_socket = -1;
    }
    g_am.probe_sent_ms = 0;
}

/* A new connection has no ticket and has not been told what queues exist:
 * the server holds both per socket. Called from `welcome` and on disconnect
 * so the availability question is re-asked, not answered from a stale yes. */
static void automatch_on_connection_reset(void)
{
    automatch_reset_queue_state();
    automatch_probe_close();
    g_am.have_rulesets = 0;
    g_am.rulesets_in_flight = 0;
    g_am.ruleset_count = 0;
    g_am.rtt_ms = -1;
    g_am.in_automatch_room = 0;
}

/*
 * The 14-byte probe: magic, type, then a nonce.
 *
 * LITTLE-ENDIAN, because that is what the relay's header is (`read_u32_le` /
 * `read_u16_le` in input_relay.rs) -- a big-endian magic is simply not this
 * protocol's magic, so the packet is dropped on the `magic` counter with no
 * reply. The nonce sits at offset 6, where an ordinary packet carries its
 * session id; the relay echoes those bytes untouched and only rewrites the
 * type (200 -> 201), so it comes back as sent.
 */
static void automatch_probe_pack(unsigned char *out, uint32_t nonce)
{
    const unsigned magic = g_am.probe_magic;
    const int type = g_am.probe_type ? g_am.probe_type : 200;
    memset(out, 0, AUTOMATCH_PROBE_LEN);
    out[0] = (unsigned char)(magic & 0xFF);
    out[1] = (unsigned char)((magic >> 8) & 0xFF);
    out[2] = (unsigned char)((magic >> 16) & 0xFF);
    out[3] = (unsigned char)((magic >> 24) & 0xFF);
    out[4] = (unsigned char)(type & 0xFF);
    out[5] = (unsigned char)((type >> 8) & 0xFF);
    out[6] = (unsigned char)(nonce & 0xFF);
    out[7] = (unsigned char)((nonce >> 8) & 0xFF);
    out[8] = (unsigned char)((nonce >> 16) & 0xFF);
    out[9] = (unsigned char)((nonce >> 24) & 0xFF);
}

static int automatch_probe_send(void)
{
    struct addrinfo hints, *res = NULL;
    unsigned char pkt[AUTOMATCH_PROBE_LEN];
    char portstr[16];
    int fd;

    if (!g_am.probe_host[0] || g_am.probe_port <= 0) return -1;
    if (g_am.probe_sent_ms) return 0;   /* one outstanding at a time */

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, sizeof(portstr), "%d", g_am.probe_port);
    if (getaddrinfo(g_am.probe_host, portstr, &hints, &res) != 0 || !res)
        return -1;

    fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    set_nonblock(fd);

    /* A fresh nonce per attempt, so a late reply to a previous probe cannot
     * be timed against this one's clock and report an absurdly low number. */
    g_am.probe_nonce = (uint32_t)(lobby_mono_ms() * 2654435761u) ^ 0x9E3779B9u;
    automatch_probe_pack(pkt, g_am.probe_nonce);

    if (sendto(fd, (const char *)pkt, (int)sizeof(pkt), 0,
               res->ai_addr, (int)res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    automatch_probe_close();
    g_am.probe_socket = fd;
    g_am.probe_sent_ms = lobby_mono_ms();
    return 0;
}

static void automatch_send_rtt(void)
{
    char msg[128];
    if (g_am.rtt_ms < 0 || g_am.rtt_reported) return;
    snprintf(msg, sizeof(msg),
             "{\"op\":\"automatch_rtt\",\"rtt_ms\":%d}", g_am.rtt_ms);
    queue_send(msg);
    g_am.rtt_reported = 1;
}

/* Polled every pump. Times the 201 reply, or gives up after 2 s. */
static void automatch_probe_poll(void)
{
    unsigned char buf[64];
    uint64_t now;

    if (g_am.probe_socket < 0 || !g_am.probe_sent_ms) return;
    now = lobby_mono_ms();

    for (;;) {
        int n = (int)recv(g_am.probe_socket, (char *)buf, (int)sizeof(buf), 0);
        if (n < 0) break;
        if (n < 10) continue;
        /* Match the nonce: the socket is unconnected and anything can arrive
         * on it, and an unrelated packet timed as our reply is a wrong number
         * reported as fact. */
        if (((uint32_t)buf[6] | (uint32_t)buf[7] << 8 |
             (uint32_t)buf[8] << 16 | (uint32_t)buf[9] << 24) != g_am.probe_nonce)
            continue;
        g_am.rtt_ms = (int)(now - g_am.probe_sent_ms);
        if (g_am.rtt_ms < 0) g_am.rtt_ms = 0;
        if (g_am.rtt_ms > 2000) g_am.rtt_ms = 2000;   /* the server clamps here too */
        fprintf(stderr, "psx_lobby: automatch probe %s:%d rtt=%d ms\n",
                g_am.probe_host, g_am.probe_port, g_am.rtt_ms);
        automatch_probe_close();
        /* Queued already? Then the ticket was enqueued on an unknown latency
         * and the server is holding it out of pairing for the probe grace --
         * tell it now rather than letting the grace lapse. */
        if (g_am.state == PSX_LOBBY_AUTOMATCH_QUEUED) automatch_send_rtt();
        return;
    }
    if (now - g_am.probe_sent_ms > 2000) {
        fprintf(stderr, "psx_lobby: automatch probe timed out (%s:%d) -- "
                        "queueing without a latency estimate\n",
                g_am.probe_host, g_am.probe_port);
        automatch_probe_close();
    }
}

/* Walk to the next {...} in an array, copying it out. Returns 0 at ']'. */
static int automatch_next_object(const char **pp, char *out, size_t cap)
{
    const char *p = *pp;
    const char *start;
    int depth = 0;
    size_t n;

    while (*p && *p != '{') {
        if (*p == ']') { *pp = p; return 0; }
        ++p;
    }
    if (*p != '{') { *pp = p; return 0; }
    start = p;
    do {
        if (*p == '{') ++depth;
        else if (*p == '}') --depth;
        ++p;
    } while (*p && depth > 0);
    n = (size_t)(p - start);
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    *pp = p;
    return 1;
}

/* `probe: { endpoint, magic, type }` -- where to measure the path. Published
 * on both rulesets_ok and queued, and taken from whichever arrived last. */
static void automatch_ingest_probe(const char *obj)
{
    char endpoint[160];
    char *colon;
    endpoint[0] = '\0';
    json_get_str(obj, "endpoint", endpoint, sizeof(endpoint));
    /* Rightmost colon: an IPv6 literal has several, and the port is last. */
    colon = strrchr(endpoint, ':');
    if (!colon || !colon[1]) return;
    *colon = '\0';
    snprintf(g_am.probe_host, sizeof(g_am.probe_host), "%s", endpoint);
    g_am.probe_port = atoi(colon + 1);
    g_am.probe_magic = (unsigned)json_get_int(obj, "magic", 0);
    g_am.probe_type = json_get_int(obj, "type", 200);
}

/* `titles: [ { ..., pool: N } ]` -- this host queues one title, so the first
 * row is the one the player is waiting in. */
static int automatch_first_pool(const char *json)
{
    const char *p = strstr(json, "\"titles\"");
    char obj[512];
    if (!p) return g_am.pool;
    p = strchr(p, '[');
    if (!p) return g_am.pool;
    ++p;
    if (!automatch_next_object(&p, obj, sizeof(obj))) return 0;
    return json_get_int(obj, "pool", 0);
}

int psx_lobby_automatch_request_rulesets(void)
{
    char msg[256];
    char gn_esc[JSON_ESC_CAP(PSX_LOBBY_NAME_LEN)];
    const char *gn = g_lc.filter_game_name;
    if (!g_lc.connected) return -1;
    if (!gn || !gn[0]) return -1;
    if (g_am.rulesets_in_flight) return 0;
    json_escape(gn, gn_esc, sizeof(gn_esc));
    snprintf(msg, sizeof(msg),
             "{\"op\":\"automatch_rulesets\",\"game_name\":\"%s\"}", gn_esc);
    queue_send(msg);
    g_am.rulesets_in_flight = 1;
    return 0;
}

int psx_lobby_automatch_available(void)
{
    /* Zero rulesets is a real answer and means the same as "no": this
     * deployment has none loaded for this title. Not having ASKED yet is also
     * no -- the button must not be offered on an assumption. */
    return g_am.have_rulesets && g_am.ruleset_count > 0;
}

int psx_lobby_automatch_ruleset_count(void) { return g_am.ruleset_count; }

int psx_lobby_automatch_ruleset_get(int index, PsxLobbyRuleset *out)
{
    if (!out || index < 0 || index >= g_am.ruleset_count) return 0;
    *out = g_am.rulesets[index];
    return 1;
}

int psx_lobby_automatch_queue(const char *ruleset_id, int mods_enabled,
                              const char *mod_exempt)
{
    char msg[2048];
    char exempt_json[768];
    char gn_esc[JSON_ESC_CAP(PSX_LOBBY_NAME_LEN)];
    char gv_esc[JSON_ESC_CAP(PSX_LOBBY_VERSION_LEN)];
    char rid_esc[JSON_ESC_CAP(PSX_LOBBY_RULESET_ID_LEN)];
    const char *rid = (ruleset_id && ruleset_id[0]) ? ruleset_id
                      : (g_am.ruleset_count > 0 ? g_am.rulesets[0].id : "");
    const char *disc_fp = psx_lobby_disc_fp();
    char rtt[48];

    if (!g_lc.connected) return -1;
    if (!rid[0]) return -1;
    if (g_am.state == PSX_LOBBY_AUTOMATCH_QUEUED ||
        g_am.state == PSX_LOBBY_AUTOMATCH_FOUND ||
        g_am.state == PSX_LOBBY_AUTOMATCH_ACCEPTED ||
        g_am.queue_in_flight)
        return -1;
    /* The queue key REQUIRES a fingerprint: in `join` an empty one means
     * "legacy host, no check", and a wildcard in a queue silently pairs a
     * different dump against this one. Refuse here rather than let the server
     * answer need_disc_fp, so the reason is available before the round trip. */
    if (!disc_fp || strlen(disc_fp) != 64) {
        automatch_fail("this build cannot fingerprint its disc, so it cannot queue");
        return -1;
    }

    json_escape(g_lc.filter_game_name, gn_esc, sizeof(gn_esc));
    json_escape(psx_lobby_game_version(), gv_esc, sizeof(gv_esc));
    json_escape(rid, rid_esc, sizeof(rid_esc));

    /* The exemption evidence, as a JSON ARRAY of strings -- never a
     * ';'-joined string, which is valid JSON that every reader using
     * as_array() sees as empty and so fails open (recomp-net's
     * chat_report.h records the instance this repo already paid for). */
    {
        size_t o = 0;
        const char *p = mod_exempt;
        int first = 1;
        exempt_json[o++] = '[';
        while (p && *p) {
            const char *end = p;
            char entry[160];
            char esc[JSON_ESC_CAP(sizeof(entry))];
            size_t len;
            while (*end && *end != ';' && *end != '\n') ++end;
            len = (size_t)(end - p);
            if (len && len < sizeof(entry)) {
                memcpy(entry, p, len);
                entry[len] = '\0';
                json_escape(entry, esc, sizeof(esc));
                if (o + strlen(esc) + 4 < sizeof(exempt_json)) {
                    if (!first) exempt_json[o++] = ',';
                    exempt_json[o++] = '"';
                    memcpy(exempt_json + o, esc, strlen(esc));
                    o += strlen(esc);
                    exempt_json[o++] = '"';
                    first = 0;
                } else {
                    /* Refuse rather than send a SHORT list: a truncated list
                     * of exemptions is one the server approves in full while
                     * the client relies on more than it declared. */
                    automatch_fail("too many mod exemptions to declare");
                    return -1;
                }
            }
            p = *end ? end + 1 : end;
        }
        exempt_json[o++] = ']';
        exempt_json[o] = '\0';
    }

    /* Measure before queueing when we can: a client that probes first never
     * waits out the server's probe grace at all. */
    if (g_am.rtt_ms < 0) automatch_probe_send();
    rtt[0] = '\0';
    if (g_am.rtt_ms >= 0)
        snprintf(rtt, sizeof(rtt), ",\"rtt_ms\":%d", g_am.rtt_ms);

    /* One title: this host runs one game. A multi-title launcher sends
     * several here, in preference order; the wire has always allowed it. */
    snprintf(msg, sizeof(msg),
             "{\"op\":\"automatch_queue\",\"titles\":[{"
             "\"game_name\":\"%s\",\"game_version\":\"%s\","
             "\"disc_fp\":\"%s\",\"ruleset_id\":\"%s\",\"max_slots\":2}],"
             "\"mods_enabled\":%s,\"mod_exempt\":%s%s}",
             gn_esc, gv_esc, disc_fp, rid_esc,
             mods_enabled ? "true" : "false", exempt_json, rtt);
    queue_send(msg);
    flush_pending();
    g_am.error[0] = '\0';
    g_am.queue_in_flight = 1;
    g_am.rtt_reported = (g_am.rtt_ms >= 0);
    return 0;
}

int psx_lobby_automatch_cancel(void)
{
    if (!g_lc.connected) return -1;
    queue_send("{\"op\":\"automatch_cancel\"}");
    flush_pending();
    return 0;
}

int psx_lobby_automatch_state(void) { return g_am.state; }
int psx_lobby_automatch_queued_secs(void) { return g_am.queued_secs; }
int psx_lobby_automatch_pool(void) { return g_am.pool; }

int psx_lobby_automatch_found_get(PsxLobbyAutomatchFound *out)
{
    if (!out || g_am.state != PSX_LOBBY_AUTOMATCH_FOUND) return 0;
    *out = g_am.found;
    /* Recomputed on every read, so the caller can poll it each frame and get
     * a live count. Never below 0: the offer is about to lapse, and a
     * negative would draw as one. */
    if (g_am.found_deadline_ms) {
        uint64_t now = lobby_mono_ms();
        out->accept_secs = now >= g_am.found_deadline_ms
                               ? 0
                               : (int)((g_am.found_deadline_ms - now + 999ull) / 1000ull);
    }
    return 1;
}

int psx_lobby_automatch_accept(int accept)
{
    char msg[192];
    char mid_esc[JSON_ESC_CAP(PSX_LOBBY_ID_LEN)];
    if (!g_lc.connected) return -1;
    if (g_am.state != PSX_LOBBY_AUTOMATCH_FOUND) return -1;
    /* The match id is echoed so a late answer to a LAPSED offer is discarded
     * rather than applied to whatever offer is current by then. */
    json_escape(g_am.match_id, mid_esc, sizeof(mid_esc));
    snprintf(msg, sizeof(msg),
             "{\"op\":\"automatch_accept\",\"match_id\":\"%s\","
             "\"accept\":%s}", mid_esc, accept ? "true" : "false");
    queue_send(msg);
    flush_pending();
    if (accept) {
        g_am.state = PSX_LOBBY_AUTOMATCH_ACCEPTED;
    } else {
        /* Declining ends the ticket. The strike is the server's to record. */
        automatch_reset_queue_state();
    }
    return 0;
}

int psx_lobby_automatch_room(void) { return g_am.in_automatch_room; }

void psx_lobby_automatch_refuse_local(const char *why)
{
    automatch_fail(why);
}

const char *psx_lobby_automatch_error(void) { return g_am.error; }

/* Inbound automatch ops. Returns 1 when `op` was one of ours. */
static int automatch_handle_op(const char *op, const char *json)
{
    if (strcmp(op, "automatch_rulesets_ok") == 0) {
        const char *p2 = strstr(json, "\"rulesets\"");
        char probe[256];
        int n = 0;
        g_am.have_rulesets = 1;
        g_am.rulesets_in_flight = 0;
        g_am.ruleset_count = 0;
        if (json_extract_object(json, "probe", probe, sizeof(probe)))
            automatch_ingest_probe(probe);
        if (p2) {
            p2 = strchr(p2, '[');
            if (p2) {
                ++p2;
                while (*p2 && n < PSX_LOBBY_MAX_RULESETS) {
                    char obj[1024];
                    PsxLobbyRuleset *r = &g_am.rulesets[n];
                    if (!automatch_next_object(&p2, obj, sizeof(obj))) break;
                    memset(r, 0, sizeof(*r));
                    json_get_str(obj, "id", r->id, sizeof(r->id));
                    json_get_str(obj, "label", r->label, sizeof(r->label));
                    json_get_str(obj, "caps_summary", r->caps_summary,
                                 sizeof(r->caps_summary));
                    json_get_str(obj, "game_version", r->game_version,
                                 sizeof(r->game_version));
                    r->max_slots = json_get_int(obj, "max_slots", 2);
                    if (r->id[0]) ++n;
                }
            }
        }
        g_am.ruleset_count = n;
        fprintf(stderr, "psx_lobby: automatch %d ruleset(s) for \"%s\"\n",
                n, g_lc.filter_game_name);
        /* Measure now rather than at queue time: a client that has already
         * probed never waits out the server's probe grace. */
        if (n > 0 && g_am.rtt_ms < 0) automatch_probe_send();
        return 1;
    }
    if (strcmp(op, "automatch_queued") == 0) {
        char probe[256];
        g_am.state = PSX_LOBBY_AUTOMATCH_QUEUED;
        g_am.error[0] = '\0';
        g_am.queue_in_flight = 0;
        json_get_str(json, "ticket_id", g_am.ticket_id, sizeof(g_am.ticket_id));
        g_am.queued_secs = 0;
        g_am.pool = automatch_first_pool(json);
        if (json_extract_object(json, "probe", probe, sizeof(probe)))
            automatch_ingest_probe(probe);
        if (g_am.rtt_ms < 0) automatch_probe_send();
        else automatch_send_rtt();
        fprintf(stderr, "psx_lobby: automatch queued (pool=%d)\n", g_am.pool);
        return 1;
    }
    if (strcmp(op, "automatch_status") == 0) {
        /* Pushed at most 1 Hz while queued. Not a state change: a status for
         * a ticket we already gave up on must not resurrect the queue. */
        if (g_am.state != PSX_LOBBY_AUTOMATCH_QUEUED) return 1;
        g_am.queued_secs = json_get_int(json, "queued_secs", g_am.queued_secs);
        g_am.pool = automatch_first_pool(json);
        return 1;
    }
    if (strcmp(op, "automatch_found") == 0) {
        char opp[512];
        memset(&g_am.found, 0, sizeof(g_am.found));
        json_get_str(json, "match_id", g_am.match_id, sizeof(g_am.match_id));
        /* The opponent is an OBJECT: { handle, discord_username, country }.
         * Read from the extracted object, not the whole frame -- "country"
         * at top level would be found inside it either way, but "handle" is
         * theirs and nothing else's. */
        if (json_extract_object(json, "opponent", opp, sizeof(opp))) {
            json_get_str(opp, "handle", g_am.found.opponent,
                         sizeof(g_am.found.opponent));
            json_get_str(opp, "discord_username", g_am.found.opponent_username,
                         sizeof(g_am.found.opponent_username));
            json_get_str(opp, "country", g_am.found.opponent_country,
                         sizeof(g_am.found.opponent_country));
        }
        json_get_str(json, "ruleset_id", g_am.found.ruleset_id,
                     sizeof(g_am.found.ruleset_id));
        json_get_str(json, "ruleset_label", g_am.found.ruleset_label,
                     sizeof(g_am.found.ruleset_label));
        g_am.found.est_rtt_ms = json_get_int(json, "est_rtt_ms", -1);
        g_am.found.accept_secs = json_get_int(json, "accept_secs", 15);
        g_am.found_deadline_ms =
            lobby_mono_ms() + (uint64_t)(g_am.found.accept_secs > 0
                                             ? g_am.found.accept_secs : 0) * 1000ull;
        g_am.state = PSX_LOBBY_AUTOMATCH_FOUND;
        fprintf(stderr, "psx_lobby: automatch found opponent=\"%s\" "
                        "est_rtt=%d ms, %d s to answer\n",
                g_am.found.opponent, g_am.found.est_rtt_ms,
                g_am.found.accept_secs);
        return 1;
    }
    if (strcmp(op, "automatch_accept_ok") == 0) {
        /* The echo carries WHICH answer was acknowledged, and honouring it is
         * not optional: setting ACCEPTED unconditionally reopened the gate on
         * a player who had just declined. A declined ack ends the ticket
         * here; the server's automatch_cancelled follows and is idempotent
         * with this. */
        if (json_get_bool(json, "accept", 1)) {
            if (g_am.state == PSX_LOBBY_AUTOMATCH_FOUND ||
                g_am.state == PSX_LOBBY_AUTOMATCH_ACCEPTED)
                g_am.state = PSX_LOBBY_AUTOMATCH_ACCEPTED;
        } else {
            automatch_reset_queue_state();
        }
        return 1;
    }
    if (strcmp(op, "automatch_requeue") == 0) {
        /* The other side declined or let it lapse. Back to waiting, with the
         * ticket intact -- this is not a failure and must not read as one.
         * `queued:false` (lobby_limit) is the one case the ticket is gone. */
        if (json_get_bool(json, "queued", 1)) {
            g_am.state = PSX_LOBBY_AUTOMATCH_QUEUED;
            memset(&g_am.found, 0, sizeof(g_am.found));
            g_am.found_deadline_ms = 0;
            g_am.pool = automatch_first_pool(json);
            fprintf(stderr, "psx_lobby: automatch re-queued (the offer lapsed)\n");
        } else {
            automatch_fail("The server is out of rooms -- try again shortly");
        }
        return 1;
    }
    if (strcmp(op, "automatch_cancelled") == 0) {
        char reason[32];
        int cooldown;
        reason[0] = '\0';
        json_get_str(json, "reason", reason, sizeof(reason));
        cooldown = json_get_int(json, "cooldown_secs", 0);
        automatch_reset_queue_state();
        /* A lapsed offer the player never answered is worth a line: the gate
         * simply vanishes otherwise, and the cooldown it cost is invisible. */
        if (strcmp(reason, "timeout") == 0 || strcmp(reason, "declined") == 0) {
            if (cooldown > 0) {
                char line[96];
                snprintf(line, sizeof(line),
                         "%d Second Cooldown For Declining", cooldown);
                automatch_fail(line);
            } else if (strcmp(reason, "timeout") == 0) {
                automatch_fail("The offer lapsed before you answered");
            }
        }
        return 1;
    }
    if (strcmp(op, "automatch_rtt_ok") == 0) {
        return 1;   /* acknowledgement only */
    }
    return 0;
}

/* An automatch refusal arrives as a plain `error`, so it has to be claimed
 * or it would be filed as a join failure and the queue would sit waiting for
 * a pairing that was never going to come. Only while an attempt is actually
 * in flight: these codes are automatch's, but `error` is everyone's.
 * Returns 1 when claimed. */
static int automatch_claim_error(const char *code, const char *json)
{
    const char *why = NULL;
    char line[160];
    if (!(g_am.queue_in_flight ||
          g_am.state == PSX_LOBBY_AUTOMATCH_QUEUED ||
          g_am.state == PSX_LOBBY_AUTOMATCH_FOUND ||
          g_am.state == PSX_LOBBY_AUTOMATCH_ACCEPTED))
        return 0;
    if      (!strcmp(code, "need_account"))       why = "Sign in to use automatch";
    else if (!strcmp(code, "automatch_off"))      why = "This server has no automatch queues";
    else if (!strcmp(code, "already_queued"))     why = "This account is already in a queue";
    else if (!strcmp(code, "already_in_lobby"))   why = "Leave the room first";
    else if (!strcmp(code, "unknown_ruleset"))    why = "That queue type is gone -- refresh";
    else if (!strcmp(code, "need_disc_fp"))       why = "The server needs a disc fingerprint this build did not send";
    else if (!strcmp(code, "version_not_pooled")) why = "This release is not the one this queue pools";
    else if (!strcmp(code, "mods_not_pooled"))    why = "Turn off sim-affecting mods to queue";
    else if (!strcmp(code, "mod_not_approved"))   why = "A mod exemption is not on this queue's approved list";
    else if (!strcmp(code, "slots_not_pooled"))   why = "Automatch is two-player only";
    else if (!strcmp(code, "queue_full"))         why = "The queue is full -- try again shortly";
    else if (!strcmp(code, "cooldown")) {
        int retry = json_get_int(json, "retry_secs", 0);
        if (retry > 0)
            snprintf(line, sizeof(line), "%d Second Cooldown For Declining", retry);
        else
            snprintf(line, sizeof(line), "Cooldown For Declining");
        why = line;
    }
    if (!why) return 0;
    automatch_fail(why);
    return 1;
}

static void handle_server_json(const char *json);

/* Parse complete unmasked server text frames from ws_pending; leave remainder. */
static void drain_ws_pending(void)
{
    while (g_lc.ws_pending_len >= 2) {
        size_t i = 0;
        uint8_t b0 = g_lc.ws_pending[i++];
        uint8_t b1 = g_lc.ws_pending[i++];
        int opcode = b0 & 0x0f;
        size_t plen = b1 & 0x7f;
        if (b1 & 0x80) {
            /* Server frames must not be masked. */
            g_lc.ws_pending_len = 0;
            return;
        }
        if (plen == 126) {
            if (g_lc.ws_pending_len < i + 2) {
                return;
            }
            plen = ((size_t)g_lc.ws_pending[i] << 8) | g_lc.ws_pending[i + 1];
            i += 2;
        } else if (plen == 127) {
            g_lc.ws_pending_len = 0;
            return;
        }
        if (g_lc.ws_pending_len < i + plen) {
            return;
        }
        if (opcode == 0x1 && plen + 1 < sizeof(g_lc.rx_http)) {
            char text[4096];
            memcpy(text, g_lc.ws_pending + i, plen);
            text[plen] = '\0';
            handle_server_json(text);
        }
        i += plen;
        memmove(g_lc.ws_pending, g_lc.ws_pending + i, g_lc.ws_pending_len - i);
        g_lc.ws_pending_len -= i;
        if (opcode == 0x8) {
            psx_lobby_disconnect();
            return;
        }
    }
}

static void chat_push(const char *player_id, const char *from, const char *text,
                      const char *mid, const char *account, int is_system)
{
    PsxLobbyChatMsg *m;
    int idx;
    if (!text || !text[0]) return;
    if (g_lc.chat_count < PSX_LOBBY_CHAT_RING) {
        idx = (g_lc.chat_head + g_lc.chat_count) % PSX_LOBBY_CHAT_RING;
        g_lc.chat_count++;
    } else {
        idx = g_lc.chat_head;
        g_lc.chat_head = (g_lc.chat_head + 1) % PSX_LOBBY_CHAT_RING;
    }
    m = &g_lc.chat[idx];
    memset(m, 0, sizeof(*m));
    snprintf(m->player_id, sizeof(m->player_id), "%s", player_id ? player_id : "");
    snprintf(m->from, sizeof(m->from), "%s", from ? from : "");
    snprintf(m->text, sizeof(m->text), "%s", text);
    snprintf(m->mid, sizeof(m->mid), "%s",
             (mid && !is_system) ? mid : "");
    snprintf(m->account, sizeof(m->account), "%s",
             (account && !is_system) ? account : "");
    /* Masked on arrival, whatever relayed it: the server already did this,
     * an older server did not, and the rule is that nothing unmasked is
     * ever shown. */
    if (!is_system) (void)rnet_chat_filter_apply(m->text, sizeof(m->text));
    m->is_system = is_system ? 1 : 0;
    m->is_local = (!is_system && g_lc.player_id[0] && player_id &&
                   strcmp(player_id, g_lc.player_id) == 0) ? 1 : 0;
    m->seq = ++g_lc.chat_seq;
}

static void schat_push(const char *player_id, const char *from, const char *text,
                       const char *mid, const char *account)
{
    PsxLobbyChatMsg *m;
    int idx;
    if (!text || !text[0]) return;
    if (g_lc.schat_count < PSX_LOBBY_CHAT_RING) {
        idx = (g_lc.schat_head + g_lc.schat_count) % PSX_LOBBY_CHAT_RING;
        g_lc.schat_count++;
    } else {
        idx = g_lc.schat_head;
        g_lc.schat_head = (g_lc.schat_head + 1) % PSX_LOBBY_CHAT_RING;
    }
    m = &g_lc.schat[idx];
    memset(m, 0, sizeof(*m));
    snprintf(m->player_id, sizeof(m->player_id), "%s", player_id ? player_id : "");
    snprintf(m->from, sizeof(m->from), "%s", from ? from : "");
    snprintf(m->text, sizeof(m->text), "%s", text);
    snprintf(m->mid, sizeof(m->mid), "%s", mid ? mid : "");
    snprintf(m->account, sizeof(m->account), "%s", account ? account : "");
    (void)rnet_chat_filter_apply(m->text, sizeof(m->text));
    m->is_local = (g_lc.player_id[0] && player_id &&
                   strcmp(player_id, g_lc.player_id) == 0) ? 1 : 0;
    m->seq = ++g_lc.schat_seq;
}

void psx_lobby_chat_clear(void)
{
    g_lc.chat_head = 0;
    g_lc.chat_count = 0;
    /* seq keeps counting: a UI comparing "newest seen" must not mistake the
     * first line of a new room for one it already scrolled to. */
}

/* The identity message: who we are and what we are playing. Sent once on
 * `welcome` and again whenever the player renames. Both fields are escaped
 * -- a name with a quote in it used to build malformed JSON, which the
 * server drops whole, so the rename simply never happened. */
static void queue_hello(void)
{
    char name_esc[PSX_LOBBY_NAME_LEN * 2 + 8];
    char game_esc[PSX_LOBBY_NAME_LEN * 2 + 8];
    char sess_esc[2048];
    char msg[PSX_LOBBY_NAME_LEN * 4 + 2176];
    const char *sess = rnet_account_session();
    json_escape(g_lc.display_name, name_esc, sizeof(name_esc));
    json_escape(g_lc.filter_game_name, game_esc, sizeof(game_esc));
    /* The session is OPTIONAL and omitted entirely when this client is a
     * guest, which is what keeps an unauthenticated hello byte-identical to
     * the one this client has always sent. */
    if (sess && sess[0]) {
        json_escape(sess, sess_esc, sizeof(sess_esc));
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"hello\",\"display_name\":\"%s\",\"game_name\":\"%s\","
                 "\"session\":\"%s\"}",
                 name_esc, game_esc, sess_esc);
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"hello\",\"display_name\":\"%s\",\"game_name\":\"%s\"}",
                 name_esc, game_esc);
    }
    queue_send(msg);
    flush_pending();
}

static void handle_server_json(const char *json)
{
    char op[32];
    json_get_str(json, "op", op, sizeof(op));
    if (strcmp(op, "welcome") == 0) {
        json_get_str(json, "player_id", g_lc.player_id, sizeof(g_lc.player_id));
        /* A fresh socket: the server holds the ticket and the ruleset answer
         * per connection, so neither survives from the last one. */
        automatch_on_connection_reset();
        {
            /* Say who we are AND what we are playing in the first message:
             * the server scopes players-online and server chat by title, and
             * waiting for a `list` to tell it left a client that chatted
             * first with no title at all. Title only -- never the version. */
            queue_hello();
            fprintf(stderr, "psx_lobby: hello as \"%s\" for game \"%s\"\n",
                    g_lc.display_name,
                    g_lc.filter_game_name[0] ? g_lc.filter_game_name : "(none)");
        }
        queue_list_request();
        /* Prefetch Coturn creds for ICE (no-op reply if server lacks COTURN_*). */
        (void)queue_turn_credentials_request();
        return;
    }
    if (strcmp(op, "turn_credentials") == 0) {
        int ok = json_get_bool(json, "ok", 0);
        g_lc.turn_request_pending = 0;
        memset(&g_lc.turn, 0, sizeof(g_lc.turn));
        g_lc.turn_received_at = 0;
        if (!ok) {
            char err[64];
            json_get_str(json, "error", err, sizeof(err));
            fprintf(stderr,
                    "psx_lobby: turn_credentials failed (%s) — online ICE "
                    "requires Coturn (or PSX_NET_TURN_* / "
                    "PSX_NET_ALLOW_STUN_ONLY=1)\n",
                    err[0] ? err : "unknown");
            return;
        }
        json_get_str(json, "stun_host", g_lc.turn.stun_host,
                     sizeof(g_lc.turn.stun_host));
        json_get_str(json, "turn_host", g_lc.turn.turn_host,
                     sizeof(g_lc.turn.turn_host));
        json_get_str(json, "username", g_lc.turn.username,
                     sizeof(g_lc.turn.username));
        json_get_str(json, "password", g_lc.turn.password,
                     sizeof(g_lc.turn.password));
        g_lc.turn.stun_port = json_get_int(json, "stun_port", 3478);
        g_lc.turn.turn_port = json_get_int(json, "turn_port", 3478);
        g_lc.turn.ttl_secs = (uint32_t)json_get_int(json, "ttl_secs", 86400);
        if (g_lc.turn.turn_host[0] && g_lc.turn.username[0] &&
            g_lc.turn.password[0]) {
            g_lc.turn.valid = 1;
            g_lc.turn_received_at = time(NULL);
            fprintf(stderr,
                    "psx_lobby: turn_credentials ok stun=%s:%d turn=%s:%d "
                    "user=%s ttl=%us\n",
                    g_lc.turn.stun_host[0] ? g_lc.turn.stun_host : "(none)",
                    g_lc.turn.stun_port,
                    g_lc.turn.turn_host, g_lc.turn.turn_port,
                    g_lc.turn.username, (unsigned)g_lc.turn.ttl_secs);
        } else {
            fprintf(stderr,
                    "psx_lobby: turn_credentials ok but incomplete fields\n");
        }
        return;
    }
    if (strcmp(op, "lobby_list") == 0) {
        const char *p = strstr(json, "\"lobbies\"");
        int n = 0;
        lobby_list_parse_players(json);
        /* Keep prior RTTs across server list pushes; Refresh re-probes.
         * Invalidate when host_endpoint or lan_endpoints change. */
        char prev_ids[PSX_LOBBY_MAX_LIST][PSX_LOBBY_ID_LEN];
        char prev_eps[PSX_LOBBY_MAX_LIST][PSX_LOBBY_ENDPOINT_LEN];
        char prev_lan[PSX_LOBBY_MAX_LIST][256];
        int prev_ms[PSX_LOBBY_MAX_LIST];
        int prev_n = g_lc.list_count;
        int i;
        int want_probe = g_list_rtt_on_next_list;
        g_list_rtt_on_next_list = 0;
        for (i = 0; i < prev_n && i < PSX_LOBBY_MAX_LIST; ++i) {
            strncpy(prev_ids[i], g_lc.list[i].lobby_id, PSX_LOBBY_ID_LEN - 1);
            prev_ids[i][PSX_LOBBY_ID_LEN - 1] = '\0';
            strncpy(prev_eps[i], g_lc.list[i].host_endpoint,
                    PSX_LOBBY_ENDPOINT_LEN - 1);
            prev_eps[i][PSX_LOBBY_ENDPOINT_LEN - 1] = '\0';
            lobby_row_lan_fingerprint(&g_lc.list[i], prev_lan[i], sizeof(prev_lan[i]));
            prev_ms[i] = g_lc.list[i].latency_ms;
        }
        g_lc.list_count = 0;
        if (!p) {
            return;
        }
        p = strchr(p, '[');
        if (!p) {
            return;
        }
        ++p;
        while (*p && n < PSX_LOBBY_MAX_LIST) {
            const char *obj;
            while (*p && *p != '{') {
                if (*p == ']') {
                    g_lc.list_count = n;
                    if (want_probe)
                        lobby_list_rtt_start(1);
                    else if (n > 0 && !g_list_rtt_active)
                        lobby_list_rtt_start(0);
                    return;
                }
                ++p;
            }
            if (*p != '{') {
                break;
            }
            obj = p;
            {
                int depth = 0;
                const char *end = p;
                do {
                    if (*end == '{') {
                        ++depth;
                    } else if (*end == '}') {
                        --depth;
                    }
                    ++end;
                } while (*end && depth > 0);
                {
                    char chunk[1536];
                    char lan_fp[256];
                    size_t len = (size_t)(end - obj);
                    if (len >= sizeof(chunk)) {
                        len = sizeof(chunk) - 1;
                    }
                    memcpy(chunk, obj, len);
                    chunk[len] = '\0';
                    memset(&g_lc.list[n], 0, sizeof(g_lc.list[n]));
                    g_lc.list[n].latency_ms = -1;
                    json_get_str(chunk, "lobby_id", g_lc.list[n].lobby_id, sizeof(g_lc.list[n].lobby_id));
                    json_get_str(chunk, "name", g_lc.list[n].name, sizeof(g_lc.list[n].name));
                    json_get_str(chunk, "game_name", g_lc.list[n].game_name, sizeof(g_lc.list[n].game_name));
                    json_get_str(chunk, "game_version", g_lc.list[n].game_version,
                                 sizeof(g_lc.list[n].game_version));
                    /* Missing version stays empty for filter decisions; display
                     * defaults to "dev" only after accept. Old servers omitted
                     * game_version — rewriting to "dev" before the strict pin
                     * hid those rows from release clients. */
                    const int has_game_version = g_lc.list[n].game_version[0] != '\0';
                    /* Drop lobbies that don't match our title (broadcast list
                     * is unfiltered). Release builds also pin game_version;
                     * "dev" keeps other versions visible for testing. */
                    if (g_lc.filter_game_name[0] &&
                        strcmp(g_lc.list[n].game_name, g_lc.filter_game_name) != 0) {
                        p = end;
                        continue;
                    }
                    if (list_filter_version_strict() && has_game_version) {
                        const char *want_ver = effective_game_version(NULL);
                        if (want_ver && want_ver[0] &&
                            strcmp(g_lc.list[n].game_version, want_ver) != 0) {
                            p = end;
                            continue;
                        }
                    }
                    if (!has_game_version) {
                        strncpy(g_lc.list[n].game_version, "dev",
                                sizeof(g_lc.list[n].game_version) - 1);
                    }
                    g_lc.list[n].player_count = json_get_int(chunk, "player_count", 0);
                    g_lc.list[n].max_slots = json_get_int(chunk, "max_slots", 2);
                    g_lc.list[n].has_password = json_get_bool(chunk, "has_password", 0);
                    json_get_str(chunk, "host_country", g_lc.list[n].host_country,
                                 sizeof(g_lc.list[n].host_country));
                    g_lc.list[n].allow_spectators = json_get_bool(chunk, "allow_spectators", 0);
                    g_lc.list[n].max_spectators = json_get_int(chunk, "max_spectators", 0);
                    g_lc.list[n].spectator_count = json_get_int(chunk, "spectator_count", 0);
                    json_get_str(chunk, "host_endpoint", g_lc.list[n].host_endpoint,
                                 sizeof(g_lc.list[n].host_endpoint));
                    g_lc.list[n].lan_count = json_parse_str_array(
                        chunk, "lan_endpoints", g_lc.list[n].lan_endpoints,
                        PSX_LOBBY_MAX_LAN_EPS);
                    lobby_row_lan_fingerprint(&g_lc.list[n], lan_fp, sizeof(lan_fp));
                    if (!want_probe) {
                        for (i = 0; i < prev_n; ++i) {
                            if (prev_ids[i][0] &&
                                strcmp(prev_ids[i], g_lc.list[n].lobby_id) == 0 &&
                                strcmp(prev_eps[i], g_lc.list[n].host_endpoint) == 0 &&
                                strcmp(prev_lan[i], lan_fp) == 0) {
                                g_lc.list[n].latency_ms = prev_ms[i];
                                break;
                            }
                        }
                    }
                    ++n;
                    p = end;
                }
            }
        }
        g_lc.list_count = n;
        if (want_probe)
            lobby_list_rtt_start(1);
        else {
            int need = 0;
            for (i = 0; i < n; ++i) {
                if (g_lc.list[i].latency_ms < 0 &&
                    (g_lc.list[i].host_endpoint[0] || g_lc.list[i].lan_count > 0)) {
                    need = 1;
                    break;
                }
            }
            /* Restart even if a prior burst is mid-flight — advertise may have
             * just published a public host_endpoint / LAN candidate. */
            if (need)
                lobby_list_rtt_start(0);
        }
        return;
    }
    if (strcmp(op, "created") == 0) {
        psx_lobby_chat_clear();
        g_lc.in_lobby = 1;
        g_lc.is_host = 1;
        g_lc.join.ok = 1;
        g_lc.launch_pending = 0;
        g_lc.all_ready = 0;
        member_rtt_clear();
        json_get_str(json, "lobby_id", g_lc.join.lobby_id, sizeof(g_lc.join.lobby_id));
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", 1);
        g_lc.join.local_slot = json_get_int(json, "local_slot", 0);
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = 1;
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lobby_max_slots);
        g_lc.join.last_error[0] = '\0';
        if (g_lc.player_id[0]) {
            strncpy(g_lc.host_player_id, g_lc.player_id, sizeof(g_lc.host_player_id) - 1);
            g_lc.host_player_id[sizeof(g_lc.host_player_id) - 1] = '\0';
        }
        ingest_host_player_id(json);
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        if (g_lc.member_count == 0) {
            g_lc.members[0].slot = 0;
            strncpy(g_lc.members[0].player_id, g_lc.player_id, sizeof(g_lc.members[0].player_id) - 1);
            strncpy(g_lc.members[0].display_name, g_lc.display_name,
                    sizeof(g_lc.members[0].display_name) - 1);
            g_lc.members[0].ready = 0;
            g_lc.member_count = 1;
            g_lc.local_ready = 0;
        }
        /* After create: LAN beacon immediately; STUN for public host_endpoint. */
        g_host_adv_state = HOST_ADV_WAIT_TURN;
        g_host_adv_deadline_ms = lobby_mono_ms() + 500ull;
        lobby_lan_beacon_publish_update();
        return;
    }
    if (strcmp(op, "host_endpoint_ok") == 0) {
        return;
    }
    if (strcmp(op, "joined") == 0) {
        psx_lobby_chat_clear();
        /* A ticket that reached a room is spent: `joined` is that moment for
         * automatch (AUTOMATCH.md 8). Recorded before the reset below, which
         * erases the evidence -- ACCEPTED (or FOUND, if the pair resolved in
         * the same breath) is the only signal that this seat came from a
         * queue rather than from somebody's room. */
        g_am.in_automatch_room =
            (g_am.state == PSX_LOBBY_AUTOMATCH_ACCEPTED ||
             g_am.state == PSX_LOBBY_AUTOMATCH_FOUND);
        automatch_reset_queue_state();
        g_lc.in_lobby = 1;
        g_lc.is_host = 0;
        g_lc.join.ok = 1;
        g_lc.launch_pending = 0;
        g_lc.all_ready = 0;
        member_rtt_clear();
        json_get_str(json, "lobby_id", g_lc.join.lobby_id, sizeof(g_lc.join.lobby_id));
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", 1);
        g_lc.join.local_slot = json_get_int(json, "local_slot", 1);
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", 2);
        g_lc.join.max_slots = json_get_int(json, "max_slots", 2);
        g_lc.join.last_error[0] = '\0';
        ingest_host_player_id(json);
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        /* Prefer slots on joined when present; lobby_update usually follows. */
        parse_slots_array(json);
        return;
    }
    if (strcmp(op, "lobby_update") == 0) {
        g_lc.in_lobby = 1;
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", g_lc.join.player_count);
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lc.join.max_slots);
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", (int)g_lc.join.session_id);
        g_lc.all_ready = json_get_bool(json, "all_ready", 0);
        ingest_host_player_id(json);
        if (g_lc.host_player_id[0] && g_lc.player_id[0]) {
            g_lc.is_host = (strcmp(g_lc.host_player_id, g_lc.player_id) == 0);
        }
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        return;
    }
    if (strcmp(op, "launch") == 0) {
        char relay_endpoint[PSX_LOBBY_ENDPOINT_LEN];
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        relay_endpoint[0] = '\0';
        json_get_str(json, "relay_endpoint", relay_endpoint, sizeof(relay_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", g_lc.join.player_count);
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lc.join.max_slots);
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", (int)g_lc.join.session_id);
        ingest_match_caps_from_json(json);
        /* Prefer explicit relay_endpoint when the server opened input relay.
         * Apply after caps ingest: omitted force_input_relay must not leave
         * hosts on the hub path while guests dial the relay. */
        if (relay_endpoint[0] && !endpoint_port_is_zero(relay_endpoint)) {
            char relay_raw[PSX_LOBBY_ENDPOINT_LEN];
            strncpy(relay_raw, relay_endpoint, sizeof(relay_raw) - 1);
            relay_raw[sizeof(relay_raw) - 1] = '\0';
            if (rewrite_relay_endpoint_to_lobby_host(relay_endpoint,
                                                    sizeof(relay_endpoint)) > 0) {
                fprintf(stderr,
                        "psx_lobby: relay_endpoint %s → %s (WS peer/host)\n",
                        relay_raw, relay_endpoint);
            }
            strncpy(g_lc.join.host_endpoint, relay_endpoint,
                    sizeof(g_lc.join.host_endpoint) - 1);
            g_lc.join.host_endpoint[sizeof(g_lc.join.host_endpoint) - 1] = '\0';
            strncpy(g_lc.join.guest_endpoint, relay_endpoint,
                    sizeof(g_lc.join.guest_endpoint) - 1);
            g_lc.join.guest_endpoint[sizeof(g_lc.join.guest_endpoint) - 1] = '\0';
            if (!g_lc.match_caps.valid)
                g_lc.match_caps.valid = 1;
            g_lc.match_caps.force_input_relay = 1;
        } else if (g_lc.match_caps.valid) {
            /* Non-SFU launch (legacy ice_p2p) — clear stale force_input_relay. */
            g_lc.match_caps.force_input_relay = 0;
        }
        fill_peer_bind_from_join();
        parse_slots_array(json);
        /* §108: online start always SFU — every peer dials relay_endpoint.
         * Legacy ice_p2p from an old server is refused (need SFU). */
        {
            char transport[24];
            const int force_relay = using_server_input_relay(&g_lc.join);
            const int seats = g_lc.join.player_count >= 2 ? g_lc.join.player_count
                                                         : g_lc.join.max_slots;
            const int host_hub =
                (g_lc.is_host && seats >= 3 && !force_relay) ? 1 : 0;
            const int peer_bad = !g_lc.join.peer_hostport[0] ||
                                 endpoint_port_is_zero(g_lc.join.peer_hostport);
            int ice_p2p = 0;
            transport[0] = '\0';
            json_get_str(json, "transport", transport, sizeof(transport));
            if (strcmp(transport, "ice_p2p") == 0)
                ice_p2p = 1;
            if (force_relay) {
                if (peer_bad) {
                    strncpy(g_lc.join.last_error, "missing_endpoints",
                            sizeof(g_lc.join.last_error) - 1);
                    g_lc.launch_pending = 0;
                    return;
                }
            } else if (ice_p2p) {
                fprintf(stderr,
                        "psx_lobby: launch transport=ice_p2p refused — "
                        "online lobbies require SFU (§108; upgrade lobby "
                        "server)\n");
                fflush(stderr);
                strncpy(g_lc.join.last_error, "sfu_required",
                        sizeof(g_lc.join.last_error) - 1);
                g_lc.launch_pending = 0;
                return;
            } else if (!g_lc.join.host_endpoint[0] ||
                       (g_lc.is_host && !host_hub && !g_lc.join.guest_endpoint[0]) ||
                       (!g_lc.is_host && peer_bad)) {
                strncpy(g_lc.join.last_error, "missing_endpoints",
                        sizeof(g_lc.join.last_error) - 1);
                g_lc.launch_pending = 0;
                return;
            }
        }
        g_lc.join.last_error[0] = '\0';
        g_lc.launch_pending = 1;
        /* Match owns WS ICE signals until rematch — clear_launch_pending must
         * not revive the waiting-room probe (it was stealing match candidates). */
        g_lc.ice_rtt_suspended = 1;
        lobby_ice_rtt_close();
        lobby_rtt_close();
        psx_lobby_clear_signals();
        /* Accept ICE for this match; lobby probe had accept=1 while seated. */
        g_lc.ice_signal_accept = 1;
        rnet_lan_beacon_close(&g_lan_beacon_pub);
        lobby_host_advertise_reset();
        return;
    }
    if (strcmp(op, "seat_swap_ask") == 0) {
        g_lc.swap_in_valid = 1;
        json_get_str(json, "asker_player_id", g_lc.swap_in_asker_id,
                     sizeof(g_lc.swap_in_asker_id));
        json_get_str(json, "asker_name", g_lc.swap_in_asker_name,
                     sizeof(g_lc.swap_in_asker_name));
        g_lc.swap_in_from_slot = json_get_int(json, "from_slot", -1);
        return;
    }
    if (strcmp(op, "seat_swap_result") == 0) {
        g_lc.swap_out = json_get_bool(json, "accept", 0) ? 2 : -1;
        return;
    }
    if (strcmp(op, "server_chat") == 0) {
        char text[PSX_LOBBY_CHAT_TEXT_LEN];
        char from_id[PSX_LOBBY_ID_LEN];
        char from[PSX_LOBBY_NAME_LEN];
        char mid[40];
        char account[PSX_LOBBY_ID_LEN];
        text[0] = '\0';
        from_id[0] = '\0';
        from[0] = '\0';
        mid[0] = '\0';
        account[0] = '\0';
        json_get_str(json, "text", text, sizeof(text));
        json_get_str(json, "from_player_id", from_id, sizeof(from_id));
        json_get_str(json, "from", from, sizeof(from));
        json_get_str(json, "mid", mid, sizeof(mid));
        json_get_str(json, "from_account", account, sizeof(account));
        schat_push(from_id, from, text, mid, account);
        return;
    }
    if (strcmp(op, "chat") == 0) {
        char text[PSX_LOBBY_CHAT_TEXT_LEN];
        char from_id[PSX_LOBBY_ID_LEN];
        char from[PSX_LOBBY_NAME_LEN];
        char mid[40];
        char account[PSX_LOBBY_ID_LEN];
        text[0] = '\0';
        from_id[0] = '\0';
        from[0] = '\0';
        mid[0] = '\0';
        account[0] = '\0';
        json_get_str(json, "text", text, sizeof(text));
        json_get_str(json, "from_player_id", from_id, sizeof(from_id));
        json_get_str(json, "from", from, sizeof(from));
        json_get_str(json, "mid", mid, sizeof(mid));
        json_get_str(json, "from_account", account, sizeof(account));
        chat_push(from_id, from, text, mid, account,
                  json_get_bool(json, "system", 0));
        return;
    }
    if (strcmp(op, "signal") == 0) {
        char text_buf[2048];
        char from[PSX_LOBBY_ID_LEN];
        int type = json_get_int(json, "type", 0);
        int flag = json_get_int(json, "flag", 0);
        text_buf[0] = '\0';
        from[0] = '\0';
        json_get_str(json, "text", text_buf, sizeof(text_buf));
        json_get_str(json, "from_player_id", from, sizeof(from));
        /* Legacy WS RTT_PING/PONG ignored — waiting-room latency uses UDP
         * rnet_rtt_probe (peer path). REPORT still accepted from peers. */
        if (type == PSX_LOBBY_SIG_RTT_PING || type == PSX_LOBBY_SIG_RTT_PONG)
            return;
        if (type == PSX_LOBBY_SIG_RTT_REPORT) {
            int slot = rtt_index_for_slot(member_slot_for_player(from));
            int ms = (int)strtol(text_buf, NULL, 10);
            /* Max with local measure — never let an optimistic peer REPORT
             * undercut delay provisioning (Force TURN / asymmetric ICE). */
            if (slot >= 0 && ms >= 0 && ms <= 60000) {
                if (g_lc.member_rtt_ms[slot] < 0 ||
                    ms > g_lc.member_rtt_ms[slot])
                    g_lc.member_rtt_ms[slot] = ms;
            }
            return;
        }
        enqueue_signal(type, flag, text_buf);
        (void)flag;
        return;
    }
    if (automatch_handle_op(op, json)) {
        return;
    }
    if (strcmp(op, "error") == 0) {
        char code[64];
        json_get_str(json, "code", code, sizeof(code));
        strncpy(g_lc.join.last_error, code, sizeof(g_lc.join.last_error) - 1);
        g_lc.join.last_error[sizeof(g_lc.join.last_error) - 1] = '\0';
        if (automatch_claim_error(code, json)) {
            return;
        }
        /* Create/join failures are fatal to the seat. In-lobby ops (kick/move
         * on an older server, not_host, …) must not clear join.ok or the room
         * looks abandoned after a rejected host action. */
        if (!g_lc.in_lobby ||
            strcmp(code, "bad_password") == 0 ||
            strcmp(code, "full") == 0 ||
            strcmp(code, "gone") == 0 ||
            strcmp(code, "already_in_lobby") == 0 ||
            strcmp(code, "lobby_limit") == 0 ||
            strcmp(code, "version_mismatch") == 0 ||
            strcmp(code, "game_mismatch") == 0 ||
            strcmp(code, "disc_mismatch") == 0) {
            g_lc.join.ok = 0;
        }
        return;
    }
    if (strcmp(op, "lobby_closed") == 0 || strcmp(op, "left") == 0 ||
        strcmp(op, "kicked") == 0) {
        psx_lobby_chat_clear();
        /* Leaving a room must never leave a queue ticket looking live, and
         * no seat means no automatch room either. */
        automatch_reset_queue_state();
        g_am.in_automatch_room = 0;
        g_lc.swap_in_valid = 0;
        g_lc.swap_out = 0;
        g_lc.ice_rtt_suspended = 0;
        lobby_ice_rtt_close();
        lobby_rtt_close();
        lobby_host_advertise_reset();
        g_lc.in_lobby = 0;
        g_lc.is_host = 0;
        g_lc.host_player_id[0] = '\0';
        g_lc.member_count = 0;
        g_lc.local_ready = 0;
        g_lc.all_ready = 0;
        g_lc.launch_pending = 0;
        memset(&g_lc.join, 0, sizeof(g_lc.join));
        match_caps_clear(&g_lc.match_caps);
        member_rtt_clear();
        return;
    }
}

static int set_nonblock(int fd)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int set_block(int fd)
{
#if defined(_WIN32)
    u_long mode = 0;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

/* Off-thread DNS + TCP + WS upgrade so the launcher UI never blocks on
 * getaddrinfo / connect (common multi-second freeze on Windows). */
enum { PSX_LOBBY_CONNECT_TIMEOUT_MS = 3000 };

static int g_lc_tcp_connecting;
static unsigned g_connect_gen;
static volatile int g_connect_cancel;
static volatile int g_connect_worker_done;
static volatile int g_connect_worker_rc;
static volatile unsigned g_connect_worker_gen;
static int g_connect_worker_fd;
#if defined(_WIN32)
static HANDLE g_connect_thread;
#else
static pthread_t g_connect_thread;
static int g_connect_thread_valid;
#endif

static int socket_connect_in_progress(void)
{
#if defined(_WIN32)
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
}

static int wait_socket_connected(int fd, int timeout_ms)
{
    fd_set wfds;
    fd_set efds;
    struct timeval tv;
    int soerr = 0;
#if defined(_WIN32)
    int len = (int)sizeof(soerr);
#else
    socklen_t len = sizeof(soerr);
#endif
    int r;

    FD_ZERO(&wfds);
    FD_ZERO(&efds);
#if defined(_WIN32)
    FD_SET((SOCKET)fd, &wfds);
    FD_SET((SOCKET)fd, &efds);
#else
    FD_SET(fd, &wfds);
    FD_SET(fd, &efds);
#endif
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    r = select(0, NULL, &wfds, &efds, &tv);
#else
    r = select(fd + 1, NULL, &wfds, &efds, &tv);
#endif
    if (r <= 0)
        return -1;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) != 0 || soerr != 0)
        return -1;
    return 0;
}

static int lobby_tcp_connect(const char *host, int port, int *out_fd)
{
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int fd = -1;

    if (!host || !out_fd)
        return -2;
    *out_fd = -1;
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -2;
    for (rp = res; rp; rp = rp->ai_next) {
        int rc;
        if (g_connect_cancel)
            break;
        fd = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        set_nonblock(fd);
        rc = connect(fd, rp->ai_addr, (int)rp->ai_addrlen);
        if (rc == 0)
            break;
        if (socket_connect_in_progress()) {
            if (wait_socket_connected(fd, PSX_LOBBY_CONNECT_TIMEOUT_MS) == 0)
                break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -3;
    set_block(fd);
    *out_fd = fd;
    return 0;
}

static int lobby_send_ws_upgrade(int fd, const char *host, int port, const char *path)
{
    char key_raw[16];
    char key_b64[32];
    char req[512];
    int i;
    static const char *B64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;

    for (i = 0; i < 16; ++i)
        key_raw[i] = (char)(rand() & 0xff);
    for (i = 0; i < 16; i += 3) {
        unsigned v = ((unsigned char)key_raw[i] << 16);
        if (i + 1 < 16)
            v |= ((unsigned char)key_raw[i + 1] << 8);
        if (i + 2 < 16)
            v |= (unsigned char)key_raw[i + 2];
        key_b64[o++] = B64[(v >> 18) & 63];
        key_b64[o++] = B64[(v >> 12) & 63];
        key_b64[o++] = (i + 1 < 16) ? B64[(v >> 6) & 63] : '=';
        key_b64[o++] = (i + 2 < 16) ? B64[v & 63] : '=';
    }
    key_b64[o] = '\0';
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             path && path[0] ? path : "/", host, port, key_b64);
    if (send(fd, req, (int)strlen(req), 0) < 0)
        return -4;
    return 0;
}

#if defined(_WIN32)
static unsigned __stdcall lobby_connect_worker(void *arg)
#else
static void *lobby_connect_worker(void *arg)
#endif
{
    const unsigned gen = (unsigned)(uintptr_t)arg;
    int fd = -1;
    int rc;

    rc = lobby_tcp_connect(g_lc.host, g_lc.port, &fd);
    if (rc == 0 && !g_connect_cancel && gen == g_connect_gen)
        rc = lobby_send_ws_upgrade(fd, g_lc.host, g_lc.port, g_lc.path);
    if (g_connect_cancel || gen != g_connect_gen) {
        if (fd >= 0)
            close(fd);
        g_connect_worker_fd = -1;
        g_connect_worker_rc = -1;
    } else if (rc != 0) {
        if (fd >= 0)
            close(fd);
        g_connect_worker_fd = -1;
        g_connect_worker_rc = rc;
    } else {
        g_connect_worker_fd = fd;
        g_connect_worker_rc = 0;
    }
    g_connect_worker_gen = gen;
    g_connect_worker_done = 1;
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void lobby_join_connect_worker(void)
{
#if defined(_WIN32)
    if (g_connect_thread) {
        WaitForSingleObject(g_connect_thread, INFINITE);
        CloseHandle(g_connect_thread);
        g_connect_thread = NULL;
    }
#else
    if (g_connect_thread_valid) {
        pthread_join(g_connect_thread, NULL);
        g_connect_thread_valid = 0;
    }
#endif
}

static int lobby_connect_thread_active(void)
{
#if defined(_WIN32)
    return g_connect_thread != NULL;
#else
    return g_connect_thread_valid != 0;
#endif
}

/* Join a finished worker and adopt the socket when the generation still
 * matches. Never call this while the worker is mid-DNS/connect from the UI
 * thread — that would reintroduce the hang. */
static void lobby_finish_connect_worker(void)
{
    int fd;
    int rc;
    unsigned gen;

    if (!g_connect_worker_done)
        return;
    lobby_join_connect_worker();
    fd = g_connect_worker_fd;
    rc = g_connect_worker_rc;
    gen = g_connect_worker_gen;
    g_connect_worker_fd = -1;
    g_connect_worker_done = 0;
    g_lc_tcp_connecting = 0;

    if (gen != g_connect_gen || rc != 0 || fd < 0) {
        if (fd >= 0)
            close(fd);
        return;
    }

    g_lc.fd = fd;
    set_nonblock(fd);
    lobby_store_connected_peer_ip(fd);
    if (g_lc.connected_peer_ip[0] &&
        strcmp(g_lc.connected_peer_ip, g_lc.host) != 0) {
        fprintf(stderr, "psx_lobby: WS connected peer %s (url host %s)\n",
                g_lc.connected_peer_ip, g_lc.host);
    }
    g_lc.connected = 1;
    g_lc.handshake_done = 0;
    g_lc.rx_http_len = 0;
}

static void lobby_cancel_connect_async(void)
{
    g_connect_cancel = 1;
    g_connect_gen++;
    if (g_connect_gen == 0)
        g_connect_gen = 1;
    g_lc_tcp_connecting = 0;
    /* Reap only if the worker already finished; otherwise pump() joins later. */
    if (g_connect_worker_done)
        lobby_finish_connect_worker();
}

/* Connect/disconnect memset g_lc for a clean WS seat, but must keep the
 * mount fingerprint + game pin. ae_np_connect used to set disc_fp then call
 * connect, which wiped it — create stored "" and a later soft-return verify
 * refilled the client, so leave+rejoin hit disc_mismatch (empty vs hash). */
static void lobby_client_reset_keep_identity(void)
{
    char dname[PSX_LOBBY_NAME_LEN];
    char disc_fp[65];
    char filter_game_name[PSX_LOBBY_NAME_LEN];
    char filter_game_version[PSX_LOBBY_VERSION_LEN];

    strncpy(dname, g_lc.display_name, sizeof(dname) - 1);
    dname[sizeof(dname) - 1] = '\0';
    memcpy(disc_fp, g_lc.disc_fp, sizeof(disc_fp));
    strncpy(filter_game_name, g_lc.filter_game_name, sizeof(filter_game_name) - 1);
    filter_game_name[sizeof(filter_game_name) - 1] = '\0';
    strncpy(filter_game_version, g_lc.filter_game_version,
            sizeof(filter_game_version) - 1);
    filter_game_version[sizeof(filter_game_version) - 1] = '\0';

    memset(&g_lc, 0, sizeof(g_lc));
    g_lc.fd = -1;
    strncpy(g_lc.display_name, dname, sizeof(g_lc.display_name) - 1);
    memcpy(g_lc.disc_fp, disc_fp, sizeof(g_lc.disc_fp));
    strncpy(g_lc.filter_game_name, filter_game_name,
            sizeof(g_lc.filter_game_name) - 1);
    if (filter_game_version[0]) {
        strncpy(g_lc.filter_game_version, filter_game_version,
                sizeof(g_lc.filter_game_version) - 1);
    } else {
        strncpy(g_lc.filter_game_version, PSX_GAME_VERSION,
                sizeof(g_lc.filter_game_version) - 1);
        g_lc.filter_game_version[sizeof(g_lc.filter_game_version) - 1] = '\0';
    }
    member_rtt_clear();
}

int psx_lobby_connect(const char *ws_url)
{
#if defined(_WIN32)
    uintptr_t th;
#else
    int prc;
#endif

    if (psx_lobby_connected())
        return 0;
    /* Already starting a wanted connect — UI may call connect+list every frame. */
    if (g_lc_tcp_connecting)
        return 0;

    /* One worker slot: if a cancelled connect is still in DNS/TCP, wait for it
     * before starting another (bounded by PSX_LOBBY_CONNECT_TIMEOUT_MS). */
    if (lobby_connect_thread_active()) {
        g_connect_cancel = 1;
        if (!g_connect_worker_done)
            lobby_join_connect_worker();
        if (g_connect_worker_done)
            lobby_finish_connect_worker();
    }

    g_lc.ice_rtt_suspended = 0;
    lobby_ice_rtt_close();
    lobby_rtt_close();
    lobby_list_rtt_close();
    lobby_lan_beacon_close_all();
    lobby_host_advertise_reset();
    g_list_rtt_on_next_list = 0;
    if (g_lc.fd >= 0) {
        close(g_lc.fd);
        g_lc.fd = -1;
    }
    lobby_client_reset_keep_identity();

#if defined(_WIN32)
    {
        static int wsa;
        if (!wsa) {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
            wsa = 1;
        }
    }
#endif
    if (parse_ws_url(ws_url ? ws_url : psx_lobby_default_url(), g_lc.host, sizeof(g_lc.host),
                     &g_lc.port, g_lc.path, sizeof(g_lc.path)) != 0) {
        return -1;
    }

    g_connect_cancel = 0;
    g_connect_worker_done = 0;
    g_connect_worker_rc = -1;
    g_connect_worker_fd = -1;
    g_connect_gen++;
    if (g_connect_gen == 0)
        g_connect_gen = 1;
    g_lc_tcp_connecting = 1;

#if defined(_WIN32)
    th = _beginthreadex(NULL, 0, lobby_connect_worker,
                        (void *)(uintptr_t)g_connect_gen, 0, NULL);
    if (!th) {
        g_lc_tcp_connecting = 0;
        return -3;
    }
    g_connect_thread = (HANDLE)th;
#else
    prc = pthread_create(&g_connect_thread, NULL, lobby_connect_worker,
                         (void *)(uintptr_t)g_connect_gen);
    if (prc != 0) {
        g_lc_tcp_connecting = 0;
        return -3;
    }
    g_connect_thread_valid = 1;
#endif
    return 0;
}

void psx_lobby_disconnect(void)
{
    /* Never block the UI on DNS/connect — cancel and let pump reap. */
    lobby_cancel_connect_async();
    automatch_on_connection_reset();

    g_lc.ice_rtt_suspended = 0;
    lobby_ice_rtt_close();
    lobby_rtt_close();
    lobby_list_rtt_close();
    lobby_lan_beacon_close_all();
    lobby_host_advertise_reset();
    g_list_rtt_on_next_list = 0;
    if (g_lc.fd >= 0) {
        close(g_lc.fd);
    }
    lobby_client_reset_keep_identity();
}

int psx_lobby_connected(void)
{
    return g_lc.connected && g_lc.fd >= 0;
}

int psx_lobby_connecting(void)
{
    return g_lc_tcp_connecting || lobby_connect_thread_active() || g_connect_worker_done;
}

void psx_lobby_set_display_name(const char *name)
{
    char prev[PSX_LOBBY_NAME_LEN];
    if (!name) {
        return;
    }
    snprintf(prev, sizeof(prev), "%s", g_lc.display_name);
    strncpy(g_lc.display_name, name, sizeof(g_lc.display_name) - 1);
    g_lc.display_name[sizeof(g_lc.display_name) - 1] = '\0';
    /* A rename after connect has to reach the server, or the players-online
     * list and our seat keep the name from the first hello until the next
     * reconnect. `hello` IS the identity message and the server takes it at
     * any time, so re-sending it is the whole rename protocol. */
    if (psx_lobby_connected() && strcmp(prev, g_lc.display_name) != 0)
        queue_hello();
}

const char *psx_lobby_display_name(void)
{
    return g_lc.display_name;
}

const char *psx_lobby_player_id(void)
{
    return g_lc.player_id;
}

static int first_guest_member_slot(void)
{
    int i;
    for (i = 0; i < g_lc.member_count; ++i) {
        if (!psx_lobby_member_is_host(&g_lc.members[i]))
            return g_lc.members[i].slot;
    }
    return -1;
}

/* UDP peer RTT for the waiting-room latency column (not WS signal RTT). */
static void lobby_rtt_ensure(void)
{
    const char *bind;
    const char *peer;

    if (!g_lc.in_lobby || g_lc.launch_pending || g_lc.ice_rtt_suspended ||
        using_server_input_relay(&g_lc.join)) {
        lobby_rtt_close();
        return;
    }

    if (!g_rtt_probe) {
        bind = g_lc.my_bind[0] ? g_lc.my_bind : NULL;
        /* 3+ guests use ephemeral session binds; probe the same way. */
        if (!g_lc.is_host && g_lc.join.max_slots >= 3)
            bind = NULL;
        if (rnet_rtt_probe_open(&g_rtt_probe, bind) != 0)
            return;
    }

    peer = NULL;
    if (g_lc.join.peer_hostport[0] && !endpoint_port_is_zero(g_lc.join.peer_hostport))
        peer = g_lc.join.peer_hostport;
    else if (!g_lc.is_host && g_lc.join.host_endpoint[0] &&
             !endpoint_port_is_zero(g_lc.join.host_endpoint))
        peer = g_lc.join.host_endpoint;
    if (peer)
        (void)rnet_rtt_probe_set_peer(g_rtt_probe, peer);
}

static void lobby_rtt_tick(void)
{
    int ms = 0;
    int got;

    if (!g_lc.in_lobby || g_lc.launch_pending || g_lc.ice_rtt_suspended) {
        lobby_rtt_close();
        return;
    }
    if (using_server_input_relay(&g_lc.join)) {
        lobby_rtt_close();
        return;
    }

    lobby_rtt_ensure();
    if (!g_rtt_probe)
        return;

    got = rnet_rtt_probe_pump(g_rtt_probe, &ms);
    if (got == 1) {
        char peer_id[PSX_LOBBY_ID_LEN];
        if (lobby_ice_rtt_peer_id(peer_id, sizeof(peer_id)) == 0)
            lobby_rtt_store_for_peer(peer_id, ms);
        if (!g_lc.is_host) {
            char report[32];
            snprintf(report, sizeof(report), "%d", ms);
            (void)psx_lobby_send_signal(PSX_LOBBY_SIG_RTT_REPORT, 0, report);
        }
    }

    {
        uint64_t now = lobby_mono_ms();
        if (now >= g_lc.rtt_next_ping_ms && rnet_rtt_probe_peer_known(g_rtt_probe)) {
            (void)rnet_rtt_probe_ping(g_rtt_probe);
            g_lc.rtt_next_ping_ms = now + 2500ull;
        }
    }
}

static void lobby_ice_rtt_close(void)
{
    rnet_ice_rtt_close(&g_ice_rtt);
    g_ice_rtt_peer_id[0] = '\0';
    g_ice_rtt_force_relay = 0;
    g_ice_rtt_last_log_ms = 0;
    g_ice_path_reported[0] = '\0';
    g_ice_path_report_ms = 0;
    /* Keep accept=1 through launch / match so match ICE can queue offers.
     * Waiting-room resume clears accept until the probe restarts. */
    if (!g_lc.launch_pending && !g_lc.ice_rtt_suspended)
        g_lc.ice_signal_accept = 0;
}

/* Store RTT under the remote seat (not self). Keep the pessimistic sample:
 * guest REPORT can undercut a higher host ICE measure and starve delay. */
static void lobby_rtt_store_for_peer(const char *peer_id, int ms)
{
    int slot;
    if (ms < 0 || !peer_id || !peer_id[0])
        return;
    slot = rtt_index_for_slot(member_slot_for_player(peer_id));
    if (slot < 0)
        return;
    if (g_lc.member_rtt_ms[slot] < 0 || ms > g_lc.member_rtt_ms[slot])
        g_lc.member_rtt_ms[slot] = ms;
}

static void lobby_ice_rtt_emit(const RNetSignal *msg, void *user)
{
    (void)user;
    if (!msg)
        return;
    (void)psx_lobby_send_signal((int)msg->type, (int)msg->flag, msg->text);
}

static int lobby_ice_rtt_peer_id(char *out, size_t cap)
{
    int i;
    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    for (i = 0; i < g_lc.member_count; ++i) {
        if (!g_lc.members[i].player_id[0])
            continue;
        if (g_lc.player_id[0] &&
            strcmp(g_lc.members[i].player_id, g_lc.player_id) == 0)
            continue;
        strncpy(out, g_lc.members[i].player_id, cap - 1);
        out[cap - 1] = '\0';
        return 0;
    }
    return -1;
}

static void lobby_ice_rtt_apply_ms(int ms)
{
    static int s_last_report = -1;
    if (ms < 0)
        return;
    /* Attribute to the probed remote seat (host from guest, guest from host). */
    if (g_ice_rtt_peer_id[0])
        lobby_rtt_store_for_peer(g_ice_rtt_peer_id, ms);
    if (!g_lc.is_host && ms != s_last_report) {
        char report[32];
        snprintf(report, sizeof(report), "%d", ms);
        (void)psx_lobby_send_signal(PSX_LOBBY_SIG_RTT_REPORT, 0, report);
        s_last_report = ms;
    }
}

/* ICE/TURN waiting-room RTT — works on CGNAT when direct UDP probe cannot. */
static void lobby_ice_rtt_tick(void)
{
    char peer_id[PSX_LOBBY_ID_LEN];
    int want_force_relay;
    int type = 0, flag = 0;
    char text[2048];
    int ms;
    RNetIceState st;
    char path[16];

    if (!g_lc.in_lobby || g_lc.launch_pending || g_lc.ice_rtt_suspended ||
        g_lc.member_count < 2) {
        lobby_ice_rtt_close();
        return;
    }

#if !defined(RNET_ENABLE_ICE)
    return;
#else
    if (!g_lc.turn.valid) {
        if (!g_lc.turn_request_pending)
            (void)queue_turn_credentials_request();
        return;
    }

    want_force_relay = (g_lc.match_caps.valid && g_lc.match_caps.force_turn) ? 1 : 0;
    if (lobby_ice_rtt_peer_id(peer_id, sizeof(peer_id)) != 0) {
        lobby_ice_rtt_close();
        return;
    }

    /* Queue peer SDP/candidates even before the local agent exists. */
    g_lc.ice_signal_accept = 1;

    if (g_ice_rtt &&
        (strcmp(peer_id, g_ice_rtt_peer_id) != 0 ||
         want_force_relay != g_ice_rtt_force_relay)) {
        lobby_ice_rtt_close();
        psx_lobby_clear_signals();
        g_lc.ice_signal_accept = 1;
    }

    if (!g_ice_rtt) {
        RNetIceConfig ice;
        RNetIpv4Address addrs[8];
        int naddr;
        char bind_addr[64];

        rnet_ice_config_init_defaults(&ice);
        ice.controlling = g_lc.is_host ? 1u : 0u;
        ice.force_relay = want_force_relay ? 1u : 0u;
        if (g_lc.turn.stun_host[0]) {
            ice.stun_host = g_lc.turn.stun_host;
            ice.stun_port = (rnet_u16)(g_lc.turn.stun_port > 0 ? g_lc.turn.stun_port
                                                               : 3478);
        }
        if (g_lc.turn.turn_host[0] && g_lc.turn.username[0] &&
            g_lc.turn.password[0]) {
            ice.turn_host = g_lc.turn.turn_host;
            ice.turn_user = g_lc.turn.username;
            ice.turn_pass = g_lc.turn.password;
            ice.turn_port = (rnet_u16)(g_lc.turn.turn_port > 0 ? g_lc.turn.turn_port
                                                               : 3478);
        } else if (want_force_relay) {
            /* Force TURN without creds — wait for mint. */
            return;
        }
        bind_addr[0] = '\0';
        naddr = rnet_ipv4_enumerate(addrs, (int)(sizeof(addrs) / sizeof(addrs[0])));
        if (naddr > 0 && addrs[0].address[0]) {
            snprintf(bind_addr, sizeof(bind_addr), "%s", addrs[0].address);
            ice.bind_address = bind_addr;
        }
        /* Ephemeral UDP — do not steal the game bind used by STUN advertise. */
        ice.bind_port = 0;

        if (rnet_ice_rtt_open(&g_ice_rtt, &ice, lobby_ice_rtt_emit, NULL) != 0)
            return;
        strncpy(g_ice_rtt_peer_id, peer_id, sizeof(g_ice_rtt_peer_id) - 1);
        g_ice_rtt_peer_id[sizeof(g_ice_rtt_peer_id) - 1] = '\0';
        g_ice_rtt_force_relay = want_force_relay;
        fprintf(stderr,
                "psx_lobby: ICE RTT probe start (controlling=%d force_relay=%d)\n",
                g_lc.is_host ? 1 : 0, want_force_relay);
        fflush(stderr);
    }

    while (psx_lobby_poll_signal(&type, &flag, text, sizeof(text))) {
        RNetSignal sig;
        memset(&sig, 0, sizeof(sig));
        if (type == (int)RNET_SIGNAL_LOCAL_SDP)
            type = (int)RNET_SIGNAL_REMOTE_SDP;
        else if (type == (int)RNET_SIGNAL_LOCAL_CANDIDATE)
            type = (int)RNET_SIGNAL_REMOTE_CANDIDATE;
        sig.type = (RNetSignalType)type;
        sig.flag = (rnet_u8)(flag & 0xFF);
        strncpy(sig.text, text, sizeof(sig.text) - 1);
        rnet_ice_rtt_push_signal(g_ice_rtt, &sig);
    }

    rnet_ice_rtt_pump(g_ice_rtt);
    ms = rnet_ice_rtt_ms(g_ice_rtt);
    if (ms >= 0)
        lobby_ice_rtt_apply_ms(ms);

    st = rnet_ice_rtt_state(g_ice_rtt);
    rnet_ice_rtt_selected_path(g_ice_rtt, path, sizeof(path));
    {
        uint64_t now = lobby_mono_ms();
        const char *report = NULL;
        if (now - g_ice_rtt_last_log_ms >= 3000ull) {
            g_ice_rtt_last_log_ms = now;
            fprintf(stderr,
                    "psx_lobby: ICE RTT state=%s path=%s rtt=%d\n",
                    rnet_ice_state_name(st), path, ms);
            fflush(stderr);
        }
        /* §108: path_report is telemetry only — server always picks SFU. */
        if (strcmp(path, "host") == 0 || strcmp(path, "srflx") == 0 ||
            strcmp(path, "prflx") == 0)
            report = "direct";
        else if (strcmp(path, "relay") == 0)
            report = "relay";
        else if (strcmp(path, "failed") == 0)
            report = "fail";
        if (report && g_lc.member_count == 2 &&
            (strcmp(report, g_ice_path_reported) != 0 ||
             now - g_ice_path_report_ms >= 10000ull)) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "{\"op\":\"path_report\",\"path\":\"%s\"}", report);
            queue_send(msg);
            strncpy(g_ice_path_reported, report, sizeof(g_ice_path_reported) - 1);
            g_ice_path_reported[sizeof(g_ice_path_reported) - 1] = '\0';
            g_ice_path_report_ms = now ? now : 1ull;
            fprintf(stderr, "psx_lobby: path_report %s (telemetry)\n", report);
            fflush(stderr);
        }
    }
#endif /* RNET_ENABLE_ICE */
}

void psx_lobby_pump(void)
{
    char buf[4096];
    automatch_probe_poll();
#if defined(_WIN32)
    int n;
#else
    ssize_t n;
#endif
    if (g_connect_worker_done)
        lobby_finish_connect_worker();
    if (!psx_lobby_connected()) {
        return;
    }
    if (!g_lc.handshake_done) {
        n = recv(g_lc.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (socket_would_block()) {
                return;
            }
            psx_lobby_disconnect();
            return;
        }
        if (n == 0) {
            psx_lobby_disconnect();
            return;
        }
        if (g_lc.rx_http_len + (size_t)n >= sizeof(g_lc.rx_http)) {
            psx_lobby_disconnect();
            return;
        }
        memcpy(g_lc.rx_http + g_lc.rx_http_len, buf, (size_t)n);
        g_lc.rx_http_len += (size_t)n;
        g_lc.rx_http[g_lc.rx_http_len] = '\0';
        {
            char *hdr_end = strstr(g_lc.rx_http, "\r\n\r\n");
            if (hdr_end) {
                size_t hdr_len;
                size_t leftover;
                if (!strstr(g_lc.rx_http, "101")) {
                    psx_lobby_disconnect();
                    return;
                }
                hdr_len = (size_t)(hdr_end - g_lc.rx_http) + 4;
                leftover = g_lc.rx_http_len > hdr_len ? g_lc.rx_http_len - hdr_len : 0;
                g_lc.handshake_done = 1;
                g_lc.ws_pending_len = 0;
                if (leftover > 0 && leftover <= sizeof(g_lc.ws_pending)) {
                    memcpy(g_lc.ws_pending, g_lc.rx_http + hdr_len, leftover);
                    g_lc.ws_pending_len = leftover;
                }
                g_lc.rx_http_len = 0;
                flush_pending();
                drain_ws_pending();
            }
        }
        return;
    }
    flush_pending();
    drain_ws_pending();
    /* Non-blocking recv into ws_pending + frame parse. Avoid MSG_PEEK /
     * MSG_WAITALL / temporary blocking — those break on Windows MinGW when
     * the socket stays O_NONBLOCK (list/create never see welcome/created). */
    for (;;) {
        size_t available = sizeof(g_lc.ws_pending) - g_lc.ws_pending_len;
        if (available == 0) {
            psx_lobby_disconnect();
            return;
        }
        n = recv(g_lc.fd,
                 (char *)g_lc.ws_pending + g_lc.ws_pending_len,
                 (int)available, 0);
        if (n < 0) {
            if (socket_would_block()) {
                break;
            }
            psx_lobby_disconnect();
            return;
        }
        if (n == 0) {
            psx_lobby_disconnect();
            return;
        }
        g_lc.ws_pending_len += (size_t)n;
        drain_ws_pending();
        if (!psx_lobby_connected()) {
            break;
        }
    }
    /* Host STUN advertise (may briefly close the waiting-room RTT sock). */
    lobby_host_advertise_tick();
    /* Local UDP broadcast: host announce / guest cache for list RTT. */
    lobby_lan_beacon_tick();
    /* Peer-path UDP latency for the lobby seat table. */
    lobby_rtt_tick();
    /* ICE/TURN data-channel RTT (CGNAT / Force TURN); overrides UDP when ready. */
    lobby_ice_rtt_tick();
    /* One-shot list latency after Refresh / lobby_list. */
    lobby_list_rtt_tick();
}

void psx_lobby_set_game_identity(const char *game_name, const char *game_version)
{
    if (game_name) {
        strncpy(g_lc.filter_game_name, game_name, sizeof(g_lc.filter_game_name) - 1);
        g_lc.filter_game_name[sizeof(g_lc.filter_game_name) - 1] = '\0';
    } else {
        g_lc.filter_game_name[0] = '\0';
    }
    if (game_version && game_version[0]) {
        strncpy(g_lc.filter_game_version, game_version, sizeof(g_lc.filter_game_version) - 1);
        g_lc.filter_game_version[sizeof(g_lc.filter_game_version) - 1] = '\0';
    } else {
        strncpy(g_lc.filter_game_version, PSX_GAME_VERSION,
                sizeof(g_lc.filter_game_version) - 1);
        g_lc.filter_game_version[sizeof(g_lc.filter_game_version) - 1] = '\0';
    }
}

const char *psx_lobby_game_version(void)
{
    return effective_game_version(NULL);
}

void psx_lobby_set_disc_fp(const char *disc_fp)
{
    size_t i;
    g_lc.disc_fp[0] = '\0';
    if (!disc_fp || !disc_fp[0]) return;
    /* Accept only 64 lowercase/uppercase hex chars; normalize to lower. */
    for (i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)disc_fp[i];
        if (!isxdigit(c)) return;
        g_lc.disc_fp[i] = (char)tolower(c);
    }
    if (disc_fp[64] != '\0') {
        g_lc.disc_fp[0] = '\0';
        return;
    }
    g_lc.disc_fp[64] = '\0';
}

const char *psx_lobby_disc_fp(void)
{
    return g_lc.disc_fp;
}

void psx_lobby_request_list(void)
{
    g_list_rtt_on_next_list = 1;
    queue_list_request();
    flush_pending();
}

int psx_lobby_list_count(void)
{
    return g_lc.list_count;
}

int psx_lobby_list_get(int index, PsxLobbyRow *out)
{
    if (!out || index < 0 || index >= g_lc.list_count) {
        return 0;
    }
    *out = g_lc.list[index];
    return 1;
}

int psx_lobby_create(const char *name, const char *game_name, const char *game_version,
                     const char *password, const char *host_bind,
                     const PsxLobbyMatchCaps *match_caps)
{
    char msg[2304];
    char caps_json[512];
    char name_esc[JSON_ESC_CAP(128)];
    char gn_esc[JSON_ESC_CAP(PSX_LOBBY_NAME_LEN)];
    char gv_esc[JSON_ESC_CAP(PSX_LOBBY_VERSION_LEN)];
    char pw_esc[JSON_ESC_CAP(128)];
    char bind_esc[JSON_ESC_CAP(PSX_LOBBY_ENDPOINT_LEN)];
    char dn_esc[JSON_ESC_CAP(PSX_LOBBY_NAME_LEN)];
    char fp_esc[JSON_ESC_CAP(72)];
    const char *gn;
    const char *gv;
    int n;
    if (!psx_lobby_connected()) {
        return -1;
    }
    gn = game_name && game_name[0] ? game_name
         : (g_lc.filter_game_name[0] ? g_lc.filter_game_name : "Game");
    gv = effective_game_version(game_version);
    if (game_name && game_name[0]) {
        psx_lobby_set_game_identity(game_name, gv);
    }
    strncpy(g_lc.my_bind, host_bind && host_bind[0] ? host_bind : "0.0.0.0:7777",
            sizeof(g_lc.my_bind) - 1);
    g_lc.join.last_error[0] = '\0';
    caps_json[0] = '\0';
    if (match_caps && match_caps->valid) {
        g_lc.match_caps = *match_caps;
        append_match_caps_json(caps_json, sizeof(caps_json), match_caps);
    }
    json_escape(name && name[0] ? name : "Lobby", name_esc, sizeof(name_esc));
    json_escape(gn, gn_esc, sizeof(gn_esc));
    json_escape(gv, gv_esc, sizeof(gv_esc));
    json_escape(password ? password : "", pw_esc, sizeof(pw_esc));
    json_escape(g_lc.my_bind, bind_esc, sizeof(bind_esc));
    json_escape(g_lc.display_name[0] ? g_lc.display_name : "Host",
                dn_esc, sizeof(dn_esc));
    json_escape(g_lc.disc_fp, fp_esc, sizeof(fp_esc));
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"create\",\"name\":\"%s\",\"game_name\":\"%s\",\"game_version\":\"%s\","
                 "\"password\":\"%s\",\"max_slots\":%d,\"allow_spectators\":%s,"
                 "\"host_bind\":\"%s\",\"display_name\":\"%s\","
                 "\"disc_fp\":\"%s\"%s}",
                 name_esc, gn_esc, gv_esc,
                 pw_esc, g_lobby_max_slots,
                 g_allow_spectators_pref ? "true" : "false", bind_esc,
                 dn_esc, fp_esc, caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_join(const char *lobby_id, const char *password, const char *guest_bind)
{
    char msg[1536];
    char lid_esc[JSON_ESC_CAP(PSX_LOBBY_ID_LEN)];
    char pw_esc[JSON_ESC_CAP(128)];
    char bind_esc[JSON_ESC_CAP(PSX_LOBBY_ENDPOINT_LEN)];
    char dn_esc[JSON_ESC_CAP(PSX_LOBBY_NAME_LEN)];
    char gn_esc[JSON_ESC_CAP(PSX_LOBBY_NAME_LEN)];
    char gv_esc[JSON_ESC_CAP(PSX_LOBBY_VERSION_LEN)];
    char fp_esc[JSON_ESC_CAP(72)];
    const char *gn;
    const char *gv;
    int n;
    if (!psx_lobby_connected() || !lobby_id) {
        return -1;
    }
    gn = g_lc.filter_game_name;
    gv = effective_game_version(NULL);
    strncpy(g_lc.my_bind, guest_bind && guest_bind[0] ? guest_bind : "0.0.0.0:7778",
            sizeof(g_lc.my_bind) - 1);
    g_lc.join.last_error[0] = '\0';
    json_escape(lobby_id, lid_esc, sizeof(lid_esc));
    json_escape(password ? password : "", pw_esc, sizeof(pw_esc));
    json_escape(g_lc.my_bind, bind_esc, sizeof(bind_esc));
    json_escape(g_lc.display_name[0] ? g_lc.display_name : "Guest",
                dn_esc, sizeof(dn_esc));
    json_escape(gn, gn_esc, sizeof(gn_esc));
    json_escape(gv, gv_esc, sizeof(gv_esc));
    json_escape(g_lc.disc_fp, fp_esc, sizeof(fp_esc));
    n = snprintf(msg, sizeof(msg),
             "{\"op\":\"join\",\"lobby_id\":\"%s\",\"password\":\"%s\",\"guest_bind\":\"%s\","
             "\"display_name\":\"%s\",\"game_name\":\"%s\",\"game_version\":\"%s\","
             "\"disc_fp\":\"%s\"}",
             lid_esc, pw_esc, bind_esc, dn_esc, gn_esc, gv_esc, fp_esc);
    /* A truncated frame is a frame the server drops whole, so the join would
     * silently never happen. create checked this; join did not. */
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_leave(void)
{
    queue_send("{\"op\":\"leave\"}");
    flush_pending();
    g_lc.ice_rtt_suspended = 0;
    lobby_ice_rtt_close();
    lobby_rtt_close();
    lobby_host_advertise_reset();
    g_lc.in_lobby = 0;
    g_lc.is_host = 0;
    g_lc.host_player_id[0] = '\0';
    g_lc.member_count = 0;
    g_lc.local_ready = 0;
    g_lc.all_ready = 0;
    g_lc.launch_pending = 0;
    g_lc.ice_signal_accept = 0;
    psx_lobby_clear_signals();
    match_caps_clear(&g_lc.match_caps);
    member_rtt_clear();
    return 0;
}

void psx_lobby_set_allow_spectators(int allow)
{
    g_allow_spectators_pref = allow ? 1 : 0;
}

int psx_lobby_allow_spectators_pref(void)
{
    return g_allow_spectators_pref;
}

int psx_lobby_allow_spectators(void)
{
    return g_lc.join.allow_spectators ? 1 : 0;
}

int psx_lobby_max_spectators(void)
{
    return g_lc.join.max_spectators;
}

int psx_lobby_spectator_count(void)
{
    return g_lc.join.spectator_count;
}

int psx_lobby_local_is_spectator(void)
{
    return g_lc.join.local_is_spectator ? 1 : 0;
}

int psx_lobby_spectator_slot_base(void)
{
    /* The server republishes its base in every update, and the namespace is
     * the server's to define. The compiled-in value is only a fallback for an
     * update that arrives without it, so `move` stays addressable. */
    return g_lc.join.spectator_slot_base > 0 ? g_lc.join.spectator_slot_base
                                             : PSX_LOBBY_SPECTATOR_SLOT_BASE;
}

int psx_lobby_spectator_slot(int index)
{
    if (index < 0 || index >= PSX_LOBBY_MAX_SPECTATORS) return -1;
    return psx_lobby_spectator_slot_base() + index;
}

int psx_lobby_local_wire_slot(void)
{
    int gallery_index;
    if (!g_lc.join.local_is_spectator) return -1;
    if (g_lc.join.spectator_relay_base <= 0) return -1;
    gallery_index = g_lc.join.local_slot - psx_lobby_spectator_slot_base();
    if (gallery_index < 0 || gallery_index >= PSX_LOBBY_MAX_SPECTATORS)
        return -1;
    return g_lc.join.spectator_relay_base + gallery_index;
}

int psx_lobby_seat_valid(int slot)
{
    const int base = psx_lobby_spectator_slot_base();
    if (slot < 0) return 0;
    if (slot < PSX_LOBBY_MAX_PLAYERS) return 1;
    return slot >= base && slot < base + PSX_LOBBY_MAX_SPECTATORS;
}

int psx_lobby_kick(int slot)
{
    char msg[64];
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host) {
        return -1;
    }
    if (!psx_lobby_seat_valid(slot)) {
        return -1;
    }
    snprintf(msg, sizeof(msg), "{\"op\":\"kick\",\"slot\":%d}", slot);
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_move_member(int from_slot, int to_slot)
{
    char msg[96];
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host) {
        return -1;
    }
    /* Either seat may be in the gallery: this is the call that promotes and
     * demotes, and it is the same call that reorders within one table. */
    if (!psx_lobby_seat_valid(from_slot) || !psx_lobby_seat_valid(to_slot) ||
        from_slot == to_slot) {
        return -1;
    }
    snprintf(msg, sizeof(msg),
             "{\"op\":\"move\",\"from_slot\":%d,\"to_slot\":%d}",
             from_slot, to_slot);
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_in_lobby(void)
{
    return g_lc.in_lobby;
}

int psx_lobby_is_host(void)
{
    return g_lc.is_host;
}

const char *psx_lobby_host_player_id(void)
{
    return g_lc.host_player_id;
}

const PsxLobbyJoinInfo *psx_lobby_join_info(void)
{
    return &g_lc.join;
}

const PsxLobbyMatchCaps *psx_lobby_match_caps(void)
{
    return &g_lc.match_caps;
}

int psx_lobby_set_match_caps(const PsxLobbyMatchCaps *caps)
{
    char msg[896];
    char caps_json[640];
    int n;
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host || !caps || !caps->valid)
        return -1;
    g_lc.match_caps = *caps;
    caps_json[0] = '\0';
    append_match_caps_json(caps_json, sizeof(caps_json), caps);
    /* caps_json begins with a comma — strip it for a standalone object field. */
    n = snprintf(msg, sizeof(msg), "{\"op\":\"set_match_caps\"%s}", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_member_count(void)
{
    return g_lc.member_count;
}

int psx_lobby_member_get(int index, PsxLobbyMember *out)
{
    if (!out || index < 0 || index >= g_lc.member_count) {
        return 0;
    }
    *out = g_lc.members[index];
    return 1;
}

int psx_lobby_member_latency_ms(int slot)
{
    int local, idx;
    idx = rtt_index_for_slot(slot);
    if (idx < 0)
        return -1;
    local = local_member_slot();
    if (local >= 0 && slot == local)
        return -1; /* never show self-RTT */
    return g_lc.member_rtt_ms[idx];
}

int psx_lobby_member_is_host(const PsxLobbyMember *member)
{
    const char *host_id;
    if (!member || !member->player_id[0])
        return 0;
    host_id = psx_lobby_host_player_id();
    return host_id && host_id[0] && strcmp(member->player_id, host_id) == 0;
}

int psx_lobby_local_ready(void)
{
    return g_lc.local_ready;
}

int psx_lobby_all_ready(void)
{
    return g_lc.all_ready != 0 && g_lc.in_lobby && g_lc.join.player_count >= 2;
}

void psx_lobby_set_bios_offer(const PsxLobbyBiosOffer *offer)
{
    if (!offer) {
        memset(&g_lc.bios_offer, 0, sizeof(g_lc.bios_offer));
        return;
    }
    g_lc.bios_offer = *offer;
    if (g_lc.bios_offer.valid) {
        /* OpenBIOS is always expected when the title allows it; keep the flag. */
        if (!g_lc.bios_offer.can_openbios && !g_lc.bios_offer.can_scph1001)
            g_lc.bios_offer.can_openbios = 1;
    }
}

const PsxLobbyBiosOffer *psx_lobby_bios_offer(void)
{
    return &g_lc.bios_offer;
}

void psx_lobby_set_memcard_offer(const PsxLobbyMemcardOffer *offer)
{
    if (!offer) {
        memset(&g_lc.memcard_offer, 0, sizeof(g_lc.memcard_offer));
        return;
    }
    g_lc.memcard_offer = *offer;
}

const PsxLobbyMemcardOffer *psx_lobby_memcard_offer(void)
{
    return &g_lc.memcard_offer;
}

int psx_lobby_seat_move_self(int to_slot)
{
    char msg[80];
    if (!psx_lobby_connected() || !g_lc.in_lobby) return -1;
    /* A player seat or a gallery seat: the server takes either, empty only. */
    if (!psx_lobby_seat_valid(to_slot)) return -1;
    snprintf(msg, sizeof(msg), "{\"op\":\"seat_move\",\"to_slot\":%d}", to_slot);
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_seat_swap_request(int target_slot)
{
    char msg[96];
    if (!psx_lobby_connected() || !g_lc.in_lobby) return -1;
    /* A player seat or a gallery seat: the occupant of either can be asked. */
    if (!psx_lobby_seat_valid(target_slot)) return -1;
    if (g_lc.swap_out == 1) return -1; /* one ask at a time */
    snprintf(msg, sizeof(msg),
             "{\"op\":\"seat_swap_request\",\"target_slot\":%d}", target_slot);
    queue_send(msg);
    flush_pending();
    g_lc.swap_out = 1;
    return 0;
}

int psx_lobby_seat_swap_incoming(char *who, size_t who_cap, int *from_slot)
{
    if (!g_lc.swap_in_valid) return 0;
    if (who && who_cap) snprintf(who, who_cap, "%s", g_lc.swap_in_asker_name);
    if (from_slot) *from_slot = g_lc.swap_in_from_slot;
    return 1;
}

int psx_lobby_seat_swap_respond(int accept)
{
    char msg[160];
    if (!g_lc.swap_in_valid) return -1;
    g_lc.swap_in_valid = 0;
    if (!psx_lobby_connected() || !g_lc.in_lobby) return -1;
    {
        char asker_esc[JSON_ESC_CAP(PSX_LOBBY_ID_LEN)];
        json_escape(g_lc.swap_in_asker_id, asker_esc, sizeof(asker_esc));
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"seat_swap_answer\",\"accept\":%s,"
                 "\"asker_player_id\":\"%s\"}",
                 accept ? "true" : "false", asker_esc);
    }
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_seat_swap_outgoing(void)
{
    return g_lc.swap_out;
}

void psx_lobby_seat_swap_clear(void)
{
    if (g_lc.swap_out != 1) g_lc.swap_out = 0;
}

int psx_lobby_report_chat(const char *const *mids, int mid_count,
                          const char *reason, const char *note)
{
    /* Thin on purpose, and identical in shape to the SNES copy. What a report
     * CONTAINS lives in recomp-net (recomp_net/chat_report.h) so there is one
     * implementation rather than one per console; this says where we are and
     * hands the frame to the socket. */
    RNetChatReportMeta meta;
    char msg[2048];
    size_t n;

    if (!psx_lobby_connected())
        return -1;

    memset(&meta, 0, sizeof(meta));
    meta.game = g_lc.filter_game_name;
    meta.game_version = psx_lobby_game_version();
    /* Metadata only. Nothing downstream may name a file or a directory after
     * it -- one moderation queue spans every title, and splitting the evidence
     * by console would fragment it along a line that has nothing to do with
     * moderation. */
    meta.platform = "psx";
    /* The host this client connected to. PSX keeps the parsed host rather
     * than the whole URL, which is the part that identifies a deployment and
     * the part a moderator needs. */
    meta.server = g_lc.host;
    meta.lobby = g_lc.join.lobby_id[0] ? g_lc.join.lobby_id : "";
    meta.scope = g_lc.in_lobby ? "lobby" : "server";

    n = rnet_chat_report_build(msg, sizeof(msg), mids, mid_count,
                               reason, note, &meta);
    if (n == 0)
        return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_send_chat(const char *text)
{
    char esc[PSX_LOBBY_CHAT_TEXT_LEN * 2 + 8];
    char msg[PSX_LOBBY_CHAT_TEXT_LEN * 2 + 64];
    int n;
    if (!psx_lobby_connected() || !g_lc.in_lobby) return -1;
    if (!text || !text[0]) return -1;
    json_escape(text, esc, sizeof(esc));
    n = snprintf(msg, sizeof(msg), "{\"op\":\"chat\",\"text\":\"%s\"}", esc);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_chat_count(void)
{
    return g_lc.chat_count;
}

int psx_lobby_send_server_chat(const char *text)
{
    char esc[PSX_LOBBY_CHAT_TEXT_LEN * 2 + 8];
    char game_esc[JSON_ESC_CAP(PSX_LOBBY_NAME_LEN)];
    char msg[PSX_LOBBY_CHAT_TEXT_LEN * 2 + 256];
    int n;
    if (!psx_lobby_connected()) return -1;
    if (!text || !text[0]) return -1;
    json_escape(text, esc, sizeof(esc));
    /* Carry the title on the line itself: the server scopes by it, and this
     * works even against a server that has not seen our `list` yet. */
    json_escape(g_lc.filter_game_name, game_esc, sizeof(game_esc));
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"server_chat\",\"game_name\":\"%s\",\"text\":\"%s\"}",
                 game_esc, esc);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_server_chat_count(void)
{
    return g_lc.schat_count;
}

int psx_lobby_server_chat_get(int index, PsxLobbyChatMsg *out)
{
    if (!out || index < 0 || index >= g_lc.schat_count) return 0;
    *out = g_lc.schat[(g_lc.schat_head + index) % PSX_LOBBY_CHAT_RING];
    return 1;
}

int psx_lobby_chat_get(int index, PsxLobbyChatMsg *out)
{
    if (!out || index < 0 || index >= g_lc.chat_count) return 0;
    *out = g_lc.chat[(g_lc.chat_head + index) % PSX_LOBBY_CHAT_RING];
    return 1;
}

int psx_lobby_settle_session_bios(char *out, size_t out_cap)
{
    int i;
    int any_prefer_open = 0;
    int any_cannot_scph = 0;
    int host_prefer_scph = 0;
    int saw_peer = 0;
    if (!out || out_cap < 9) return -1;
    out[0] = '\0';

    for (i = 0; i < g_lc.member_count; ++i) {
        const PsxLobbyMember *m = &g_lc.members[i];
        if (!m->player_id[0] && !m->display_name[0]) continue;
        saw_peer = 1;
        if (!m->bios_offer_valid) {
            /* Legacy client / not ready yet — cannot assume SCPH. */
            any_cannot_scph = 1;
            continue;
        }
        if (m->bios_prefer_openbios) any_prefer_open = 1;
        if (!m->bios_can_scph1001) any_cannot_scph = 1;
        if (!m->bios_can_openbios && !m->bios_can_scph1001)
            any_cannot_scph = 1;
        if (psx_lobby_member_is_host(m) && !m->bios_prefer_openbios &&
            m->bios_can_scph1001)
            host_prefer_scph = 1;
    }

    /* Include local offer even before lobby_update echoes it. */
    if (g_lc.bios_offer.valid) {
        saw_peer = 1;
        if (g_lc.bios_offer.prefer_openbios) any_prefer_open = 1;
        if (!g_lc.bios_offer.can_scph1001) any_cannot_scph = 1;
        if (g_lc.is_host && !g_lc.bios_offer.prefer_openbios &&
            g_lc.bios_offer.can_scph1001)
            host_prefer_scph = 1;
    } else if (!saw_peer) {
        any_cannot_scph = 1;
    }

    /* Capability first: without universal SCPH support, OpenBIOS is required.
     * Otherwise the host's retail pick wins over guest OpenBIOS preferences. */
    if (any_cannot_scph || !saw_peer)
        strncpy(out, "openbios", out_cap - 1);
    else if (host_prefer_scph)
        strncpy(out, "scph1001", out_cap - 1);
    else if (any_prefer_open)
        strncpy(out, "openbios", out_cap - 1);
    else
        strncpy(out, "scph1001", out_cap - 1);
    out[out_cap - 1] = '\0';
    return 0;
}

int psx_lobby_set_ready(int ready)
{
    char msg[512];
    char memcard[96];
    int n;
    if (!psx_lobby_connected() || !g_lc.in_lobby) {
        return -1;
    }
    memcard[0] = '\0';
    if (g_lc.memcard_offer.valid) {
        snprintf(memcard, sizeof(memcard),
                 ",\"memcard_offer\":{\"v\":1,\"has_card\":%s,\"share\":%s}",
                 g_lc.memcard_offer.has_card ? "true" : "false",
                 g_lc.memcard_offer.share ? "true" : "false");
    }
    if (g_lc.bios_offer.valid) {
        n = snprintf(msg, sizeof(msg),
                     "{\"op\":\"set_ready\",\"ready\":%s,"
                     "\"bios_offer\":{\"v\":1,\"prefer\":\"%s\","
                     "\"can_openbios\":%s,\"can_scph1001\":%s}%s}",
                     ready ? "true" : "false",
                     g_lc.bios_offer.prefer_openbios ? "openbios" : "scph1001",
                     g_lc.bios_offer.can_openbios ? "true" : "false",
                     g_lc.bios_offer.can_scph1001 ? "true" : "false",
                     memcard);
    } else {
        n = snprintf(msg, sizeof(msg), "{\"op\":\"set_ready\",\"ready\":%s%s}",
                     ready ? "true" : "false", memcard);
    }
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_request_start(const PsxLobbyMatchCaps *match_caps)
{
    char msg[896];
    char caps_json[640];
    PsxLobbyMatchCaps caps_local;
    int n;
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host) {
        return -1;
    }
    caps_json[0] = '\0';
    if (match_caps && match_caps->valid) {
        caps_local = *match_caps;
        if (!caps_local.session_bios[0])
            (void)psx_lobby_settle_session_bios(caps_local.session_bios,
                                                sizeof(caps_local.session_bios));
        g_lc.match_caps = caps_local;
        append_match_caps_json(caps_json, sizeof(caps_json), &caps_local);
    }
    n = snprintf(msg, sizeof(msg), "{\"op\":\"start\"%s}", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_launch_pending(void)
{
    return g_lc.launch_pending;
}

void psx_lobby_clear_launch_pending(void)
{
    g_lc.launch_pending = 0;
    /* Match may already own ICE — keep waiting-room probes dead. */
    lobby_ice_rtt_close();
    lobby_rtt_close();
}

void psx_lobby_resume_waiting_room_rtt(void)
{
    g_lc.ice_rtt_suspended = 0;
    lobby_ice_rtt_close();
    lobby_rtt_close();
}

int psx_lobby_send_signal(int type, int flag, const char *text)
{
    char esc[4096];
    char lid_esc[JSON_ESC_CAP(PSX_LOBBY_ID_LEN)];
    char msg[4608];
    const char *lid;
    if (!psx_lobby_connected() || !g_lc.in_lobby) {
        return -1;
    }
    lid = g_lc.join.lobby_id[0] ? g_lc.join.lobby_id : "";
    json_escape(text ? text : "", esc, sizeof(esc));
    json_escape(lid, lid_esc, sizeof(lid_esc));
    snprintf(msg, sizeof(msg),
             "{\"op\":\"signal\",\"lobby_id\":\"%s\",\"to_player_id\":\"\","
             "\"type\":%d,\"flag\":%d,\"text\":\"%s\"}",
             lid_esc, type, flag, esc);
    /* Write immediately — ICE candidates arrive in bursts larger than pending_tx. */
    if (g_lc.handshake_done && g_lc.fd >= 0) {
        if (rnet_ws_write_text(g_lc.fd, msg, 1) < 0)
            return -1;
        return 0;
    }
    queue_send(msg);
    return 0;
}

int psx_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap)
{
    int i;
    if (g_lc.sig_count <= 0) {
        return 0;
    }
    i = g_lc.sig_head;
    if (type) *type = g_lc.sig_q[i].type;
    if (flag) *flag = g_lc.sig_q[i].flag;
    if (text && text_cap) {
        strncpy(text, g_lc.sig_q[i].text, text_cap - 1);
        text[text_cap - 1] = '\0';
    }
    g_lc.sig_head = (g_lc.sig_head + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
    g_lc.sig_count--;
    return 1;
}

int psx_lobby_request_turn_credentials(void)
{
    if (!psx_lobby_connected())
        return -1;
    if (g_lc.turn.valid && g_lc.turn_received_at > 0 && g_lc.turn.ttl_secs > 0) {
        time_t now = time(NULL);
        if (now >= g_lc.turn_received_at &&
            (uint32_t)(now - g_lc.turn_received_at) + 60u < g_lc.turn.ttl_secs) {
            return 0; /* still fresh (60s skew margin) */
        }
    }
    return queue_turn_credentials_request();
}

const PsxLobbyTurnCredentials *psx_lobby_turn_credentials(void)
{
    if (g_lc.turn.valid && g_lc.turn_received_at > 0 && g_lc.turn.ttl_secs > 0) {
        time_t now = time(NULL);
        if (now < g_lc.turn_received_at ||
            (uint32_t)(now - g_lc.turn_received_at) >= g_lc.turn.ttl_secs) {
            clear_turn_credentials();
        }
    }
    return &g_lc.turn;
}

#endif /* PSX_HAS_LOBBY_CLIENT */
