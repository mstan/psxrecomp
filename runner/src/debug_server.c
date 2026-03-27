/*
 * debug_server.c — TCP debug server for PSX recomp (shared runner)
 *
 * Single-threaded, non-blocking TCP server polled once per frame.
 * JSON-over-newline protocol on localhost:4370.
 *
 * Game-specific commands are dispatched via game_handle_debug_cmd() hook.
 * Game-specific frame data is filled via game_fill_frame_record() hook.
 *
 * Ported from nesrecomp/runner/src/debug_server.c and
 * segagenesisrecomp-v2/cmd_server.c
 */
#include "debug_server.h"
#include "game_extras.h"
#include "psx_runtime.h"
#include "func_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* ---- Platform sockets ---- */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
#  define SOCK_WOULDBLOCK WSAEWOULDBLOCK
   static int sock_error(void) { return WSAGetLastError(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
#  define SOCK_WOULDBLOCK EWOULDBLOCK
   static int sock_error(void) { return errno; }
#endif

/* GLFW for pause loop */
struct GLFWwindow;
typedef struct GLFWwindow GLFWwindow;
extern void glfwPollEvents(void);
extern int  glfwWindowShouldClose(GLFWwindow *window);
extern int  glfwGetKey(GLFWwindow *window, int key);
#define GLFW_KEY_ESCAPE 256
#define GLFW_PRESS      1

/* ---- Externs from runtime / main_runner ---- */
extern uint32_t g_ps1_frame;

/* ---- Server state ---- */
static sock_t s_listen  = SOCK_INVALID;
static sock_t s_client  = SOCK_INVALID;
static int    s_port    = 4370;

static GLFWwindow *s_glfw_window = NULL;

#define RECV_BUF_SIZE 8192
static char s_recv_buf[RECV_BUF_SIZE];
static int  s_recv_len = 0;

/* ---- Pause / step ---- */
static volatile int s_paused     = 0;
static int          s_step_count = 0;
static uint32_t     s_run_to     = 0;  /* target frame (0=disabled) */

/* ---- Input override ---- */
static int s_input_override = -1;  /* -1 = no override */

/* ---- Ring buffer ---- */
static PSXFrameRecord s_frame_history[FRAME_HISTORY_CAP];
static uint64_t       s_history_count = 0;

/* ---- Watchpoints ---- */
#define MAX_WATCHPOINTS 8
typedef struct {
    uint32_t addr;
    uint8_t  prev_val;
    int      active;
} Watchpoint;
static Watchpoint s_watchpoints[MAX_WATCHPOINTS];

/* ---- Platform helpers ---- */
static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void platform_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* ---- PSX memory access helpers ---- */

static uint8_t psx_read_byte(uint32_t addr)
{
    /* KSEG0 (0x80000000-0x801FFFFF) */
    if (addr >= 0x80000000u && addr < 0x80200000u)
        return psx_get_ram()[addr & 0x1FFFFFu];
    /* KUSEG (0x00000000-0x001FFFFF) */
    if (addr < 0x00200000u)
        return psx_get_ram()[addr & 0x1FFFFFu];
    /* KSEG1 (0xA0000000-0xA01FFFFF) */
    if (addr >= 0xA0000000u && addr < 0xA0200000u)
        return psx_get_ram()[addr & 0x1FFFFFu];
    /* Scratchpad (0x1F800000-0x1F8003FF) */
    if (addr >= 0x1F800000u && addr < 0x1F800400u)
        return psx_get_scratch()[addr & 0x3FFu];
    return 0;
}

static void psx_write_byte(uint32_t addr, uint8_t val)
{
    if (addr >= 0x80000000u && addr < 0x80200000u)
        psx_get_ram()[addr & 0x1FFFFFu] = val;
    else if (addr < 0x00200000u)
        psx_get_ram()[addr & 0x1FFFFFu] = val;
    else if (addr >= 0xA0000000u && addr < 0xA0200000u)
        psx_get_ram()[addr & 0x1FFFFFu] = val;
    else if (addr >= 0x1F800000u && addr < 0x1F800400u)
        psx_get_scratch()[addr & 0x3FFu] = val;
}

/* ---- JSON helpers (hand-parsed, no library) ---- */

static const char *json_get_str(const char *json, const char *key,
                                 char *out, int out_sz)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
    /* Unquoted value (number etc) */
    {
        int i = 0;
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
}

static int json_get_int(const char *json, const char *key, int def)
{
    char buf[64];
    if (!json_get_str(json, key, buf, sizeof(buf))) return def;
    return atoi(buf);
}

static uint32_t hex_to_u32(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (uint32_t)strtoul(s, NULL, 16);
}

/* ---- Send helpers (public API) ---- */

void debug_server_send_line(const char *json)
{
    if (s_client == SOCK_INVALID) return;
    int len = (int)strlen(json);
    send(s_client, json, len, 0);
    send(s_client, "\n", 1, 0);
}

void debug_server_send_fmt(const char *fmt, ...)
{
    char buf[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    debug_server_send_line(buf);
}

/* Internal short aliases */
#define send_line  debug_server_send_line
#define send_fmt   debug_server_send_fmt

static void send_ok(int id)
{
    send_fmt("{\"id\":%d,\"ok\":true}", id);
}

static void send_err(int id, const char *msg)
{
    send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"%s\"}", id, msg);
}

/* ---- Command handlers ---- */

static void handle_ping(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u}",
             id, g_ps1_frame);
}

static void handle_frame(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u}",
             id, g_ps1_frame);
}

static void handle_get_registers(int id, const char *json)
{
    (void)json;
    /* Read CPU state from the ring buffer's most recent frame,
     * which has a full GPR snapshot */
    if (s_history_count == 0) {
        send_err(id, "no frames recorded yet");
        return;
    }
    uint32_t idx = (uint32_t)((s_history_count - 1) % FRAME_HISTORY_CAP);
    const PSXFrameRecord *r = &s_frame_history[idx];

    /* Build register dump as JSON object */
    char buf[2048];
    int pos = snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"ok\":true,\"frame\":%u,"
        "\"pc\":\"0x%08X\",\"hi\":\"0x%08X\",\"lo\":\"0x%08X\",\"regs\":{",
        id, r->frame_number, r->pc, r->hi, r->lo);

    static const char *reg_names[] = {
        "zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra"
    };
    for (int i = 0; i < 32; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "\"%s\":\"0x%08X\"", reg_names[i], r->gpr[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
    send_line(buf);
}

static void handle_read_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;

    char *hex = (char *)malloc(len * 2 + 1);
    if (!hex) { send_err(id, "alloc failed"); return; }
    for (int i = 0; i < len; i++)
        snprintf(hex + i * 2, 3, "%02x", psx_read_byte(addr + i));

    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"%s\"}",
             id, addr, len, hex);
    free(hex);
}

static void handle_dump_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 256);
    if (len < 1) len = 1;
    if (len > 65536) len = 65536;

    /* Send in chunks of 4096 bytes */
    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > 4096) chunk = 4096;
        char *hex = (char *)malloc(chunk * 2 + 1);
        if (!hex) { send_err(id, "alloc failed"); return; }
        for (int i = 0; i < chunk; i++)
            snprintf(hex + i * 2, 3, "%02x", psx_read_byte(addr + offset + i));
        send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"offset\":%d,\"len\":%d,\"hex\":\"%s\"}",
                 id, addr + offset, offset, chunk, hex);
        free(hex);
        offset += chunk;
    }
}

static void handle_write_ram(int id, const char *json)
{
    char addr_str[32], hex_str[8193];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    if (!json_get_str(json, "hex", hex_str, sizeof(hex_str))) {
        send_err(id, "missing hex");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int hex_len = (int)strlen(hex_str);
    for (int i = 0; i + 1 < hex_len; i += 2) {
        char byte_str[3] = { hex_str[i], hex_str[i+1], '\0' };
        uint8_t val = (uint8_t)strtoul(byte_str, NULL, 16);
        psx_write_byte(addr + i / 2, val);
    }
    send_ok(id);
}

static void handle_read_scratch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 1024) len = 1024;

    uint8_t *scratch = psx_get_scratch();
    char *hex = (char *)malloc(len * 2 + 1);
    if (!hex) { send_err(id, "alloc failed"); return; }
    for (int i = 0; i < len; i++) {
        uint32_t off = (addr + i) & 0x3FFu;
        snprintf(hex + i * 2, 3, "%02x", scratch[off]);
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"%s\"}",
             id, addr, len, hex);
    free(hex);
}

static void handle_gpu_state(int id, const char *json)
{
    (void)json;
    uint16_t da_x, da_y;
    int16_t do_x, do_y;
    uint8_t h_res, v_res, disp_en;

    psx_debug_get_display_area(&da_x, &da_y);
    psx_debug_get_draw_offset(&do_x, &do_y);
    psx_debug_get_display_mode(&h_res, &v_res, &disp_en);

    uint32_t gpustat = psx_debug_get_gpustat();

    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"display_area_x\":%d,\"display_area_y\":%d,"
             "\"draw_offset_x\":%d,\"draw_offset_y\":%d,"
             "\"h_resolution\":%d,\"v_resolution\":%d,"
             "\"display_enable\":%d,\"gpustat\":\"0x%08X\"}",
             id, da_x, da_y, do_x, do_y,
             h_res, v_res, disp_en, gpustat);
}

static void handle_overlay_state(int id, const char *json)
{
    (void)json;
    /* Overlay state is captured in the most recent frame record */
    if (s_history_count == 0) {
        send_fmt("{\"id\":%d,\"ok\":true,\"overlay_base\":0,\"overlay_pc\":0}", id);
        return;
    }
    uint32_t idx = (uint32_t)((s_history_count - 1) % FRAME_HISTORY_CAP);
    const PSXFrameRecord *r = &s_frame_history[idx];
    send_fmt("{\"id\":%d,\"ok\":true,\"overlay_base\":\"0x%08X\",\"overlay_pc\":\"0x%08X\"}",
             id, r->overlay_base, r->overlay_pc);
}

static void handle_watch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) {
            s_watchpoints[i].addr = addr;
            s_watchpoints[i].prev_val = psx_read_byte(addr);
            s_watchpoints[i].active = 1;
            send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"addr\":\"0x%08X\"}",
                     id, i, addr);
            return;
        }
    }
    send_err(id, "all watchpoint slots full (max 8)");
}

static void handle_unwatch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (s_watchpoints[i].active && s_watchpoints[i].addr == addr) {
            s_watchpoints[i].active = 0;
            send_ok(id);
            return;
        }
    }
    send_err(id, "watchpoint not found");
}

static void handle_set_input(int id, const char *json)
{
    char val_str[32];
    if (!json_get_str(json, "buttons", val_str, sizeof(val_str))) {
        send_err(id, "missing buttons");
        return;
    }
    s_input_override = (int)hex_to_u32(val_str);
    send_ok(id);
}

static void handle_clear_input(int id, const char *json)
{
    (void)json;
    s_input_override = -1;
    send_ok(id);
}

static void handle_pause(int id, const char *json)
{
    (void)json;
    s_paused = 1;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":true,\"frame\":%u}",
             id, g_ps1_frame);
}

static void handle_continue(int id, const char *json)
{
    (void)json;
    s_paused = 0;
    s_step_count = 0;
    s_run_to = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":false}", id);
}

static void handle_step(int id, const char *json)
{
    int n = json_get_int(json, "count", 1);
    if (n < 1) n = 1;
    s_step_count = n;
    s_paused = 0;  /* unpause for N frames, then re-pause */
    send_fmt("{\"id\":%d,\"ok\":true,\"stepping\":%d}", id, n);
}

static void handle_run_to_frame(int id, const char *json)
{
    int target = json_get_int(json, "frame", 0);
    if (target <= (int)g_ps1_frame) {
        send_err(id, "target frame already passed");
        return;
    }
    s_run_to = (uint32_t)target;
    s_paused = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"running_to\":%d}", id, target);
}

/* ---- Ring buffer queries ---- */

static void handle_history(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"oldest\":%llu,\"newest\":%llu}",
             id,
             (unsigned long long)s_history_count,
             (unsigned long long)oldest,
             (unsigned long long)(s_history_count > 0 ? s_history_count - 1 : 0));
}

static void handle_get_frame(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
        send_err(id, "frame not in buffer");
        return;
    }

    uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
    const PSXFrameRecord *r = &s_frame_history[idx];
    if (r->frame_number != (uint32_t)f) {
        send_err(id, "frame record mismatch");
        return;
    }

    /* Encode game_data as hex */
    char gd_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

    /* Build response with key fields (not all 32 GPRs — use get_registers for that) */
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"frame\":%u,\"pc\":\"0x%08X\","
             "\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
             "\"v0\":\"0x%08X\",\"a0\":\"0x%08X\","
             "\"gpu\":{\"da_x\":%d,\"da_y\":%d,\"do_x\":%d,\"do_y\":%d},"
             "\"pad\":\"0x%04X\","
             "\"overlay_base\":\"0x%08X\","
             "\"dispatch_misses\":%u,"
             "\"game_data\":\"%s\","
             "\"last_func\":\"%s\"}",
             id, r->frame_number, r->pc,
             r->gpr[29], r->gpr[31],  /* sp=r29, ra=r31 */
             r->gpr[2], r->gpr[4],    /* v0=r2, a0=r4 */
             r->display_area_x, r->display_area_y,
             r->draw_offset_x, r->draw_offset_y,
             r->pad_buttons,
             r->overlay_base,
             r->dispatch_miss_count,
             gd_hex,
             r->last_func);
}

static void handle_frame_range(int id, const char *json)
{
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end", -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    char *buf = (char *)malloc(200 * 256 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"frames\":[", id);

    int first = 1;
    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;

        if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
            pos += snprintf(buf + pos, 128,
                "{\"frame\":%d,\"available\":false}", f);
            continue;
        }

        uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
        const PSXFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 128,
                "{\"frame\":%d,\"available\":false}", f);
            continue;
        }

        char gd_hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

        pos += snprintf(buf + pos, 256,
            "{\"frame\":%u,\"pc\":\"0x%08X\",\"pad\":\"0x%04X\","
            "\"game_data\":\"%s\"}",
            r->frame_number, r->pc,
            r->pad_buttons, gd_hex);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_frame_timeseries(int id, const char *json)
{
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end", -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    char *buf = (char *)malloc(200 * 320 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"ts\":[", id);

    int first = 1;
    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;

        if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
            pos += snprintf(buf + pos, 32, "null");
            continue;
        }

        uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
        const PSXFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 32, "null");
            continue;
        }

        char gd_hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

        pos += snprintf(buf + pos, 320,
            "{\"f\":%u,\"pc\":\"0x%08X\","
            "\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"pad\":%d,\"da_x\":%d,\"da_y\":%d,"
            "\"misses\":%u,\"gd\":\"%s\"}",
            r->frame_number, r->pc,
            r->gpr[29], r->gpr[31],
            r->pad_buttons,
            r->display_area_x, r->display_area_y,
            r->dispatch_miss_count, gd_hex);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_first_failure(int id, const char *json)
{
    (void)json;
    /* No verify mode in PSX yet — report no failures */
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":-1,\"message\":\"verify mode not available\"}", id);
}

static void handle_discovered_functions(int id, const char *json)
{
    (void)json;
    int count = func_logger_count();
    /* Build JSON response with function list */
    char *buf = (char *)malloc(count * 16 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 128, "{\"id\":%d,\"ok\":true,\"count\":%d,\"addrs\":", id, count);
    pos += func_logger_json(buf + pos, count * 16 + 128);
    pos += snprintf(buf + pos, 8, "}");
    send_line(buf);
    free(buf);
}

static void handle_dump_functions(int id, const char *json)
{
    char path[256];
    if (!json_get_str(json, "path", path, sizeof(path)))
        strcpy(path, "discovered_functions.log");
    func_logger_dump(path);
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%d,\"path\":\"%s\"}",
             id, func_logger_count(), path);
}

static void handle_quit(int id, const char *json)
{
    (void)json;
    /* Dump discovered functions before quitting */
    if (func_logger_count() > 0)
        func_logger_dump("discovered_functions.log");
    send_ok(id);
    debug_server_shutdown();
    exit(0);
}

/* ---- Command dispatch ---- */

typedef void (*CmdHandler)(int id, const char *json);
typedef struct {
    const char *name;
    CmdHandler  handler;
} CmdEntry;

static const CmdEntry s_commands[] = {
    { "ping",              handle_ping },
    { "frame",             handle_frame },
    { "get_registers",     handle_get_registers },
    { "read_ram",          handle_read_ram },
    { "dump_ram",          handle_dump_ram },
    { "write_ram",         handle_write_ram },
    { "read_scratch",      handle_read_scratch },
    { "gpu_state",         handle_gpu_state },
    { "overlay_state",     handle_overlay_state },
    { "watch",             handle_watch },
    { "unwatch",           handle_unwatch },
    { "set_input",         handle_set_input },
    { "clear_input",       handle_clear_input },
    { "pause",             handle_pause },
    { "continue",          handle_continue },
    { "step",              handle_step },
    { "run_to_frame",      handle_run_to_frame },
    { "history",           handle_history },
    { "get_frame",         handle_get_frame },
    { "frame_range",       handle_frame_range },
    { "frame_timeseries",  handle_frame_timeseries },
    { "first_failure",     handle_first_failure },
    { "discovered_functions", handle_discovered_functions },
    { "dump_functions",    handle_dump_functions },
    { "quit",              handle_quit },
    { NULL, NULL }
};

static void process_command(const char *line)
{
    char cmd[64];
    if (!json_get_str(line, "cmd", cmd, sizeof(cmd))) {
        /* Maybe bare command name (not JSON) */
        strncpy(cmd, line, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        int len = (int)strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\r' || cmd[len-1] == ' '))
            cmd[--len] = '\0';
    }

    int id = json_get_int(line, "id", 0);

    for (const CmdEntry *e = s_commands; e->name; e++) {
        if (strcmp(cmd, e->name) == 0) {
            e->handler(id, line);
            return;
        }
    }

    /* Try game-specific command handler */
    if (game_handle_debug_cmd(cmd, id, line))
        return;

    send_err(id, "unknown command");
}

/* ---- Public API ---- */

void debug_server_init(int port)
{
    if (port > 0) s_port = port;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == SOCK_INVALID) {
        fprintf(stderr, "[debug] Failed to create socket\n");
        return;
    }

    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)s_port);

    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[debug] Failed to bind port %d\n", s_port);
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
        return;
    }

    listen(s_listen, 1);
    set_nonblocking(s_listen);

    memset(s_frame_history, 0, sizeof(s_frame_history));
    s_history_count = 0;
    memset(s_watchpoints, 0, sizeof(s_watchpoints));

    fprintf(stderr, "[debug] TCP server listening on 127.0.0.1:%d\n", s_port);
}

void debug_server_set_glfw_window(void *win)
{
    s_glfw_window = (GLFWwindow *)win;
}

void debug_server_poll(void)
{
    if (s_listen == SOCK_INVALID) return;

    /* Accept new client if none connected */
    if (s_client == SOCK_INVALID) {
        struct sockaddr_in caddr;
        int clen = sizeof(caddr);
        sock_t c = accept(s_listen, (struct sockaddr *)&caddr, &clen);
        if (c != SOCK_INVALID) {
            s_client = c;
            set_nonblocking(s_client);
            s_recv_len = 0;
            fprintf(stderr, "[debug] Client connected\n");
        }
        return;
    }

    /* Receive data */
    int space = RECV_BUF_SIZE - s_recv_len - 1;
    if (space > 0) {
        int n = recv(s_client, s_recv_buf + s_recv_len, space, 0);
        if (n > 0) {
            s_recv_len += n;
            s_recv_buf[s_recv_len] = '\0';
        } else if (n == 0) {
            fprintf(stderr, "[debug] Client disconnected\n");
            sock_close(s_client);
            s_client = SOCK_INVALID;
            s_paused = 0;
            s_input_override = -1;
            return;
        } else {
            int err = sock_error();
#ifdef _WIN32
            if (err != WSAEWOULDBLOCK) {
#else
            if (err != EAGAIN && err != EWOULDBLOCK) {
#endif
                fprintf(stderr, "[debug] recv error %d, dropping client\n", err);
                sock_close(s_client);
                s_client = SOCK_INVALID;
                s_paused = 0;
                s_input_override = -1;
                return;
            }
        }
    }

    /* Process complete lines */
    char *nl;
    while ((nl = strchr(s_recv_buf, '\n')) != NULL) {
        *nl = '\0';
        if (nl > s_recv_buf && *(nl - 1) == '\r')
            *(nl - 1) = '\0';
        if (s_recv_buf[0] != '\0')
            process_command(s_recv_buf);
        int consumed = (int)(nl - s_recv_buf) + 1;
        s_recv_len -= consumed;
        memmove(s_recv_buf, nl + 1, s_recv_len + 1);
    }
}

void debug_server_record_frame(void)
{
    uint32_t idx = (uint32_t)(g_ps1_frame % FRAME_HISTORY_CAP);
    PSXFrameRecord *r = &s_frame_history[idx];

    r->frame_number = g_ps1_frame;

    /* CPU state — read from the live CPUState via g_diag_cpu extern */
    /* g_diag_cpu is set in psx_runtime_init() to point at the active CPUState */
    extern CPUState *g_diag_cpu;
    if (g_diag_cpu) {
        /* Copy all 32 GPRs from the CPUState struct.
         * CPUState layout: zero, at, v0, v1, a0..a3, t0..t7, s0..s7, t8, t9, k0, k1, gp, sp, fp, ra
         * These are the first 32 uint32_t fields. */
        const uint32_t *regs = &g_diag_cpu->zero;
        for (int i = 0; i < 32; i++)
            r->gpr[i] = regs[i];
        r->pc = g_diag_cpu->pc;
        r->hi = g_diag_cpu->hi;
        r->lo = g_diag_cpu->lo;
    } else {
        memset(r->gpr, 0, sizeof(r->gpr));
        r->pc = r->hi = r->lo = 0;
    }

    /* GPU state */
    psx_debug_get_display_area(&r->display_area_x, &r->display_area_y);
    psx_debug_get_draw_offset(&r->draw_offset_x, &r->draw_offset_y);
    psx_debug_get_display_mode(&r->h_resolution, &r->v_resolution, &r->display_enable);
    r->_gpu_pad = 0;

    /* Overlay state — placeholder, will be populated when overlay tracking is wired up */
    r->overlay_base = 0;
    r->overlay_pc = 0;

    /* Input — read from pad state captured this frame */
    extern uint16_t g_pad1_state;  /* defined in runtime.c */
    r->pad_buttons = g_pad1_state;

    /* Dispatch miss count */
    r->dispatch_miss_count = psx_get_dispatch_miss_count();

    /* Game-specific data */
    memset(r->game_data, 0, sizeof(r->game_data));
    game_fill_frame_record(r);

    /* Last function name — placeholder */
    strcpy(r->last_func, "(no tracking)");

    r->_reserved = 0;
    s_history_count = (uint64_t)g_ps1_frame + 1;

    /* Step mode: count down and re-pause */
    if (s_step_count > 0) {
        s_step_count--;
        if (s_step_count == 0) {
            s_paused = 1;
            send_fmt("{\"event\":\"step_done\",\"frame\":%u}", g_ps1_frame);
        }
    }

    /* Run-to-frame: pause when target reached */
    if (s_run_to > 0 && g_ps1_frame >= s_run_to) {
        s_paused = 1;
        s_run_to = 0;
        send_fmt("{\"event\":\"run_to_done\",\"frame\":%u}", g_ps1_frame);
    }
}

void debug_server_wait_if_paused(void)
{
    while (s_paused) {
        debug_server_poll();

        if (s_glfw_window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(s_glfw_window)) exit(0);
            if (glfwGetKey(s_glfw_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) exit(0);
        }

        platform_sleep_ms(5);
    }
}

void debug_server_check_watchpoints(void)
{
    if (s_client == SOCK_INVALID) return;

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) continue;
        uint8_t cur = psx_read_byte(s_watchpoints[i].addr);
        if (cur != s_watchpoints[i].prev_val) {
            send_fmt("{\"event\":\"watchpoint\","
                     "\"addr\":\"0x%08X\",\"old\":\"0x%02X\",\"new\":\"0x%02X\","
                     "\"frame\":%u}",
                     s_watchpoints[i].addr,
                     s_watchpoints[i].prev_val, cur,
                     g_ps1_frame);
            s_watchpoints[i].prev_val = cur;
        }
    }
}

void debug_server_shutdown(void)
{
    if (s_client != SOCK_INVALID) {
        sock_close(s_client);
        s_client = SOCK_INVALID;
    }
    if (s_listen != SOCK_INVALID) {
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    fprintf(stderr, "[debug] Server shut down\n");
}

int debug_server_is_connected(void)
{
    return s_client != SOCK_INVALID;
}

int debug_server_get_input_override(void)
{
    return s_input_override;
}
