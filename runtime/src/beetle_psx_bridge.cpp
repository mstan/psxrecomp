/*
 * beetle_psx_bridge.cpp — Beetle PSX (mednafen-psx) libretro oracle backend.
 *
 * Loads beetle-psx as a statically-linked oracle emulator alongside the
 * recompiled BIOS code, exposing a psx_oracle_backend_t that the generic
 * psx_oracle_cmds.c dispatches through.
 *
 * Only compiled when ENABLE_BEETLE_PSX_ORACLE is defined.
 *
 * Pattern mirrors snesrecomp/runner/src/snes9x_bridge.cpp.
 */

#ifdef ENABLE_BEETLE_PSX_ORACLE

#include "psx_oracle_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <vector>

/* Beetle PSX libretro API. */
#include "libretro.h"

/* Beetle PSX internals — access CPU registers, VRAM, scratchpad directly.
 * These are the same globals beetle uses internally. */
#include "mednafen/psx/psx.h"
#include "mednafen/psx/cpu.h"
#include "mednafen/psx/gpu.h"
#include "mednafen/psx/frontio.h"

namespace {

/* ---- State ---- */
uint32_t  s_framebuf[1024 * 512] = {};
unsigned  s_frame_width  = 320;
unsigned  s_frame_height = 240;
uint16_t  s_joypad       = 0xFFFF;  /* all released (active-low) */
bool      s_loaded       = false;
uint32_t  s_frame_count  = 0;

/* Per-frame RAM snapshot for delta tracking. */
static uint8_t s_ram_before[2 * 1024 * 1024];

/* ---- Beetle SIO byte trace ring buffer ---- */
#define BEETLE_SIO_TRACE_CAP 65536

struct BeetleSioTraceEntry {
    uint32_t seq;
    uint8_t  tx;
    uint8_t  rx;
    uint16_t ctrl;
};

static BeetleSioTraceEntry s_beetle_sio_trace[BEETLE_SIO_TRACE_CAP];
static int     s_beetle_sio_trace_idx = 0;
static uint32_t s_beetle_sio_trace_seq = 0;

static void beetle_sio_trace_callback(uint8_t tx, uint8_t rx, uint16_t ctrl) {
    BeetleSioTraceEntry *e = &s_beetle_sio_trace[s_beetle_sio_trace_idx];
    e->seq  = s_beetle_sio_trace_seq;
    e->tx   = tx;
    e->rx   = rx;
    e->ctrl = ctrl;
    s_beetle_sio_trace_idx = (s_beetle_sio_trace_idx + 1) % BEETLE_SIO_TRACE_CAP;
    s_beetle_sio_trace_seq++;
}

/* Pointer to system directory (where BIOS lives). */
static char s_system_dir[4096] = ".";

/* Capture Beetle's disk-control callback so we can eject the fake disc post-load.
 * Without this, the BIOS sees `dummy.cue` as a real disc and auto-launches CD player
 * instead of showing the shell selector (CD PLAYER + MEMORY CARD). */
static struct retro_disk_control_callback s_disk_cb = {0};
static struct retro_disk_control_ext_callback s_disk_cb_ext = {0};
static int s_have_disk_cb     = 0;
static int s_have_disk_cb_ext = 0;

/* ---- Libretro callbacks ---- */

/* Beetle framebuffer capture (XRGB8888 = 4 bytes/pixel).
 * Max PSX display is 640x512 = 327680 pixels = 1.25 MB.
 * Allocate enough for the worst case. */
#define BEETLE_FB_MAX_PIXELS (640 * 512)
static uint32_t s_beetle_fb[BEETLE_FB_MAX_PIXELS];
static unsigned s_beetle_fb_width  = 0;
static unsigned s_beetle_fb_height = 0;

void retro_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    s_frame_width  = width;
    s_frame_height = height;
    /* Capture framebuffer for visual inspection via beetle_screenshot debug cmd. */
    if (!data || width == 0 || height == 0) return;
    if (width > 640) width = 640;
    if (height > 512) height = 512;
    s_beetle_fb_width  = width;
    s_beetle_fb_height = height;
    const uint8_t *src = (const uint8_t *)data;
    for (unsigned y = 0; y < height; y++) {
        const uint32_t *src_row = (const uint32_t *)(src + y * pitch);
        uint32_t *dst_row = &s_beetle_fb[y * width];
        for (unsigned x = 0; x < width; x++) dst_row[x] = src_row[x];
    }
}

extern "C" int beetle_get_framebuffer(uint32_t **out_pixels,
                                       unsigned *out_w, unsigned *out_h)
{
    if (s_beetle_fb_width == 0 || s_beetle_fb_height == 0) return 0;
    *out_pixels = s_beetle_fb;
    *out_w = s_beetle_fb_width;
    *out_h = s_beetle_fb_height;
    return 1;
}

void retro_audio_sample(int16_t left, int16_t right) {
    /* Discard audio. */
}

size_t retro_audio_sample_batch(const int16_t *data, size_t frames) {
    return frames; /* Discard audio. */
}

void retro_input_poll(void) {
    /* Nothing — we inject pad state via s_joypad. */
}

int16_t retro_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    /* Map libretro button ID → PS1 pad bit (active-low).
     * Libretro: B=0 Y=1 Sel=2 Sta=3 U=4 D=5 L=6 R=7 A=8 X=9 L1=10 R1=11 L2=12 R2=13 L3=14 R3=15
     * PS1 pad:  Sel=0 L3=1 R3=2 Sta=3 U=4 R=5 D=6 L=7 L2=8 R2=9 L1=10 R1=11 Tri=12 Cir=13 X=14 Sq=15 */
    static const int lr_to_psx[16] = {
        14, /* B(Cross)   → PS1 bit 14 */
        15, /* Y(Square)  → PS1 bit 15 */
         0, /* Select     → PS1 bit 0  */
         3, /* Start      → PS1 bit 3  */
         4, /* Up         → PS1 bit 4  */
         6, /* Down       → PS1 bit 6  */
         7, /* Left       → PS1 bit 7  */
         5, /* Right      → PS1 bit 5  */
        13, /* A(Circle)  → PS1 bit 13 */
        12, /* X(Triangle)→ PS1 bit 12 */
        10, /* L1         → PS1 bit 10 */
        11, /* R1         → PS1 bit 11 */
         8, /* L2         → PS1 bit 8  */
         9, /* R2         → PS1 bit 9  */
         1, /* L3         → PS1 bit 1  */
         2, /* R3         → PS1 bit 2  */
    };
    if (id >= 16) return 0;
    int psx_bit = lr_to_psx[id];
    /* PS1 active-low: bit=0 means pressed. Libretro: return 1 for pressed. */
    return ((s_joypad >> psx_bit) & 1) ? 0 : 1;
}

static void retro_log_printf(enum retro_log_level level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

bool retro_environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            *(const char **)data = s_system_dir;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            *(const char **)data = ".";
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            /* Accept whatever format beetle wants. */
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = (struct retro_variable *)data;
            /* Configure for interpreter (no dynarec) and no skip BIOS. */
            if (var && var->key) {
                if (!strcmp(var->key, "beetle_psx_cpu_dynarec"))
                    { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_skip_bios"))
                    { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_override_bios"))
                    { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_bios_region"))
                    { var->value = "any"; return true; }
                if (!strcmp(var->key, "beetle_psx_renderer"))
                    { var->value = "software"; return true; }
                if (!strcmp(var->key, "beetle_psx_analog_toggle"))
                    { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_cd_fastload"))
                    { var->value = "2x(native)"; return true; }
                /* Memory cards: enable both slots. Use MEDNAFEN method
                 * because it's the one that actually does LoadMemcard(0, path)
                 * for slot 0 (the libretro method special-cases slot 0 to
                 * call write-only LoadMemcard(which) then skips i=0 in the
                 * loop, so slot 0 NEVER loads from file).
                 * Per beetle-psx/libretro.cpp ~line 2164. */
                if (!strcmp(var->key, "beetle_psx_use_mednafen_memcard0_method"))
                    { var->value = "mednafen"; return true; }
                if (!strcmp(var->key, "beetle_psx_enable_memcard1"))
                    { var->value = "enabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_shared_memory_cards"))
                    { var->value = "disabled"; return true; }
                /* CD access: sync to avoid async callback issues with dummy disc */
                if (!strcmp(var->key, "beetle_psx_cd_access_method"))
                    { var->value = "sync"; return true; }
            }
            return false;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            *(bool *)data = false;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback *cb = (struct retro_log_callback *)data;
            cb->log = retro_log_printf;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            return false;
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
            /* Capture Beetle's disk control callback for later eject. */
            const struct retro_disk_control_callback *cb =
                (const struct retro_disk_control_callback *)data;
            if (cb) { s_disk_cb = *cb; s_have_disk_cb = 1; }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: {
            const struct retro_disk_control_ext_callback *cb =
                (const struct retro_disk_control_ext_callback *)data;
            if (cb) { s_disk_cb_ext = *cb; s_have_disk_cb_ext = 1; }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: {
            /* Advertise extended disk control so Beetle uses the EXT callback. */
            unsigned *ver = (unsigned *)data;
            if (ver) *ver = 1;
            return true;
        }
        default:
            return false;
    }
}

} /* anonymous namespace */

/* ---- Public C API ---- */

static int beetle_init(const char *bios_path) {
    if (s_loaded) return 0;

    /* Set system directory to the directory containing the BIOS.
     * Beetle PSX looks for scph5501.bin (etc.) in the system dir. */
    strncpy(s_system_dir, bios_path, sizeof(s_system_dir) - 1);
    /* Strip filename, keep directory. */
    char *last_sep = strrchr(s_system_dir, '/');
    if (!last_sep) last_sep = strrchr(s_system_dir, '\\');
    if (last_sep) *last_sep = '\0';
    else strcpy(s_system_dir, ".");

    retro_set_environment(retro_environment);
    retro_set_video_refresh(retro_video_refresh);
    retro_set_audio_sample(retro_audio_sample);
    retro_set_audio_sample_batch(retro_audio_sample_batch);
    retro_set_input_poll(retro_input_poll);
    retro_set_input_state(retro_input_state);

    std::fprintf(stderr, "[beetle-psx] retro_init...\n"); std::fflush(stderr);
    retro_init();

    /* Beetle PSX requires a disc image path even for BIOS-only boot.
     * Use a dummy CUE that points to a minimal BIN. The BIOS will boot
     * to the shell when it can't read a valid PS1 disc. */
    char dummy_cue[4096];
    snprintf(dummy_cue, sizeof(dummy_cue), "%s/dummy.cue", s_system_dir);
    std::fprintf(stderr, "[beetle-psx] retro_load_game(%s)...\n", dummy_cue); std::fflush(stderr);

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = dummy_cue;

    if (!retro_load_game(&info)) {
        std::fprintf(stderr, "[beetle-psx] retro_load_game failed\n");
        retro_deinit();
        return -1;
    }
    std::fprintf(stderr, "[beetle-psx] retro_load_game succeeded\n"); std::fflush(stderr);

    s_loaded = true;
    s_frame_count = 0;

    /* Eject the fake disc so BIOS sees no disc → shows the shell selector
     * (CD PLAYER + MEMORY CARD icons) instead of auto-launching CD player.
     * Done via Beetle's disk control callback captured during init. */
    std::fprintf(stderr, "[beetle-psx] pre-eject: have_ext=%d have=%d\n",
                 s_have_disk_cb_ext, s_have_disk_cb);
    std::fflush(stderr);
    if (s_have_disk_cb_ext && s_disk_cb_ext.set_eject_state) {
        s_disk_cb_ext.set_eject_state(true);
        std::fprintf(stderr, "[beetle-psx] disc ejected → BIOS will show shell selector\n");
    } else if (s_have_disk_cb && s_disk_cb.set_eject_state) {
        s_disk_cb.set_eject_state(true);
        std::fprintf(stderr, "[beetle-psx] disc ejected → BIOS will show shell selector\n");
    }
    std::fflush(stderr);

    /* Sanity-check Beetle memcard files exist on disk so we never silently regress
     * to "Beetle has no cards" again. Beetle expects:
     *   - <save_dir>/<game_basename>.0.mcr  (slot 1 — config = mednafen-method)
     *   - <save_dir>/<game_basename>.1.mcr  (slot 2)
     * With save_dir="." and game="bios/dummy.cue" → "./dummy.0.mcr" / "./dummy.1.mcr".
     * If these are missing, Beetle silently uses blank cards → diff oracle is useless. */
    {
        std::fprintf(stderr, "[beetle-psx] checking memcard files...\n"); std::fflush(stderr);
        const char *card_files[] = { "dummy.0.mcr", "dummy.1.mcr" };
        for (int s = 0; s < 2; s++) {
            FILE *f = fopen(card_files[s], "rb");
            if (!f) {
                std::fprintf(stderr,
                    "[beetle-psx] WARNING: %s missing - Beetle slot %d will be blank.\n",
                    card_files[s], s+1);
                std::fflush(stderr);
                continue;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char magic[2] = {0, 0};
            fread(magic, 1, 2, f);
            fclose(f);
            int valid = (sz == 131072 && magic[0] == 'M' && magic[1] == 'C');
            std::fprintf(stderr,
                "[beetle-psx] memcard slot %d: %s  size=%ld  magic=%c%c  %s\n",
                s+1, card_files[s], sz, magic[0]?magic[0]:'?', magic[1]?magic[1]:'?',
                valid ? "OK" : "INVALID");
            std::fflush(stderr);
        }
    }

    /* Register SIO byte trace callback with Beetle's FrontIO */
    if (PSX_FIO) {
        PSX_FIO->SetSIOTraceCallback(beetle_sio_trace_callback);
        std::fprintf(stderr, "[beetle-psx] SIO trace callback registered\n");
    }

    std::fprintf(stderr, "[beetle-psx] Oracle backend loaded (system_dir=%s)\n", s_system_dir);
    return 0;
}

static void beetle_shutdown(void) {
    if (!s_loaded) return;
    retro_unload_game();
    retro_deinit();
    s_loaded = false;
}

static int beetle_is_loaded(void) {
    return s_loaded ? 1 : 0;
}

static void beetle_run_frame(uint16_t pad1_buttons) {
    if (!s_loaded) return;
    s_joypad = pad1_buttons;

    /* Snapshot RAM before frame for delta tracking. */
    void *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t ram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (ram && ram_size > 0) {
        size_t copy_size = ram_size < sizeof(s_ram_before) ? ram_size : sizeof(s_ram_before);
        memcpy(s_ram_before, ram, copy_size);
    }

    s_frame_count++;
    retro_run();
}

static uint32_t beetle_get_frame_count(void) {
    return s_frame_count;
}

static void beetle_get_ram(uint8_t *out_2mb) {
    void *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t ram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (ram && ram_size > 0) {
        size_t copy_size = ram_size < (2 * 1024 * 1024) ? ram_size : (2 * 1024 * 1024);
        memcpy(out_2mb, ram, copy_size);
        if (copy_size < 2 * 1024 * 1024)
            memset(out_2mb + copy_size, 0, 2 * 1024 * 1024 - copy_size);
    } else {
        memset(out_2mb, 0, 2 * 1024 * 1024);
    }
}

static uint8_t beetle_read_byte(uint32_t phys) {
    /* Main RAM: 0x00000000-0x001FFFFF (2MB, mirrored at 0x00200000 intervals) */
    if (phys < 0x00800000u) {
        uint32_t offset = phys & 0x1FFFFF;
        if (MainRAM) return MainRAM->data8[offset];
    }
    /* Scratchpad: 0x1F800000-0x1F8003FF (1KB) */
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        if (ScratchRAM) return ScratchRAM->data8[phys & 0x3FF];
    }
    /* BIOS ROM: 0x1FC00000-0x1FC7FFFF (512KB) */
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        if (BIOSROM) return BIOSROM->data8[phys & 0x7FFFF];
    }
    return 0;
}

static uint32_t beetle_read_word(uint32_t phys) {
    if (phys < 0x00800000u) {
        uint32_t offset = phys & 0x1FFFFF;
        if (MainRAM && offset + 3 < 0x200000) {
            uint32_t val;
            memcpy(&val, MainRAM->data8 + offset, 4);
            return val;
        }
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        uint32_t offset = phys & 0x3FF;
        if (ScratchRAM && offset + 3 < 1024) {
            uint32_t val;
            memcpy(&val, ScratchRAM->data8 + offset, 4);
            return val;
        }
    }
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        uint32_t offset = phys & 0x7FFFF;
        if (BIOSROM && offset + 3 < 0x80000) {
            uint32_t val;
            memcpy(&val, BIOSROM->data8 + offset, 4);
            return val;
        }
    }
    return 0;
}

static void beetle_get_vram(uint16_t *out_1mb) {
    uint16_t *vram = GPU_get_vram();
    if (vram)
        memcpy(out_1mb, vram, 1024 * 512 * 2);
    else
        memset(out_1mb, 0, 1024 * 512 * 2);
}

static void beetle_get_scratchpad(uint8_t *out_1kb) {
    if (ScratchRAM)
        memcpy(out_1kb, ScratchRAM->data8, 1024);
    else
        memset(out_1kb, 0, 1024);
}

static int beetle_sync_to_state(uint32_t addr, uint32_t val, uint16_t pad, int max_frames) {
    if (!s_loaded) return -1;
    uint32_t offset = addr & 0x1FFFFF;
    for (int i = 0; i < max_frames; i++) {
        beetle_run_frame(pad);
        if (MainRAM) {
            uint32_t cur;
            memcpy(&cur, MainRAM->data8 + offset, 4);
            if (cur == val) return i + 1; /* frames needed */
        }
    }
    return -1; /* timeout */
}

/* Drive Beetle for `frames` frames with the given pad held down (active-low).
 * Alternates pressed/released every frame so the shell sees clean button
 * edges (matches the run_frame cadence from beetle_sync_then_press). */
extern "C" int beetle_press_frames(uint16_t pad, int frames) {
    if (!s_loaded) return -1;
    if (frames < 1) frames = 1;
    for (int i = 0; i < frames; i++) {
        uint16_t frame_pad = (i % 2 == 0) ? 0xFFFF : pad;
        beetle_run_frame(frame_pad);
    }
    return frames;
}

/* Exposed for emu_sync_press: sync to state with no buttons, then
 * run N more frames with pad pressed. Used to navigate the shell
 * menu where input must arrive at exactly the right state. */
extern "C" int beetle_sync_then_press(uint32_t wait_addr, uint32_t wait_val,
                                       uint32_t goal_addr, uint32_t goal_val,
                                       uint16_t pad, int wait_max, int press_max) {
    if (!s_loaded) return -1;
    /* Phase 1: reach wait state with no buttons. */
    int r1 = beetle_sync_to_state(wait_addr, wait_val, 0xFFFF, wait_max);
    if (r1 < 0) return -1;
    /* Phase 2: run with pad pressed until goal state.
     * Alternate between released (0xFFFF) and pressed (pad) to generate
     * button-edge transitions that the shell's input polling detects. */
    uint32_t goal_off = goal_addr & 0x1FFFFF;
    for (int i = 0; i < press_max; i++) {
        /* 2-frame cycle: release then press. This creates a clean
         * button-down edge every 2 frames. */
        uint16_t frame_pad = (i % 2 == 0) ? 0xFFFF : pad;
        beetle_run_frame(frame_pad);
        if (MainRAM) {
            uint32_t cur;
            memcpy(&cur, MainRAM->data8 + goal_off, 4);
            if (cur == goal_val) return r1 + i + 1;
        }
    }
    return -1;
}

static void beetle_get_cpu_regs(PsxCpuRegs *out) {
    memset(out, 0, sizeof(*out));
    if (!s_loaded || !PSX_CPU) return;
    for (int i = 0; i < 32; i++)
        out->gpr[i] = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR + i, NULL, 0);
    out->pc      = PSX_CPU->GetRegister(PS_CPU::GSREG_PC, NULL, 0);
    out->hi      = PSX_CPU->GetRegister(PS_CPU::GSREG_HI, NULL, 0);
    out->lo      = PSX_CPU->GetRegister(PS_CPU::GSREG_LO, NULL, 0);
    out->cop0_sr    = PSX_CPU->GetRegister(PS_CPU::GSREG_SR, NULL, 0);
    out->cop0_cause = PSX_CPU->GetRegister(PS_CPU::GSREG_CAUSE, NULL, 0);
    out->cop0_epc   = PSX_CPU->GetRegister(PS_CPU::GSREG_EPC, NULL, 0);
}

/* ---- Backend instance ---- */

static const psx_oracle_backend_t s_beetle_backend = {
    "beetle-psx",
    beetle_init,
    beetle_shutdown,
    beetle_is_loaded,
    beetle_run_frame,
    beetle_get_frame_count,
    beetle_get_ram,
    beetle_get_vram,
    beetle_get_scratchpad,
    beetle_read_byte,
    beetle_read_word,
    beetle_get_cpu_regs,
    beetle_sync_to_state,
};

/* ---- Convenience wrappers (replaces psx_oracle_stub.c when enabled) ---- */

const psx_oracle_backend_t *g_psx_oracle = nullptr;

extern "C" int psx_oracle_init(const char *bios_path) {
    g_psx_oracle = &s_beetle_backend;
    return g_psx_oracle->init(bios_path);
}

extern "C" void psx_oracle_shutdown(void) {
    if (g_psx_oracle) {
        g_psx_oracle->shutdown();
        g_psx_oracle = nullptr;
    }
}

extern "C" void psx_oracle_run_frame(uint16_t pad1_buttons) {
    if (g_psx_oracle && g_psx_oracle->is_loaded())
        g_psx_oracle->run_frame(pad1_buttons);
}

/* ---- Beetle SIO trace access (for psx_oracle_cmds.c) ---- */

extern "C" uint32_t beetle_get_sio_trace(uint32_t *out_seq, uint8_t *out_tx,
                                          uint8_t *out_rx, uint16_t *out_ctrl,
                                          int max_count)
{
    int avail = (int)(s_beetle_sio_trace_seq < (uint32_t)BEETLE_SIO_TRACE_CAP
                      ? s_beetle_sio_trace_seq : BEETLE_SIO_TRACE_CAP);
    int count = max_count < avail ? max_count : avail;
    int start = (s_beetle_sio_trace_idx - count + BEETLE_SIO_TRACE_CAP) % BEETLE_SIO_TRACE_CAP;

    for (int i = 0; i < count; i++) {
        int idx = (start + i) % BEETLE_SIO_TRACE_CAP;
        const BeetleSioTraceEntry *e = &s_beetle_sio_trace[idx];
        out_seq[i]  = e->seq;
        out_tx[i]   = e->tx;
        out_rx[i]   = e->rx;
        out_ctrl[i] = e->ctrl;
    }
    return (uint32_t)count;
}

extern "C" uint32_t beetle_get_sio_trace_total(void) {
    return s_beetle_sio_trace_seq;
}

#endif /* ENABLE_BEETLE_PSX_ORACLE */
