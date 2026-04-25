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
    if (n < 1) n = 1;
    if (n > 600) n = 600;
    for (int i = 0; i < n; i++) {
        g_psx_oracle->run_frame(0xFFFF); /* no buttons */
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"frames_stepped\":%d,\"oracle_frame\":%u}\n",
             id, n, g_psx_oracle->get_frame_count());
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
