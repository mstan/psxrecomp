/*
 * psx_oracle_cmds.c -- TCP debug server commands for golden-oracle comparison.
 *
 * Handles emu_* and find_first_divergence commands.
 * Compiled into the runtime only when ENABLE_DUCKSTATION_ORACLE is defined.
 *
 * Pattern follows snesrecomp/runner/src/emu_oracle_cmds.c.
 */

#if defined(ENABLE_DUCKSTATION_ORACLE) || defined(ENABLE_BEETLE_PSX_ORACLE)

#include "psx_oracle_backend.h"
#include "sio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- Helpers (same as debug_server.c) ---- */

static int json_get_str(const char *json, const char *key, char *out, int outsz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
        out[i] = '\0';
        return 1;
    }
    /* Unquoted (number). */
    int i = 0;
    while (*p && *p != ',' && *p != '}' && *p != ' ' && i < outsz - 1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

static uint32_t hex_to_u32(const char *s) {
    return (uint32_t)strtoul(s, NULL, 0);
}

static int json_get_int(const char *json, const char *key, int defval) {
    char buf[32];
    if (!json_get_str(json, key, buf, sizeof(buf))) return defval;
    return atoi(buf);
}

/* ---- Send helpers (forward-declared from debug_server.c) ---- */
extern void debug_server_send_fmt(const char *fmt, ...);
#define send_fmt debug_server_send_fmt

static void send_ok(int id) {
    send_fmt("{\"id\":%d,\"ok\":true}\n", id);
}
static void send_err(int id, const char *msg) {
    send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"%s\"}\n", id, msg);
}

/* ---- RAM pointer from memory.c ---- */
extern uint8_t *memory_get_ram_ptr(void);

/* ---- Command handlers ---- */

static void h_emu_is_loaded(int id, const char *json) {
    (void)json;
    int loaded = g_psx_oracle && g_psx_oracle->is_loaded();
    send_fmt("{\"id\":%d,\"ok\":true,\"loaded\":%s}\n",
             id, loaded ? "true" : "false");
}

static void h_emu_read_ram(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded"); return;
    }
    char abuf[32] = {0};
    json_get_str(json, "addr", abuf, sizeof(abuf));
    uint32_t addr = hex_to_u32(abuf);
    int len = json_get_int(json, "len", 4);
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;

    /* Physical mask. */
    uint32_t phys = addr & 0x1FFFFFFFu;

    /* Read from oracle byte by byte. */
    char *hex = (char *)malloc((size_t)(len * 2 + 1));
    if (!hex) { send_err(id, "alloc"); return; }
    for (int i = 0; i < len; i++) {
        uint8_t b = g_psx_oracle->read_byte(phys + (uint32_t)i);
        snprintf(hex + i * 2, 3, "%02x", b);
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"%s\"}\n",
             id, addr, len, hex);
    free(hex);
}

static void h_emu_cpu_regs(int id, const char *json) {
    (void)json;
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded"); return;
    }
    PsxCpuRegs regs;
    g_psx_oracle->get_cpu_regs(&regs);

    char *buf = (char *)malloc(4096);
    if (!buf) { send_err(id, "alloc"); return; }
    int pos = snprintf(buf, 4096,
        "{\"id\":%d,\"ok\":true,"
        "\"pc\":\"0x%08X\",\"hi\":\"0x%08X\",\"lo\":\"0x%08X\","
        "\"sr\":\"0x%08X\",\"cause\":\"0x%08X\",\"epc\":\"0x%08X\","
        "\"gpr\":[",
        id, regs.pc, regs.hi, regs.lo,
        regs.cop0_sr, regs.cop0_cause, regs.cop0_epc);
    for (int i = 0; i < 32; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, 4096 - pos, "\"0x%08X\"", regs.gpr[i]);
    }
    pos += snprintf(buf + pos, 4096 - pos, "]}\n");
    send_fmt("%s", buf);
    free(buf);
}

static void h_emu_step(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded"); return;
    }
    int n = json_get_int(json, "frames", 1);
    int pad1 = json_get_int(json, "pad1", 0xFFFF);
    if (n < 1) n = 1;
    if (n > 600) n = 600;
    for (int i = 0; i < n; i++) {
        g_psx_oracle->run_frame((uint16_t)pad1);
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"frames_stepped\":%d,\"oracle_frame\":%u}\n",
             id, n, g_psx_oracle->get_frame_count());
}

/* emu_trace_addr: step N frames capturing a RAM word each frame.
 * Returns time series of (frame, value) pairs. */
static void h_emu_trace_addr(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded"); return;
    }
    char buf[32];
    uint32_t addr = 0x66940;  /* default: shell state */
    if (json_get_str(json, "addr", buf, sizeof(buf))) addr = hex_to_u32(buf);
    int n = json_get_int(json, "frames", 60);
    int pad1 = json_get_int(json, "pad1", 0xFFFF);
    if (n < 1) n = 1;
    if (n > 3000) n = 3000;

    /* Mask to physical 2MB RAM */
    uint32_t phys = addr & 0x1FFFFF;

    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"frames\":%d,\"entries\":[", id, addr, n);

    uint32_t prev_val = 0xDEADBEEF;
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        g_psx_oracle->run_frame((uint16_t)pad1);
        uint32_t val = g_psx_oracle->read_word(phys);
        /* Only emit on change */
        if (val != prev_val) {
            if (emitted > 0) send_fmt(",");
            send_fmt("{\"frame\":%u,\"val\":\"0x%08X\"}", g_psx_oracle->get_frame_count(), val);
            prev_val = val;
            emitted++;
        }
    }
    send_fmt("],\"oracle_frame\":%u}\n", g_psx_oracle->get_frame_count());
}

static void h_find_first_divergence(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded"); return;
    }

    char buf[32];
    uint32_t lo = 0, hi = 0x1FFFFF;
    int context = 16;
    if (json_get_str(json, "lo", buf, sizeof(buf))) lo = hex_to_u32(buf);
    if (json_get_str(json, "hi", buf, sizeof(buf))) hi = hex_to_u32(buf);
    context = json_get_int(json, "context", 16);
    if (context < 0) context = 0;
    if (context > 256) context = 256;
    if (hi > 0x1FFFFF) hi = 0x1FFFFF;
    if (lo > hi) lo = hi;

    /* Snapshot oracle RAM. */
    static uint8_t oracle_ram[0x200000];
    g_psx_oracle->get_ram(oracle_ram);

    const uint8_t *recomp_ram = memory_get_ram_ptr();

    int32_t first_diff = -1;
    int diff_count = 0;
    for (uint32_t i = lo; i <= hi; i++) {
        if (recomp_ram[i] != oracle_ram[i]) {
            if (first_diff < 0) first_diff = (int32_t)i;
            diff_count++;
        }
    }

    if (first_diff < 0) {
        send_fmt("{\"id\":%d,\"ok\":true,\"match\":true,\"bytes_scanned\":%u}\n",
                 id, hi - lo + 1);
        return;
    }

    /* Build context window. */
    uint32_t ctx_lo = (uint32_t)first_diff >= (uint32_t)context
                      ? (uint32_t)first_diff - (uint32_t)context : lo;
    uint32_t ctx_hi = (uint32_t)first_diff + (uint32_t)context;
    if (ctx_hi > hi) ctx_hi = hi;
    if (ctx_lo < lo) ctx_lo = lo;

    /* Emit context bytes. */
    int ctx_len = (int)(ctx_hi - ctx_lo + 1);
    char *rbuf = (char *)malloc((size_t)(ctx_len * 2 + 1));
    char *obuf = (char *)malloc((size_t)(ctx_len * 2 + 1));
    if (!rbuf || !obuf) { free(rbuf); free(obuf); send_err(id, "alloc"); return; }
    for (int i = 0; i < ctx_len; i++) {
        snprintf(rbuf + i * 2, 3, "%02x", recomp_ram[ctx_lo + i]);
        snprintf(obuf + i * 2, 3, "%02x", oracle_ram[ctx_lo + i]);
    }

    send_fmt("{\"id\":%d,\"ok\":true,\"match\":false,"
             "\"first_diff\":\"0x%08X\","
             "\"recomp_val\":\"0x%02X\",\"oracle_val\":\"0x%02X\","
             "\"diff_count\":%d,\"bytes_scanned\":%u,"
             "\"ctx_lo\":\"0x%08X\",\"ctx_hi\":\"0x%08X\","
             "\"recomp_ctx\":\"%s\",\"oracle_ctx\":\"%s\"}\n",
             id, (uint32_t)first_diff,
             recomp_ram[first_diff], oracle_ram[first_diff],
             diff_count, hi - lo + 1,
             ctx_lo, ctx_hi, rbuf, obuf);
    free(rbuf);
    free(obuf);
}

static void h_emu_ram_delta(int id, const char *json) {
    /* TODO: implement once bridge has per-frame snapshot */
    (void)json;
    send_err(id, "not yet implemented");
}

static void h_emu_sync(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded() || !g_psx_oracle->sync_to_state) {
        send_err(id, "oracle not loaded or sync not supported"); return;
    }
    char buf[32];
    uint32_t addr = 0, val = 0;
    uint16_t pad = 0xFFFF; /* no buttons by default */
    int max_frames = 1000;
    if (json_get_str(json, "addr", buf, sizeof(buf))) addr = hex_to_u32(buf);
    if (json_get_str(json, "val", buf, sizeof(buf)))  val  = hex_to_u32(buf);
    if (json_get_str(json, "pad", buf, sizeof(buf)))  pad  = (uint16_t)hex_to_u32(buf);
    max_frames = json_get_int(json, "max_frames", 1000);
    if (max_frames < 1) max_frames = 1;
    if (max_frames > 10000) max_frames = 10000;

    int result = g_psx_oracle->sync_to_state(addr, val, pad, max_frames);
    if (result > 0) {
        send_fmt("{\"id\":%d,\"ok\":true,\"synced\":true,\"frames_needed\":%d,\"oracle_frame\":%u}\n",
                 id, result, g_psx_oracle->get_frame_count());
    } else {
        send_fmt("{\"id\":%d,\"ok\":true,\"synced\":false,\"max_frames\":%d,\"oracle_frame\":%u}\n",
                 id, max_frames, g_psx_oracle->get_frame_count());
    }
}

static void h_emu_sync_press(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded"); return;
    }
    extern int beetle_sync_then_press(uint32_t, uint32_t, uint32_t, uint32_t,
                                       uint16_t, int, int);
    char buf[32];
    uint32_t wait_addr = 0, wait_val = 0, goal_addr = 0, goal_val = 0;
    uint16_t pad = 0xBFFF;
    int wait_max = 2000, press_max = 500;
    if (json_get_str(json, "wait_addr", buf, sizeof(buf))) wait_addr = hex_to_u32(buf);
    if (json_get_str(json, "wait_val", buf, sizeof(buf)))  wait_val  = hex_to_u32(buf);
    if (json_get_str(json, "goal_addr", buf, sizeof(buf))) goal_addr = hex_to_u32(buf);
    if (json_get_str(json, "goal_val", buf, sizeof(buf)))  goal_val  = hex_to_u32(buf);
    if (json_get_str(json, "pad", buf, sizeof(buf)))       pad = (uint16_t)hex_to_u32(buf);
    wait_max = json_get_int(json, "wait_max", 2000);
    press_max = json_get_int(json, "press_max", 500);

    int result = beetle_sync_then_press(wait_addr, wait_val, goal_addr, goal_val,
                                         pad, wait_max, press_max);
    if (result > 0) {
        send_fmt("{\"id\":%d,\"ok\":true,\"synced\":true,\"frames_needed\":%d,\"oracle_frame\":%u}\n",
                 id, result, g_psx_oracle->get_frame_count());
    } else {
        send_fmt("{\"id\":%d,\"ok\":true,\"synced\":false,\"oracle_frame\":%u}\n",
                 id, g_psx_oracle->get_frame_count());
    }
}

static void h_emu_read_vram(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded() || !g_psx_oracle->get_vram) {
        send_err(id, "oracle not loaded or VRAM not supported"); return;
    }
    /* Read a rectangular region of VRAM as hex (16bpp pixels). */
    int x = json_get_int(json, "x", 0);
    int y = json_get_int(json, "y", 0);
    int w = json_get_int(json, "w", 16);
    int h = json_get_int(json, "h", 1);
    if (w < 1) w = 1; if (w > 1024) w = 1024;
    if (h < 1) h = 1; if (h > 512) h = 512;
    if (x < 0) x = 0; if (x + w > 1024) w = 1024 - x;
    if (y < 0) y = 0; if (y + h > 512) h = 512 - y;

    static uint16_t vram[1024 * 512];
    g_psx_oracle->get_vram(vram);

    int pixels = w * h;
    if (pixels > 4096) { send_err(id, "region too large (max 4096 pixels)"); return; }
    char *hex = (char *)malloc((size_t)(pixels * 4 + 1));
    if (!hex) { send_err(id, "alloc"); return; }
    int pos = 0;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t px = vram[(y + row) * 1024 + (x + col)];
            pos += snprintf(hex + pos, 5, "%04x", px);
        }
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"hex\":\"%s\"}\n",
             id, x, y, w, h, hex);
    free(hex);
}

static void h_emu_read_scratchpad(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded() || !g_psx_oracle->get_scratchpad) {
        send_err(id, "oracle not loaded"); return;
    }
    uint8_t scratch[1024];
    g_psx_oracle->get_scratchpad(scratch);
    char buf[32]; int off = 0, len = 1024;
    if (json_get_str(json, "offset", buf, sizeof(buf))) off = (int)hex_to_u32(buf);
    len = json_get_int(json, "len", 1024);
    if (off < 0) off = 0; if (off >= 1024) off = 0;
    if (len < 1) len = 1; if (off + len > 1024) len = 1024 - off;
    char *hex = (char *)malloc((size_t)(len * 2 + 1));
    if (!hex) { send_err(id, "alloc"); return; }
    for (int i = 0; i < len; i++)
        snprintf(hex + i * 2, 3, "%02x", scratch[off + i]);
    send_fmt("{\"id\":%d,\"ok\":true,\"offset\":%d,\"len\":%d,\"hex\":\"%s\"}\n",
             id, off, len, hex);
    free(hex);
}

/* ---- Beetle SIO trace (from beetle_psx_bridge.cpp) ---- */
extern uint32_t beetle_get_sio_trace(uint32_t *out_seq, uint8_t *out_tx,
                                      uint8_t *out_rx, uint16_t *out_ctrl,
                                      int max_count);
extern uint32_t beetle_get_sio_trace_total(void);

static void h_emu_sio_trace(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded");
        return;
    }
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > 65536) count = 65536;  /* match BEETLE_SIO_TRACE_CAP */

    uint32_t *seqs   = (uint32_t *)malloc(count * sizeof(uint32_t));
    uint8_t  *txs    = (uint8_t  *)malloc(count);
    uint8_t  *rxs    = (uint8_t  *)malloc(count);
    uint16_t *ctrls  = (uint16_t *)malloc(count * sizeof(uint16_t));
    if (!seqs || !txs || !rxs || !ctrls) {
        free(seqs); free(txs); free(rxs); free(ctrls);
        send_err(id, "alloc");
        return;
    }

    uint32_t got = beetle_get_sio_trace(seqs, txs, rxs, ctrls, count);
    uint32_t total = beetle_get_sio_trace_total();

    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%u,\"count\":%u,\"entries\":[",
             id, (unsigned)total, (unsigned)got);

    for (uint32_t i = 0; i < got; i++) {
        if (i > 0) send_fmt(",");
        send_fmt("{\"seq\":%u,\"tx\":\"0x%02X\",\"rx\":\"0x%02X\",\"ctrl\":\"0x%04X\"}",
                 (unsigned)seqs[i], txs[i], rxs[i], ctrls[i]);
    }

    send_fmt("]}\n");
    free(seqs); free(txs); free(rxs); free(ctrls);
}

/* ---- SIO first-divergence: parse both streams into card-protocol
 *      transactions (each transaction starts with TX=0x81 and ends at
 *      the next TX=0x81 or pad-select TX=0x01). Compare the most-recent
 *      K transactions from each side, finding the first one where
 *      ours[i].rx != beetle[i].rx within matching TX, or where ours
 *      truncates before beetle continues. */
static int is_card_select(uint8_t tx) { return tx == 0x81; }
static int is_pad_select(uint8_t tx)  { return tx == 0x01; }

#define SIO_FFD_MAX_BYTES 16384
#define SIO_FFD_MAX_TXNS  256

typedef struct {
    int start;   /* index in flat byte array */
    int len;
} SioTxn;

/* Split flat (tx,rx) byte arrays into transactions. Each transaction starts
 * at a TX=0x81 byte and runs until the next 0x81 or until a pad-select 0x01
 * is encountered. Returns transaction count (capped at max_txns). */
static int split_transactions(const uint8_t *txs, int n_bytes,
                              SioTxn *out_txns, int max_txns) {
    int n = 0;
    int cur_start = -1;
    for (int i = 0; i < n_bytes; i++) {
        if (is_card_select(txs[i])) {
            if (cur_start >= 0 && n < max_txns) {
                out_txns[n].start = cur_start;
                out_txns[n].len   = i - cur_start;
                n++;
            }
            cur_start = i;
        } else if (is_pad_select(txs[i])) {
            if (cur_start >= 0 && n < max_txns) {
                out_txns[n].start = cur_start;
                out_txns[n].len   = i - cur_start;
                n++;
            }
            cur_start = -1;
        }
    }
    if (cur_start >= 0 && n < max_txns) {
        /* trailing transaction (no terminator yet) */
        out_txns[n].start = cur_start;
        out_txns[n].len   = n_bytes - cur_start;
        n++;
    }
    return n;
}

static void h_sio_first_divergence(int id, const char *json) {
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded"); return;
    }

    int max_txns = json_get_int(json, "max_txns", 8);
    if (max_txns < 1) max_txns = 1;
    if (max_txns > SIO_FFD_MAX_TXNS) max_txns = SIO_FFD_MAX_TXNS;
    int context = json_get_int(json, "context", 8);
    if (context < 0) context = 0;
    if (context > 256) context = 256;

    /* Allocate flat-byte buffers shared by both sides. */
    uint8_t *our_tx = (uint8_t *)malloc(SIO_FFD_MAX_BYTES);
    uint8_t *our_rx = (uint8_t *)malloc(SIO_FFD_MAX_BYTES);
    uint8_t *bee_tx = (uint8_t *)malloc(SIO_FFD_MAX_BYTES);
    uint8_t *bee_rx = (uint8_t *)malloc(SIO_FFD_MAX_BYTES);
    if (!our_tx || !our_rx || !bee_tx || !bee_rx) {
        free(our_tx); free(our_rx); free(bee_tx); free(bee_rx);
        send_err(id, "alloc"); return;
    }

    /* ---- Read OUR ring (most recent SIO_FFD_MAX_BYTES entries) ---- */
    const SioTraceEntry *our_buf;
    int our_w;
    uint32_t our_total = sio_get_trace(&our_buf, &our_w);
    int our_avail = (int)(our_total < (uint32_t)SIO_TRACE_CAP
                          ? our_total : SIO_TRACE_CAP);
    int our_take = our_avail < SIO_FFD_MAX_BYTES ? our_avail : SIO_FFD_MAX_BYTES;
    int our_n = 0;
    {
        int start = (our_w - our_take + SIO_TRACE_CAP) % SIO_TRACE_CAP;
        for (int i = 0; i < our_take; i++) {
            const SioTraceEntry *e = &our_buf[(start + i) % SIO_TRACE_CAP];
            our_tx[our_n] = e->tx; our_rx[our_n] = e->rx; our_n++;
        }
    }

    /* ---- Read Beetle's ring ---- */
    int beetle_cap = SIO_FFD_MAX_BYTES;
    uint32_t *bseqs  = (uint32_t *)malloc((size_t)beetle_cap * sizeof(uint32_t));
    uint16_t *bctrls = (uint16_t *)malloc((size_t)beetle_cap * sizeof(uint16_t));
    if (!bseqs || !bctrls) {
        free(our_tx); free(our_rx); free(bee_tx); free(bee_rx);
        free(bseqs); free(bctrls);
        send_err(id, "alloc"); return;
    }
    uint32_t bgot = beetle_get_sio_trace(bseqs, bee_tx, bee_rx, bctrls, beetle_cap);
    int bee_n = (int)bgot;
    free(bseqs); free(bctrls);

    /* ---- Split each side into transactions ---- */
    SioTxn *our_txns = (SioTxn *)malloc(sizeof(SioTxn) * SIO_FFD_MAX_TXNS);
    SioTxn *bee_txns = (SioTxn *)malloc(sizeof(SioTxn) * SIO_FFD_MAX_TXNS);
    if (!our_txns || !bee_txns) {
        free(our_tx); free(our_rx); free(bee_tx); free(bee_rx);
        free(our_txns); free(bee_txns); send_err(id, "alloc"); return;
    }
    int our_t = split_transactions(our_tx, our_n, our_txns, SIO_FFD_MAX_TXNS);
    int bee_t = split_transactions(bee_tx, bee_n, bee_txns, SIO_FFD_MAX_TXNS);

    /* ---- Compare the K most-recent transactions on each side, where
     *      K = min(max_txns, available). For each transaction pair
     *      (our[our_t-k-1], beetle[bee_t-k-1] for k=0..K-1, walking from
     *      most-recent backward), find first byte where TX matches but
     *      RX differs, or where lengths/TX diverge. */
    int K = max_txns;
    if (K > our_t) K = our_t;
    if (K > bee_t) K = bee_t;

    /* We report the FIRST diverging transaction (most recent end first).
     * Within that transaction, we report the first byte index where the
     * divergence occurs. */
    int diverge_txn   = -1;   /* k index, 0=most recent */
    int diverge_byte  = -1;
    const char *kind  = "none";

    for (int k = 0; k < K; k++) {
        const SioTxn *ot = &our_txns[our_t - 1 - k];
        const SioTxn *bt = &bee_txns[bee_t - 1 - k];
        int minlen = ot->len < bt->len ? ot->len : bt->len;
        for (int i = 0; i < minlen; i++) {
            uint8_t otx = our_tx[ot->start + i], orx = our_rx[ot->start + i];
            uint8_t btx = bee_tx[bt->start + i], brx = bee_rx[bt->start + i];
            if (otx != btx) { diverge_txn = k; diverge_byte = i; kind = "tx"; goto done; }
            if (orx != brx) { diverge_txn = k; diverge_byte = i; kind = "rx"; goto done; }
        }
        if (ot->len != bt->len) {
            diverge_txn = k; diverge_byte = minlen;
            kind = (ot->len < bt->len) ? "ours_truncated" : "beetle_truncated";
            goto done;
        }
    }
done:;

    int match = (diverge_txn < 0);

    /* Emit JSON. For the diverging transaction (or last K if match), dump
     * bytes from each side with i=byte-within-txn for easy alignment. */
    send_fmt("{\"id\":%d,\"ok\":true,\"match\":%s,"
             "\"our_total_card_txns\":%d,\"beetle_total_card_txns\":%d,"
             "\"compared_txns\":%d,"
             "\"diverge_txn\":%d,\"diverge_byte\":%d,\"diverge_kind\":\"%s\"",
             id, match ? "true" : "false",
             our_t, bee_t, K, diverge_txn, diverge_byte, kind);

    /* Emit the transaction in question (or last txn if matched). */
    int show_k = diverge_txn >= 0 ? diverge_txn : (K > 0 ? K - 1 : 0);
    if (K > 0) {
        const SioTxn *ot = &our_txns[our_t - 1 - show_k];
        const SioTxn *bt = &bee_txns[bee_t - 1 - show_k];
        int center = diverge_byte >= 0 ? diverge_byte : 0;
        int lo = center - context; if (lo < 0) lo = 0;
        int our_hi = center + context; if (our_hi > ot->len) our_hi = ot->len;
        int bee_hi = center + context; if (bee_hi > bt->len) bee_hi = bt->len;

        send_fmt(",\"shown_txn_k\":%d,\"our_len\":%d,\"beetle_len\":%d,\"ours\":[",
                 show_k, ot->len, bt->len);
        for (int i = lo; i < our_hi; i++) {
            if (i > lo) send_fmt(",");
            send_fmt("{\"i\":%d,\"tx\":\"0x%02X\",\"rx\":\"0x%02X\"}",
                     i, our_tx[ot->start + i], our_rx[ot->start + i]);
        }
        send_fmt("],\"beetle\":[");
        for (int i = lo; i < bee_hi; i++) {
            if (i > lo) send_fmt(",");
            send_fmt("{\"i\":%d,\"tx\":\"0x%02X\",\"rx\":\"0x%02X\"}",
                     i, bee_tx[bt->start + i], bee_rx[bt->start + i]);
        }
        send_fmt("]");
    }
    send_fmt("}\n");

    free(our_tx); free(our_rx); free(bee_tx); free(bee_rx);
    free(our_txns); free(bee_txns);
}

/* ---- Beetle screenshot (XRGB8888 framebuffer → BMP) ---- */
extern int beetle_get_framebuffer(uint32_t **out_pixels, unsigned *out_w, unsigned *out_h);

static void h_emu_screenshot(int id, const char *json) {
    (void)json;
    if (!g_psx_oracle || !g_psx_oracle->is_loaded()) {
        send_err(id, "oracle not loaded");
        return;
    }
    uint32_t *pixels = NULL;
    unsigned w = 0, h = 0;
    if (!beetle_get_framebuffer(&pixels, &w, &h) || !pixels || w == 0 || h == 0) {
        send_err(id, "no framebuffer");
        return;
    }
    const char *path = "beetle_screenshot.bmp";
    FILE *f = fopen(path, "wb");
    if (!f) { send_err(id, "cannot open file"); return; }

    uint32_t row_stride = (w * 3 + 3) & ~3u;
    uint32_t pixel_size = row_stride * h;
    uint32_t file_size  = 14 + 40 + pixel_size;

    uint8_t bfh[14] = {0};
    bfh[0] = 'B'; bfh[1] = 'M';
    bfh[2] = file_size & 0xFF; bfh[3] = (file_size >> 8) & 0xFF;
    bfh[4] = (file_size >> 16) & 0xFF; bfh[5] = (file_size >> 24) & 0xFF;
    bfh[10] = 54;
    fwrite(bfh, 1, 14, f);

    uint8_t bih[40] = {0};
    bih[0] = 40;
    bih[4] = w & 0xFF; bih[5] = (w >> 8) & 0xFF;
    int32_t neg_h = -(int32_t)h;
    memcpy(bih + 8, &neg_h, 4);
    bih[12] = 1;
    bih[14] = 24;
    fwrite(bih, 1, 40, f);

    uint8_t *row = (uint8_t *)malloc(row_stride);
    for (unsigned y = 0; y < h; y++) {
        memset(row, 0, row_stride);
        for (unsigned x = 0; x < w; x++) {
            uint32_t px = pixels[y * w + x];
            /* XRGB8888: byte order in memory = B G R X.  Write BGR. */
            row[x*3 + 0] = (uint8_t)(px & 0xFF);
            row[x*3 + 1] = (uint8_t)((px >> 8) & 0xFF);
            row[x*3 + 2] = (uint8_t)((px >> 16) & 0xFF);
        }
        fwrite(row, 1, row_stride, f);
    }
    free(row);
    fclose(f);
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%u,\"height\":%u}\n",
             id, path, w, h);
}

/* ---- Command dispatch ---- */

typedef struct {
    const char *name;
    void (*handler)(int id, const char *json);
} OracleCmd;

static const OracleCmd s_oracle_cmds[] = {
    { "emu_is_loaded",          h_emu_is_loaded },
    { "emu_read_ram",           h_emu_read_ram },
    { "emu_read_vram",          h_emu_read_vram },
    { "emu_read_scratchpad",    h_emu_read_scratchpad },
    { "emu_cpu_regs",           h_emu_cpu_regs },
    { "emu_step",               h_emu_step },
    { "emu_sync",               h_emu_sync },
    { "emu_sync_press",         h_emu_sync_press },
    { "emu_ram_delta",          h_emu_ram_delta },
    { "find_first_divergence",  h_find_first_divergence },
    { "emu_sio_trace",          h_emu_sio_trace },
    { "emu_trace_addr",         h_emu_trace_addr },
    { "emu_screenshot",         h_emu_screenshot },
    { "sio_first_divergence",   h_sio_first_divergence },
    { NULL, NULL }
};

/*
 * Called from debug_server.c process_command() before the "unknown" fallback.
 * Returns 1 if the command was handled, 0 otherwise.
 */
int psx_oracle_handle_cmd(const char *cmd, int id, const char *json) {
    for (const OracleCmd *c = s_oracle_cmds; c->name; c++) {
        if (strcmp(cmd, c->name) == 0) {
            c->handler(id, json);
            return 1;
        }
    }
    return 0;
}

#endif /* ENABLE_DUCKSTATION_ORACLE || ENABLE_BEETLE_PSX_ORACLE */
