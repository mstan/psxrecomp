#include "psx_lobby_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(PSX_HAS_LOBBY_CLIENT)

const char *psx_lobby_default_url(void)
{
    return "ws://netplay.technicallycomputers.ca:8765";
}
int  psx_lobby_connect(const char *ws_url) { (void)ws_url; return -1; }
void psx_lobby_disconnect(void) {}
int  psx_lobby_connected(void) { return 0; }
void psx_lobby_set_display_name(const char *name) { (void)name; }
const char *psx_lobby_display_name(void) { return ""; }
const char *psx_lobby_player_id(void) { return ""; }
void psx_lobby_pump(void) {}
void psx_lobby_request_list(void) {}
int  psx_lobby_list_count(void) { return 0; }
int  psx_lobby_list_get(int index, PsxLobbyRow *out) { (void)index; (void)out; return 0; }
int  psx_lobby_create(const char *a, const char *b, const char *c, const char *d,
                      const PsxLobbyMatchCaps *e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return -1; }
int  psx_lobby_join(const char *a, const char *b, const char *c)
{ (void)a; (void)b; (void)c; return -1; }
int  psx_lobby_leave(void) { return -1; }
int  psx_lobby_in_lobby(void) { return 0; }
int  psx_lobby_is_host(void) { return 0; }
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
int  psx_lobby_local_ready(void) { return 0; }
int  psx_lobby_all_ready(void) { return 0; }
int  psx_lobby_set_ready(int ready) { (void)ready; return -1; }
int  psx_lobby_request_start(const PsxLobbyMatchCaps *c) { (void)c; return -1; }
int  psx_lobby_launch_pending(void) { return 0; }
void psx_lobby_clear_launch_pending(void) {}

#else /* PSX_HAS_LOBBY_CLIENT */

#include "rnet_ws.h"
#include "rnet_sha1.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

typedef struct {
    int fd;
    int connected;
    int handshake_done;
    char player_id[PSX_LOBBY_ID_LEN];
    char display_name[PSX_LOBBY_NAME_LEN];
    char host[128];
    int port;
    char path[128];
    char rx_http[4096];
    size_t rx_http_len;
    /* Bytes that arrived with the HTTP 101 response after the header end. */
    uint8_t ws_pending[4096];
    size_t ws_pending_len;
    PsxLobbyRow list[PSX_LOBBY_MAX_LIST];
    int list_count;
    int in_lobby;
    int is_host;
    char my_bind[PSX_LOBBY_ENDPOINT_LEN];
    PsxLobbyJoinInfo join;
    PsxLobbyMember members[PSX_LOBBY_MAX_MEMBERS];
    int member_count;
    int local_ready;
    int all_ready;
    int launch_pending;
    PsxLobbyMatchCaps match_caps;
    char pending_tx[8][2048];
    int pending_n;
} LobbyClient;

static LobbyClient g_lc = { .fd = -1 };

static void match_caps_clear(PsxLobbyMatchCaps *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->aspect_num = 4;
    c->aspect_den = 3;
    c->input_delay = 2;
}

static int json_extract_object(const char *json, const char *key, char *out, size_t out_cap);
static void parse_match_caps_object(const char *obj, PsxLobbyMatchCaps *out);
static void ingest_match_caps_from_json(const char *json);
static int append_match_caps_json(char *dst, size_t dst_cap, const PsxLobbyMatchCaps *caps);

const char *psx_lobby_default_url(void)
{
    const char *e = getenv("PSX_NET_LOBBY_URL");
    return (e && e[0]) ? e : "ws://netplay.technicallycomputers.ca:8765";
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
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
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
    out->input_delay = json_get_int(obj, "input_delay", 2);
    if (out->input_delay < 0) out->input_delay = 0;
    if (out->input_delay > 16) out->input_delay = 16;
    json_get_str(obj, "language", out->language, sizeof(out->language));
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
    return snprintf(dst, dst_cap,
                    ",\"match_caps\":{\"v\":1,\"aspect_num\":%d,\"aspect_den\":%d,"
                    "\"turbo_loads\":%s,\"bios_hle\":%s,\"fast_boot\":%s,"
                    "\"auto_skip_fmv\":%s,\"input_delay\":%d,\"language\":\"%s\"}",
                    caps->aspect_num, caps->aspect_den,
                    caps->turbo_loads ? "true" : "false",
                    caps->bios_hle ? "true" : "false",
                    caps->fast_boot ? "true" : "false",
                    caps->auto_skip_fmv ? "true" : "false",
                    caps->input_delay, lang);
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

static void fill_peer_bind_from_join(void)
{
    PsxLobbyJoinInfo *j = &g_lc.join;
    memset(j->bind_hostport, 0, sizeof(j->bind_hostport));
    memset(j->peer_hostport, 0, sizeof(j->peer_hostport));
    if (g_lc.is_host) {
        strncpy(j->bind_hostport, g_lc.my_bind, sizeof(j->bind_hostport) - 1);
        strncpy(j->peer_hostport, j->guest_endpoint, sizeof(j->peer_hostport) - 1);
    } else {
        strncpy(j->bind_hostport, g_lc.my_bind, sizeof(j->bind_hostport) - 1);
        strncpy(j->peer_hostport, j->host_endpoint, sizeof(j->peer_hostport) - 1);
    }
    j->bind_hostport[sizeof(j->bind_hostport) - 1] = '\0';
    j->peer_hostport[sizeof(j->peer_hostport) - 1] = '\0';
}

static void parse_slots_array(const char *json)
{
    const char *p = strstr(json, "\"slots\"");
    int n = 0;
    g_lc.member_count = 0;
    g_lc.local_ready = 0;
    if (!p) {
        return;
    }
    p = strchr(p, '[');
    if (!p) {
        return;
    }
    ++p;
    while (*p && n < PSX_LOBBY_MAX_MEMBERS) {
        const char *obj;
        while (*p && *p != '{') {
            if (*p == ']') {
                g_lc.member_count = n;
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
                char chunk[512];
                size_t len = (size_t)(end - obj);
                if (len >= sizeof(chunk)) {
                    len = sizeof(chunk) - 1;
                }
                memcpy(chunk, obj, len);
                chunk[len] = '\0';
                g_lc.members[n].slot = json_get_int(chunk, "slot", n);
                json_get_str(chunk, "player_id", g_lc.members[n].player_id,
                             sizeof(g_lc.members[n].player_id));
                json_get_str(chunk, "display_name", g_lc.members[n].display_name,
                             sizeof(g_lc.members[n].display_name));
                g_lc.members[n].ready = json_get_bool(chunk, "ready", 0);
                if (g_lc.player_id[0] &&
                    strcmp(g_lc.members[n].player_id, g_lc.player_id) == 0) {
                    g_lc.local_ready = g_lc.members[n].ready;
                }
                ++n;
                p = end;
            }
        }
    }
    g_lc.member_count = n;
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

static void handle_server_json(const char *json)
{
    char op[32];
    json_get_str(json, "op", op, sizeof(op));
    if (strcmp(op, "welcome") == 0) {
        json_get_str(json, "player_id", g_lc.player_id, sizeof(g_lc.player_id));
        if (g_lc.display_name[0]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "{\"op\":\"hello\",\"display_name\":\"%s\"}", g_lc.display_name);
            queue_send(msg);
        }
        queue_send("{\"op\":\"list\"}");
        return;
    }
    if (strcmp(op, "lobby_list") == 0) {
        const char *p = strstr(json, "\"lobbies\"");
        int n = 0;
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
                    char chunk[1024];
                    size_t len = (size_t)(end - obj);
                    if (len >= sizeof(chunk)) {
                        len = sizeof(chunk) - 1;
                    }
                    memcpy(chunk, obj, len);
                    chunk[len] = '\0';
                    json_get_str(chunk, "lobby_id", g_lc.list[n].lobby_id, sizeof(g_lc.list[n].lobby_id));
                    json_get_str(chunk, "name", g_lc.list[n].name, sizeof(g_lc.list[n].name));
                    json_get_str(chunk, "game_name", g_lc.list[n].game_name, sizeof(g_lc.list[n].game_name));
                    g_lc.list[n].player_count = json_get_int(chunk, "player_count", 0);
                    g_lc.list[n].max_slots = json_get_int(chunk, "max_slots", 2);
                    g_lc.list[n].has_password = json_get_bool(chunk, "has_password", 0);
                    ++n;
                    p = end;
                }
            }
        }
        g_lc.list_count = n;
        return;
    }
    if (strcmp(op, "created") == 0) {
        g_lc.in_lobby = 1;
        g_lc.is_host = 1;
        g_lc.join.ok = 1;
        g_lc.launch_pending = 0;
        g_lc.all_ready = 0;
        json_get_str(json, "lobby_id", g_lc.join.lobby_id, sizeof(g_lc.join.lobby_id));
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", 1);
        g_lc.join.local_slot = json_get_int(json, "local_slot", 0);
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = 1;
        g_lc.join.max_slots = 2;
        g_lc.join.last_error[0] = '\0';
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
        return;
    }
    if (strcmp(op, "joined") == 0) {
        g_lc.in_lobby = 1;
        g_lc.is_host = 0;
        g_lc.join.ok = 1;
        g_lc.launch_pending = 0;
        g_lc.all_ready = 0;
        json_get_str(json, "lobby_id", g_lc.join.lobby_id, sizeof(g_lc.join.lobby_id));
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", 1);
        g_lc.join.local_slot = json_get_int(json, "local_slot", 1);
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = 2;
        g_lc.join.max_slots = 2;
        g_lc.join.last_error[0] = '\0';
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        return;
    }
    if (strcmp(op, "lobby_update") == 0) {
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", g_lc.join.player_count);
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lc.join.max_slots);
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", (int)g_lc.join.session_id);
        g_lc.all_ready = json_get_bool(json, "all_ready", 0);
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        return;
    }
    if (strcmp(op, "launch") == 0) {
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", g_lc.join.player_count);
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lc.join.max_slots);
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", (int)g_lc.join.session_id);
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        /* Rematch/join without a peer endpoint would hang forever in HELLO. */
        if (!g_lc.join.peer_hostport[0] || !g_lc.join.host_endpoint[0] ||
            (g_lc.is_host && !g_lc.join.guest_endpoint[0])) {
            strncpy(g_lc.join.last_error, "missing_endpoints",
                    sizeof(g_lc.join.last_error) - 1);
            g_lc.launch_pending = 0;
            return;
        }
        g_lc.join.last_error[0] = '\0';
        g_lc.launch_pending = 1;
        return;
    }
    if (strcmp(op, "error") == 0) {
        json_get_str(json, "code", g_lc.join.last_error, sizeof(g_lc.join.last_error));
        g_lc.join.ok = 0;
        return;
    }
    if (strcmp(op, "lobby_closed") == 0 || strcmp(op, "left") == 0) {
        g_lc.in_lobby = 0;
        g_lc.is_host = 0;
        g_lc.member_count = 0;
        g_lc.local_ready = 0;
        g_lc.all_ready = 0;
        g_lc.launch_pending = 0;
        memset(&g_lc.join, 0, sizeof(g_lc.join));
        match_caps_clear(&g_lc.match_caps);
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

int psx_lobby_connect(const char *ws_url)
{
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int fd = -1;
    char key_raw[16];
    char key_b64[32];
    char req[512];
    int i;

    psx_lobby_disconnect();
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
    snprintf(portstr, sizeof(portstr), "%d", g_lc.port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_lc.host, portstr, &hints, &res) != 0) {
        return -2;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        fd = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return -3;
    }
    g_lc.fd = fd;
    for (i = 0; i < 16; ++i) {
        key_raw[i] = (char)(rand() & 0xff);
    }
    /* base64 16 bytes -> 24 chars; reuse server-side style via sha1 helper file's b64? */
    {
        static const char *B64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int o = 0;
        for (i = 0; i < 16; i += 3) {
            unsigned v = ((unsigned char)key_raw[i] << 16);
            if (i + 1 < 16) {
                v |= ((unsigned char)key_raw[i + 1] << 8);
            }
            if (i + 2 < 16) {
                v |= (unsigned char)key_raw[i + 2];
            }
            key_b64[o++] = B64[(v >> 18) & 63];
            key_b64[o++] = B64[(v >> 12) & 63];
            key_b64[o++] = (i + 1 < 16) ? B64[(v >> 6) & 63] : '=';
            key_b64[o++] = (i + 2 < 16) ? B64[v & 63] : '=';
        }
        key_b64[o] = '\0';
    }
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             g_lc.path, g_lc.host, g_lc.port, key_b64);
    if (send(fd, req, (int)strlen(req), 0) < 0) {
        close(fd);
        g_lc.fd = -1;
        return -4;
    }
    set_nonblock(fd);
    g_lc.connected = 1;
    g_lc.handshake_done = 0;
    g_lc.rx_http_len = 0;
    return 0;
}

void psx_lobby_disconnect(void)
{
    if (g_lc.fd >= 0) {
        close(g_lc.fd);
    }
    {
        char dname[PSX_LOBBY_NAME_LEN];
        strncpy(dname, g_lc.display_name, sizeof(dname) - 1);
        memset(&g_lc, 0, sizeof(g_lc));
        g_lc.fd = -1;
        strncpy(g_lc.display_name, dname, sizeof(g_lc.display_name) - 1);
    }
}

int psx_lobby_connected(void)
{
    return g_lc.connected && g_lc.fd >= 0;
}

void psx_lobby_set_display_name(const char *name)
{
    if (!name) {
        return;
    }
    strncpy(g_lc.display_name, name, sizeof(g_lc.display_name) - 1);
    g_lc.display_name[sizeof(g_lc.display_name) - 1] = '\0';
}

const char *psx_lobby_display_name(void)
{
    return g_lc.display_name;
}

const char *psx_lobby_player_id(void)
{
    return g_lc.player_id;
}

void psx_lobby_pump(void)
{
    char buf[4096];
#if defined(_WIN32)
    int n;
#else
    ssize_t n;
#endif
    if (!psx_lobby_connected()) {
        return;
    }
    if (!g_lc.handshake_done) {
        n = recv(g_lc.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
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
    for (;;) {
        int closed = 0;
        int fl;
#if !defined(_WIN32)
        fl = fcntl(g_lc.fd, F_GETFL, 0);
        /* Non-blocking peek: if no data, EAGAIN from first recv inside read */
#endif
        {
            uint8_t peek[1];
            n = recv(g_lc.fd, (char *)peek, 1, MSG_PEEK);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                psx_lobby_disconnect();
                return;
            }
            if (n == 0) {
                psx_lobby_disconnect();
                return;
            }
        }
#if !defined(_WIN32)
        fcntl(g_lc.fd, F_SETFL, fl & ~O_NONBLOCK);
#endif
        n = rnet_ws_read_text(g_lc.fd, buf, sizeof(buf), &closed);
#if !defined(_WIN32)
        fcntl(g_lc.fd, F_SETFL, fl | O_NONBLOCK);
#endif
        if (closed || n < 0) {
            psx_lobby_disconnect();
            return;
        }
        if (n == 0) {
            break;
        }
        handle_server_json(buf);
    }
}

void psx_lobby_request_list(void)
{
    queue_send("{\"op\":\"list\"}");
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

int psx_lobby_create(const char *name, const char *game_name, const char *password,
                     const char *host_bind, const PsxLobbyMatchCaps *match_caps)
{
    char msg[1536];
    char caps_json[512];
    int n;
    if (!psx_lobby_connected()) {
        return -1;
    }
    strncpy(g_lc.my_bind, host_bind && host_bind[0] ? host_bind : "0.0.0.0:7777",
            sizeof(g_lc.my_bind) - 1);
    g_lc.join.last_error[0] = '\0';
    caps_json[0] = '\0';
    if (match_caps && match_caps->valid) {
        g_lc.match_caps = *match_caps;
        append_match_caps_json(caps_json, sizeof(caps_json), match_caps);
    }
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"create\",\"name\":\"%s\",\"game_name\":\"%s\",\"password\":\"%s\","
                 "\"max_slots\":2,\"host_bind\":\"%s\",\"display_name\":\"%s\"%s}",
                 name && name[0] ? name : "Lobby", game_name && game_name[0] ? game_name : "Game",
                 password ? password : "", g_lc.my_bind,
                 g_lc.display_name[0] ? g_lc.display_name : "Host", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_join(const char *lobby_id, const char *password, const char *guest_bind)
{
    char msg[1024];
    if (!psx_lobby_connected() || !lobby_id) {
        return -1;
    }
    strncpy(g_lc.my_bind, guest_bind && guest_bind[0] ? guest_bind : "0.0.0.0:7778",
            sizeof(g_lc.my_bind) - 1);
    g_lc.join.last_error[0] = '\0';
    snprintf(msg, sizeof(msg),
             "{\"op\":\"join\",\"lobby_id\":\"%s\",\"password\":\"%s\",\"guest_bind\":\"%s\","
             "\"display_name\":\"%s\"}",
             lobby_id, password ? password : "", g_lc.my_bind,
             g_lc.display_name[0] ? g_lc.display_name : "Guest");
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_leave(void)
{
    queue_send("{\"op\":\"leave\"}");
    flush_pending();
    g_lc.in_lobby = 0;
    g_lc.is_host = 0;
    g_lc.member_count = 0;
    g_lc.local_ready = 0;
    g_lc.all_ready = 0;
    g_lc.launch_pending = 0;
    match_caps_clear(&g_lc.match_caps);
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
    char msg[768];
    char caps_json[512];
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

int psx_lobby_local_ready(void)
{
    return g_lc.local_ready;
}

int psx_lobby_all_ready(void)
{
    return g_lc.all_ready != 0 && g_lc.in_lobby && g_lc.join.player_count >= 2;
}

int psx_lobby_set_ready(int ready)
{
    char msg[64];
    if (!psx_lobby_connected() || !g_lc.in_lobby) {
        return -1;
    }
    snprintf(msg, sizeof(msg), "{\"op\":\"set_ready\",\"ready\":%s}", ready ? "true" : "false");
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_request_start(const PsxLobbyMatchCaps *match_caps)
{
    char msg[768];
    char caps_json[512];
    int n;
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host) {
        return -1;
    }
    caps_json[0] = '\0';
    if (match_caps && match_caps->valid) {
        g_lc.match_caps = *match_caps;
        append_match_caps_json(caps_json, sizeof(caps_json), match_caps);
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
}

#endif /* PSX_HAS_LOBBY_CLIENT */
