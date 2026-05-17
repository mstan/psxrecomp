/* beetle_debug_server.c — TCP debug server for psx-beetle.
 *
 * Mirrors the wire protocol of psx-runtime's runtime/src/debug_server.c
 * for the commands that apply to a libretro core: ping, read_ram, press,
 * set_input, clear_input, pad_status, screenshot_file, sio_trace_*,
 * wtrace_*, fntrace_*.
 *
 * Default port: 4380 (compile-time DEFAULT_DEBUG_PORT).
 * JSON-over-newline protocol, same as recomp side.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#ifndef DEFAULT_DEBUG_PORT
#error DEFAULT_DEBUG_PORT must be defined by the beetle runtime target.
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
#endif

/* ---- Externs from beetle_libretro.cpp ---- */
extern uint8_t  beetle_read_byte(uint32_t phys);
extern uint32_t beetle_read_word(uint32_t phys);
extern void     beetle_get_ram(uint8_t *out_2mb);
extern uint16_t beetle_get_pad(void);
extern int      beetle_get_framebuffer(uint32_t **out_pixels,
                                        unsigned *out_w, unsigned *out_h);
extern uint32_t beetle_get_frame_count(void);

/* SIO trace */
extern uint32_t beetle_get_sio_trace(uint32_t *out_seq, uint8_t *out_tx,
                                      uint8_t *out_rx, uint16_t *out_ctrl,
                                      int max_count);
extern uint32_t beetle_get_sio_trace_total(void);
extern void     beetle_reset_sio_trace(void);

/* wtrace */
extern int      beetle_wtrace_arm(uint32_t lo, uint32_t hi);
extern int      beetle_wtrace_disarm(int slot);
extern void     beetle_wtrace_disarm_all(void);
extern int      beetle_wtrace_range_count(void);
extern int      beetle_wtrace_max_ranges(void);
extern int      beetle_wtrace_capacity(void);
extern int      beetle_wtrace_get_range(int slot, uint32_t *out_lo, uint32_t *out_hi);
extern void     beetle_wtrace_reset(void);
extern uint64_t beetle_wtrace_total(void);
extern uint32_t beetle_wtrace_get_rich(uint64_t *out_seq, uint32_t *out_addr,
                                        uint32_t *out_value, uint32_t *out_pc,
                                        uint32_t *out_ra,
                                        uint32_t *out_sp,
                                        uint32_t *out_v0, uint32_t *out_v1,
                                        uint32_t *out_a0, uint32_t *out_a1,
                                        uint32_t *out_a2, uint32_t *out_a3,
                                        uint32_t *out_t0, uint32_t *out_t1,
                                        uint32_t *out_frame,
                                        uint8_t *out_slot, uint8_t *out_size,
                                        int max_count);

/* wtrace_all (always-on, no-filter, lean fields) */
extern uint64_t beetle_wtrace_all_total(void);
extern int      beetle_wtrace_all_capacity(void);
extern void     beetle_wtrace_all_reset(void);
extern uint32_t beetle_wtrace_all_get(uint64_t *out_seq, uint32_t *out_addr,
                                       uint32_t *out_new, uint32_t *out_pc,
                                       uint32_t *out_ra, uint32_t *out_frame,
                                       uint8_t *out_w, int max_count);

/* SPU event ring (always-on, mirrors recomp's spu_events). */
extern uint64_t beetle_spu_event_total(void);
extern uint32_t beetle_spu_event_get(uint64_t *out_seq, uint32_t *out_frame,
                                     uint32_t *out_addr, uint16_t *out_env,
                                     uint16_t *out_pitch,
                                     uint16_t *out_vol_l, uint16_t *out_vol_r,
                                     uint16_t *out_adsr_lo, uint16_t *out_adsr_hi,
                                     uint8_t *out_kind, uint8_t *out_voice,
                                     uint32_t max_count);

/* SPU register peeks (oracle ground truth via PS_SPU::GetRegister). */
extern int beetle_spu_get_voice_state(int v,
    uint16_t *vol_ctrl_l, uint16_t *vol_ctrl_r,
    uint16_t *vol_l,      uint16_t *vol_r,
    uint16_t *pitch,
    uint32_t *start_addr, uint32_t *cur_addr, uint32_t *loop_addr,
    uint32_t *adsr_ctrl,  uint16_t *adsr_level);
extern int beetle_spu_get_global_state(
    uint16_t *spu_ctrl,
    uint16_t *main_vol_ctrl_l, uint16_t *main_vol_ctrl_r,
    uint16_t *main_vol_l,      uint16_t *main_vol_r,
    uint32_t *fm_mode, uint32_t *noise_mode, uint32_t *reverb_mode,
    uint32_t *voice_on, uint32_t *voice_off, uint32_t *block_end);

/* Per-frame history ring (parity with runtime's frame_history). */
#include "beetle_history.h"
extern void beetle_history_get_bounds(uint64_t *out_count,
                                       uint64_t *out_oldest,
                                       uint64_t *out_newest);
extern const BeetleFrameRecord *beetle_history_get_frame(uint32_t frame);
extern int  beetle_history_first_failure(uint32_t *out_frame, int *out_diff_count);
extern int  beetle_history_set_snapshot(int slot, uint32_t addr);
extern int  beetle_history_get_snapshot(int slot, uint32_t *out_addr, int *out_active);

/* fntrace */
extern int      beetle_fntrace_arm(uint32_t target_pc);
extern void     beetle_fntrace_disarm_all(void);
extern int      beetle_fntrace_arm_count(void);
extern uint32_t beetle_fntrace_get_arm(int slot);
extern void     beetle_fntrace_set_unfiltered(int on);
extern void     beetle_fntrace_reset(void);
extern uint64_t beetle_fntrace_total(void);
extern uint32_t beetle_fntrace_get(uint64_t *out_seq,
                                    uint32_t *out_caller, uint32_t *out_target,
                                    uint32_t *out_ra, uint32_t *out_a0,
                                    uint32_t *out_a1, uint32_t *out_frame,
                                    uint8_t *out_kind, int max_count);

/* ---- Server state ---- */
static sock_t s_listen = SOCK_INVALID;
static sock_t s_client = SOCK_INVALID;
static int    s_port   = DEFAULT_DEBUG_PORT;

#define RECV_BUF_SIZE 8192
static char s_recv_buf[RECV_BUF_SIZE];
static int  s_recv_len = 0;

static int s_input_override = -1;
static int s_input_frames   = 0;

/* ---- Send helpers ---- */
static void send_raw(const char *data, int n) {
    if (s_client == SOCK_INVALID) return;
    int off = 0;
    while (off < n) {
        int k = send(s_client, data + off, n - off, 0);
        if (k <= 0) {
            sock_close(s_client);
            s_client = SOCK_INVALID;
            return;
        }
        off += k;
    }
}

static void send_fmt(const char *fmt, ...) {
    static char buf[1 << 17];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    send_raw(buf, n);
}

static void send_ok(int id) {
    send_fmt("{\"id\":%d,\"ok\":true}\n", id);
}
static void send_err(int id, const char *msg) {
    send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"%s\"}\n", id, msg);
}

/* ---- JSON helpers (minimal) ---- */
static int json_get_int(const char *json, const char *key, int dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (!*p) return dflt;
    if (*p == '"') {
        p++;
        return (int)strtol(p, NULL, 0);
    }
    return (int)strtol(p, NULL, 0);
}

static int json_get_str(const char *json, const char *key, char *out, int outlen) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < outlen - 1) out[i++] = *p++;
    out[i] = 0;
    return 1;
}

static uint32_t hex_to_u32(const char *s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (uint32_t)strtoul(s + 2, NULL, 16);
    return (uint32_t)strtoul(s, NULL, 0);
}

/* ---- Command handlers ---- */

static void h_ping(int id, const char *json) {
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"backend\":\"beetle\",\"port\":%d,\"frame\":%u}\n",
             id, s_port, beetle_get_frame_count());
}

static void h_read_ram(int id, const char *json) {
    char addr_s[32] = {0};
    if (!json_get_str(json, "addr", addr_s, sizeof(addr_s))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_s);
    int len = json_get_int(json, "len", 16);
    if (len < 1) len = 1;
    if (len > 65536) len = 65536;

    uint8_t *buf = (uint8_t*)malloc(len);
    if (!buf) { send_err(id, "alloc"); return; }
    for (int i = 0; i < len; i++) buf[i] = beetle_read_byte((addr + i) & 0x1FFFFFFFu);

    char *hex = (char*)malloc(len * 2 + 1);
    if (!hex) { free(buf); send_err(id, "alloc"); return; }
    static const char H[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        hex[i*2]   = H[(buf[i] >> 4) & 0xF];
        hex[i*2+1] = H[buf[i] & 0xF];
    }
    hex[len*2] = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"%s\"}\n",
             id, addr, len, hex);
    free(hex); free(buf);
}

static void h_press(int id, const char *json) {
    int buttons = json_get_int(json, "buttons", -1);
    int frames  = json_get_int(json, "frames", 2);
    if (buttons < 0) { send_err(id, "missing buttons"); return; }
    s_input_override = buttons;
    s_input_frames   = frames;
    send_ok(id);
}

static void h_set_input(int id, const char *json) {
    char val[16] = {0};
    if (!json_get_str(json, "buttons", val, sizeof(val))) {
        send_err(id, "missing buttons"); return;
    }
    s_input_override = (int)hex_to_u32(val);
    s_input_frames   = json_get_int(json, "frames", 1);
    send_ok(id);
}

static void h_clear_input(int id, const char *json) {
    (void)json;
    s_input_override = -1;
    s_input_frames   = 0;
    send_ok(id);
}

static void h_pad_status(int id, const char *json) {
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"pad\":\"0x%04X\","
             "\"override\":%d,\"override_frames\":%d}\n",
             id, beetle_get_pad(), s_input_override, s_input_frames);
}

/* ---- Screenshot ---- */
static void h_screenshot_file(int id, const char *json) {
    char path[512] = {0};
    if (!json_get_str(json, "path", path, sizeof(path))) {
        strncpy(path, "psx_screenshot.bmp", sizeof(path) - 1);
    }
    uint32_t *pixels = NULL;
    unsigned w = 0, h = 0;
    if (!beetle_get_framebuffer(&pixels, &w, &h) || !pixels || w == 0 || h == 0) {
        send_err(id, "no frame"); return;
    }
    /* BMP write (XRGB8888 → 24bpp BGR for portability). */
    FILE *f = fopen(path, "wb");
    if (!f) { send_err(id, "fopen"); return; }
    int row_bytes = ((int)w * 3 + 3) & ~3;
    int img_size = row_bytes * (int)h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    int filesz = 54 + img_size;
    hdr[2] = filesz & 0xFF; hdr[3] = (filesz >> 8) & 0xFF;
    hdr[4] = (filesz >> 16) & 0xFF; hdr[5] = (filesz >> 24) & 0xFF;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
    hdr[20] = (w >> 16) & 0xFF; hdr[21] = (w >> 24) & 0xFF;
    /* height stored positive → bottom-up rows */
    hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
    hdr[24] = (h >> 16) & 0xFF; hdr[25] = (h >> 24) & 0xFF;
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = img_size & 0xFF; hdr[35] = (img_size >> 8) & 0xFF;
    hdr[36] = (img_size >> 16) & 0xFF; hdr[37] = (img_size >> 24) & 0xFF;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = (uint8_t*)malloc(row_bytes);
    if (!row) { fclose(f); send_err(id, "alloc"); return; }
    memset(row, 0, row_bytes);
    for (int y = (int)h - 1; y >= 0; y--) {
        for (unsigned x = 0; x < w; x++) {
            uint32_t px = pixels[y * w + x];
            row[x*3 + 0] = (px      ) & 0xFF; /* B */
            row[x*3 + 1] = (px >>  8) & 0xFF; /* G */
            row[x*3 + 2] = (px >> 16) & 0xFF; /* R */
        }
        fwrite(row, 1, row_bytes, f);
    }
    free(row);
    fclose(f);
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"w\":%u,\"h\":%u}\n",
             id, path, w, h);
}

/* ---- SIO trace ---- */
static void h_sio_trace_reset(int id, const char *json) {
    (void)json;
    beetle_reset_sio_trace();
    send_ok(id);
}

static void h_sio_trace(int id, const char *json) {
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 65536) count = 65536;

    uint32_t *seqs = (uint32_t*)malloc(count * sizeof(uint32_t));
    uint8_t  *txs  = (uint8_t*) malloc(count);
    uint8_t  *rxs  = (uint8_t*) malloc(count);
    uint16_t *ctrl = (uint16_t*)malloc(count * sizeof(uint16_t));
    if (!seqs || !txs || !rxs || !ctrl) {
        free(seqs); free(txs); free(rxs); free(ctrl);
        send_err(id, "alloc"); return;
    }

    uint32_t got = beetle_get_sio_trace(seqs, txs, rxs, ctrl, count);
    uint32_t total = beetle_get_sio_trace_total();

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"count\":%u,\"entries\":[",
             id, total, got);
    for (uint32_t i = 0; i < got; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("{\"seq\":%u,\"tx\":\"0x%02X\",\"rx\":\"0x%02X\","
                 "\"ctrl\":\"0x%04X\"}",
                 seqs[i], txs[i], rxs[i], ctrl[i]);
    }
    send_fmt("]}\n");

    free(seqs); free(txs); free(rxs); free(ctrl);
}

static void h_sio_write_window(int id, const char *json) {
    int which = json_get_int(json, "which", 0);
    int before = json_get_int(json, "before", 4);
    int after = json_get_int(json, "after", 156);
    if (which < 0) which = 0;
    if (before < 0) before = 0;
    if (before > 64) before = 64;
    if (after < 1) after = 1;
    if (after > 512) after = 512;

    const int count = 65536;
    uint32_t *seqs = (uint32_t*)malloc(count * sizeof(uint32_t));
    uint8_t  *txs  = (uint8_t*) malloc(count);
    uint8_t  *rxs  = (uint8_t*) malloc(count);
    uint16_t *ctrl = (uint16_t*)malloc(count * sizeof(uint16_t));
    if (!seqs || !txs || !rxs || !ctrl) {
        free(seqs); free(txs); free(rxs); free(ctrl);
        send_err(id, "alloc"); return;
    }

    uint32_t got = beetle_get_sio_trace(seqs, txs, rxs, ctrl, count);
    uint32_t total = beetle_get_sio_trace_total();
    int write_count = 0;
    int start_idx = -1;
    for (uint32_t i = 0; i + 1 < got; i++) {
        if (txs[i] == 0x81 && txs[i + 1] == 0x57) {
            if (write_count == which) start_idx = (int)i;
            write_count++;
        }
    }

    if (start_idx < 0) {
        send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"count\":%u,"
                 "\"write_count\":%d,\"found\":false}\n",
                 id, total, got, write_count);
        free(seqs); free(txs); free(rxs); free(ctrl);
        return;
    }

    int window_start = start_idx - before;
    if (window_start < 0) window_start = 0;
    int window_end = start_idx + after;
    if (window_end > (int)got) window_end = (int)got;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"count\":%u,"
             "\"write_count\":%d,\"found\":true,\"which\":%d,"
             "\"start_seq\":%u,\"expected_write_bytes\":138,\"entries\":[",
             id, total, got, write_count, which, seqs[start_idx]);
    for (int i = window_start; i < window_end; i++) {
        if (i > window_start) send_fmt(",");
        send_fmt("{\"rel\":%d,\"seq\":%u,\"tx\":\"0x%02X\",\"rx\":\"0x%02X\","
                 "\"ctrl\":\"0x%04X\"}",
                 i - start_idx, seqs[i], txs[i], rxs[i], ctrl[i]);
    }
    send_fmt("]}\n");

    free(seqs); free(txs); free(rxs); free(ctrl);
}

/* ---- wtrace ---- */
static void h_wtrace_arm(int id, const char *json) {
    char lo_s[32] = {0}, hi_s[32] = {0};
    if (!json_get_str(json, "lo", lo_s, sizeof(lo_s)) ||
        !json_get_str(json, "hi", hi_s, sizeof(hi_s))) {
        send_err(id, "need lo,hi"); return;
    }
    /* Mask kseg so the wire response matches runtime's handle_wtrace_add:
     * stored physical address, NOT the as-typed virtual.  Tools that diff
     * "lo" across backends would otherwise see 0x8009... vs 0x0009... and
     * spuriously flag a mismatch. */
    uint32_t lo = hex_to_u32(lo_s) & 0x1FFFFFFFu;
    uint32_t hi = hex_to_u32(hi_s) & 0x1FFFFFFFu;
    int slot = beetle_wtrace_arm(lo, hi);
    if (slot < 0) {
        send_err(id, slot == -1 ? "ranges full" : "lo>=hi"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}\n",
             id, slot, lo, hi);
}

/* Per-slot disarm (parity with runtime wtrace_del). slot=N required. */
static void h_wtrace_disarm(int id, const char *json) {
    int slot = json_get_int(json, "slot", -1);
    if (slot < 0) { send_err(id, "missing slot"); return; }
    if (beetle_wtrace_disarm(slot) != 0) {
        send_err(id, "invalid slot"); return;
    }
    send_ok(id);
}

/* Disarm all armed ranges. */
static void h_wtrace_disarm_all(int id, const char *json) {
    (void)json; beetle_wtrace_disarm_all(); send_ok(id);
}

static void h_wtrace_reset(int id, const char *json) {
    (void)json; beetle_wtrace_reset(); send_ok(id);
}

/* Ring stats — parity with runtime's wtrace_stats. */
static void h_wtrace_stats(int id, const char *json) {
    (void)json;
    uint64_t total = beetle_wtrace_total();
    int cap = beetle_wtrace_capacity();
    uint64_t oldest = (total <= (uint64_t)cap) ? 0 : total - (uint64_t)cap;
    uint64_t newest = (total > 0) ? total - 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%d,"
             "\"oldest_seq\":%llu,\"newest_seq\":%llu,\"ranges\":%d}\n",
             id, (unsigned long long)total, cap,
             (unsigned long long)oldest, (unsigned long long)newest,
             beetle_wtrace_range_count());
}

static void h_wtrace_ranges(int id, const char *json) {
    (void)json;
    int n = beetle_wtrace_range_count();
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%d,\"ranges\":[", id, n);
    for (int i = 0; i < n; i++) {
        uint32_t lo = 0, hi = 0;
        beetle_wtrace_get_range(i, &lo, &hi);
        if (i > 0) send_fmt(",");
        send_fmt("{\"slot\":%d,\"lo\":\"0x%08X\",\"hi\":\"0x%08X\"}", i, lo, hi);
    }
    send_fmt("]}\n");
}

/* wtrace_dump — rich-window entries, FIELD NAMES MUST MATCH runtime
 * (handle_wtrace_dump in debug_server.c) so cross-backend tools read
 * `entry["new"]`, `entry["pc"]`, `entry["w"]` etc. without per-backend
 * branches. Documented gaps vs runtime: no `old` (callback fires post-
 * write), no `func` (no per-function dispatch gate on beetle). `cpu_pc`
 * is the same as `pc` on beetle, emitted equal for parity. */
static void h_wtrace_dump(int id, const char *json) {
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 65536) count = 65536;

    /* Optional post-hoc address filter — parity with runtime. */
    char lo_s[32] = {0}, hi_s[32] = {0};
    uint32_t flo = 0, fhi = 0xFFFFFFFFu;
    if (json_get_str(json, "addr_lo", lo_s, sizeof(lo_s)))
        flo = hex_to_u32(lo_s) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", hi_s, sizeof(hi_s)))
        fhi = hex_to_u32(hi_s) & 0x1FFFFFFFu;

    uint64_t *seqs   = (uint64_t*)malloc((size_t)count * sizeof(uint64_t));
    uint32_t *addrs  = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *vals   = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *pcs    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *ras    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *sps    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *v0s    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *v1s    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *a0s    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *a1s    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *a2s    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *a3s    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *t0s    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *t1s    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *frames = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint8_t  *slots  = (uint8_t*) malloc((size_t)count);
    uint8_t  *sizes  = (uint8_t*) malloc((size_t)count);
    if (!seqs || !addrs || !vals || !pcs || !ras || !sps ||
        !v0s || !v1s || !a0s || !a1s || !a2s || !a3s || !t0s || !t1s ||
        !frames || !slots || !sizes) {
        free(seqs); free(addrs); free(vals); free(pcs); free(ras); free(sps);
        free(v0s); free(v1s); free(a0s); free(a1s); free(a2s); free(a3s);
        free(t0s); free(t1s); free(frames); free(slots); free(sizes);
        send_err(id, "alloc"); return;
    }

    uint32_t got = beetle_wtrace_get_rich(seqs, addrs, vals, pcs, ras, sps,
                                          v0s, v1s, a0s, a1s, a2s, a3s,
                                          t0s, t1s, frames, slots, sizes,
                                          count);
    uint64_t total = beetle_wtrace_total();
    int cap = beetle_wtrace_capacity();
    uint32_t avail = (total < (uint64_t)cap) ? (uint32_t)total : (uint32_t)cap;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
             id, (unsigned long long)total, avail);
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < got; i++) {
        uint32_t phys = addrs[i] & 0x1FFFFFFFu;
        if (phys < flo || phys >= fhi) continue;
        if (emitted > 0) send_fmt(",");
        /* Field names match runtime's handle_wtrace_dump exactly: addr,
         * new, ra, pc, cpu_pc, sp, v0/v1/a0..a3/t0/t1, frame, w.
         * Beetle-only extras: slot. Runtime-only fields (old, func) are
         * absent — tools must use .get(). */
        send_fmt("{\"seq\":%llu,\"addr\":\"0x%08X\","
                 "\"new\":\"0x%08X\",\"ra\":\"0x%08X\","
                 "\"pc\":\"0x%08X\",\"cpu_pc\":\"0x%08X\",\"sp\":\"0x%08X\","
                 "\"v0\":\"0x%08X\",\"v1\":\"0x%08X\","
                 "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                 "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\","
                 "\"t0\":\"0x%08X\",\"t1\":\"0x%08X\","
                 "\"frame\":%u,\"w\":%u,\"slot\":%u}",
                 (unsigned long long)seqs[i], addrs[i],
                 vals[i], ras[i],
                 pcs[i], pcs[i], sps[i],
                 v0s[i], v1s[i],
                 a0s[i], a1s[i], a2s[i], a3s[i],
                 t0s[i], t1s[i],
                 frames[i], (unsigned)sizes[i], (unsigned)slots[i]);
        emitted++;
    }
    send_fmt("],\"emitted\":%u}\n", emitted);

    free(seqs); free(addrs); free(vals); free(pcs); free(ras); free(sps);
    free(v0s); free(v1s); free(a0s); free(a1s); free(a2s); free(a3s);
    free(t0s); free(t1s); free(frames); free(slots); free(sizes);
}

/* ---- fntrace ---- */
static const char *fn_kind_str(uint8_t k) {
    switch (k) { case 1: return "J"; case 2: return "JAL";
                 case 3: return "JR"; case 4: return "JALR"; default: return "?"; }
}

static void h_fntrace_arm(int id, const char *json) {
    char target_s[32] = {0};
    if (!json_get_str(json, "target", target_s, sizeof(target_s))) {
        send_err(id, "missing target"); return;
    }
    uint32_t target = hex_to_u32(target_s);
    int slot = beetle_fntrace_arm(target);
    if (slot < 0) { send_err(id, "arms full"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"target\":\"0x%08X\"}\n",
             id, slot, target);
}

static void h_fntrace_disarm(int id, const char *json) {
    (void)json; beetle_fntrace_disarm_all(); send_ok(id);
}

static void h_fntrace_arms(int id, const char *json) {
    (void)json;
    int n = beetle_fntrace_arm_count();
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%d,\"arms\":[", id, n);
    for (int i = 0; i < n; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("\"0x%08X\"", beetle_fntrace_get_arm(i));
    }
    send_fmt("]}\n");
}

static void h_fntrace_unfiltered(int id, const char *json) {
    int on = json_get_int(json, "on", 0);
    beetle_fntrace_set_unfiltered(on);
    send_fmt("{\"id\":%d,\"ok\":true,\"unfiltered\":%d}\n", id, on);
}

static void h_fntrace_reset(int id, const char *json) {
    (void)json; beetle_fntrace_reset(); send_ok(id);
}

static void h_fntrace_dump(int id, const char *json) {
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 65536) count = 65536;

    uint64_t *seqs    = (uint64_t*)malloc(count * sizeof(uint64_t));
    uint32_t *callers = (uint32_t*)malloc(count * sizeof(uint32_t));
    uint32_t *targets = (uint32_t*)malloc(count * sizeof(uint32_t));
    uint32_t *ras     = (uint32_t*)malloc(count * sizeof(uint32_t));
    uint32_t *a0s     = (uint32_t*)malloc(count * sizeof(uint32_t));
    uint32_t *a1s     = (uint32_t*)malloc(count * sizeof(uint32_t));
    uint32_t *frames  = (uint32_t*)malloc(count * sizeof(uint32_t));
    uint8_t  *kinds   = (uint8_t*) malloc(count);
    if (!seqs || !callers || !targets || !ras || !a0s || !a1s || !frames || !kinds) {
        free(seqs); free(callers); free(targets); free(ras);
        free(a0s); free(a1s); free(frames); free(kinds);
        send_err(id, "alloc"); return;
    }
    uint32_t got = beetle_fntrace_get(seqs, callers, targets, ras,
                                       a0s, a1s, frames, kinds, count);
    uint64_t total = beetle_fntrace_total();

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"count\":%u,\"entries\":[",
             id, (unsigned long long)total, (unsigned)got);
    for (uint32_t i = 0; i < got; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("{\"seq\":%llu,\"caller\":\"0x%08X\",\"target\":\"0x%08X\","
                 "\"ra\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
                 "\"frame\":%u,\"kind\":\"%s\"}",
                 (unsigned long long)seqs[i], callers[i], targets[i],
                 ras[i], a0s[i], a1s[i], frames[i], fn_kind_str(kinds[i]));
    }
    send_fmt("]}\n");

    free(seqs); free(callers); free(targets); free(ras);
    free(a0s); free(a1s); free(frames); free(kinds);
}

/* ---- spu_voices: Beetle oracle ground truth via PS_SPU::GetRegister ----
 *
 * Wire-protocol mirror of psx-runtime's spu_voices (port 4370). Both
 * backends emit a structurally-identical JSON shape so a diff tool can
 * compare the same fields across processes. Fields:
 *   per voice: v, active, vol_l/r (post-sweep), vol_l_ctrl/r_ctrl,
 *              pitch, start, loop, cur_addr, adsr_ctrl, adsr_level
 *   global:    ctrl, main_l/r, kon/koff, endx, voice_on, voice_off
 *
 * Beetle has no recomp-style "active" flag — we synthesize it as
 * (adsr_level > 0), the closest oracle equivalent.
 */
static void h_spu_voices(int id, const char *json) {
    (void)json;
    uint16_t spu_ctrl = 0, mvc_l = 0, mvc_r = 0, mv_l = 0, mv_r = 0;
    uint32_t fm = 0, nz = 0, rv = 0, von = 0, voff = 0, bend = 0;
    if (!beetle_spu_get_global_state(&spu_ctrl, &mvc_l, &mvc_r, &mv_l, &mv_r,
                                     &fm, &nz, &rv, &von, &voff, &bend)) {
        send_err(id, "spu_unavailable");
        return;
    }
    /* Build active mask from per-voice adsr_level > 0. */
    uint32_t active_mask = 0;
    struct {
        uint16_t vc_l, vc_r, v_l, v_r, pitch, adsr_lvl;
        uint32_t sa, ca, la, adsr_ctrl;
    } vstate[24];
    for (int v = 0; v < 24; v++) {
        beetle_spu_get_voice_state(v,
            &vstate[v].vc_l, &vstate[v].vc_r,
            &vstate[v].v_l,  &vstate[v].v_r,
            &vstate[v].pitch,
            &vstate[v].sa, &vstate[v].ca, &vstate[v].la,
            &vstate[v].adsr_ctrl, &vstate[v].adsr_lvl);
        if (vstate[v].adsr_lvl > 0) active_mask |= (1u << v);
    }

    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"ctrl\":\"0x%04X\","
             "\"main_l\":\"0x%04X\",\"main_r\":\"0x%04X\","
             "\"main_l_ctrl\":\"0x%04X\",\"main_r_ctrl\":\"0x%04X\","
             "\"kon\":\"0x%06X\",\"koff\":\"0x%06X\","
             "\"endx\":\"0x%06X\",\"active_mask\":\"0x%06X\","
             "\"fm\":\"0x%06X\",\"non\":\"0x%06X\",\"eon\":\"0x%06X\","
             "\"voices\":[",
             id,
             spu_ctrl, mv_l, mv_r, mvc_l, mvc_r,
             von & 0xFFFFFFu, voff & 0xFFFFFFu,
             bend & 0xFFFFFFu, active_mask,
             fm & 0xFFFFFFu, nz & 0xFFFFFFu, rv & 0xFFFFFFu);
    for (int v = 0; v < 24; v++) {
        if (v > 0) send_fmt(",");
        int active = (vstate[v].adsr_lvl > 0) ? 1 : 0;
        send_fmt("{\"v\":%d,\"active\":%d,"
                 "\"vol_l_ctrl\":\"0x%04X\",\"vol_r_ctrl\":\"0x%04X\","
                 "\"vol_l\":\"0x%04X\",\"vol_r\":\"0x%04X\","
                 "\"pitch\":\"0x%04X\","
                 "\"start\":\"0x%05X\",\"cur_addr\":\"0x%05X\",\"loop\":\"0x%05X\","
                 "\"adsr_ctrl\":\"0x%08X\",\"adsr_level\":\"0x%04X\"}",
                 v, active,
                 vstate[v].vc_l, vstate[v].vc_r,
                 vstate[v].v_l,  vstate[v].v_r,
                 vstate[v].pitch,
                 vstate[v].sa, vstate[v].ca, vstate[v].la,
                 vstate[v].adsr_ctrl, vstate[v].adsr_lvl);
    }
    send_fmt("]}\n");
}

/* ---- spu_events: dump Beetle's SPU event ring ---- */
static void h_spu_events(int id, const char *json) {
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 4096) count = 4096;
    uint64_t *seqs   = malloc(sizeof(uint64_t)  * (size_t)count);
    uint32_t *frames = malloc(sizeof(uint32_t)  * (size_t)count);
    uint32_t *addrs  = malloc(sizeof(uint32_t)  * (size_t)count);
    uint16_t *envs   = malloc(sizeof(uint16_t)  * (size_t)count);
    uint16_t *pits   = malloc(sizeof(uint16_t)  * (size_t)count);
    uint16_t *vlcs   = malloc(sizeof(uint16_t)  * (size_t)count);
    uint16_t *vrcs   = malloc(sizeof(uint16_t)  * (size_t)count);
    uint16_t *als    = malloc(sizeof(uint16_t)  * (size_t)count);
    uint16_t *ahs    = malloc(sizeof(uint16_t)  * (size_t)count);
    uint8_t  *kinds  = malloc(sizeof(uint8_t)   * (size_t)count);
    uint8_t  *vs     = malloc(sizeof(uint8_t)   * (size_t)count);
    if (!seqs || !frames || !addrs || !envs || !pits || !vlcs || !vrcs ||
        !als || !ahs || !kinds || !vs) {
        free(seqs); free(frames); free(addrs); free(envs); free(pits);
        free(vlcs); free(vrcs); free(als); free(ahs); free(kinds); free(vs);
        send_err(id, "alloc"); return;
    }
    uint32_t got = beetle_spu_event_get(seqs, frames, addrs, envs, pits,
                                        vlcs, vrcs, als, ahs, kinds, vs,
                                        (uint32_t)count);
    uint64_t total = beetle_spu_event_total();

    static const char *kind_names[5] = { "?", "KEYON", "KEYOFF", "END_STOP", "END_LOOP" };
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"count\":%u,\"events\":[",
             id, (unsigned long long)total, (unsigned)got);
    for (uint32_t i = 0; i < got; i++) {
        const char *kn = (kinds[i] <= 4) ? kind_names[kinds[i]] : "?";
        if (i > 0) send_fmt(",");
        send_fmt("{\"seq\":%llu,\"frame\":%u,\"kind\":\"%s\",\"v\":%d,"
                 "\"pitch\":\"0x%04X\",\"addr\":\"0x%05X\",\"env\":\"0x%04X\","
                 "\"adsr_lo\":\"0x%04X\",\"adsr_hi\":\"0x%04X\","
                 "\"vol_l\":\"0x%04X\",\"vol_r\":\"0x%04X\"}",
                 (unsigned long long)seqs[i], frames[i], kn, (int)vs[i],
                 pits[i], addrs[i], envs[i],
                 als[i], ahs[i], vlcs[i], vrcs[i]);
    }
    send_fmt("]}\n");

    free(seqs); free(frames); free(addrs); free(envs); free(pits);
    free(vlcs); free(vrcs); free(als); free(ahs); free(kinds); free(vs);
}

/* ---- wtrace_all (always-on, no-filter, lean fields) ----
 * Mirrors runtime's wtrace_all surface so probes that haven't pre-armed
 * a range can still see the last ~1 second of writes the moment they
 * connect. Lean fields only (no register window) to keep capacity
 * affordable. */

static void h_wtrace_all_stats(int id, const char *json) {
    (void)json;
    uint64_t total = beetle_wtrace_all_total();
    int cap = beetle_wtrace_all_capacity();
    uint64_t oldest = (total <= (uint64_t)cap) ? 0 : total - (uint64_t)cap;
    uint64_t newest = (total > 0) ? total - 1 : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%d,"
             "\"oldest_seq\":%llu,\"newest_seq\":%llu}\n",
             id, (unsigned long long)total, cap,
             (unsigned long long)oldest, (unsigned long long)newest);
}

static void h_wtrace_all_reset(int id, const char *json) {
    (void)json; beetle_wtrace_all_reset(); send_ok(id);
}

static void h_wtrace_all_dump(int id, const char *json) {
    int count = json_get_int(json, "count", 1024);
    if (count < 1) count = 1;
    if (count > 65536) count = 65536;

    char lo_s[32] = {0}, hi_s[32] = {0};
    uint32_t flo = 0, fhi = 0xFFFFFFFFu;
    if (json_get_str(json, "addr_lo", lo_s, sizeof(lo_s)))
        flo = hex_to_u32(lo_s) & 0x1FFFFFFFu;
    if (json_get_str(json, "addr_hi", hi_s, sizeof(hi_s)))
        fhi = hex_to_u32(hi_s) & 0x1FFFFFFFu;

    uint64_t *seqs   = (uint64_t*)malloc((size_t)count * sizeof(uint64_t));
    uint32_t *addrs  = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *news   = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *pcs    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *ras    = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *frames = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint8_t  *ws     = (uint8_t*) malloc((size_t)count);
    if (!seqs || !addrs || !news || !pcs || !ras || !frames || !ws) {
        free(seqs); free(addrs); free(news); free(pcs);
        free(ras); free(frames); free(ws);
        send_err(id, "alloc"); return;
    }
    uint32_t got = beetle_wtrace_all_get(seqs, addrs, news, pcs, ras,
                                          frames, ws, count);
    uint64_t total = beetle_wtrace_all_total();
    int cap = beetle_wtrace_all_capacity();
    uint32_t avail = (total < (uint64_t)cap) ? (uint32_t)total : (uint32_t)cap;

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"available\":%u,\"entries\":[",
             id, (unsigned long long)total, avail);
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < got; i++) {
        uint32_t phys = addrs[i] & 0x1FFFFFFFu;
        if (phys < flo || phys >= fhi) continue;
        if (emitted > 0) send_fmt(",");
        send_fmt("{\"seq\":%llu,\"addr\":\"0x%08X\",\"new\":\"0x%08X\","
                 "\"pc\":\"0x%08X\",\"ra\":\"0x%08X\","
                 "\"frame\":%u,\"w\":%u}",
                 (unsigned long long)seqs[i], addrs[i], news[i],
                 pcs[i], ras[i], frames[i], (unsigned)ws[i]);
        emitted++;
    }
    send_fmt("],\"emitted\":%u}\n", emitted);

    free(seqs); free(addrs); free(news); free(pcs);
    free(ras); free(frames); free(ws);
}

/* ---- Per-frame history ring (wire-format identical to runtime) ---- */

static void h_history(int id, const char *json) {
    (void)json;
    uint64_t cnt = 0, oldest = 0, newest = 0;
    beetle_history_get_bounds(&cnt, &oldest, &newest);
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"oldest\":%llu,\"newest\":%llu}\n",
             id,
             (unsigned long long)cnt,
             (unsigned long long)oldest,
             (unsigned long long)newest);
}

static void h_get_frame(int id, const char *json) {
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }
    const BeetleFrameRecord *r = beetle_history_get_frame((uint32_t)f);
    if (!r) { send_err(id, "frame not in buffer"); return; }

    static char buf[8192];
    int pos = snprintf(buf, sizeof(buf),
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
        "(beetle)");
    for (int i = 0; i < 32; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"0x%08X\"", r->gpr[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}\n");
    send_raw(buf, pos);
}

static void h_frame_range(int id, const char *json) {
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end",   -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    static char buf[200 * 256 + 256];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"frames\":[", id);
    int first = 1;
    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;
        const BeetleFrameRecord *r = beetle_history_get_frame((uint32_t)f);
        if (!r) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"frame\":%d,\"available\":false}", f);
            continue;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"frame\":%u,\"verify\":%d,"
            "\"sr\":\"0x%08X\",\"i_stat\":\"0x%08X\","
            "\"pad\":\"0x%04X\"}",
            r->frame_number, r->verify_pass,
            r->cop0_sr, r->i_stat,
            r->pad_buttons);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}\n");
    send_raw(buf, pos);
}

static void h_frame_timeseries(int id, const char *json) {
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end",   -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    static char buf[200 * 320 + 256];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"ts\":[", id);
    int first = 1;
    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;
        const BeetleFrameRecord *r = beetle_history_get_frame((uint32_t)f);
        if (!r) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "null");
            continue;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"f\":%u,\"v\":%d,"
            "\"sr\":\"0x%08X\",\"ist\":\"0x%08X\",\"imk\":\"0x%08X\","
            "\"pad\":\"0x%04X\",\"dc\":%u}",
            r->frame_number, r->verify_pass,
            r->cop0_sr, r->i_stat, r->i_mask,
            r->pad_buttons, r->dispatch_count);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}\n");
    send_raw(buf, pos);
}

static void h_first_failure(int id, const char *json) {
    (void)json;
    uint32_t frame = 0; int diff = 0;
    int found = beetle_history_first_failure(&frame, &diff);
    if (found) {
        send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u,\"diff_count\":%d}\n",
                 id, frame, diff);
    } else {
        send_fmt("{\"id\":%d,\"ok\":true,\"frame\":-1,\"message\":\"no failures found\"}\n",
                 id);
    }
}

static void h_read_frame_ram(int id, const char *json) {
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }
    char addr_s[32] = {0};
    if (!json_get_str(json, "addr", addr_s, sizeof(addr_s))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_s);
    int len = json_get_int(json, "len", 1);
    if (len < 1)   len = 1;
    if (len > 128) len = 128;

    const BeetleFrameRecord *r = beetle_history_get_frame((uint32_t)f);
    if (!r) { send_err(id, "frame not in buffer"); return; }

    /* Search the 4 snapshot regions for a span that covers [addr, addr+len). */
    char hex[257];
    int found = 0;
    for (int i = 0; i < BEETLE_RAM_SNAPSHOT_REGIONS; i++) {
        uint32_t saddr = r->snapshot_addr[i];
        if (saddr == 0) continue;
        if (addr >= saddr &&
            (uint64_t)addr + (uint64_t)len <= (uint64_t)saddr + BEETLE_RAM_SNAPSHOT_SIZE) {
            uint32_t off = addr - saddr;
            for (int j = 0; j < len; j++) {
                snprintf(hex + j * 2, 3, "%02x", r->snapshot_data[i][off + j]);
            }
            found = 1;
            break;
        }
    }
    if (!found) {
        send_err(id, "address not in any snapshot region for this frame");
        return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%d,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"%s\"}\n",
             id, f, addr, len, hex);
}

static void h_set_snapshot(int id, const char *json) {
    int slot = json_get_int(json, "slot", -1);
    if (slot < 0 || slot >= BEETLE_RAM_SNAPSHOT_REGIONS) {
        send_err(id, "invalid slot (0-3)"); return;
    }
    char addr_s[32] = {0};
    if (!json_get_str(json, "addr", addr_s, sizeof(addr_s))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_s);
    if (!beetle_history_set_snapshot(slot, addr)) {
        send_err(id, "set_snapshot failed"); return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"addr\":\"0x%08X\"}\n",
             id, slot, addr);
}

static void h_get_snapshots(int id, const char *json) {
    (void)json;
    uint32_t a[BEETLE_RAM_SNAPSHOT_REGIONS] = {0};
    int act[BEETLE_RAM_SNAPSHOT_REGIONS] = {0};
    for (int i = 0; i < BEETLE_RAM_SNAPSHOT_REGIONS; i++) {
        beetle_history_get_snapshot(i, &a[i], &act[i]);
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"snapshots\":["
             "{\"slot\":0,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":1,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":2,\"addr\":\"0x%08X\",\"active\":%d},"
             "{\"slot\":3,\"addr\":\"0x%08X\",\"active\":%d}]}\n",
             id,
             a[0], act[0], a[1], act[1],
             a[2], act[2], a[3], act[3]);
}

/* ---- Command dispatch ---- */
typedef void (*cmd_handler)(int id, const char *json);
typedef struct { const char *name; cmd_handler handler; } CmdEntry;

static const CmdEntry CMDS[] = {
    { "ping",                  h_ping },
    { "read_ram",              h_read_ram },
    { "press",                 h_press },
    { "set_input",             h_set_input },
    { "clear_input",           h_clear_input },
    { "pad_status",            h_pad_status },
    { "screenshot_file",       h_screenshot_file },
    { "sio_trace_reset",       h_sio_trace_reset },
    { "sio_trace",             h_sio_trace },
    { "sio_write_window",      h_sio_write_window },
    /* wtrace — normalized verb set (parity contract with psx-runtime). */
    { "wtrace_arm",            h_wtrace_arm },
    { "wtrace_disarm",         h_wtrace_disarm },       /* per-slot */
    { "wtrace_disarm_all",     h_wtrace_disarm_all },
    { "wtrace_reset",          h_wtrace_reset },
    { "wtrace_ranges",         h_wtrace_ranges },
    { "wtrace_dump",           h_wtrace_dump },
    { "wtrace_stats",          h_wtrace_stats },
    /* Always-on catch-all wtrace (parity with psx-runtime). */
    { "wtrace_all_dump",       h_wtrace_all_dump },
    { "wtrace_all_stats",      h_wtrace_all_stats },
    { "wtrace_all_reset",      h_wtrace_all_reset },
    { "fntrace_arm",           h_fntrace_arm },
    { "fntrace_disarm",        h_fntrace_disarm },
    { "fntrace_arms",          h_fntrace_arms },
    { "fntrace_unfiltered",    h_fntrace_unfiltered },
    { "fntrace_reset",         h_fntrace_reset },
    { "fntrace_dump",          h_fntrace_dump },
    { "spu_voices",            h_spu_voices },
    { "spu_events",            h_spu_events },
    /* Per-frame history ring (parity with psx-runtime). */
    { "history",               h_history },
    { "get_frame",             h_get_frame },
    { "frame_range",           h_frame_range },
    { "frame_timeseries",      h_frame_timeseries },
    { "first_failure",         h_first_failure },
    { "read_frame_ram",        h_read_frame_ram },
    { "set_snapshot",          h_set_snapshot },
    { "get_snapshots",         h_get_snapshots },
    { NULL, NULL }
};

static void process_line(char *line) {
    int id = json_get_int(line, "id", 0);
    char cmd[64] = {0};
    if (!json_get_str(line, "cmd", cmd, sizeof(cmd))) {
        send_err(id, "missing cmd"); return;
    }
    for (const CmdEntry *e = CMDS; e->name; e++) {
        if (strcmp(e->name, cmd) == 0) {
            e->handler(id, line);
            return;
        }
    }
    send_err(id, "unknown command");
}

/* ---- Public API ---- */

void beetle_debug_server_init(int port) {
    s_port = port > 0 ? port : 4380;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[beetle-dbg] WSAStartup failed\n");
        return;
    }
#endif

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == SOCK_INVALID) {
        fprintf(stderr, "[beetle-dbg] socket() failed\n");
        return;
    }
    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port        = htons((u_short)s_port);
    if (bind(s_listen, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "[beetle-dbg] bind(%d) failed\n", s_port);
        sock_close(s_listen); s_listen = SOCK_INVALID; return;
    }
    /* Backlog 16 (was 4): matches runtime's debug_server.c. Lets probes
     * queue when the main thread is stalled, instead of returning RST. */
    if (listen(s_listen, 16) < 0) {
        fprintf(stderr, "[beetle-dbg] listen() failed\n");
        sock_close(s_listen); s_listen = SOCK_INVALID; return;
    }

    /* Non-blocking accept. */
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s_listen, FIONBIO, &nb);
#else
    int fl = fcntl(s_listen, F_GETFL, 0);
    fcntl(s_listen, F_SETFL, fl | O_NONBLOCK);
#endif

    fprintf(stderr, "[beetle-dbg] listening on 127.0.0.1:%d\n", s_port);
}

void beetle_debug_server_poll(void) {
    if (s_listen == SOCK_INVALID) return;

    /* Accept new client (one at a time). */
    if (s_client == SOCK_INVALID) {
        sock_t c = accept(s_listen, NULL, NULL);
        if (c != SOCK_INVALID) {
            s_client = c;
#ifdef _WIN32
            u_long nb = 1;
            ioctlsocket(s_client, FIONBIO, &nb);
#else
            int fl = fcntl(s_client, F_GETFL, 0);
            fcntl(s_client, F_SETFL, fl | O_NONBLOCK);
#endif
            s_recv_len = 0;
        }
    }
    if (s_client == SOCK_INVALID) return;

    /* Read available bytes. */
    while (s_recv_len < RECV_BUF_SIZE - 1) {
        int n = recv(s_client, s_recv_buf + s_recv_len,
                     RECV_BUF_SIZE - 1 - s_recv_len, 0);
        if (n > 0) {
            s_recv_len += n;
        } else if (n == 0) {
            sock_close(s_client); s_client = SOCK_INVALID;
            return;
        } else {
            break;
        }
    }

    /* Process complete lines. */
    int start = 0;
    for (int i = 0; i < s_recv_len; i++) {
        if (s_recv_buf[i] == '\n') {
            s_recv_buf[i] = 0;
            process_line(s_recv_buf + start);
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(s_recv_buf, s_recv_buf + start, s_recv_len - start);
        s_recv_len -= start;
    }
    if (s_recv_len >= RECV_BUF_SIZE - 1) {
        /* Overflow, drop the connection. */
        sock_close(s_client); s_client = SOCK_INVALID;
        s_recv_len = 0;
    }
}

void beetle_debug_server_shutdown(void) {
    if (s_client  != SOCK_INVALID) { sock_close(s_client);  s_client  = SOCK_INVALID; }
    if (s_listen  != SOCK_INVALID) { sock_close(s_listen);  s_listen  = SOCK_INVALID; }
#ifdef _WIN32
    WSACleanup();
#endif
}

int beetle_debug_server_get_input_override(void) {
    if (s_input_override >= 0 && s_input_frames > 0) {
        if (--s_input_frames == 0) s_input_override = -1;
    }
    return s_input_override;
}
