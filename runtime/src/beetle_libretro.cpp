/* beetle_libretro.cpp — psx-beetle libretro driver + ring buffers.
 *
 * Drives the Beetle PSX (mednafen-psx) libretro core for psx-beetle.
 * Exposes:
 *   - beetle_init / beetle_shutdown / beetle_run_frame  (lifecycle)
 *   - beetle_get_framebuffer / beetle_get_pad           (display + input)
 *   - beetle_read_byte / beetle_read_word               (RAM read)
 *   - beetle_get_ram / beetle_get_vram / beetle_get_scratchpad
 *   - SIO trace ring buffer (beetle_get_sio_trace/reset/total)
 *   - wtrace ring buffer (beetle_wtrace_arm/disarm/reset/get/total/...)
 *   - fntrace ring buffer (beetle_fntrace_arm/disarm/reset/get/total/...)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#include "libretro.h"
#include "mednafen/psx/psx.h"
#include "mednafen/psx/cpu.h"
#include "mednafen/psx/gpu.h"
#include "mednafen/psx/spu.h"
#include "mednafen/psx/frontio.h"
#include "mednafen/psx/irq.h"
#include "beetle_history.h"

/* GPU global lives in PS_GPU's translation unit; psx.h doesn't extern it.
 * Declared here for the per-frame display-state snapshot. NOTE: GPU is
 * a struct VALUE in gpu.cpp (`PS_GPU GPU;`), not a pointer, despite the
 * `extern PS_GPU *GPU;` declaration in beetle-psx/mednafen/psx/debug.cpp.
 * That debug.cpp declaration is buggy and would crash if its file were
 * actually compiled — we get the value-type declaration right here. */
extern PS_GPU GPU;

namespace {

/* ---- State ---- */
unsigned  s_frame_width  = 320;
unsigned  s_frame_height = 240;
uint16_t  s_joypad       = 0xFFFF;  /* all released (active-low) */
bool      s_loaded       = false;
uint32_t  s_frame_count  = 0;

/* ---- SIO byte trace ---- */
#define BEETLE_SIO_TRACE_CAP 65536
struct BeetleSioTraceEntry {
    uint32_t seq; uint8_t tx, rx; uint16_t ctrl;
};
static BeetleSioTraceEntry s_sio_trace[BEETLE_SIO_TRACE_CAP];
static int      s_sio_trace_idx = 0;
static uint32_t s_sio_trace_seq = 0;
static void sio_trace_callback(uint8_t tx, uint8_t rx, uint16_t ctrl) {
    BeetleSioTraceEntry *e = &s_sio_trace[s_sio_trace_idx];
    e->seq = s_sio_trace_seq++; e->tx = tx; e->rx = rx; e->ctrl = ctrl;
    s_sio_trace_idx = (s_sio_trace_idx + 1) % BEETLE_SIO_TRACE_CAP;
}

/* ---- wtrace_all ring (ALWAYS-ON; no filter; captures every write) ----
 * Per global rule "ring buffers always-on": every write hits this ring
 * unconditionally so probes connected late can still query the last ~1s
 * of activity without needing to have armed a range pre-event.
 *
 * Sizing trade-off: 256K * 32B = 8 MB.  At ~250k writes/s idle / ~2M
 * writes/s under heavy emulation, retention window is ~1s under load
 * and ~1s idle.  If the user needs longer retention of specific cells
 * they should arm the focused wtrace ring (large capacity, narrow
 * filter, longer wrap).  The two rings are complementary, NOT
 * redundant — focused gives rich register window + long retention for
 * armed cells; all gives lean coverage of EVERY write for last second. */
#define BEETLE_WTRACE_ALL_CAP 262144
struct BeetleWtraceAllEntry {
    uint64_t seq;
    uint32_t addr;
    uint32_t new_val;
    uint32_t pc;
    uint32_t ra;
    uint32_t frame;
    uint8_t  w;
    uint8_t  pad[3];
};
static BeetleWtraceAllEntry s_wtrace_all[BEETLE_WTRACE_ALL_CAP];
static uint32_t s_wtrace_all_idx = 0;
static uint64_t s_wtrace_all_seq = 0;

/* ---- wtrace ring (filtered by armed physical-RAM ranges) ----
 * Parity contract with runtime's WriteTraceEntry: same field names and
 * widths on the wire, captured by reading PSX_CPU GPRs at callback time.
 *
 * Gaps relative to runtime (documented, never zero-faked):
 *   - old_val: callback fires AFTER the write so the pre-write value is
 *     already gone. Field omitted from beetle output. Wire it later by
 *     extending psxrecomp_wtrace_cb_t to receive the old value.
 *   - func_addr: beetle has no per-function dispatch gate; field omitted.
 *   - cpu_pc vs pc: identical on beetle (single in-order interpreter), so
 *     cpu_pc is reported equal to pc rather than duplicated. */
#define BEETLE_WTRACE_CAP        65536
#define BEETLE_WTRACE_MAX_RANGES 16
struct BeetleWtraceEntry {
    uint64_t seq;
    uint32_t addr;
    uint32_t value;     /* "new" on the wire */
    uint32_t pc;
    uint32_t ra;
    uint32_t sp;
    uint32_t v0, v1;
    uint32_t a0, a1, a2, a3;
    uint32_t t0, t1;
    uint32_t frame;
    uint8_t  slot_idx;  /* beetle-only: current memcard slot byte */
    uint8_t  size;      /* "w" on the wire */
    uint8_t  pad[2];
};
static BeetleWtraceEntry s_wtrace[BEETLE_WTRACE_CAP];
static uint32_t s_wtrace_idx = 0;
static uint64_t s_wtrace_seq = 0;
static struct { uint32_t lo, hi; } s_wtrace_ranges[BEETLE_WTRACE_MAX_RANGES];
static int s_wtrace_range_count = 0;

static void wtrace_callback(uint32_t addr, uint32_t value,
                            uint32_t pc, uint32_t ra, uint8_t size)
{
    /* ALWAYS-ON catch-all ring: lean fields, no filter, fires before the
     * focused-range filter so a connected probe can read recent writes
     * regardless of what's armed. */
    {
        BeetleWtraceAllEntry *a = &s_wtrace_all[s_wtrace_all_idx];
        a->seq     = s_wtrace_all_seq++;
        a->addr    = addr;
        a->new_val = value;
        a->pc      = pc;
        a->ra      = ra;
        a->frame   = s_frame_count;
        a->w       = size;
        s_wtrace_all_idx = (s_wtrace_all_idx + 1) % BEETLE_WTRACE_ALL_CAP;
    }

    if (s_wtrace_range_count == 0) return;
    uint32_t phys = addr & 0x1FFFFFFFu;
    int hit = 0;
    for (int i = 0; i < s_wtrace_range_count; i++) {
        uint32_t lo = s_wtrace_ranges[i].lo;
        uint32_t hi = s_wtrace_ranges[i].hi;
        uint32_t phi = phys + size;
        if (phi > lo && phys < hi) { hit = 1; break; }
    }
    if (!hit) return;

    uint8_t slot = 0;
    if (MainRAM) slot = MainRAM->data8[0x7264];

    BeetleWtraceEntry *e = &s_wtrace[s_wtrace_idx];
    e->seq      = s_wtrace_seq++;
    e->addr     = addr;
    e->value    = value;
    e->pc       = pc;
    e->ra       = ra;
    e->frame    = s_frame_count;
    e->slot_idx = slot;
    e->size     = size;

    /* Rich register window — read live from PSX_CPU. Parity with runtime's
     * WriteTraceEntry. Only paid on filter hits so volume stays bounded. */
    if (PSX_CPU) {
        char dummy[8] = {0};
        e->sp = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR + 29, dummy, sizeof(dummy));
        e->v0 = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR +  2, dummy, sizeof(dummy));
        e->v1 = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR +  3, dummy, sizeof(dummy));
        e->a0 = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR +  4, dummy, sizeof(dummy));
        e->a1 = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR +  5, dummy, sizeof(dummy));
        e->a2 = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR +  6, dummy, sizeof(dummy));
        e->a3 = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR +  7, dummy, sizeof(dummy));
        e->t0 = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR +  8, dummy, sizeof(dummy));
        e->t1 = PSX_CPU->GetRegister(PS_CPU::GSREG_GPR +  9, dummy, sizeof(dummy));
    } else {
        e->sp = e->v0 = e->v1 = 0;
        e->a0 = e->a1 = e->a2 = e->a3 = 0;
        e->t0 = e->t1 = 0;
    }
    s_wtrace_idx = (s_wtrace_idx + 1) % BEETLE_WTRACE_CAP;
}

/* ---- fntrace ring (filtered by armed target PCs, or unfiltered) ---- */
#define BEETLE_FNTRACE_CAP        65536
#define BEETLE_FNTRACE_MAX_ARMS   64
struct BeetleFnTraceEntry {
    uint64_t seq;
    uint32_t caller_pc, target_pc, parent_ra, a0, a1, frame;
    uint8_t  kind, pad[3];
};
static BeetleFnTraceEntry s_fntrace[BEETLE_FNTRACE_CAP];
static uint32_t s_fntrace_idx = 0;
static uint64_t s_fntrace_seq = 0;
static uint32_t s_fntrace_arms[BEETLE_FNTRACE_MAX_ARMS];
static int      s_fntrace_arm_count = 0;
static int      s_fntrace_unfiltered = 0;

static void fntrace_callback(uint32_t caller_pc, uint32_t target_pc,
                             uint32_t parent_ra, uint32_t a0, uint32_t a1,
                             uint8_t kind)
{
    if (!s_fntrace_unfiltered) {
        if (s_fntrace_arm_count == 0) return;
        uint32_t tphys = target_pc & 0x1FFFFFFFu;
        int hit = 0;
        for (int i = 0; i < s_fntrace_arm_count; i++)
            if (s_fntrace_arms[i] == tphys) { hit = 1; break; }
        if (!hit) return;
    }
    BeetleFnTraceEntry *e = &s_fntrace[s_fntrace_idx];
    e->seq       = s_fntrace_seq++;
    e->caller_pc = caller_pc;
    e->target_pc = target_pc;
    e->parent_ra = parent_ra;
    e->a0        = a0;
    e->a1        = a1;
    e->frame     = s_frame_count;
    e->kind      = kind;
    s_fntrace_idx = (s_fntrace_idx + 1) % BEETLE_FNTRACE_CAP;
}

/* ---- Disk control + system dir ---- */
static char s_system_dir[4096] = ".";
static struct retro_disk_control_callback     s_disk_cb     = {0};
static struct retro_disk_control_ext_callback s_disk_cb_ext = {0};
static int s_have_disk_cb     = 0;
static int s_have_disk_cb_ext = 0;

/* ---- Framebuffer capture ---- */
#define BEETLE_FB_MAX_PIXELS (640 * 512)
static uint32_t s_fb[BEETLE_FB_MAX_PIXELS];
static unsigned s_fb_width  = 0;
static unsigned s_fb_height = 0;

void on_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    s_frame_width  = width;
    s_frame_height = height;
    if (!data || width == 0 || height == 0) return;
    if (width  > 640) width  = 640;
    if (height > 512) height = 512;
    s_fb_width  = width;
    s_fb_height = height;
    const uint8_t *src = (const uint8_t *)data;
    for (unsigned y = 0; y < height; y++) {
        const uint32_t *src_row = (const uint32_t *)(src + y * pitch);
        uint32_t *dst_row = &s_fb[y * width];
        for (unsigned x = 0; x < width; x++) dst_row[x] = src_row[x];
    }
}

void on_audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
size_t on_audio_sample_batch(const int16_t *data, size_t frames) { (void)data; return frames; }
void on_input_poll(void) {}

int16_t on_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    /* Map libretro button ID → PS1 pad bit. */
    static const int lr_to_psx[16] = {
        14, 15,  0,  3,  4,  6,  7,  5, 13, 12, 10, 11,  8,  9,  1,  2
    };
    if (id >= 16) return 0;
    int psx_bit = lr_to_psx[id];
    return ((s_joypad >> psx_bit) & 1) ? 0 : 1;
}

void on_log_printf(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

bool on_environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = s_system_dir; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char**)data = ".";          return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:                                          return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = (struct retro_variable *)data;
            if (var && var->key) {
                if (!strcmp(var->key, "beetle_psx_cpu_dynarec"))                { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_skip_bios"))                  { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_override_bios"))              { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_bios_region"))                { var->value = "any";      return true; }
                if (!strcmp(var->key, "beetle_psx_renderer"))                   { var->value = "software"; return true; }
                if (!strcmp(var->key, "beetle_psx_analog_toggle"))              { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_cd_fastload"))                { var->value = "2x(native)"; return true; }
                if (!strcmp(var->key, "beetle_psx_use_mednafen_memcard0_method")){ var->value = "mednafen";  return true; }
                if (!strcmp(var->key, "beetle_psx_enable_memcard1"))            { var->value = "enabled";  return true; }
                if (!strcmp(var->key, "beetle_psx_shared_memory_cards"))        { var->value = "disabled"; return true; }
                if (!strcmp(var->key, "beetle_psx_cd_access_method"))           { var->value = "sync";     return true; }
            }
            return false;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:  *(bool*)data = false; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback *cb = (struct retro_log_callback *)data;
            cb->log = on_log_printf;
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
            unsigned *ver = (unsigned*)data;
            if (ver) *ver = 1;
            return true;
        }
        default: return false;
    }
}

} /* anonymous namespace */

/* ============================================================ */
/* ============== Public extern "C" interface =================== */
/* ============================================================ */

extern "C" int beetle_init_with_disc(const char *bios_path, const char *disc_path);

extern "C" int beetle_init(const char *bios_path) {
    return beetle_init_with_disc(bios_path, NULL);
}

extern "C" int beetle_init_with_disc(const char *bios_path, const char *disc_path) {
    if (s_loaded) return 0;

    /* Allocate the per-frame history ring once. Mirrors runtime's
     * frame_history allocation in debug_server_init(). */
    beetle_history_init();

    /* Set system directory to the directory containing the BIOS. */
    strncpy(s_system_dir, bios_path, sizeof(s_system_dir) - 1);
    char *last_sep = strrchr(s_system_dir, '/');
    if (!last_sep) last_sep = strrchr(s_system_dir, '\\');
    if (last_sep) *last_sep = '\0';
    else strcpy(s_system_dir, ".");

    retro_set_environment(on_environment);
    retro_set_video_refresh(on_video_refresh);
    retro_set_audio_sample(on_audio_sample);
    retro_set_audio_sample_batch(on_audio_sample_batch);
    retro_set_input_poll(on_input_poll);
    retro_set_input_state(on_input_state);

    std::fprintf(stderr, "[psx-beetle] retro_init...\n"); std::fflush(stderr);
    retro_init();

    /* If disc_path is non-NULL, load that real disc and keep it inserted.
     * Otherwise fall back to the dummy-cue + eject behavior so the BIOS
     * shows its shell (memcard editor / CD player). */
    char load_path[4096];
    if (disc_path && *disc_path) {
        strncpy(load_path, disc_path, sizeof(load_path) - 1);
        load_path[sizeof(load_path) - 1] = '\0';
    } else {
        snprintf(load_path, sizeof(load_path), "%s/dummy.cue", s_system_dir);
    }
    std::fprintf(stderr, "[psx-beetle] retro_load_game(%s)...\n", load_path); std::fflush(stderr);

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = load_path;
    if (!retro_load_game(&info)) {
        std::fprintf(stderr, "[psx-beetle] retro_load_game failed\n");
        retro_deinit();
        return -1;
    }

    s_loaded = true;
    s_frame_count = 0;

    /* Eject the fake disc so BIOS shows the shell selector — only when no
     * real disc was provided. With a real disc, keep it inserted so the
     * BIOS proceeds straight into game boot. */
    if (!disc_path || !*disc_path) {
        if (s_have_disk_cb_ext && s_disk_cb_ext.set_eject_state) {
            s_disk_cb_ext.set_eject_state(true);
        } else if (s_have_disk_cb && s_disk_cb.set_eject_state) {
            s_disk_cb.set_eject_state(true);
        }
    }

    /* Memcard sanity-check (warn-only). */
    {
        const char *card_files[] = { "dummy.0.mcr", "dummy.1.mcr" };
        for (int sl = 0; sl < 2; sl++) {
            FILE *f = fopen(card_files[sl], "rb");
            if (!f) {
                std::fprintf(stderr,
                    "[psx-beetle] WARNING: %s missing — slot %d will be blank\n",
                    card_files[sl], sl + 1);
                continue;
            }
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            unsigned char magic[2] = {0, 0};
            fread(magic, 1, 2, f); fclose(f);
            int valid = (sz == 131072 && magic[0] == 'M' && magic[1] == 'C');
            std::fprintf(stderr,
                "[psx-beetle] memcard slot %d: %s size=%ld magic=%c%c %s\n",
                sl + 1, card_files[sl], sz,
                magic[0]?magic[0]:'?', magic[1]?magic[1]:'?',
                valid ? "OK" : "INVALID");
        }
    }

    if (PSX_FIO) {
        PSX_FIO->SetSIOTraceCallback(sio_trace_callback);
        std::fprintf(stderr, "[psx-beetle] SIO trace callback registered\n");
    }

    g_psxrecomp_wtrace_cb = wtrace_callback;
    g_psxrecomp_fntrace_cb = fntrace_callback;
    std::fprintf(stderr, "[psx-beetle] wtrace + fntrace callbacks registered\n");
    std::fflush(stderr);

    return 0;
}

extern "C" void beetle_shutdown(void) {
    if (!s_loaded) return;
    retro_unload_game();
    retro_deinit();
    s_loaded = false;
}

extern "C" void beetle_run_frame(uint16_t pad1_buttons) {
    if (!s_loaded) return;
    s_joypad = pad1_buttons;
    s_frame_count++;
    retro_run();
    beetle_history_record_frame();
}

extern "C" int beetle_get_framebuffer(uint32_t **out_pixels,
                                       unsigned *out_w, unsigned *out_h) {
    if (s_fb_width == 0 || s_fb_height == 0) return 0;
    *out_pixels = s_fb;
    *out_w = s_fb_width;
    *out_h = s_fb_height;
    return 1;
}

extern "C" uint16_t beetle_get_pad(void) { return s_joypad; }
extern "C" uint32_t beetle_get_frame_count(void) { return s_frame_count; }
extern "C" int beetle_is_loaded(void) { return s_loaded ? 1 : 0; }

extern "C" uint8_t beetle_read_byte(uint32_t phys) {
    if (phys < 0x00800000u) {
        uint32_t offset = phys & 0x1FFFFF;
        if (MainRAM) return MainRAM->data8[offset];
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        if (ScratchRAM) return ScratchRAM->data8[phys & 0x3FF];
    }
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        if (BIOSROM) return BIOSROM->data8[phys & 0x7FFFF];
    }
    return 0;
}

extern "C" uint32_t beetle_read_word(uint32_t phys) {
    if (phys < 0x00800000u) {
        uint32_t offset = phys & 0x1FFFFF;
        if (MainRAM && offset + 3 < 0x200000) {
            uint32_t val; memcpy(&val, MainRAM->data8 + offset, 4); return val;
        }
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        uint32_t offset = phys & 0x3FF;
        if (ScratchRAM && offset + 3 < 1024) {
            uint32_t val; memcpy(&val, ScratchRAM->data8 + offset, 4); return val;
        }
    }
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        uint32_t offset = phys & 0x7FFFF;
        if (BIOSROM && offset + 3 < 0x80000) {
            uint32_t val; memcpy(&val, BIOSROM->data8 + offset, 4); return val;
        }
    }
    return 0;
}

extern "C" void beetle_get_ram(uint8_t *out_2mb) {
    void *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t ram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (ram && ram_size > 0) {
        size_t copy = ram_size < (2u * 1024 * 1024) ? ram_size : (2u * 1024 * 1024);
        memcpy(out_2mb, ram, copy);
        if (copy < 2u * 1024 * 1024) memset(out_2mb + copy, 0, 2u * 1024 * 1024 - copy);
    } else {
        memset(out_2mb, 0, 2u * 1024 * 1024);
    }
}

extern "C" void beetle_get_vram(uint16_t *out_1mb) {
    uint16_t *vram = GPU_get_vram();
    if (vram) memcpy(out_1mb, vram, 1024 * 512 * 2);
    else      memset(out_1mb, 0, 1024 * 512 * 2);
}

extern "C" void beetle_get_scratchpad(uint8_t *out_1kb) {
    if (ScratchRAM) memcpy(out_1kb, ScratchRAM->data8, 1024);
    else            memset(out_1kb, 0, 1024);
}

/* ---- SIO trace accessors ---- */
extern "C" uint32_t beetle_get_sio_trace(uint32_t *out_seq, uint8_t *out_tx,
                                          uint8_t *out_rx, uint16_t *out_ctrl,
                                          int max_count)
{
    int avail = (int)(s_sio_trace_seq < (uint32_t)BEETLE_SIO_TRACE_CAP
                      ? s_sio_trace_seq : BEETLE_SIO_TRACE_CAP);
    int count = max_count < avail ? max_count : avail;
    int start = (s_sio_trace_idx - count + BEETLE_SIO_TRACE_CAP) % BEETLE_SIO_TRACE_CAP;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % BEETLE_SIO_TRACE_CAP;
        const BeetleSioTraceEntry *e = &s_sio_trace[idx];
        out_seq[i]  = e->seq;
        out_tx[i]   = e->tx;
        out_rx[i]   = e->rx;
        out_ctrl[i] = e->ctrl;
    }
    return (uint32_t)count;
}
extern "C" uint32_t beetle_get_sio_trace_total(void) { return s_sio_trace_seq; }
extern "C" void beetle_reset_sio_trace(void) {
    s_sio_trace_idx = 0;
    s_sio_trace_seq = 0;
    memset(s_sio_trace, 0, sizeof(s_sio_trace));
}

/* ---- wtrace accessors ---- */
extern "C" int beetle_wtrace_arm(uint32_t lo, uint32_t hi) {
    if (s_wtrace_range_count >= BEETLE_WTRACE_MAX_RANGES) return -1;
    if (lo >= hi) return -2;
    int slot = s_wtrace_range_count++;
    s_wtrace_ranges[slot].lo = lo & 0x1FFFFFFFu;
    s_wtrace_ranges[slot].hi = hi & 0x1FFFFFFFu;
    return slot;
}
extern "C" void beetle_wtrace_disarm_all(void) { s_wtrace_range_count = 0; }
extern "C" int  beetle_wtrace_disarm(int slot) {
    if (slot < 0 || slot >= s_wtrace_range_count) return -1;
    /* Compact remaining slots down — matches runtime's wtrace_del semantics. */
    for (int i = slot; i < s_wtrace_range_count - 1; i++)
        s_wtrace_ranges[i] = s_wtrace_ranges[i + 1];
    s_wtrace_range_count--;
    return 0;
}
extern "C" int  beetle_wtrace_range_count(void)  { return s_wtrace_range_count; }
extern "C" int  beetle_wtrace_max_ranges(void)   { return BEETLE_WTRACE_MAX_RANGES; }
extern "C" int  beetle_wtrace_capacity(void)     { return BEETLE_WTRACE_CAP; }
extern "C" int  beetle_wtrace_get_range(int slot, uint32_t *out_lo, uint32_t *out_hi) {
    if (slot < 0 || slot >= s_wtrace_range_count) return -1;
    *out_lo = s_wtrace_ranges[slot].lo;
    *out_hi = s_wtrace_ranges[slot].hi;
    return 0;
}
extern "C" void beetle_wtrace_reset(void) {
    s_wtrace_idx = 0; s_wtrace_seq = 0;
    memset(s_wtrace, 0, sizeof(s_wtrace));
}
extern "C" uint64_t beetle_wtrace_total(void) { return s_wtrace_seq; }

/* ---- wtrace_all (always-on, no-filter) accessors ---- */
extern "C" uint64_t beetle_wtrace_all_total(void) { return s_wtrace_all_seq; }
extern "C" int      beetle_wtrace_all_capacity(void) { return BEETLE_WTRACE_ALL_CAP; }
extern "C" void     beetle_wtrace_all_reset(void) {
    s_wtrace_all_idx = 0; s_wtrace_all_seq = 0;
    memset(s_wtrace_all, 0, sizeof(s_wtrace_all));
}
extern "C" uint32_t beetle_wtrace_all_get(uint64_t *out_seq, uint32_t *out_addr,
                                          uint32_t *out_new, uint32_t *out_pc,
                                          uint32_t *out_ra, uint32_t *out_frame,
                                          uint8_t *out_w, int max_count)
{
    int avail = (int)(s_wtrace_all_seq < (uint64_t)BEETLE_WTRACE_ALL_CAP
                      ? s_wtrace_all_seq : BEETLE_WTRACE_ALL_CAP);
    int count = max_count < avail ? max_count : avail;
    int start = ((int)s_wtrace_all_idx - count + BEETLE_WTRACE_ALL_CAP) % BEETLE_WTRACE_ALL_CAP;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % BEETLE_WTRACE_ALL_CAP;
        const BeetleWtraceAllEntry *e = &s_wtrace_all[idx];
        out_seq[i]   = e->seq;
        out_addr[i]  = e->addr;
        out_new[i]   = e->new_val;
        out_pc[i]    = e->pc;
        out_ra[i]    = e->ra;
        out_frame[i] = e->frame;
        out_w[i]     = e->w;
    }
    return (uint32_t)count;
}

/* Rich-window accessor. Out-params mirror the runtime's WriteTraceEntry
 * fields, with the documented gaps (old_val, func_addr) absent. */
extern "C" uint32_t beetle_wtrace_get_rich(uint64_t *out_seq, uint32_t *out_addr,
                                           uint32_t *out_value, uint32_t *out_pc,
                                           uint32_t *out_ra,
                                           uint32_t *out_sp,
                                           uint32_t *out_v0, uint32_t *out_v1,
                                           uint32_t *out_a0, uint32_t *out_a1,
                                           uint32_t *out_a2, uint32_t *out_a3,
                                           uint32_t *out_t0, uint32_t *out_t1,
                                           uint32_t *out_frame,
                                           uint8_t *out_slot, uint8_t *out_size,
                                           int max_count)
{
    int avail = (int)(s_wtrace_seq < (uint64_t)BEETLE_WTRACE_CAP
                      ? s_wtrace_seq : BEETLE_WTRACE_CAP);
    int count = max_count < avail ? max_count : avail;
    int start = ((int)s_wtrace_idx - count + BEETLE_WTRACE_CAP) % BEETLE_WTRACE_CAP;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % BEETLE_WTRACE_CAP;
        const BeetleWtraceEntry *e = &s_wtrace[idx];
        out_seq[i]   = e->seq;
        out_addr[i]  = e->addr;
        out_value[i] = e->value;
        out_pc[i]    = e->pc;
        out_ra[i]    = e->ra;
        out_sp[i]    = e->sp;
        out_v0[i]    = e->v0;  out_v1[i] = e->v1;
        out_a0[i]    = e->a0;  out_a1[i] = e->a1;
        out_a2[i]    = e->a2;  out_a3[i] = e->a3;
        out_t0[i]    = e->t0;  out_t1[i] = e->t1;
        out_frame[i] = e->frame;
        out_slot[i]  = e->slot_idx;
        out_size[i]  = e->size;
    }
    return (uint32_t)count;
}


/* ---- fntrace accessors ---- */
extern "C" int beetle_fntrace_arm(uint32_t target_pc) {
    if (s_fntrace_arm_count >= BEETLE_FNTRACE_MAX_ARMS) return -1;
    uint32_t phys = target_pc & 0x1FFFFFFFu;
    for (int i = 0; i < s_fntrace_arm_count; i++)
        if (s_fntrace_arms[i] == phys) return i;
    int slot = s_fntrace_arm_count++;
    s_fntrace_arms[slot] = phys;
    return slot;
}
extern "C" void beetle_fntrace_disarm_all(void) { s_fntrace_arm_count = 0; }
extern "C" int  beetle_fntrace_arm_count(void)  { return s_fntrace_arm_count; }
extern "C" uint32_t beetle_fntrace_get_arm(int slot) {
    if (slot < 0 || slot >= s_fntrace_arm_count) return 0;
    return s_fntrace_arms[slot];
}
extern "C" void beetle_fntrace_set_unfiltered(int on) { s_fntrace_unfiltered = on ? 1 : 0; }
extern "C" void beetle_fntrace_reset(void) {
    s_fntrace_idx = 0; s_fntrace_seq = 0;
    memset(s_fntrace, 0, sizeof(s_fntrace));
}
extern "C" uint64_t beetle_fntrace_total(void) { return s_fntrace_seq; }

/* ---- SPU event ring (always-on, mirrors recomp's spu_events).
 *
 * Beetle's spu.cpp calls psxrecomp_beetle_spu_event() at every transition
 * we care about: KEYON, KEYOFF, END_STOP (loop-end-no-repeat), END_LOOP
 * (loop-end-with-repeat). Each entry stamps frame + voice + envelope
 * level + sample address, so the boot-chime timeline can be diffed
 * directly against recomp's port-4370 spu_events output. */
/* 1M entries × ~32 B = 32 MB. Sized for full boot-chime capture plus
 * minutes of in-game timeline; mirrors recomp's SPU_EVENT_CAP exactly. */
#define BEETLE_SPU_EVENT_CAP (1u << 20)
struct BeetleSpuEvent {
    uint64_t seq;
    uint32_t frame;
    uint32_t addr;
    uint16_t env;
    uint16_t pitch;
    uint16_t vol_l_ctrl;
    uint16_t vol_r_ctrl;
    uint16_t adsr_lo;
    uint16_t adsr_hi;
    uint8_t  kind;     /* 1=KEYON 2=KEYOFF 3=END_STOP 4=END_LOOP */
    uint8_t  voice;
};
static BeetleSpuEvent s_spu_events[BEETLE_SPU_EVENT_CAP];
static uint32_t s_spu_event_idx = 0;
static uint64_t s_spu_event_seq = 0;

extern "C" void psxrecomp_beetle_spu_event(unsigned kind, unsigned voice,
                                           unsigned addr, unsigned env)
{
    /* Snapshot the per-voice raw control registers via PS_SPU peek so the
     * payload is structurally identical to recomp's SpuEvent. */
    BeetleSpuEvent *e = &s_spu_events[s_spu_event_idx & (BEETLE_SPU_EVENT_CAP - 1u)];
    e->seq    = s_spu_event_seq++;
    e->frame  = s_frame_count;
    e->addr   = addr;
    e->env    = (uint16_t)env;
    e->kind   = (uint8_t)kind;
    e->voice  = (uint8_t)voice;
    if (PSX_SPU && voice < 24u) {
        char dummy[32]; const uint32_t dl = sizeof(dummy);
        unsigned base = 0x8000u | ((unsigned)voice << 8);
        e->pitch       = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_PITCH      & 0xFF), dummy, dl);
        e->vol_l_ctrl  = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_VOL_CTRL_L & 0xFF), dummy, dl);
        e->vol_r_ctrl  = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_VOL_CTRL_R & 0xFF), dummy, dl);
        unsigned ctrl  =          PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_ADSR_CTRL  & 0xFF), dummy, dl);
        e->adsr_lo     = (uint16_t)(ctrl & 0xFFFFu);
        e->adsr_hi     = (uint16_t)((ctrl >> 16) & 0xFFFFu);
    } else {
        e->pitch = e->vol_l_ctrl = e->vol_r_ctrl = e->adsr_lo = e->adsr_hi = 0;
    }
    s_spu_event_idx++;
}

extern "C" uint64_t beetle_spu_event_total(void) { return s_spu_event_seq; }

extern "C" uint32_t beetle_spu_event_get(uint64_t *out_seq, uint32_t *out_frame,
                                         uint32_t *out_addr, uint16_t *out_env,
                                         uint16_t *out_pitch,
                                         uint16_t *out_vol_l, uint16_t *out_vol_r,
                                         uint16_t *out_adsr_lo, uint16_t *out_adsr_hi,
                                         uint8_t *out_kind, uint8_t *out_voice,
                                         uint32_t max_count)
{
    uint64_t avail = s_spu_event_seq < (uint64_t)BEETLE_SPU_EVENT_CAP
                     ? s_spu_event_seq : (uint64_t)BEETLE_SPU_EVENT_CAP;
    if ((uint64_t)max_count > avail) max_count = (uint32_t)avail;
    uint32_t start = (s_spu_event_idx + BEETLE_SPU_EVENT_CAP - max_count) & (BEETLE_SPU_EVENT_CAP - 1u);
    for (uint32_t i = 0; i < max_count; i++) {
        const BeetleSpuEvent *e = &s_spu_events[(start + i) & (BEETLE_SPU_EVENT_CAP - 1u)];
        out_seq[i]     = e->seq;
        out_frame[i]   = e->frame;
        out_addr[i]    = e->addr;
        out_env[i]     = e->env;
        out_pitch[i]   = e->pitch;
        out_vol_l[i]   = e->vol_l_ctrl;
        out_vol_r[i]   = e->vol_r_ctrl;
        out_adsr_lo[i] = e->adsr_lo;
        out_adsr_hi[i] = e->adsr_hi;
        out_kind[i]    = e->kind;
        out_voice[i]   = e->voice;
    }
    return max_count;
}

/* ---- SPU register peeks via PS_SPU::GetRegister ----
 *
 * Mirrors the spu_voices wire protocol psx-runtime exposes on port 4370
 * so cross-process diff tooling sees the same JSON schema on both ports.
 * Beetle's SPU keeps full ADSR, sweep volumes, distinct StartAddr/CurAddr/
 * LoopAddr, and a real BlockEnd latch — so this is the oracle ground truth
 * for "is voice v actually voicing right now and where in the sample?"
 */
extern "C" int beetle_spu_get_voice_state(int v,
    uint16_t *vol_ctrl_l, uint16_t *vol_ctrl_r,
    uint16_t *vol_l,      uint16_t *vol_r,
    uint16_t *pitch,
    uint32_t *start_addr, uint32_t *cur_addr, uint32_t *loop_addr,
    uint32_t *adsr_ctrl,  uint16_t *adsr_level)
{
    if (v < 0 || v >= 24 || !PSX_SPU) return 0;
    unsigned base = 0x8000u | ((unsigned)v << 8);
    char dummy[32]; const uint32_t dl = sizeof(dummy);
    *vol_ctrl_l = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_VOL_CTRL_L & 0xFF), dummy, dl);
    *vol_ctrl_r = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_VOL_CTRL_R & 0xFF), dummy, dl);
    *vol_l      = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_VOL_L      & 0xFF), dummy, dl);
    *vol_r      = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_VOL_R      & 0xFF), dummy, dl);
    *pitch      = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_PITCH      & 0xFF), dummy, dl);
    *start_addr =          PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_STARTADDR   & 0xFF), dummy, dl);
    *cur_addr   =          PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_READ_ADDR   & 0xFF), dummy, dl);
    *loop_addr  =          PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_LOOP_ADDR   & 0xFF), dummy, dl);
    *adsr_ctrl  =          PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_ADSR_CTRL   & 0xFF), dummy, dl);
    *adsr_level = (uint16_t)PSX_SPU->GetRegister(base + (PS_SPU::GSREG_V0_ADSR_LEVEL & 0xFF), dummy, dl);
    return 1;
}

extern "C" int beetle_spu_get_global_state(
    uint16_t *spu_ctrl,
    uint16_t *main_vol_ctrl_l, uint16_t *main_vol_ctrl_r,
    uint16_t *main_vol_l,      uint16_t *main_vol_r,
    uint32_t *fm_mode, uint32_t *noise_mode, uint32_t *reverb_mode,
    uint32_t *voice_on, uint32_t *voice_off, uint32_t *block_end)
{
    if (!PSX_SPU) return 0;
    char dummy[32]; const uint32_t dl = sizeof(dummy);
    *spu_ctrl         = (uint16_t)PSX_SPU->GetRegister(PS_SPU::GSREG_SPUCONTROL,      dummy, dl);
    *main_vol_ctrl_l  = (uint16_t)PSX_SPU->GetRegister(PS_SPU::GSREG_MAINVOL_CTRL_L,  dummy, dl);
    *main_vol_ctrl_r  = (uint16_t)PSX_SPU->GetRegister(PS_SPU::GSREG_MAINVOL_CTRL_R,  dummy, dl);
    *main_vol_l       = (uint16_t)PSX_SPU->GetRegister(PS_SPU::GSREG_MAINVOL_L,       dummy, dl);
    *main_vol_r       = (uint16_t)PSX_SPU->GetRegister(PS_SPU::GSREG_MAINVOL_R,       dummy, dl);
    *fm_mode          =          PSX_SPU->GetRegister(PS_SPU::GSREG_FM_ON,           dummy, dl);
    *noise_mode       =          PSX_SPU->GetRegister(PS_SPU::GSREG_NOISE_ON,        dummy, dl);
    *reverb_mode      =          PSX_SPU->GetRegister(PS_SPU::GSREG_REVERB_ON,       dummy, dl);
    *voice_on         =          PSX_SPU->GetRegister(PS_SPU::GSREG_VOICEON,         dummy, dl);
    *voice_off        =          PSX_SPU->GetRegister(PS_SPU::GSREG_VOICEOFF,        dummy, dl);
    *block_end        =          PSX_SPU->GetRegister(PS_SPU::GSREG_BLOCKEND,        dummy, dl);
    return 1;
}

extern "C" uint32_t beetle_fntrace_get(uint64_t *out_seq,
                                       uint32_t *out_caller, uint32_t *out_target,
                                       uint32_t *out_ra, uint32_t *out_a0,
                                       uint32_t *out_a1, uint32_t *out_frame,
                                       uint8_t *out_kind, int max_count)
{
    int avail = (int)(s_fntrace_seq < (uint64_t)BEETLE_FNTRACE_CAP
                      ? s_fntrace_seq : BEETLE_FNTRACE_CAP);
    int count = max_count < avail ? max_count : avail;
    int start = ((int)s_fntrace_idx - count + BEETLE_FNTRACE_CAP) % BEETLE_FNTRACE_CAP;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % BEETLE_FNTRACE_CAP;
        const BeetleFnTraceEntry *e = &s_fntrace[idx];
        out_seq[i]    = e->seq;
        out_caller[i] = e->caller_pc;
        out_target[i] = e->target_pc;
        out_ra[i]     = e->parent_ra;
        out_a0[i]     = e->a0;
        out_a1[i]     = e->a1;
        out_frame[i]  = e->frame;
        out_kind[i]   = e->kind;
    }
    return (uint32_t)count;
}

/* ====================================================================== */
/* ====================  PER-FRAME HISTORY RING  ======================== */
/* ====================================================================== */
/* Mirrors runtime/src/debug_server.c's s_frame_history. Same capacity,
 * same recording cadence (post-frame), same snapshot model
 * (4 configurable regions × 128 bytes per frame).
 *
 * Why duplicate the runtime's mechanism instead of sharing code: psx-beetle
 * has no recompiler CPUState; it reads MIPS state via PSX_CPU::GetRegister
 * and RAM via the MainRAM byte array. The runtime's recorder reads s_cpu
 * (recomp CPUState) and psx_read_byte. Different sources, same wire shape. */

static BeetleFrameRecord *s_beetle_history = nullptr;
static uint64_t           s_beetle_history_count = 0;
static uint32_t           s_beetle_snap_addrs[BEETLE_RAM_SNAPSHOT_REGIONS];
static int                s_beetle_snap_active[BEETLE_RAM_SNAPSHOT_REGIONS];

extern "C" void beetle_history_init(void) {
    if (s_beetle_history) return;
    s_beetle_history = (BeetleFrameRecord *)std::calloc(
        BEETLE_FRAME_HISTORY_CAP, sizeof(BeetleFrameRecord));
    s_beetle_history_count = 0;
    std::memset(s_beetle_snap_addrs,  0, sizeof(s_beetle_snap_addrs));
    std::memset(s_beetle_snap_active, 0, sizeof(s_beetle_snap_active));
    if (!s_beetle_history) {
        std::fprintf(stderr,
            "[psx-beetle] WARNING: failed to allocate %u-frame history ring\n",
            (unsigned)BEETLE_FRAME_HISTORY_CAP);
    }
}

extern "C" void beetle_history_record_frame(void) {
    if (!s_beetle_history || !PSX_CPU) return;

    uint32_t idx = (uint32_t)(s_frame_count % BEETLE_FRAME_HISTORY_CAP);
    BeetleFrameRecord *r = &s_beetle_history[idx];

    r->frame_number = s_frame_count;
    r->verify_pass  = -1;     /* no cross-check verify mode on beetle */
    r->diff_count   = 0;

    /* MIPS CPU state via PS_CPU::GetRegister. The `special` buffer is
     * unused by the GPR/HI/LO/COP0-SR/CAUSE/EPC paths in cpu.cpp, but
     * we pass a real backing array so future register kinds that DO
     * print into it can't smash the stack. */
    char dummy[64] = {0};
    for (int i = 0; i < 32; i++) {
        r->gpr[i] = PSX_CPU->GetRegister(
            (unsigned)(PS_CPU::GSREG_GPR + i), dummy, sizeof(dummy));
    }
    r->hi = PSX_CPU->GetRegister(PS_CPU::GSREG_HI, dummy, sizeof(dummy));
    r->lo = PSX_CPU->GetRegister(PS_CPU::GSREG_LO, dummy, sizeof(dummy));
    r->cop0_sr    = PSX_CPU->GetRegister(PS_CPU::GSREG_SR,    dummy, sizeof(dummy));
    r->cop0_cause = PSX_CPU->GetRegister(PS_CPU::GSREG_CAUSE, dummy, sizeof(dummy));
    r->cop0_epc   = PSX_CPU->GetRegister(PS_CPU::GSREG_EPC,   dummy, sizeof(dummy));

    /* Interrupt controller. */
    r->i_stat = IRQ_GetRegister(IRQ_GSREG_STATUS, dummy, sizeof(dummy));
    r->i_mask = IRQ_GetRegister(IRQ_GSREG_MASK,   dummy, sizeof(dummy));

    /* GPU display state.  PS_GPU's fields are public.  We snapshot the
     * raw register-level XStart/YStart for origin; framebuffer width/height
     * already mirrored by on_video_refresh into s_fb_*. */
    r->display_area_x   = (uint16_t)GPU.DisplayFB_XStart;
    r->display_area_y   = (uint16_t)GPU.DisplayFB_YStart;
    r->display_w        = (uint16_t)s_fb_width;
    r->display_h        = (uint16_t)s_fb_height;
    r->display_disabled = GPU.DisplayOff ? 1 : 0;

    /* SIO / pad.  pad_buttons follows runtime convention (active-low
     * 16-bit mask).  sio_stat/sio_ctrl left zero; beetle's libretro
     * surface doesn't expose them and inventing values would obscure
     * real divergences in the time-series. */
    r->pad_buttons = s_joypad;
    r->sio_stat = 0;
    r->sio_ctrl = 0;

    /* Timing: beetle has no dispatch table to count gates.  Leave zero
     * dispatch_count; total_dispatches = frame# so the field is
     * monotonic and frame_timeseries can still graph it. */
    r->dispatch_count   = 0;
    r->total_dispatches = s_frame_count;

    /* Per-frame configurable RAM snapshot regions. */
    for (int i = 0; i < BEETLE_RAM_SNAPSHOT_REGIONS; i++) {
        r->snapshot_addr[i] = s_beetle_snap_addrs[i];
        if (s_beetle_snap_active[i] && s_beetle_snap_addrs[i] != 0 && MainRAM) {
            uint32_t base = s_beetle_snap_addrs[i] & 0x1FFFFFu;
            for (int j = 0; j < BEETLE_RAM_SNAPSHOT_SIZE; j++) {
                uint32_t off = (base + (uint32_t)j) & 0x1FFFFFu;
                r->snapshot_data[i][j] = MainRAM->data8[off];
            }
        } else {
            std::memset(r->snapshot_data[i], 0, BEETLE_RAM_SNAPSHOT_SIZE);
        }
    }

    s_beetle_history_count = (uint64_t)s_frame_count + 1;
}

extern "C" void beetle_history_get_bounds(uint64_t *out_count,
                                          uint64_t *out_oldest,
                                          uint64_t *out_newest)
{
    uint64_t cnt = s_beetle_history_count;
    uint64_t oldest = (cnt > BEETLE_FRAME_HISTORY_CAP)
                     ? cnt - BEETLE_FRAME_HISTORY_CAP : 0;
    uint64_t newest = (cnt > 0) ? cnt - 1 : 0;
    if (out_count)  *out_count  = cnt;
    if (out_oldest) *out_oldest = oldest;
    if (out_newest) *out_newest = newest;
}

extern "C" const BeetleFrameRecord *beetle_history_get_frame(uint32_t frame) {
    if (!s_beetle_history) return nullptr;
    uint64_t cnt = s_beetle_history_count;
    uint64_t oldest = (cnt > BEETLE_FRAME_HISTORY_CAP)
                     ? cnt - BEETLE_FRAME_HISTORY_CAP : 0;
    if ((uint64_t)frame < oldest || (uint64_t)frame >= cnt) return nullptr;

    uint32_t idx = frame % BEETLE_FRAME_HISTORY_CAP;
    const BeetleFrameRecord *r = &s_beetle_history[idx];
    if (r->frame_number != frame) return nullptr;  /* slot reused since */
    return r;
}

extern "C" int beetle_history_first_failure(uint32_t *out_frame, int *out_diff_count) {
    /* No verify-mode on beetle yet; mirrors runtime's "no failures" path. */
    if (out_frame)      *out_frame      = (uint32_t)-1;
    if (out_diff_count) *out_diff_count = 0;
    return 0;
}

extern "C" int beetle_history_set_snapshot(int slot, uint32_t addr) {
    if (slot < 0 || slot >= BEETLE_RAM_SNAPSHOT_REGIONS) return 0;
    s_beetle_snap_addrs[slot]  = addr;
    s_beetle_snap_active[slot] = (addr != 0) ? 1 : 0;
    return 1;
}

extern "C" int beetle_history_get_snapshot(int slot, uint32_t *out_addr, int *out_active) {
    if (slot < 0 || slot >= BEETLE_RAM_SNAPSHOT_REGIONS) return 0;
    if (out_addr)   *out_addr   = s_beetle_snap_addrs[slot];
    if (out_active) *out_active = s_beetle_snap_active[slot];
    return 1;
}
