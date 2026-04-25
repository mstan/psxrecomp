/*
 * debug_server.c -- TCP debug server for PSX recomp v4
 *
 * Single-threaded, non-blocking TCP server polled once per vblank.
 * JSON-over-newline protocol on localhost:4370.
 *
 * Same function names and protocol as nesrecomp/snesrecomp versions
 * so TCP.md and DEBUG.md are reusable across projects.
 */
#include "debug_server.h"
#include "cpu_state.h"
#include "dma.h"
#include "gpu.h"
#include "sio.h"

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
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
   static int sock_error(void) { return WSAGetLastError(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
   static int sock_error(void) { return errno; }
#endif

#include <SDL.h>

/* ---- Externs from runtime ---- */
extern uint32_t i_stat;
extern uint32_t i_mask;

/* Memory access (from memory.c) */
extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);
extern uint8_t  psx_read_byte(uint32_t addr);
extern void     psx_write_byte(uint32_t addr, uint8_t val);

/* ---- Server state ---- */
static sock_t s_listen  = SOCK_INVALID;
static sock_t s_client  = SOCK_INVALID;
static int    s_port    = 4370;

#define RECV_BUF_SIZE 8192
static char s_recv_buf[RECV_BUF_SIZE];
static int  s_recv_len = 0;

/* ---- Frame counter (set by record_frame caller) ---- */
static uint64_t s_frame_count = 0;

/* ---- CPU state pointer (set at init) ---- */
static CPUState *s_cpu = NULL;

/* ---- Pause / step ---- */
static volatile int s_paused     = 0;
static int          s_step_count = 0;
static uint32_t     s_run_to     = 0;

/* ---- Input override ---- */
static int s_input_override = -1;
static int s_input_frames   = 0;

/* ---- Ring buffer (heap-allocated) ---- */
static PSXFrameRecord *s_frame_history = NULL;
static uint64_t        s_history_count = 0;

/* ---- Snapshot regions (configurable via set_snapshot command) ---- */
static uint32_t s_snapshot_addrs[RAM_SNAPSHOT_REGIONS];
static int      s_snapshot_active[RAM_SNAPSHOT_REGIONS];

/* ---- Watchpoints ---- */
#define MAX_WATCHPOINTS 8
typedef struct {
    uint32_t addr;
    uint8_t  prev_val;
    int      active;
} Watchpoint;
static Watchpoint s_watchpoints[MAX_WATCHPOINTS];

/* ---- Write trace (Tier 1 reverse debugger) ----
 * Records every RAM write matching one of up to 8 configurable address ranges.
 * 1M-entry ring buffer, heap-allocated in debug_server_init(). */
#define WRITE_TRACE_CAP (1 << 20)  /* 1M entries = 32 MB */
typedef struct {
    uint64_t seq;        /* monotonic sequence number */
    uint32_t addr;       /* physical RAM address */
    uint32_t old_val;    /* pre-write value */
    uint32_t new_val;    /* post-write value */
    uint32_t ra;         /* $ra (caller return address) */
    uint32_t func_addr;  /* dispatch target (which recompiled function) */
    uint32_t frame;      /* VBlank frame number */
    uint8_t  width;      /* 1, 2, or 4 */
    uint8_t  pad[3];     /* align to 32 bytes */
} WriteTraceEntry;
static WriteTraceEntry *s_wtrace = NULL;
static uint64_t s_wtrace_seq  = 0;  /* total writes ever recorded */
static uint32_t s_wtrace_head = 0;

/* Multi-range filter: up to 8 [lo, hi) address ranges. */
#define WTRACE_MAX_RANGES 8
static struct { uint32_t lo, hi; } s_wtrace_ranges[WTRACE_MAX_RANGES];
static int s_wtrace_range_count = 0;

/* Function attribution global — set by psx_dispatch() before each call. */
uint32_t g_debug_current_func_addr = 0;

/* ---- Dispatch trace ring buffer ----
 * Records every dispatched function address for post-mortem analysis.
 * 64K entries, stack-allocated (256 KB). */
#define DISPATCH_TRACE_CAP (1 << 16)
static uint32_t s_dispatch_ring[DISPATCH_TRACE_CAP];
static uint64_t s_dispatch_seq = 0;

/* Unique dispatch set — tracks every unique function address ever dispatched.
 * Simple hash set with linear probing. */
#define DISPATCH_UNIQUE_CAP 4096
static uint32_t s_dispatch_unique[DISPATCH_UNIQUE_CAP];
static int s_dispatch_unique_count = 0;

static void dispatch_unique_add(uint32_t addr) {
    uint32_t idx = (addr >> 2) % DISPATCH_UNIQUE_CAP;
    for (int i = 0; i < DISPATCH_UNIQUE_CAP; i++) {
        uint32_t slot = (idx + i) % DISPATCH_UNIQUE_CAP;
        if (s_dispatch_unique[slot] == addr) return; /* already present */
        if (s_dispatch_unique[slot] == 0) {
            s_dispatch_unique[slot] = addr;
            s_dispatch_unique_count++;
            return;
        }
    }
}

static int dispatch_trace_contains(uint32_t target) {
    uint32_t idx = (target >> 2) % DISPATCH_UNIQUE_CAP;
    for (int i = 0; i < DISPATCH_UNIQUE_CAP; i++) {
        uint32_t slot = (idx + i) % DISPATCH_UNIQUE_CAP;
        if (s_dispatch_unique[slot] == target) return 1;
        if (s_dispatch_unique[slot] == 0) return 0;
    }
    return 0;
}

void debug_server_trace_dispatch(uint32_t func_addr) {
    s_dispatch_ring[s_dispatch_seq % DISPATCH_TRACE_CAP] = func_addr;
    s_dispatch_seq++;
    dispatch_unique_add(func_addr);
}

/* ---- MMIO write trace (separate ring buffer) ----
 * Records every write to 0x1F801xxx MMIO registers. Unconditional (no filtering).
 * 64K entries, heap-allocated in debug_server_init(). */
#define MMIO_TRACE_CAP (1 << 16)  /* 64K entries = 2 MB */
typedef struct {
    uint64_t seq;
    uint32_t addr;       /* 0x1F801xxx */
    uint32_t val;        /* value written */
    uint32_t func_addr;  /* dispatch target */
    uint32_t ra;         /* $ra */
    uint32_t frame;      /* VBlank frame */
    uint8_t  width;      /* 1, 2, or 4 */
    uint8_t  pad[3];
} MmioTraceEntry;
static MmioTraceEntry *s_mmio_trace = NULL;
static uint64_t s_mmio_trace_seq  = 0;
static uint32_t s_mmio_trace_head = 0;

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

/* ---- JSON helpers ---- */

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

/* ---- Send helpers ---- */

static void send_all_blocking(sock_t sock, const char *data, int len)
{
#ifdef _WIN32
    u_long mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
#endif
    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n > 0) { sent += n; continue; }
        break;
    }
#ifdef _WIN32
    mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    fcntl(sock, F_SETFL, flags);
#endif
}

void debug_server_send_line(const char *json)
{
    if (s_client == SOCK_INVALID) return;
    int len = (int)strlen(json);
    send_all_blocking(s_client, json, len);
    send_all_blocking(s_client, "\n", 1);
}

void debug_server_send_fmt(const char *fmt, ...)
{
    char buf[65536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    debug_server_send_line(buf);
}

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
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu}",
             id, (unsigned long long)s_frame_count);
}

static void handle_frame(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu}",
             id, (unsigned long long)s_frame_count);
}

static void handle_get_registers(int id, const char *json)
{
    (void)json;
    if (!s_cpu) { send_err(id, "no cpu"); return; }

    char *buf = (char *)malloc(4096);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 4096,
        "{\"id\":%d,\"ok\":true,\"frame\":%llu,"
        "\"gpr\":[",
        id, (unsigned long long)s_frame_count);

    for (int i = 0; i < 32; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, 4096 - pos, "\"0x%08X\"", s_cpu->gpr[i]);
    }

    pos += snprintf(buf + pos, 4096 - pos,
        "],\"hi\":\"0x%08X\",\"lo\":\"0x%08X\","
        "\"cop0_sr\":\"0x%08X\",\"cop0_cause\":\"0x%08X\",\"cop0_epc\":\"0x%08X\","
        "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
        "\"pc\":\"0x%08X\"}",
        s_cpu->hi, s_cpu->lo,
        s_cpu->cop0[12], s_cpu->cop0[13], s_cpu->cop0[14],
        i_stat, i_mask,
        s_cpu->pc);

    send_line(buf);
    free(buf);
}

static void handle_read_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 16384) len = 16384;  /* 16KB max */

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
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 256);
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;

    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > 256) chunk = 256;
        char hex[513];
        for (int i = 0; i < chunk; i++)
            snprintf(hex + i * 2, 3, "%02x", psx_read_byte(addr + offset + i));
        send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"offset\":%d,\"len\":%d,\"hex\":\"%s\"}",
                 id, addr + offset, offset, chunk, hex);
        offset += chunk;
    }
}

static void handle_write_ram(int id, const char *json)
{
    char addr_str[32], val_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    if (!json_get_str(json, "val", val_str, sizeof(val_str))) {
        send_err(id, "missing val"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    uint8_t val = (uint8_t)hex_to_u32(val_str);
    psx_write_byte(addr, val);
    send_ok(id);
}

static void handle_gpu_state(int id, const char *json)
{
    (void)json;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    uint32_t gpustat = gpu_read_gpustat();

    GpuDrawArea da;
    gpu_get_draw_area(&da);
    uint64_t nop, fill, draw, env, copy;
    gpu_get_gp0_stats(&nop, &fill, &draw, &env, &copy);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"display_x\":%d,\"display_y\":%d,"
             "\"width\":%d,\"height\":%d,"
             "\"disabled\":%d,"
             "\"gpustat\":\"0x%08X\","
             "\"gp0_writes\":%llu,"
             "\"gp0_nop\":%llu,\"gp0_fill\":%llu,\"gp0_draw\":%llu,\"gp0_env\":%llu,\"gp0_copy\":%llu,"
             "\"draw_area\":[%u,%u,%u,%u],"
             "\"draw_offset\":[%d,%d]}",
             id, di.display_x, di.display_y,
             di.width, di.height,
             di.disabled,
             gpustat,
             (unsigned long long)gpu_get_gp0_count(),
             (unsigned long long)nop, (unsigned long long)fill,
             (unsigned long long)draw, (unsigned long long)env,
             (unsigned long long)copy,
             da.left, da.top, da.right, da.bottom,
             da.offset_x, da.offset_y);
}

static void handle_irq_state(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
             "\"pending\":\"0x%08X\","
             "\"cop0_sr\":\"0x%08X\","
             "\"IEc\":%d,\"IM2\":%d,\"BEV\":%d,"
             "\"dpcr\":\"0x%08X\",\"dicr\":\"0x%08X\"}",
             id, i_stat, i_mask, i_stat & i_mask,
             s_cpu ? s_cpu->cop0[12] : 0,
             s_cpu ? (s_cpu->cop0[12] & 1) : 0,
             s_cpu ? ((s_cpu->cop0[12] >> 10) & 1) : 0,
             s_cpu ? ((s_cpu->cop0[12] >> 22) & 1) : 0,
             dma_get_dpcr(), dma_get_dicr());
}

/* GPU opcode counter — defined in gpu.c */
extern uint32_t gpu_get_opcode_count(uint8_t op);

extern int gpu_get_a0_count(void);
extern int gpu_get_a0_history(int index, int *x, int *y, int *w, int *h,
                              uint32_t *fw0, uint32_t *fw1, int *wcount);
extern int gpu_get_a0_extra(int index, uint32_t *func, uint32_t *sp, uint32_t *ra,
                            uint32_t *s1, uint32_t *stack10);

static void handle_a0_history(int id, const char *json)
{
    (void)json;
    int count = gpu_get_a0_count();
    /* Use dynamic allocation for large output */
    int bufsz = 65536;
    char *buf = (char*)malloc(bufsz);
    if (!buf) { send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"OOM\"}", id); return; }
    int pos = snprintf(buf, bufsz, "{\"id\":%d,\"ok\":true,\"count\":%d,\"uploads\":[", id, count);
    for (int i = 0; i < count && pos < bufsz - 500; i++) {
        int x, y, w, h, wcount;
        uint32_t fw0, fw1, func, sp, ra, s1, stk[10];
        gpu_get_a0_history(i, &x, &y, &w, &h, &fw0, &fw1, &wcount);
        gpu_get_a0_extra(i, &func, &sp, &ra, &s1, stk);
        pos += snprintf(buf + pos, bufsz - pos,
            "%s{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
            "\"fw0\":\"0x%08X\",\"fw1\":\"0x%08X\",\"words\":%d,"
            "\"func\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"s1\":\"0x%08X\","
            "\"stk\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\"]}",
            i ? "," : "", i, x, y, w, h, fw0, fw1, wcount,
            func, sp, ra, s1,
            stk[0], stk[1], stk[2], stk[3], stk[4], stk[5], stk[6], stk[7],
            stk[8], stk[9]);
    }
    pos += snprintf(buf + pos, bufsz - pos, "]}");
    send_fmt("%s", buf);
    free(buf);
}

extern int gpu_get_c0_count(void);
extern int gpu_get_c0_history(int index, int *x, int *y, int *w, int *h,
                              uint32_t *func, uint32_t *sp, uint32_t *s1,
                              uint32_t *fw0, uint32_t *fw1, int *rcount);

static void handle_c0_history(int id, const char *json)
{
    (void)json;
    int count = gpu_get_c0_count();
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"count\":%d,\"reads\":[", id, count);
    for (int i = 0; i < count && pos < (int)sizeof(buf) - 300; i++) {
        int x, y, w, h, rcount;
        uint32_t func, sp, s1, fw0, fw1;
        gpu_get_c0_history(i, &x, &y, &w, &h, &func, &sp, &s1, &fw0, &fw1, &rcount);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
            "\"func\":\"0x%08X\",\"sp\":\"0x%08X\",\"s1\":\"0x%08X\","
            "\"fw0\":\"0x%08X\",\"fw1\":\"0x%08X\",\"reads\":%d}",
            i ? "," : "", i, x, y, w, h, func, sp, s1, fw0, fw1, rcount);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

static void handle_gpu_opcodes(int id, const char *json)
{
    (void)json;
    /* Report non-zero GP0 opcode counts */
    char buf[4096];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"opcodes\":{", id);
    int first = 1;
    for (int i = 0; i < 256; i++) {
        uint32_t cnt = gpu_get_opcode_count((uint8_t)i);
        if (cnt > 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%02X\":%u",
                           first ? "" : ",", i, cnt);
            first = 0;
        }
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
    send_fmt("%s", buf);
}

static void handle_capture_quads(int id, const char *json)
{
    (void)json;
    gpu_arm_shaded_quad_capture();
    send_ok(id);
}

static void handle_get_quads(int id, const char *json)
{
    (void)json;
    const GpuSqCapEntry *entries;
    int count = gpu_get_shaded_quad_capture(&entries);
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"count\":%d,\"quads\":[", id, count);
    for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++) {
        const GpuSqCapEntry *e = &entries[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"v\":[%d,%d,%d,%d,%d,%d,%d,%d],\"c\":[\"0x%06X\",\"0x%06X\",\"0x%06X\",\"0x%06X\"]}",
            i ? "," : "",
            e->vx[0], e->vy[0], e->vx[1], e->vy[1],
            e->vx[2], e->vy[2], e->vx[3], e->vy[3],
            e->color[0], e->color[1], e->color[2], e->color[3]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

extern uint64_t gte_get_exec_count(void);

static void handle_gte_state(int id, const char *json)
{
    (void)json;
    if (!s_cpu) { send_err(id, "no cpu"); return; }
    char buf[2048];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"gte_exec\":%llu,\"gte_ctrl\":[",
                       id, (unsigned long long)gte_get_exec_count());
    for (int i = 0; i < 32; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%08X\"",
                       i ? "," : "", s_cpu->gte_ctrl[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"gte_data\":[");
    for (int i = 0; i < 32; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%08X\"",
                       i ? "," : "", s_cpu->gte_data[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

static void handle_sio_state(int id, const char *json)
{
    (void)json;
    extern int sio_get_mc_probe_count(void);
    extern int sio_get_mc_ack_count(void);
    extern int sio_get_mc_cmd_count(void);
    extern int sio_get_tx_writes(void);
    extern int sio_get_tx_gated(void);
    extern uint16_t sio_get_last_ctrl_on_tx(void);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"sio_stat\":\"0x%04X\","
             "\"sio_ctrl\":\"0x%04X\","
             "\"sio_rx\":\"0x%02X\","
             "\"pad_buttons\":\"0x%04X\","
             "\"mc_probes\":%d,"
             "\"mc_acks\":%d,"
             "\"mc_cmds\":%d,"
             "\"tx_writes\":%d,"
             "\"tx_gated\":%d,"
             "\"last_ctrl_on_tx\":\"0x%04X\"}",
             id,
             (uint16_t)sio_read(0x1F801044),
             (uint16_t)sio_read(0x1F80104A),
             (uint8_t)sio_read(0x1F801040),
             sio_get_pad_buttons(),
             sio_get_mc_probe_count(),
             sio_get_mc_ack_count(),
             sio_get_mc_cmd_count(),
             sio_get_tx_writes(),
             sio_get_tx_gated(),
             sio_get_last_ctrl_on_tx());
}

static void handle_watch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
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
        send_err(id, "missing addr"); return;
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
        send_err(id, "missing buttons"); return;
    }
    s_input_override = (int)hex_to_u32(val_str);
    s_input_frames = 0;
    send_ok(id);
}

static void handle_press(int id, const char *json)
{
    int buttons = json_get_int(json, "buttons", -1);
    int frames  = json_get_int(json, "frames", 2);
    if (buttons < 0) { send_err(id, "missing buttons"); return; }
    s_input_override = buttons;
    s_input_frames   = frames;
    send_ok(id);
}

static void handle_clear_input(int id, const char *json)
{
    (void)json;
    s_input_override = -1;
    s_input_frames   = 0;
    send_ok(id);
}

static void handle_pause(int id, const char *json)
{
    (void)json;
    s_paused = 1;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":true,\"frame\":%llu}",
             id, (unsigned long long)s_frame_count);
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
    s_paused = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"stepping\":%d}", id, n);
}

static void handle_run_to_frame(int id, const char *json)
{
    int target = json_get_int(json, "frame", 0);
    if (target <= (int)s_frame_count) {
        send_err(id, "target frame already passed"); return;
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
        send_err(id, "frame not in buffer"); return;
    }

    uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
    const PSXFrameRecord *r = &s_frame_history[idx];
    if (r->frame_number != (uint32_t)f) {
        send_err(id, "frame record mismatch"); return;
    }

    char *buf = (char *)malloc(8192);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 8192,
        "{\"id\":%d,\"ok\":true,"
        "\"frame\":%u,\"verify_pass\":%d,\"diff_count\":%d,"
        "\"cop0_sr\":\"0x%08X\",\"cop0_cause\":\"0x%08X\",\"cop0_epc\":\"0x%08X\","
        "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
        "\"display\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"disabled\":%d},"
        "\"pad_buttons\":\"0x%04X\","
        "\"sio_stat\":\"0x%04X\",\"sio_ctrl\":\"0x%04X\","
        "\"dispatch_count\":%u,"
        "\"total_dispatches\":%llu,"
        "\"last_func\":\"%s\","
        "\"gpr\":[",
        id, r->frame_number, r->verify_pass, r->diff_count,
        r->cop0_sr, r->cop0_cause, r->cop0_epc,
        r->i_stat, r->i_mask,
        r->display_area_x, r->display_area_y, r->display_w, r->display_h,
        r->display_disabled,
        r->pad_buttons,
        r->sio_stat, r->sio_ctrl,
        r->dispatch_count,
        (unsigned long long)r->total_dispatches,
        r->last_func);

    for (int i = 0; i < 32; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, 8192 - pos, "\"0x%08X\"", r->gpr[i]);
    }

    pos += snprintf(buf + pos, 8192 - pos, "]}");
    send_line(buf);
    free(buf);
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
            pos += snprintf(buf + pos, 128, "{\"frame\":%d,\"available\":false}", f);
            continue;
        }
        uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
        const PSXFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 128, "{\"frame\":%d,\"available\":false}", f);
            continue;
        }

        pos += snprintf(buf + pos, 256,
            "{\"frame\":%u,\"verify\":%d,"
            "\"sr\":\"0x%08X\",\"i_stat\":\"0x%08X\","
            "\"pad\":\"0x%04X\"}",
            r->frame_number, r->verify_pass,
            r->cop0_sr, r->i_stat,
            r->pad_buttons);
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

        pos += snprintf(buf + pos, 320,
            "{\"f\":%u,\"v\":%d,"
            "\"sr\":\"0x%08X\",\"ist\":\"0x%08X\",\"imk\":\"0x%08X\","
            "\"pad\":\"0x%04X\",\"dc\":%u}",
            r->frame_number, r->verify_pass,
            r->cop0_sr, r->i_stat, r->i_mask,
            r->pad_buttons, r->dispatch_count);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_first_failure(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    for (uint64_t f = oldest; f < s_history_count; f++) {
        uint32_t idx = (uint32_t)(f % FRAME_HISTORY_CAP);
        const PSXFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number == (uint32_t)f && r->verify_pass == 0) {
            send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u,\"diff_count\":%d}",
                     id, r->frame_number, r->diff_count);
            return;
        }
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":-1,\"message\":\"no failures found\"}", id);
}

static void handle_read_frame_ram(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }
    if (!s_frame_history) { send_err(id, "ring buffer not allocated"); return; }

    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 128) len = 128;

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
        send_err(id, "frame not in buffer"); return;
    }

    uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
    const PSXFrameRecord *r = &s_frame_history[idx];

    /* Find matching snapshot region */
    char hex[257];
    int found = 0;
    for (int i = 0; i < RAM_SNAPSHOT_REGIONS; i++) {
        if (r->snapshot_addr[i] == 0) continue;
        if (addr >= r->snapshot_addr[i] && addr + len <= r->snapshot_addr[i] + RAM_SNAPSHOT_SIZE) {
            uint32_t off = addr - r->snapshot_addr[i];
            for (int j = 0; j < len; j++)
                snprintf(hex + j * 2, 3, "%02x", r->snapshot_data[i][off + j]);
            found = 1;
            break;
        }
    }

    if (!found) {
        send_err(id, "address not in any snapshot region for this frame"); return;
    }

    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%d,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"%s\"}",
             id, f, addr, len, hex);
}

static void handle_set_snapshot(int id, const char *json)
{
    int slot = json_get_int(json, "slot", -1);
    if (slot < 0 || slot >= RAM_SNAPSHOT_REGIONS) {
        send_err(id, "invalid slot (0-3)"); return;
    }
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    s_snapshot_addrs[slot] = addr;
    s_snapshot_active[slot] = (addr != 0);
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"addr\":\"0x%08X\"}", id, slot, addr);
}

static void handle_get_snapshots(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"snapshots\":["
             "{\"slot\":0,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":1,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":2,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":3,\"addr\":\"0x%08X\",\"active\":%d}]}",
             id,
             s_snapshot_addrs[0], s_snapshot_active[0],
             s_snapshot_addrs[1], s_snapshot_active[1],
             s_snapshot_addrs[2], s_snapshot_active[2],
             s_snapshot_addrs[3], s_snapshot_active[3]);
}

static void handle_screenshot_file(int id, const char *json)
{
    (void)json;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.width == 0 || di.height == 0) {
        send_err(id, "display disabled"); return;
    }

    const uint16_t *vram = gpu_get_vram();
    uint32_t w = di.width;  if (w > 640) w = 640;
    uint32_t h = di.height; if (h > 512) h = 512;

    const char *path = "psx_screenshot.bmp";
    FILE *f = fopen(path, "wb");
    if (!f) { send_err(id, "cannot open file"); return; }

    /* BMP row stride: 3 bytes/pixel, padded to 4-byte boundary */
    uint32_t row_stride = (w * 3 + 3) & ~3u;
    uint32_t pixel_size = row_stride * h;
    uint32_t file_size = 14 + 40 + pixel_size;

    /* BITMAPFILEHEADER (14 bytes) */
    uint8_t bfh[14] = {0};
    bfh[0] = 'B'; bfh[1] = 'M';
    bfh[2] = file_size & 0xFF; bfh[3] = (file_size >> 8) & 0xFF;
    bfh[4] = (file_size >> 16) & 0xFF; bfh[5] = (file_size >> 24) & 0xFF;
    bfh[10] = 54; /* offset to pixel data */
    fwrite(bfh, 1, 14, f);

    /* BITMAPINFOHEADER (40 bytes) */
    uint8_t bih[40] = {0};
    bih[0] = 40; /* header size */
    bih[4] = w & 0xFF; bih[5] = (w >> 8) & 0xFF;
    /* Height negative = top-down */
    int32_t neg_h = -(int32_t)h;
    memcpy(bih + 8, &neg_h, 4);
    bih[12] = 1;  /* planes */
    bih[14] = 24; /* bits per pixel */
    fwrite(bih, 1, 40, f);

    /* Pixel data: top-down, BGR */
    uint8_t *row = (uint8_t *)malloc(row_stride);
    for (uint32_t y = 0; y < h; y++) {
        memset(row, 0, row_stride);
        uint32_t vy = (di.display_y + y) & 511;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t vx = (di.display_x + x) & 1023;
            uint16_t c = vram[vy * 1024 + vx];
            uint8_t r = (c & 0x1F) << 3;
            uint8_t g = ((c >> 5) & 0x1F) << 3;
            uint8_t b = ((c >> 10) & 0x1F) << 3;
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row, 1, row_stride, f);
    }
    free(row);
    fclose(f);

    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%u,\"height\":%u}",
             id, path, w, h);
}

static void handle_screenshot(int id, const char *json)
{
    (void)json;
    /* Read display area from GPU and encode as hex RGB555 */
    GpuDisplayInfo di;
    gpu_get_display_info(&di);

    if (di.disabled || di.width == 0 || di.height == 0) {
        send_err(id, "display disabled"); return;
    }

    const uint16_t *vram = gpu_get_vram();
    uint32_t w = di.width;
    uint32_t h = di.height;
    if (w > 640) w = 640;
    if (h > 512) h = 512;

    /* Send metadata first */
    send_fmt("{\"id\":%d,\"ok\":true,\"width\":%u,\"height\":%u,\"format\":\"rgb555\"}",
             id, w, h);

    /* Send rows as hex lines */
    char *hex = (char *)malloc(w * 4 + 32);
    if (!hex) return;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t vy = (di.display_y + y) & 511;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t vx = (di.display_x + x) & 1023;
            uint16_t pixel = vram[vy * 1024 + vx];
            snprintf(hex + x * 4, 5, "%04x", pixel);
        }
        send_fmt("{\"row\":%u,\"hex\":\"%s\"}", y, hex);
    }
    free(hex);
}

static void handle_vram_peek(int id, const char *json)
{
    int x = json_get_int(json, "x", 0);
    int y = json_get_int(json, "y", 0);
    int w = json_get_int(json, "w", 8);
    int h = json_get_int(json, "h", 1);
    if (w > 64) w = 64;
    if (h > 64) h = 64;
    char hex[64*64*4+1];
    int pos = 0;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t p = gpu_vram_peek(x + col, y + row);
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%04x", p);
        }
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"hex\":\"%s\"}",
             id, x, y, w, h, hex);
}

/* ---- Write trace: hook + handlers (Tier 1 reverse debugger) ---- */
extern CPUState *debug_cpu_ptr;

/* Record a single write into the RAM trace ring buffer. */
static void wtrace_record(uint32_t phys, uint32_t old_val, uint32_t new_val, uint8_t width)
{
    if (!s_wtrace) return;
    uint32_t ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    WriteTraceEntry *e = &s_wtrace[s_wtrace_head];
    e->seq       = s_wtrace_seq++;
    e->addr      = phys;
    e->old_val   = old_val;
    e->new_val   = new_val;
    e->ra        = ra;
    e->func_addr = g_debug_current_func_addr;
    e->frame     = (uint32_t)s_frame_count;
    e->width     = width;
    s_wtrace_head = (s_wtrace_head + 1) % WRITE_TRACE_CAP;
}

/* Multi-range check called from memory.c write paths.
 * Iterates up to 8 ranges; records if any match. */
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width)
{
    if (s_wtrace_range_count == 0) return;
    for (int i = 0; i < s_wtrace_range_count; i++) {
        if (phys >= s_wtrace_ranges[i].lo && phys < s_wtrace_ranges[i].hi) {
            wtrace_record(phys, old_val, new_val, width);
            return;
        }
    }
}

/* MMIO write trace — called from memory.c mmio_write32/16/8. */
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width)
{
    if (!s_mmio_trace) return;
    uint32_t ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
    MmioTraceEntry *e = &s_mmio_trace[s_mmio_trace_head];
    e->seq       = s_mmio_trace_seq++;
    e->addr      = addr;
    e->val       = val;
    e->func_addr = g_debug_current_func_addr;
    e->ra        = ra;
    e->frame     = (uint32_t)s_frame_count;
    e->width     = width;
    s_mmio_trace_head = (s_mmio_trace_head + 1) % MMIO_TRACE_CAP;
}

static void handle_wtrace_range(int id, const char *json)
{
    /* Backward compat: sets slot 0, clears all other slots. */
    char lo_str[32], hi_str[32];
    if (!json_get_str(json, "lo", lo_str, sizeof(lo_str))) { send_err(id, "missing lo"); return; }
    if (!json_get_str(json, "hi", hi_str, sizeof(hi_str))) { send_err(id, "missing hi"); return; }
    uint32_t lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    uint32_t hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;
    s_wtrace_ranges[0].lo = lo;
    s_wtrace_ranges[0].hi = hi;
    s_wtrace_range_count = (lo != hi) ? 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
             id, lo, hi);
}

static void handle_wtrace_add(int id, const char *json)
{
    if (s_wtrace_range_count >= WTRACE_MAX_RANGES) {
        send_err(id, "max ranges reached (8)"); return;
    }
    char lo_str[32], hi_str[32];
    if (!json_get_str(json, "lo", lo_str, sizeof(lo_str))) { send_err(id, "missing lo"); return; }
    if (!json_get_str(json, "hi", hi_str, sizeof(hi_str))) { send_err(id, "missing hi"); return; }
    int slot = s_wtrace_range_count++;
    s_wtrace_ranges[slot].lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    s_wtrace_ranges[slot].hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
             id, slot, s_wtrace_ranges[slot].lo, s_wtrace_ranges[slot].hi);
}

static void handle_wtrace_del(int id, const char *json)
{
    int slot = json_get_int(json, "slot", -1);
    if (slot < 0 || slot >= s_wtrace_range_count) {
        send_err(id, "invalid slot"); return;
    }
    /* Compact: shift remaining slots down. */
    for (int i = slot; i < s_wtrace_range_count - 1; i++)
        s_wtrace_ranges[i] = s_wtrace_ranges[i + 1];
    s_wtrace_range_count--;
    send_ok(id);
}

static void handle_wtrace_ranges(int id, const char *json)
{
    (void)json;
    const size_t BUF_SZ = 2048;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"count\":%d,\"ranges\":[", id, s_wtrace_range_count);
    for (int i = 0; i < s_wtrace_range_count; i++) {
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"slot\":%d,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}",
                        (i == 0) ? "" : ",",
                        i, s_wtrace_ranges[i].lo, s_wtrace_ranges[i].hi);
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

static void handle_wtrace_stats(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_wtrace_seq <= WRITE_TRACE_CAP) ? 0 : s_wtrace_seq - WRITE_TRACE_CAP;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%d,"
             "\"oldest_seq\":%llu,\"newest_seq\":%llu,\"ranges\":%d}",
             id, (unsigned long long)s_wtrace_seq, WRITE_TRACE_CAP,
             (unsigned long long)oldest,
             (unsigned long long)(s_wtrace_seq > 0 ? s_wtrace_seq - 1 : 0),
             s_wtrace_range_count);
}

static void handle_wtrace_clear(int id, const char *json)
{
    (void)json;
    s_wtrace_seq = 0;
    s_wtrace_head = 0;
    if (s_wtrace) memset(s_wtrace, 0, (size_t)WRITE_TRACE_CAP * sizeof(WriteTraceEntry));
    send_ok(id);
}

static void handle_wtrace_dump(int id, const char *json)
{
    if (!s_wtrace) { send_err(id, "trace not initialized"); return; }

    /* Optional post-hoc address filter. */
    char lo_str[32], hi_str[32];
    uint32_t filter_lo = 0, filter_hi = 0xFFFFFFFFu;
    if (json_get_str(json, "addr_lo", lo_str, sizeof(lo_str)))
        filter_lo = hex_to_u32(lo_str) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", hi_str, sizeof(hi_str)))
        filter_hi = hex_to_u32(hi_str) & 0x1FFFFFFFu;

    uint64_t total = s_wtrace_seq;
    uint32_t avail = (total < WRITE_TRACE_CAP) ? (uint32_t)total : WRITE_TRACE_CAP;
    uint32_t start = (total < WRITE_TRACE_CAP) ? 0 : s_wtrace_head;

    /* Dynamically size buffer: each JSON entry is ~160 bytes max.
     * Cap output at 8192 entries to avoid multi-MB responses. */
    const uint32_t MAX_OUT = 8192;
    const size_t BUF_SZ = 2 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)total, avail);
    for (uint32_t i = 0; i < avail && emitted < MAX_OUT && pos < BUF_SZ - 256; i++) {
        uint32_t idx = (start + i) % WRITE_TRACE_CAP;
        WriteTraceEntry *e = &s_wtrace[idx];
        if (e->addr < filter_lo || e->addr >= filter_hi) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"old\":\"0x%08X\","
                        "\"new\":\"0x%08X\",\"ra\":\"0x%08X\",\"func\":\"0x%08X\","
                        "\"frame\":%u,\"w\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->old_val, e->new_val, e->ra, e->func_addr,
                        e->frame, (unsigned)e->width);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%u}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

/* ---- MMIO trace handlers ---- */

static void handle_mmio_dump(int id, const char *json)
{
    if (!s_mmio_trace) { send_err(id, "mmio trace not initialized"); return; }

    /* Optional address filter. */
    char addr_str[32];
    uint32_t filter_addr = 0;
    int has_filter = json_get_str(json, "addr", addr_str, sizeof(addr_str)) != NULL;
    if (has_filter) filter_addr = hex_to_u32(addr_str);

    uint64_t total = s_mmio_trace_seq;
    uint32_t avail = (total < MMIO_TRACE_CAP) ? (uint32_t)total : MMIO_TRACE_CAP;
    uint32_t start = (total < MMIO_TRACE_CAP) ? 0 : s_mmio_trace_head;

    const uint32_t MAX_OUT = 8192;
    const size_t BUF_SZ = 2 * 1024 * 1024;
    char *buf = (char *)malloc(BUF_SZ);
    if (!buf) { send_err(id, "oom"); return; }
    size_t pos = 0;
    uint32_t emitted = 0;
    pos += snprintf(buf + pos, BUF_SZ - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
                    id, (unsigned long long)total, avail);
    for (uint32_t i = 0; i < avail && emitted < MAX_OUT && pos < BUF_SZ - 256; i++) {
        uint32_t idx = (start + i) % MMIO_TRACE_CAP;
        MmioTraceEntry *e = &s_mmio_trace[idx];
        if (has_filter && e->addr != filter_addr) continue;
        pos += snprintf(buf + pos, BUF_SZ - pos,
                        "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"val\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"ra\":\"0x%08X\",\"frame\":%u,\"w\":%u}",
                        (emitted == 0) ? "" : ",",
                        (unsigned long long)e->seq,
                        e->addr, e->val, e->func_addr, e->ra,
                        e->frame, (unsigned)e->width);
        emitted++;
    }
    pos += snprintf(buf + pos, BUF_SZ - pos, "],\"emitted\":%u}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void handle_mmio_clear(int id, const char *json)
{
    (void)json;
    s_mmio_trace_seq = 0;
    s_mmio_trace_head = 0;
    if (s_mmio_trace) memset(s_mmio_trace, 0, (size_t)MMIO_TRACE_CAP * sizeof(MmioTraceEntry));
    send_ok(id);
}

static void handle_quit(int id, const char *json)
{
    (void)json;
    send_ok(id);
    debug_server_shutdown();
    exit(0);
}

/* ---- Command dispatch table ---- */

/* dispatch_check: check if a specific address was ever dispatched */
static void handle_dispatch_check(int id, const char *json) {
    char abuf[32] = {0};
    json_get_str(json, "addr", abuf, sizeof(abuf));
    uint32_t addr = (uint32_t)strtoul(abuf, NULL, 0);
    int found = dispatch_trace_contains(addr);
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"found\":%s,\"total\":%llu}\n",
        id, addr, found ? "true" : "false", (unsigned long long)s_dispatch_seq);
}

/* dispatch_tail: dump the last N dispatched function addresses */
static void handle_dispatch_tail(int id, const char *json) {
    int count = 64;
    { /* try to parse count */
        char buf[32];
        if (json_get_str(json, "count", buf, sizeof(buf)))
            count = atoi(buf);
    }
    if (count > 4096) count = 4096;
    if ((uint64_t)count > s_dispatch_seq) count = (int)s_dispatch_seq;

    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"count\":%d,\"addrs\":[",
        id, (unsigned long long)s_dispatch_seq, count);
    uint64_t start = s_dispatch_seq - count;
    for (int i = 0; i < count; i++) {
        uint32_t a = s_dispatch_ring[(start + i) % DISPATCH_TRACE_CAP];
        if (i > 0) debug_server_send_fmt(",");
        debug_server_send_fmt("\"0x%08X\"", a);
    }
    debug_server_send_fmt("]}\n");
}

typedef void (*CmdHandler)(int id, const char *json);
typedef struct { const char *name; CmdHandler handler; } CmdEntry;

static const CmdEntry s_commands[] = {
    { "ping",              handle_ping },
    { "frame",             handle_frame },
    { "get_registers",     handle_get_registers },
    { "read_ram",          handle_read_ram },
    { "dump_ram",          handle_dump_ram },
    { "write_ram",         handle_write_ram },
    { "gpu_state",         handle_gpu_state },
    { "vram_peek",         handle_vram_peek },
    { "irq_state",         handle_irq_state },
    { "sio_state",         handle_sio_state },
    { "watch",             handle_watch },
    { "unwatch",           handle_unwatch },
    { "wtrace_range",      handle_wtrace_range },
    { "wtrace_add",        handle_wtrace_add },
    { "wtrace_del",        handle_wtrace_del },
    { "wtrace_ranges",     handle_wtrace_ranges },
    { "wtrace_dump",       handle_wtrace_dump },
    { "wtrace_clear",      handle_wtrace_clear },
    { "wtrace_stats",      handle_wtrace_stats },
    { "mmio_dump",         handle_mmio_dump },
    { "mmio_clear",        handle_mmio_clear },
    { "set_input",         handle_set_input },
    { "press",             handle_press },
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
    { "read_frame_ram",    handle_read_frame_ram },
    { "set_snapshot",      handle_set_snapshot },
    { "get_snapshots",     handle_get_snapshots },
    { "screenshot",        handle_screenshot },
    { "screenshot_file",   handle_screenshot_file },
    { "gpu_opcodes",       handle_gpu_opcodes },
    { "a0_history",        handle_a0_history },
    { "c0_history",        handle_c0_history },
    { "capture_quads",     handle_capture_quads },
    { "get_quads",         handle_get_quads },
    { "gte_state",         handle_gte_state },
    { "quit",              handle_quit },
    { "dispatch_check",    handle_dispatch_check },
    { "dispatch_tail",     handle_dispatch_tail },
    { NULL, NULL }
};

static void process_command(const char *line)
{
    char cmd[64];
    if (!json_get_str(line, "cmd", cmd, sizeof(cmd))) {
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

    /* Oracle commands (find_first_divergence, emu_read_ram, etc.) */
#if defined(ENABLE_DUCKSTATION_ORACLE) || defined(ENABLE_BEETLE_PSX_ORACLE)
    {
        extern int psx_oracle_handle_cmd(const char *cmd, int id, const char *json);
        if (psx_oracle_handle_cmd(cmd, id, line)) return;
    }
#endif

    send_err(id, "unknown command");
}

/* ---- Public API ---- */

/* Extended init that accepts a CPU state pointer for register queries. */
static CPUState *s_init_cpu = NULL;
CPUState *debug_cpu_ptr = NULL; /* Global, used by memory.c watchpoints */

void debug_server_set_cpu(CPUState *cpu)
{
    s_cpu = cpu;
    debug_cpu_ptr = cpu;
}

void debug_server_init(int port)
{
    if (port > 0) s_port = port;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == SOCK_INVALID) return;

    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)s_port);

    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
        return;
    }

    listen(s_listen, 1);
    set_nonblocking(s_listen);

    if (!s_frame_history) {
        s_frame_history = (PSXFrameRecord *)calloc(FRAME_HISTORY_CAP, sizeof(PSXFrameRecord));
    }
    s_history_count = 0;

    /* Tier 1: heap-allocate write trace ring buffer (32 MB). */
    if (!s_wtrace) {
        s_wtrace = (WriteTraceEntry *)calloc(WRITE_TRACE_CAP, sizeof(WriteTraceEntry));
    }
    s_wtrace_seq = 0;
    s_wtrace_head = 0;

    /* Phase 4.5: watch EB4 area + DF8/DFC area + EvCB slot 1 status + state machine
     * + spiral texture buffer. */
    s_wtrace_ranges[0].lo = 0x00079EB0u;
    s_wtrace_ranges[0].hi = 0x00079EC0u;
    s_wtrace_ranges[1].lo = 0x00079DF0u;
    s_wtrace_ranges[1].hi = 0x00079E04u;
    s_wtrace_ranges[2].lo = 0x0000E044u;  /* EvCB slot 1 (class+status+spec) */
    s_wtrace_ranges[2].hi = 0x0000E054u;
    s_wtrace_ranges[3].lo = 0x00066940u;  /* shell state machine 0x80066940 */
    s_wtrace_ranges[3].hi = 0x00066954u;
    s_wtrace_ranges[4].lo = 0x001B6810u;  /* spiral texture buffer (heap @ 0x801B6814) */
    s_wtrace_ranges[4].hi = 0x001B6830u;
    s_wtrace_range_count = 5;

    /* Tier 1: heap-allocate MMIO trace ring buffer (2 MB). */
    if (!s_mmio_trace) {
        s_mmio_trace = (MmioTraceEntry *)calloc(MMIO_TRACE_CAP, sizeof(MmioTraceEntry));
    }
    s_mmio_trace_seq = 0;
    s_mmio_trace_head = 0;

    memset(s_watchpoints, 0, sizeof(s_watchpoints));
    memset(s_snapshot_addrs, 0, sizeof(s_snapshot_addrs));
    memset(s_snapshot_active, 0, sizeof(s_snapshot_active));
}

void debug_server_poll(void)
{
    if (s_listen == SOCK_INVALID) return;

    if (s_client == SOCK_INVALID) {
        struct sockaddr_in caddr;
        int clen = sizeof(caddr);
        sock_t c = accept(s_listen, (struct sockaddr *)&caddr, &clen);
        if (c != SOCK_INVALID) {
            s_client = c;
            set_nonblocking(s_client);
            s_recv_len = 0;
        }
        return;
    }

    int space = RECV_BUF_SIZE - s_recv_len - 1;
    if (space > 0) {
        int n = recv(s_client, s_recv_buf + s_recv_len, space, 0);
        if (n > 0) {
            s_recv_len += n;
            s_recv_buf[s_recv_len] = '\0';
        } else if (n == 0) {
            sock_close(s_client);
            s_client = SOCK_INVALID;
            s_paused = 0;
            return;
        } else {
            int err = sock_error();
#ifdef _WIN32
            if (err != WSAEWOULDBLOCK) {
#else
            if (err != EAGAIN && err != EWOULDBLOCK) {
#endif
                sock_close(s_client);
                s_client = SOCK_INVALID;
                s_paused = 0;
                s_input_override = -1;
                return;
            }
        }
    }

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
    if (!s_frame_history) return;
    if (!s_cpu) return;

    uint32_t idx = (uint32_t)(s_frame_count % FRAME_HISTORY_CAP);
    PSXFrameRecord *r = &s_frame_history[idx];

    r->frame_number = (uint32_t)s_frame_count;
    r->verify_pass = -1;
    r->diff_count  = 0;
    memset(r->diffs, 0, sizeof(r->diffs));

    /* MIPS CPU state */
    memcpy(r->gpr, s_cpu->gpr, sizeof(r->gpr));
    r->hi = s_cpu->hi;
    r->lo = s_cpu->lo;
    r->cop0_sr    = s_cpu->cop0[12];
    r->cop0_cause = s_cpu->cop0[13];
    r->cop0_epc   = s_cpu->cop0[14];

    /* Interrupt state */
    r->i_stat = i_stat;
    r->i_mask = i_mask;

    /* GPU display state */
    {
        GpuDisplayInfo di;
        gpu_get_display_info(&di);
        r->display_area_x = (uint16_t)di.display_x;
        r->display_area_y = (uint16_t)di.display_y;
        r->display_w      = (uint16_t)di.width;
        r->display_h      = (uint16_t)di.height;
        r->display_disabled = di.disabled;
    }

    /* SIO state */
    r->pad_buttons = 0; /* TODO: expose from sio.c */
    r->sio_stat = (uint16_t)sio_read(0x1F801044);
    r->sio_ctrl = (uint16_t)sio_read(0x1F80104A);

    /* Timing */
    r->dispatch_count = 0; /* filled externally if needed */
    r->total_dispatches = s_frame_count;

    /* Snapshot regions */
    for (int i = 0; i < RAM_SNAPSHOT_REGIONS; i++) {
        r->snapshot_addr[i] = s_snapshot_addrs[i];
        if (s_snapshot_active[i] && s_snapshot_addrs[i] != 0) {
            for (int j = 0; j < RAM_SNAPSHOT_SIZE; j++)
                r->snapshot_data[i][j] = psx_read_byte(s_snapshot_addrs[i] + j);
        } else {
            memset(r->snapshot_data[i], 0, RAM_SNAPSHOT_SIZE);
        }
    }

    /* Game-specific data */
    memset(r->game_data, 0, sizeof(r->game_data));

    /* Last function */
    strcpy(r->last_func, "(no tracking)");

    s_history_count = s_frame_count + 1;
    s_frame_count++;

    /* Step mode */
    if (s_step_count > 0) {
        s_step_count--;
        if (s_step_count == 0) {
            s_paused = 1;
            send_fmt("{\"event\":\"step_done\",\"frame\":%llu}",
                     (unsigned long long)(s_frame_count - 1));
        }
    }

    /* Run-to-frame */
    if (s_run_to > 0 && s_frame_count - 1 >= s_run_to) {
        s_paused = 1;
        s_run_to = 0;
        send_fmt("{\"event\":\"run_to_done\",\"frame\":%llu}",
                 (unsigned long long)(s_frame_count - 1));
    }
}

void debug_server_wait_if_paused(void)
{
    while (s_paused) {
        debug_server_poll();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) exit(0);
        }

        SDL_Delay(5);
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
                     "\"frame\":%llu}",
                     s_watchpoints[i].addr,
                     s_watchpoints[i].prev_val, cur,
                     (unsigned long long)s_frame_count);
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
}

int debug_server_is_connected(void)
{
    return s_client != SOCK_INVALID;
}

int debug_server_get_input_override(void)
{
    if (s_input_override >= 0 && s_input_frames > 0) {
        if (--s_input_frames == 0)
            s_input_override = -1;
    }
    return s_input_override;
}
