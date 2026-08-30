/* main.cpp — Phase 3 runtime entry point.
 *
 * Loads BIOS ROM, initializes CPU state + SDL display, calls into
 * the recompiled reset vector. BIOS drives execution; SDL presents
 * VRAM at each vblank via callback from gpu_vblank_tick().
 */

#include "cpu_state.h"
#include "psx_scheduler.h"   /* psx_scheduler_run — deterministic TCB scheduler */
#include "parity_trace.h"    /* general two-process control-flow parity ring */
#include "device_trace.h"    /* general two-process device-event cycle ring */
#include "psx_interpreter.h"
#include "cdrom.h"
#include "fntrace.h"
#include "text_xlate.h"
#include "tex_pack.h"
#include "boot_state.h"
#include "bios_hle.h"
#include "bios_hle_plan.h"
#include "psx_bios_backend.h"
#include "psx_cycles.h"
#include "starvation_ring.h"
#include "load_accel.h"
#include "savestate.h"
#include "psx_rewind.h"
#include "psx_savestate_menu.h"
#include "host_osd.h"
#include "host_keymap.h"
#include "png_write.h"       /* png_write_rgb — present_shot readback */
#include "overlay_capture.h"
#include "overlay_loader.h"
#include "autocompile.h"
#include "code_provider.h"
extern "C" void psx_event_step_conservative_env_init(void);
#include "overlay_backend.h"
#include "gpu.h"
#include "pgxp.h"
#include "interrupts.h"
#include "present_ring.h"
#include "load_transition_ring.h"
#include "gpu_sw_renderer.h"
#include "gpu_render.h"
#include "gpu_gl_renderer.h"

/* Declarations only -- STB_IMAGE_IMPLEMENTATION lives in
 * psx_window_icon.cpp, whose STBI_NO_STDIO must be matched here or the
 * declarations disagree with the definitions that exist. */
#include <random>
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"
#include "gpu_vk_renderer.h"
#include "frame_pacing.h"
#include "guest_clock.h"
#include "latency_ring.h"
#include "sio.h"
#ifndef PSX_MAX_PLAYERS
#define PSX_MAX_PLAYERS 2
#endif
#include "psx_netplay.h"
#include "psx_stick.h"       /* radial SDL-stick -> DualShock response transform */
#include "psx_netplay_rb.h"
#include "psx_selfcheck.h"
#include "psx_lobby_client.h"
#include "spu.h"
#include "audio_trace.h"
#include "spu_shadow.h"

/* Shared clock-domain bridge: band-limited polyphase resampler + P-only DRC.
 * The SPU renders at a fixed 44100/60 per wall-clock frame while the host
 * consumes on its own crystal; with no resampling/DRC the queue slowly drifts
 * to underrun (silence gaps). The bridge resamples ~1:1 with a <=+/-0.5% ratio
 * trim to hold the ring near target -- no gaps. See recomp_audio_drc.h. */
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
#include "memcard.h"
#include "debug_server.h"
#include "crash_trace.h"
#include "freeze_heartbeat.h"
#include "config_loader.h"
#include "bios_rom_alias.h"
#include "launcher_device.h"
#include "game_options.h"
#include "mod_plugins.h"
#include "psx_ram.h"
#include "mod_runtime.h"
#include "crc32.h"
#include "disc_identity.h"
#include "disc_path.h"
#include "iso_reader.h"      /* text-image guard: extract the boot EXE from the disc */
#include "psx_keybinds.h"    /* configurable keyboard->DualShock keybinds (keybinds.ini) */
#include "psx_window_icon.h"

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"   /* shared recomp-ui Dear ImGui launcher */
#include "launcher_profile.h"  /* per-system variant profile (theme/caps bundle) */
#include "launcher_boot_timing.h" /* PSX_LAUNCHER_BOOT_TIMING stamps */
#if defined(PSX_HAS_GAME_CODEGEN)
extern "C" void psx_game_codegen_setup_apply(RecompLauncherCGameInfo* gi);
extern "C" void psx_game_codegen_relaunch_or_exit(const char* disc_path);
extern "C" void psx_game_codegen_forward_if_built(int argc, char** argv);
#endif
#endif
#include "psx_sdl.h"
#if defined(PSX_SDL3)
/*
 * SDL_main.h is a single-header implementation in SDL3. Keep it in the one
 * translation unit that defines main(); including it through psx_sdl.h makes
 * every SDL-using source emit WinMain under MinGW.
 */
#include <SDL3/SDL_main.h>
#endif
#include "psx_sdl_audio.h"
#if defined(PSX_WEB)
#include <emscripten/emscripten.h>
#endif
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <psapi.h>   /* GetModuleInformation (self-profile sampler) */
#include <commdlg.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef PSX_DEFAULT_BIOS_PATH
/* Compile-time fallback name only — not an implicit player choice.
 * Runtime default with no explicit pick is bios/openbios.bin (see
 * resolve_bios_for_runtime / docs/BIOS_SELECTION.md). */
#define PSX_DEFAULT_BIOS_PATH "bios/SCPH1001.BIN"
#endif
#ifndef PSX_BUNDLED_BIOS_PATH
#define PSX_BUNDLED_BIOS_PATH "bios/openbios.bin"
#endif
#ifndef PSX_DEFAULT_GAME_CONFIG_PATH
#define PSX_DEFAULT_GAME_CONFIG_PATH ""
#endif
#ifndef PSX_WINDOW_TITLE
#define PSX_WINDOW_TITLE "psxrecomp"
#endif
#ifndef DEFAULT_DEBUG_PORT
#error DEFAULT_DEBUG_PORT must be defined by the runtime target.
#endif

extern "C" uint64_t gte_get_exec_count(void);

/* Cross-language globals defined in C translation units. Declared extern "C" at
 * file scope so MSVC gives them C linkage (matching the C definitions); without
 * this MSVC name-mangles the C++ references and they fail to link. GCC/Clang do
 * not mangle namespace-scope variables, so this is a no-op there. The existing
 * block-scope `extern` redeclarations inside functions inherit this C linkage. */
extern "C" {
    extern uint64_t psx_cycle_count;
    extern uint64_t s_frame_count;
    extern uint32_t g_overlay_region_floor;
    extern uint32_t g_text_image_lo;
    extern int      g_psx_cps_mode;
    extern uint64_t g_slice_fired, g_slice_irq_taken, g_dirty_ram_insns_run;
    extern uint64_t g_dirty_window_dispatches;
    extern uint32_t g_slice_exit_pc, g_slice_exit_reason, g_slice_exit_iter;
    extern uint32_t g_slice_exit_dispatchable, g_slice_exit_dirty, g_slice_exit_in_text, g_slice_exit_want;
}

/* memory.c */
extern "C" void     memory_init(const char* bios_path);
extern "C" void     memory_set_sr_ptr(const uint32_t *p);
/* interrupts.c */
extern "C" void     psx_irq_set_cause_ptr(uint32_t *p);
extern "C" void     psx_set_midframe_audio_pump(void (*fn)(void));
extern "C" uint32_t memory_get_bios_checksum(void);
extern "C" void     dirty_ram_register_text_image(uint32_t phys_lo,
                                                  const uint8_t *bytes,
                                                  uint32_t len);

/* Arm the dirty-RAM text-image guard with the boot EXE bytes. The guard is
 * load-bearing: dispatch native-safety (dirty_ram_text_native_ok) and the
 * fntrace alternate game-start latch both key off the registered image, so
 * every install must arm it — not just repo checkouts that happen to carry a
 * loose EXE copy next to game.toml. Source order:
 *   1. The local EXE file from game.toml (dev checkouts; recompiler input).
 *   2. The boot EXE extracted from the mounted disc image (end-user installs
 *      — the disc is the same bytes the BIOS loads, i.e. the true reference).
 * Registration passes ownership of the malloc'd buffer to memory.c. */
static void arm_text_image_guard(const std::string &exe_path,
                                 uint32_t load_address,
                                 const std::string &disc_path) {
    const uint32_t phys_lo = load_address & 0x1FFFFFFFu;
    /* 1. Local EXE file (skip the 2048-byte PS-X EXE header). */
    if (!exe_path.empty()) {
        std::ifstream ef(exe_path, std::ios::binary | std::ios::ate);
        if (ef) {
            std::streamsize sz = ef.tellg();
            if (sz > 2048) {
                uint32_t img_len = (uint32_t)(sz - 2048);
                uint8_t *img = (uint8_t *)std::malloc(img_len);
                if (img) {
                    ef.seekg(2048, std::ios::beg);
                    if (ef.read((char *)img, img_len)) {
                        dirty_ram_register_text_image(phys_lo, img, img_len);
                        std::fprintf(stdout,
                            "psxrecomp: text image guard armed (0x%08X..0x%08X, local EXE)\n",
                            load_address, load_address + img_len);
                        return;
                    }
                    std::free(img);
                }
            }
        }
    }
    /* 2. Extract the boot EXE from the disc image. */
    if (!disc_path.empty()) {
        PS1::ISOReader iso;
        if (iso.Open(disc_path)) {
            /* The boot filename: basename of the game.toml exe field (it names
             * the same file the recompiler consumed, which came off this disc);
             * fall back to the SYSTEM.CNF BOOT token for configs whose local
             * name differs from the disc name. */
            std::string boot_name;
            if (!exe_path.empty()) {
                const size_t slash = exe_path.find_last_of("/\\");
                boot_name = (slash == std::string::npos)
                                ? exe_path : exe_path.substr(slash + 1);
            }
            PS1::ISOFileEntry ent;
            if (boot_name.empty() || !iso.FindFile(boot_name, ent)) {
                /* SYSTEM.CNF: `BOOT = cdrom:\SCUS_944.23;1` */
                uint8_t cnf[2048] = {0};
                size_t n = iso.ReadFile("SYSTEM.CNF", cnf, sizeof(cnf) - 1);
                if (n > 0) {
                    std::string text((const char *)cnf, n);
                    std::string lower = text;
                    for (char &c : lower) c = (char)std::tolower((unsigned char)c);
                    const size_t key = lower.find("cdrom:");
                    if (key != std::string::npos) {
                        size_t j = key + 6;
                        while (j < text.size() && (text[j] == '\\' || text[j] == '/')) j++;
                        std::string tok;
                        while (j < text.size()) {
                            char c = text[j];
                            if (c == ';' || c == '\r' || c == '\n' || c == ' ' ||
                                c == '\t' || c == '\0') break;
                            tok += c; j++;
                            if (tok.size() > 64) break;
                        }
                        const size_t s2 = tok.find_last_of("\\/");
                        if (s2 != std::string::npos) tok = tok.substr(s2 + 1);
                        if (!tok.empty() && iso.FindFile(tok, ent)) boot_name = tok;
                    }
                }
            }
            if (!boot_name.empty() && iso.FindFile(boot_name, ent) &&
                ent.size > 2048) {
                uint8_t *file = (uint8_t *)std::malloc(ent.size);
                if (file) {
                    size_t got = iso.ReadFile(boot_name, file, ent.size);
                    if (got > 2048) {
                        uint32_t img_len = (uint32_t)(got - 2048);
                        uint8_t *img = (uint8_t *)std::malloc(img_len);
                        if (img) {
                            memcpy(img, file + 2048, img_len);
                            std::free(file);
                            dirty_ram_register_text_image(phys_lo, img, img_len);
                            std::fprintf(stdout,
                                "psxrecomp: text image guard armed (0x%08X..0x%08X, disc %s)\n",
                                load_address, load_address + img_len,
                                boot_name.c_str());
                            return;
                        }
                    }
                    std::free(file);
                }
            }
        }
    }
    std::fprintf(stdout,
        "psxrecomp: WARNING: text image guard NOT armed (no local EXE, no disc "
        "boot EXE) — native text dispatch will be conservative\n");
}

/* dma.c */
extern "C" void dma_init(void);

/* mdec.c */
extern "C" void mdec_init(void);
extern "C" int  mdec_recently_active(uint32_t within_frames);

/* timers.c */
extern "C" void timers_init(void);

/* interrupts.c */
extern "C" void interrupts_init(void);
#ifdef PSX_COSIM
extern "C" void cosim_init(void);  /* first-divergence oracle server (cosim.c) */
#endif
extern "C" uint32_t psx_read_word(uint32_t addr);
extern "C" void     psx_write_word(uint32_t addr, uint32_t val);
extern "C" uint16_t psx_read_half(uint32_t addr);
extern "C" void     psx_write_half(uint32_t addr, uint16_t val);
extern "C" uint8_t  psx_read_byte(uint32_t addr);
extern "C" void     psx_write_byte(uint32_t addr, uint8_t val);
/* Guest-side data-read wrappers: same as psx_read_* but charge PS1 main-RAM
 * read wait states (R3000A has no D-cache). Wired to cpu->read_* below so the
 * timing applies to recompiled + interpreted guest loads, not debug/device reads. */
extern "C" uint32_t psx_guest_read_word(uint32_t addr);
extern "C" uint16_t psx_guest_read_half(uint32_t addr);
extern "C" uint8_t  psx_guest_read_byte(uint32_t addr);

/* ---- SDL state ---- */
extern "C" {
SDL_Window* sdl_window = nullptr;
}
static SDL_Renderer* sdl_renderer;
static SDL_Texture*  sdl_texture;
/* Per-player input device routing (PSX ports 1 & 2). Seeded from the
 * [controller] settings the launcher writes; the runtime opens the matching
 * SDL controller (or uses the keyboard) and feeds each PSX pad slot. */
struct PlayerInput {
    int   kind = 0;            /* 0=none, 1=keyboard, 2=controller */
    char  guid[40] = {0};      /* SDL joystick GUID string when kind==controller */
    /* Pad input mode (PSXRecompV4::PadMode): 1=analog (default), 0=hybrid
     * (MOD-ONLY, requested via psx_mod_set_controller_mode_override),
     * 2=digital. hybrid_analog is the per-frame auto-switch latch used only in
     * hybrid mode: true => currently presenting DualShock (stick was the last
     * input), false => currently presenting a digital pad (D-pad was last). */
    int   mode = PSXRecompV4::PAD_MODE_ANALOG;
    bool  hybrid_analog = false;
    int   deadzone = 3277;  /* raw SDL axis units, ~10% default */
    SDL_GameController* handle = nullptr;
    SDL_JoystickID      instance = -1;
    uint8_t rumble_small = 0;
    uint8_t rumble_large = 0;
    bool    rumble_known = false;
    bool    rumble_warned = false;
};
static PlayerInput g_players[PSX_MAX_PLAYERS];
/* Offline SIO sample loop bound (from game.toml players; clamped). */
static int g_offline_pad_count = 2;
/* Set when [controller] lock_mode pins every seat to digital — blocks the
 * DualShock-on-tap hack from settings.toml / Lobby Settings / match_caps. */
static int g_force_digital_pads = 0;
/* Offline seat ceiling from game.toml players, optionally capped at 2 when
 * the launcher Multitap toggle is off (3+ player titles). Netplay ignores
 * this and arms multitap whenever session slot_count > 2. */
static void apply_offline_pad_count(int game_players, bool multitap_enabled)
{
    int n = game_players > 0 ? game_players : 1;
    if (n > PSX_MAX_PLAYERS) n = PSX_MAX_PLAYERS;
    if (game_players >= 3 && !multitap_enabled && n > 2)
        n = 2;
    g_offline_pad_count = n;
}
/* ARGB8888 staging buffer. Sized for the active internal resolution:
 * 640*scale x 512*scale. Allocated once the supersampling scale is known
 * (sized for the native 640x512 when supersampling is off). */
static uint32_t*     sdl_pixel_buf = nullptr;

typedef void (*ModFrameHook)(void);

static std::vector<ModFrameHook>& mod_frame_hooks() {
    static std::vector<ModFrameHook> hooks;
    return hooks;
}

/* Statically linked mod code can register per-frame callbacks here. Hooks run
 * after normal input sampling and before presentation/widescreen frame work. */
extern "C" void mod_register_frame_hook(ModFrameHook hook) {
    if (hook) mod_frame_hooks().push_back(hook);
}

static void mod_call_frame_hooks() {
    auto& hooks = mod_frame_hooks();
    const size_t count = hooks.size();
    for (size_t i = 0; i < count; i++) hooks[i]();
}

/* Presentation-only interpolation for software-rendered content that repeats
 * each guest image for two vblanks. This never changes guest or audio timing. */
static std::atomic<int> g_smooth_60fps{0};

struct Smooth60State {
    std::vector<uint32_t> previous_source;
    uint64_t source_hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool have_source = false;
    bool previous_was_duplicate = false;
    bool announced = false;
};

static Smooth60State g_smooth_60_state;

/* Present-path session state — must reset on rematch (function-local statics
 * survive soft-return and poison FMV/FPS after session_reboot). */
static bool     s_disabled_frame_presented = false;
static bool     s_force_present_after_load = false;
/* §33 SW hold-last: sdl_texture is 640x512; Live only uploads the active
 * display rect. Resim must reuse that src/dst — RenderCopy(NULL,NULL) sticks
 * the image in the upper-left corner (user-confirmed). */
static int s_sw_hold_valid = 0;
static SDL_Rect s_sw_hold_src;
static SDL_Rect s_sw_hold_dst;

/* After LOADED: optional freeze probe (PSX_POST_LOAD_PROBE=1). Off by default. */
static int      s_post_load_probe_enabled = -1; /* -1 = unread env */
static int      s_post_load_probe_left = 0;
static int      s_post_load_probe_i = 0;
static uint64_t s_post_load_probe_gp00 = 0;
static uint64_t s_post_load_probe_skip_tot = 0;
static uint64_t s_post_load_probe_swap_tot = 0;
static uint64_t s_post_load_probe_dirty_tot = 0;
static uint64_t s_post_load_probe_gp0_tot = 0;
static uint64_t s_post_load_probe_vb_raise0 = 0;
static uint64_t s_post_load_probe_vb_deliv0 = 0;
static uint64_t s_post_load_probe_vb_ack0 = 0;
static uint64_t s_post_load_probe_dirty_blks0 = 0;
static uint64_t s_post_load_probe_chk_entry0 = 0;
static uint64_t s_post_load_probe_chk_fsr0 = 0;
static uint64_t s_post_load_probe_chk_fnone0 = 0;
static uint64_t s_post_load_probe_chk_mid0 = 0;
static uint64_t s_post_load_probe_chk_eval0 = 0;
static uint64_t s_post_load_probe_chk_deliv0 = 0;
static uint64_t s_post_load_probe_cyc0 = 0;
static uint64_t s_post_load_probe_ci_unit0 = 0;
static uint64_t s_post_load_probe_ci_supp0 = 0;
static uint64_t s_post_load_probe_ci_none0 = 0;
static uint64_t s_post_load_probe_ci_sr0 = 0;
static uint64_t s_post_load_probe_ci_deliv0 = 0;
static uint64_t s_post_load_probe_ci_enter0 = 0;
static uint64_t s_post_load_probe_adv_calls0 = 0;
static uint64_t s_post_load_probe_adv_sum0 = 0;
static uint64_t s_post_load_probe_svc0 = 0;
static uint64_t s_post_load_probe_dirty_insns0 = 0;
static uint64_t s_post_load_probe_dirty_pump0 = 0;
static uint64_t s_post_load_probe_stores0 = 0;
static uint64_t s_post_load_probe_idle_n0 = 0;
static uint64_t s_post_load_probe_idle_cyc0 = 0;
static uint64_t s_post_load_probe_hz_hits0 = 0;
static uint64_t s_post_load_probe_hz_cyc0 = 0;
static Uint64   s_post_load_probe_host_t0 = 0;
static int      s_post_load_probe_stall_run = 0;
static int      s_post_load_probe_in_stall = 0;
#define POST_LOAD_STALL_PC_CAP 8
static uint32_t s_stall_pc[POST_LOAD_STALL_PC_CAP];
static uint32_t s_stall_pc_n[POST_LOAD_STALL_PC_CAP];
static int      s_stall_pc_used = 0;
#define POST_LOAD_LIVE_PC_CAP 8
static uint32_t s_live_pc[POST_LOAD_LIVE_PC_CAP];
static uint32_t s_live_pc_n[POST_LOAD_LIVE_PC_CAP];
static int      s_live_pc_used = 0;

static int post_load_probe_env_on(void) {
    if (s_post_load_probe_enabled < 0) {
        const char *e = std::getenv("PSX_POST_LOAD_PROBE");
        s_post_load_probe_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return s_post_load_probe_enabled;
}
static Uint64   s_fps_last_time = 0;
static uint64_t s_fps_last_frame = 0;
static std::string s_fps_base_title;
static int      s_fps_telemetry_enabled = -1; /* -1 = unread env */
static FramePacer s_frame_pacer = { 0 };
/* Last CRTC timing applied to the host presenter. Kept as exact guest-cycle
 * state so the 2% panel-match test cannot flap on floating-point rounding. */
static uint32_t s_present_crtc_period_cycles = 0;
static int      s_present_crtc_is_pal = 0;
static bool     s_present_crtc_banner_logged = false;
static int      s_turbo_present_skip = 0;
static int      s_fmv_skip_present_skip = 0;
/* Netplay + depth24: present every other vblank (admit still every tick). */
static int      s_netplay_depth24_present_skip = 0;
static uint32_t s_fmv_skip_last_mdec = 0;
static int      s_fmv_skip_hold = 0;
/* Depth24 FMV cutover: blank the first depth24+MDEC presents after leaving
 * depth24 or a *long* MDEC idle so one-frame RGB888 junk never reaches the
 * window. Short inter-frame gaps must not re-arm this (BPE ~15fps flicker). */
static int      s_d24_prev_mdec = 0;
static int      s_d24_saw_gap = 0;
static int      s_d24_cutover_blank = 0;
/* Savestate restore → audio pump: re-anchor last_cycles (declared early so
 * psx_frontend_on_savestate_loaded can set it). */
static int      g_audio_cycle_resync = 0;

static void smooth_60_reset(void) {
    g_smooth_60_state.previous_source.clear();
    g_smooth_60_state.source_hash = 0;
    g_smooth_60_state.width = 0;
    g_smooth_60_state.height = 0;
    g_smooth_60_state.have_source = false;
    g_smooth_60_state.previous_was_duplicate = false;
}

/* Declared early: present_session_reset zeros it on rematch. */
static int sdl_audio_fadein_left = 0;

static void present_session_reset(void) {
    s_disabled_frame_presented = false;
    s_force_present_after_load = false;
    s_sw_hold_valid = 0;
    s_fps_last_time = 0;
    s_fps_last_frame = 0;
    s_fps_base_title.clear();
    s_frame_pacer = FramePacer{ 0 };
    s_present_crtc_period_cycles = 0;
    s_present_crtc_is_pal = 0;
    s_present_crtc_banner_logged = false;
    s_turbo_present_skip = 0;
    s_fmv_skip_present_skip = 0;
    s_netplay_depth24_present_skip = 0;
    s_fmv_skip_last_mdec = 0;
    s_fmv_skip_hold = 0;
    s_d24_prev_mdec = 0;
    s_d24_saw_gap = 0;
    s_d24_cutover_blank = 0;
    sdl_audio_fadein_left = 0;
    /* Soft-exit can leave deferred present / flush reentrancy armed; gpu_init
     * does not clear them. Rematch then no-ops every flush → black window. */
    gpu_vblank_clear_deferred_present();
    smooth_60_reset();
}

static int fps_telemetry_enabled(void) {
    if (s_fps_telemetry_enabled < 0) {
        const char *e = std::getenv("PSX_FPS_TELEMETRY");
        s_fps_telemetry_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_fps_telemetry_enabled;
}

static void fps_telemetry_toggle(void) {
    const int enabled = fps_telemetry_enabled() ? 0 : 1;
    s_fps_telemetry_enabled = enabled;
    s_fps_last_time = 0;
    s_fps_last_frame = 0;
    if (!enabled && sdl_window && !s_fps_base_title.empty())
        SDL_SetWindowTitle(sdl_window, s_fps_base_title.c_str());
    if (!enabled)
        host_osd_set_status(NULL);
    s_fps_base_title.clear();
    host_osd_push(enabled ? "FPS readout on" : "FPS readout off", 1500);
}

static int manual_fast_forward_multiplier(void) {
    static int value = -2; /* -2 = unread, -1 = max/unbounded */
    if (value == -2) {
        const char *e = std::getenv("PSX_FAST_FORWARD_SPEED");
        value = 4;
        if (e && e[0]) {
            if (std::strcmp(e, "max") == 0 || std::strcmp(e, "MAX") == 0 ||
                std::strcmp(e, "0") == 0) {
                value = -1;
            } else {
                int parsed = std::atoi(e);
                if (parsed >= 2 && parsed <= 16)
                    value = parsed;
            }
        }
    }
    return value;
}

static void post_load_probe_stall_pc_note(uint32_t pc) {
    if (!pc) return;
    for (int i = 0; i < s_stall_pc_used; i++) {
        if (s_stall_pc[i] == pc) {
            if (s_stall_pc_n[i] < 0xffffffffu) s_stall_pc_n[i]++;
            return;
        }
    }
    if (s_stall_pc_used >= POST_LOAD_STALL_PC_CAP) return;
    s_stall_pc[s_stall_pc_used] = pc;
    s_stall_pc_n[s_stall_pc_used] = 1;
    s_stall_pc_used++;
}

static void post_load_probe_stall_pc_dump(const char *why) {
    if (s_stall_pc_used <= 0) return;
    std::fprintf(stderr, "post_load_probe stall_pcs (%s):", why);
    for (int i = 0; i < s_stall_pc_used; i++) {
        std::fprintf(stderr, " 0x%08X×%u",
                     (unsigned)s_stall_pc[i], (unsigned)s_stall_pc_n[i]);
    }
    std::fprintf(stderr, "\n");
}

static void post_load_probe_live_pc_note(uint32_t pc) {
    if (!pc) return;
    for (int i = 0; i < s_live_pc_used; i++) {
        if (s_live_pc[i] == pc) {
            if (s_live_pc_n[i] < 0xffffffffu) s_live_pc_n[i]++;
            return;
        }
    }
    if (s_live_pc_used >= POST_LOAD_LIVE_PC_CAP) return;
    s_live_pc[s_live_pc_used] = pc;
    s_live_pc_n[s_live_pc_used] = 1;
    s_live_pc_used++;
}

static void post_load_probe_live_pc_dump(const char *why) {
    if (s_live_pc_used <= 0) return;
    std::fprintf(stderr, "post_load_probe live_pcs (%s):", why);
    for (int i = 0; i < s_live_pc_used; i++) {
        std::fprintf(stderr, " 0x%08X×%u",
                     (unsigned)s_live_pc[i], (unsigned)s_live_pc_n[i]);
    }
    std::fprintf(stderr, "\n");
}

static void post_load_probe_arm(void) {
    if (!post_load_probe_env_on()) {
        s_post_load_probe_left = 0;
        g_plp_cycle_diag = 0;
        return;
    }
    extern uint64_t g_vblank_raise_count, g_vblank_deliver_count, g_vblank_ack_count;
    extern uint64_t g_dirty_ram_blocks_run;
    extern uint64_t g_dirty_ram_insns_run;
    extern uint64_t g_dirty_pump_count;
    extern uint64_t g_guest_store_count;
    uint64_t e = 0, fsr = 0, fn = 0, mid = 0, ev = 0, id = 0;
    psx_interrupt_check_path_diag(&e, &fsr, &fn, &mid, &ev, &id);
    uint64_t ci_u = 0, ci_s = 0, ci_n = 0, ci_sr = 0, ci_d = 0, ci_e = 0;
    overlay_loader_get_ci_skip_diag(&ci_u, &ci_s, &ci_n, &ci_sr, &ci_d, &ci_e);
    uint64_t hz_h = 0, hz_c = 0;
    psx_vsync_query_hle_horizon_totals(&hz_h, &hz_c);
    g_plp_cycle_diag = 1;
    g_plp_adv_calls = 0;
    g_plp_adv_max_chunk = 0;
    g_plp_adv_sum = 0;
    g_plp_svc_calls = 0;
    s_post_load_probe_left = 300;
    s_post_load_probe_i = 0;
    s_post_load_probe_gp00 = gpu_get_gp0_count();
    s_post_load_probe_skip_tot = 0;
    s_post_load_probe_swap_tot = 0;
    s_post_load_probe_dirty_tot = 0;
    s_post_load_probe_gp0_tot = 0;
    s_post_load_probe_vb_raise0 = g_vblank_raise_count;
    s_post_load_probe_vb_deliv0 = g_vblank_deliver_count;
    s_post_load_probe_vb_ack0 = g_vblank_ack_count;
    s_post_load_probe_dirty_blks0 = g_dirty_ram_blocks_run;
    s_post_load_probe_chk_entry0 = e;
    s_post_load_probe_chk_fsr0 = fsr;
    s_post_load_probe_chk_fnone0 = fn;
    s_post_load_probe_chk_mid0 = mid;
    s_post_load_probe_chk_eval0 = ev;
    s_post_load_probe_chk_deliv0 = id;
    s_post_load_probe_cyc0 = psx_get_cycle_count();
    s_post_load_probe_ci_unit0 = ci_u;
    s_post_load_probe_ci_supp0 = ci_s;
    s_post_load_probe_ci_none0 = ci_n;
    s_post_load_probe_ci_sr0 = ci_sr;
    s_post_load_probe_ci_deliv0 = ci_d;
    s_post_load_probe_ci_enter0 = ci_e;
    s_post_load_probe_adv_calls0 = 0;
    s_post_load_probe_adv_sum0 = 0;
    s_post_load_probe_svc0 = 0;
    s_post_load_probe_dirty_insns0 = g_dirty_ram_insns_run;
    s_post_load_probe_dirty_pump0 = g_dirty_pump_count;
    s_post_load_probe_stores0 = g_guest_store_count;
    s_post_load_probe_idle_n0 = g_idle_skip_count;
    s_post_load_probe_idle_cyc0 = g_idle_skip_cycles;
    s_post_load_probe_hz_hits0 = hz_h;
    s_post_load_probe_hz_cyc0 = hz_c;
    s_post_load_probe_host_t0 = SDL_GetPerformanceCounter();
    s_post_load_probe_stall_run = 0;
    s_post_load_probe_in_stall = 0;
    s_stall_pc_used = 0;
    s_live_pc_used = 0;
    gl_renderer_present_probe_reset();
    std::fprintf(stderr,
                 "savestate: post_load_probe armed (300 vblanks; "
                 "PSX_POST_LOAD_PROBE=1; live_pc/ci/adv diag on)\n");
}

static void post_load_probe_on_vblank(int turbo_active, int present_reached) {
    if (s_post_load_probe_left <= 0) return;
    s_post_load_probe_i++;
    s_post_load_probe_left--;

    uint64_t skip = 0, swap = 0, dirty_marks = 0;
    int force_left = 0;
    gl_renderer_present_probe_take(&skip, &swap, &dirty_marks, &force_left);
    s_post_load_probe_skip_tot += skip;
    s_post_load_probe_swap_tot += swap;
    s_post_load_probe_dirty_tot += dirty_marks;

    const uint64_t gp0_now = gpu_get_gp0_count();
    const uint64_t gp0_delta = gp0_now - s_post_load_probe_gp00;
    s_post_load_probe_gp00 = gp0_now;
    s_post_load_probe_gp0_tot += gp0_delta;

    extern uint64_t g_vblank_raise_count, g_vblank_deliver_count, g_vblank_ack_count;
    extern uint64_t g_dirty_ram_blocks_run;
    extern uint32_t i_stat, i_mask;
    extern CPUState *debug_cpu_ptr;
    const uint64_t vb_r = g_vblank_raise_count - s_post_load_probe_vb_raise0;
    const uint64_t vb_d = g_vblank_deliver_count - s_post_load_probe_vb_deliv0;
    const uint64_t vb_a = g_vblank_ack_count - s_post_load_probe_vb_ack0;
    s_post_load_probe_vb_raise0 = g_vblank_raise_count;
    s_post_load_probe_vb_deliv0 = g_vblank_deliver_count;
    s_post_load_probe_vb_ack0 = g_vblank_ack_count;
    const uint64_t dirty_blks = g_dirty_ram_blocks_run - s_post_load_probe_dirty_blks0;
    s_post_load_probe_dirty_blks0 = g_dirty_ram_blocks_run;

    uint32_t tcb = 0, gp_a = 0, gp_b = 0, gp_reg = 0;
    uint32_t sr = 0, cause = 0;
    int iec = 0, im2 = 0;
    if (debug_cpu_ptr) {
        gp_reg = debug_cpu_ptr->gpr[28];
        tcb = psx_sched_current_tcb(debug_cpu_ptr);
        sr = debug_cpu_ptr->cop0[12];    /* COP0 Status */
        cause = debug_cpu_ptr->cop0[13]; /* COP0 Cause */
        iec = (sr & 0x1u) ? 1 : 0;
        im2 = (sr & (1u << 10)) ? 1 : 0;
        /* func_8004FD14 frame-counter compare at 0x800501E8. */
        if (debug_cpu_ptr->read_word) {
            gp_a = debug_cpu_ptr->read_word(gp_reg + 2552u);
            gp_b = debug_cpu_ptr->read_word(gp_reg + 2632u);
        }
    }
    /* Hot-path check_interrupts attribution (not delivery_needed — that only
     * samples at present time and was misleading). */
    uint64_t chk_e = 0, chk_fsr = 0, chk_fn = 0, chk_mid = 0, chk_ev = 0, chk_id = 0;
    psx_interrupt_check_path_diag(&chk_e, &chk_fsr, &chk_fn, &chk_mid, &chk_ev, &chk_id);
    const uint64_t d_entry = chk_e - s_post_load_probe_chk_entry0;
    const uint64_t d_fsr = chk_fsr - s_post_load_probe_chk_fsr0;
    const uint64_t d_fnone = chk_fn - s_post_load_probe_chk_fnone0;
    const uint64_t d_mid = chk_mid - s_post_load_probe_chk_mid0;
    const uint64_t d_eval = chk_ev - s_post_load_probe_chk_eval0;
    const uint64_t d_irqd = chk_id - s_post_load_probe_chk_deliv0;
    s_post_load_probe_chk_entry0 = chk_e;
    s_post_load_probe_chk_fsr0 = chk_fsr;
    s_post_load_probe_chk_fnone0 = chk_fn;
    s_post_load_probe_chk_mid0 = chk_mid;
    s_post_load_probe_chk_eval0 = chk_ev;
    s_post_load_probe_chk_deliv0 = chk_id;
    const uint64_t cyc_now = psx_get_cycle_count();
    const uint64_t d_cyc = cyc_now - s_post_load_probe_cyc0;
    s_post_load_probe_cyc0 = cyc_now;

    uint64_t ci_u = 0, ci_s = 0, ci_n = 0, ci_sr = 0, ci_d = 0, ci_e = 0;
    overlay_loader_get_ci_skip_diag(&ci_u, &ci_s, &ci_n, &ci_sr, &ci_d, &ci_e);
    const uint64_t d_ci_unit = ci_u - s_post_load_probe_ci_unit0;
    const uint64_t d_ci_supp = ci_s - s_post_load_probe_ci_supp0;
    const uint64_t d_ci_none = ci_n - s_post_load_probe_ci_none0;
    const uint64_t d_ci_sr = ci_sr - s_post_load_probe_ci_sr0;
    const uint64_t d_ci_deliv = ci_d - s_post_load_probe_ci_deliv0;
    const uint64_t d_ci_enter = ci_e - s_post_load_probe_ci_enter0;
    s_post_load_probe_ci_unit0 = ci_u;
    s_post_load_probe_ci_supp0 = ci_s;
    s_post_load_probe_ci_none0 = ci_n;
    s_post_load_probe_ci_sr0 = ci_sr;
    s_post_load_probe_ci_deliv0 = ci_d;
    s_post_load_probe_ci_enter0 = ci_e;

    const uint64_t adv_calls = g_plp_adv_calls;
    const uint64_t adv_sum = g_plp_adv_sum;
    const uint32_t adv_max = g_plp_adv_max_chunk;
    const uint64_t svc_calls = g_plp_svc_calls;
    const uint64_t d_adv_calls = adv_calls - s_post_load_probe_adv_calls0;
    const uint64_t d_adv_sum = adv_sum - s_post_load_probe_adv_sum0;
    const uint64_t d_svc = svc_calls - s_post_load_probe_svc0;
    s_post_load_probe_adv_calls0 = adv_calls;
    s_post_load_probe_adv_sum0 = adv_sum;
    s_post_load_probe_svc0 = svc_calls;
    g_plp_adv_max_chunk = 0; /* per-frame max */

    extern uint64_t g_dirty_ram_insns_run;
    extern uint64_t g_dirty_pump_count;
    extern uint64_t g_guest_store_count;
    const uint64_t d_dirty_insns = g_dirty_ram_insns_run - s_post_load_probe_dirty_insns0;
    const uint64_t d_dirty_pump = g_dirty_pump_count - s_post_load_probe_dirty_pump0;
    const uint64_t d_stores = g_guest_store_count - s_post_load_probe_stores0;
    s_post_load_probe_dirty_insns0 = g_dirty_ram_insns_run;
    s_post_load_probe_dirty_pump0 = g_dirty_pump_count;
    s_post_load_probe_stores0 = g_guest_store_count;

    const uint64_t d_idle_n = g_idle_skip_count - s_post_load_probe_idle_n0;
    const uint64_t d_idle_cyc = g_idle_skip_cycles - s_post_load_probe_idle_cyc0;
    s_post_load_probe_idle_n0 = g_idle_skip_count;
    s_post_load_probe_idle_cyc0 = g_idle_skip_cycles;

    uint64_t hz_h = 0, hz_c = 0;
    psx_vsync_query_hle_horizon_totals(&hz_h, &hz_c);
    const uint64_t d_hz_hits = hz_h - s_post_load_probe_hz_hits0;
    const uint64_t d_hz_cyc = hz_c - s_post_load_probe_hz_cyc0;
    s_post_load_probe_hz_hits0 = hz_h;
    s_post_load_probe_hz_cyc0 = hz_c;

    const Uint64 host_now = SDL_GetPerformanceCounter();
    const Uint64 host_freq = SDL_GetPerformanceFrequency();
    const double host_ms = (host_freq > 0)
        ? (1000.0 * (double)(host_now - s_post_load_probe_host_t0) /
           (double)host_freq)
        : 0.0;
    s_post_load_probe_host_t0 = host_now;

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    const int rect_dirty = (di.width > 0 && di.height > 0)
        ? gl_renderer_present_rect_dirty((int)di.display_x, (int)di.display_y,
                                         (int)di.width, (int)di.height)
        : 0;

    CDROMDebugState cd;
    cdrom_debug_snapshot(&cd);
    const int cd_wait = cdrom_savestate_cd_wait_active();
    const int boost_left = cdrom_savestate_boost_vblanks_remaining();
    const int xa = cdrom_xa_stream_active();

    const uint32_t irq_pc = psx_last_irq_check_pc();
    const uint32_t resume_pc = psx_compiled_irq_resume_pc();
    extern uint32_t g_debug_current_func_addr;
    extern uint32_t g_debug_last_store_pc;
    extern int g_psx_dispatch_depth;
    const uint32_t func = g_debug_current_func_addr;
    const uint32_t store_pc = g_debug_last_store_pc;
    const uint32_t live_pc = debug_cpu_ptr ? debug_cpu_ptr->pc : 0u;
    const uint32_t live_ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0u;
    const int unit_depth = overlay_loader_call_unit_depth();
    const int disp_depth = g_psx_dispatch_depth;
    int cooldown_left = 0;
    int in_exc = 0;
    psx_get_freeze_diag(NULL, NULL, &in_exc, &cooldown_left, NULL, NULL);

    const int stalled = (gp0_delta == 0);
    if (stalled) {
        s_post_load_probe_stall_run++;
        s_post_load_probe_in_stall = 1;
        post_load_probe_stall_pc_note(irq_pc ? irq_pc : resume_pc);
        post_load_probe_stall_pc_note(func);
        post_load_probe_live_pc_note(live_pc);
    } else if (s_post_load_probe_in_stall) {
        std::fprintf(stderr,
                     "post_load_probe STALL_END at #%d after %d vblanks "
                     "(irq_pc=0x%08X resume=0x%08X func=0x%08X "
                     "live=0x%08X ra=0x%08X unit=%d disp=%d)\n",
                     s_post_load_probe_i, s_post_load_probe_stall_run,
                     (unsigned)irq_pc, (unsigned)resume_pc, (unsigned)func,
                     (unsigned)live_pc, (unsigned)live_ra,
                     unit_depth, disp_depth);
        post_load_probe_stall_pc_dump("end");
        post_load_probe_live_pc_dump("end");
        s_post_load_probe_in_stall = 0;
        s_post_load_probe_stall_run = 0;
        s_stall_pc_used = 0;
        s_live_pc_used = 0;
    }

    /* Dense samples during soft-stall; otherwise first 32 + every 15. */
    const int log_line =
        stalled ||
        (s_post_load_probe_i <= 32) ||
        (s_post_load_probe_i % 15 == 0) ||
        (s_post_load_probe_left == 0);
    if (log_line) {
        std::fprintf(stderr,
            "post_load_probe #%d: live=0x%08X ra=0x%08X "
            "irq_pc=0x%08X resume=0x%08X func=0x%08X "
            "store=0x%08X idle=0x%08X unit=%d disp=%d turbo=%d reached=%d "
            "swap=%llu skip=%llu dirty_marks=%llu force=%d rect_dirty=%d "
            "fb=%ux%u@(%u,%u) dis=%d d24=%d gp0=%llu "
            "cd(pend=%d cmd=0x%02X dly=%d read=%d rdly=%d xa=%d wait=%d boost=%d) "
            "exc=%d cool=%d istat=0x%X imask=0x%X "
            "vb(r=%llu d=%llu a=%llu) tcb=0x%08X "
            "gp9f8=%d gpA48=%d dirty_blks=%llu dins=%llu dpump=%llu stores=%llu "
            "sr=0x%08X iec=%d im2=%d cause=0x%08X cyc=%llu host_ms=%.2f "
            "chk(e=%llu fsr=%llu fn=%llu mid=%llu eval=%llu irq=%llu) "
            "ci(unit=%llu supp=%llu none=%llu sr=%llu deliv=%llu enter=%llu) "
            "adv(n=%llu sum=%llu max=%u svc=%llu) "
            "idle_skip(n=%llu cyc=%llu) hz(n=%llu cyc=%llu)\n",
            s_post_load_probe_i,
            (unsigned)live_pc, (unsigned)live_ra,
            (unsigned)irq_pc, (unsigned)resume_pc, (unsigned)func,
            (unsigned)store_pc, (unsigned)g_idle_skip_last_pc,
            unit_depth, disp_depth,
            turbo_active, present_reached,
            (unsigned long long)swap, (unsigned long long)skip,
            (unsigned long long)dirty_marks, force_left, rect_dirty,
            (unsigned)di.width, (unsigned)di.height,
            (unsigned)di.display_x, (unsigned)di.display_y,
            di.disabled ? 1 : 0, di.depth24 ? 1 : 0,
            (unsigned long long)gp0_delta,
            cd.pending_pending, (unsigned)cd.pending_cmd, cd.pending_delay,
            cd.reading, cd.read_delay, xa, cd_wait, boost_left,
            in_exc, cooldown_left, (unsigned)i_stat, (unsigned)i_mask,
            (unsigned long long)vb_r, (unsigned long long)vb_d,
            (unsigned long long)vb_a, (unsigned)tcb,
            (int)gp_a, (int)gp_b, (unsigned long long)dirty_blks,
            (unsigned long long)d_dirty_insns, (unsigned long long)d_dirty_pump,
            (unsigned long long)d_stores,
            (unsigned)sr, iec, im2, (unsigned)cause,
            (unsigned long long)d_cyc, host_ms,
            (unsigned long long)d_entry, (unsigned long long)d_fsr,
            (unsigned long long)d_fnone, (unsigned long long)d_mid,
            (unsigned long long)d_eval, (unsigned long long)d_irqd,
            (unsigned long long)d_ci_unit, (unsigned long long)d_ci_supp,
            (unsigned long long)d_ci_none, (unsigned long long)d_ci_sr,
            (unsigned long long)d_ci_deliv, (unsigned long long)d_ci_enter,
            (unsigned long long)d_adv_calls, (unsigned long long)d_adv_sum,
            (unsigned)adv_max, (unsigned long long)d_svc,
            (unsigned long long)d_idle_n, (unsigned long long)d_idle_cyc,
            (unsigned long long)d_hz_hits, (unsigned long long)d_hz_cyc);
    }
    if (s_post_load_probe_left == 0) {
        if (s_post_load_probe_in_stall) {
            post_load_probe_stall_pc_dump("done-still-stalled");
            post_load_probe_live_pc_dump("done-still-stalled");
        }
        g_plp_cycle_diag = 0;
        std::fprintf(stderr,
            "post_load_probe DONE: n=%d swap_tot=%llu skip_tot=%llu "
            "dirty_tot=%llu gp0_tot=%llu\n",
            s_post_load_probe_i,
            (unsigned long long)s_post_load_probe_swap_tot,
            (unsigned long long)s_post_load_probe_skip_tot,
            (unsigned long long)s_post_load_probe_dirty_tot,
            (unsigned long long)s_post_load_probe_gp0_tot);
    }
}

/* Called from savestate_poll after a successful restore (before scheduler
 * longjmp). Clears present latches and forces the next vblank to show the
 * restored VRAM — including a blank if display was disabled in the snapshot. */
static void savestate_input_guard_arm(void);
extern "C" void psx_frontend_on_savestate_notify(int is_load, int slot, int ok) {
    char buf[64];
    const int disp = slot + 1;
    if (!is_load && ok)
        psx_savestate_menu_note_slots_changed();
    if (is_load && ok)
        savestate_input_guard_arm();
    if (is_load) {
        if (ok)
            snprintf(buf, sizeof(buf), "Loaded slot %d", disp);
        else
            snprintf(buf, sizeof(buf), "Load failed slot %d", disp);
    } else {
        if (ok)
            snprintf(buf, sizeof(buf), "Saved slot %d", disp);
        else
            snprintf(buf, sizeof(buf), "Save failed slot %d", disp);
    }
    host_osd_push(buf, 2000);
}

extern "C" void psx_frontend_on_savestate_loaded(void) {
    mod_runtime_on_savestate_loaded();
    s_disabled_frame_presented = false;
    s_force_present_after_load = true;
    smooth_60_reset();
    /* Re-anchor wall pacing + FPS baseline: admit may have blocked for seconds
     * in the load barrier with next_deadline in the past. */
    s_frame_pacer = FramePacer{ 0 };
    s_fps_last_time = 0;
    s_fps_last_frame = 0;
    /* Re-anchor guest-cycle→sample budgeting (pump clears queued PCM too). */
    g_audio_cycle_resync = 1;
    /* Depth24 FMV: drop ephemeral present hold/cutover so restored VRAM shows.
     * Upload span came back with the GPU snap; treat MDEC as already active if
     * depth24 is on so we don't full-black the first post-load presents. */
    gpu_depth24_on_savestate_loaded();
    s_d24_cutover_blank = 0;
    s_d24_saw_gap = 0;
    s_d24_prev_mdec = gpu_display_is_depth24() ? 1 : 0;
    /* Depth24 load skipped framebuffer-sized CPU→GPU uploads — including the
     * full-VRAM boot_state blit — so restage the mirror into the FBO/image
     * before present. Otherwise post-FMV menus miss texture pages. Safe
     * no-ops when that backend was never brought up. */
    gl_renderer_restage_vram_after_savestate();
    vk_renderer_restage_vram_after_savestate();
    /* GL present-dirty early-out can skip SwapWindow when the restored frame
     * matches the last swap (typical on 2nd+ load of the same slot). Invalidate
     * tiles + force several swaps so the window actually updates. Safe no-op
     * when the GL pipeline was never brought up. */
    gl_renderer_invalidate_present();
    post_load_probe_arm();
}

/* Rollback snap apply / realign — keep depth24 hold clear + restage. Full
 * savestate present thrash (post_load_probe) stays disk-only. When the tip
 * is already in FMV/media, mirror the disk cutover reset so resume into
 * FMV entry does not treat the tip as a fresh gap (permanent black blank). */
extern "C" void psx_frontend_on_rb_snap_loaded(void) {
    const int media = gpu_display_is_depth24() || mdec_recently_active(8) ||
                      cdrom_fmv_stream_pending() || cdrom_xa_stream_active();
    g_audio_cycle_resync = 1;
    gpu_depth24_on_savestate_loaded();
    s_d24_cutover_blank = 0;
    if (media) {
        s_d24_saw_gap = 0;
        s_d24_prev_mdec = (gpu_display_is_depth24() || mdec_recently_active(8)) ? 1 : 0;
        s_disabled_frame_presented = false;
        s_force_present_after_load = true;
    }
    gl_renderer_restage_vram_after_savestate();
    vk_renderer_restage_vram_after_savestate();
    /* Dual-raster: restaged FBO must reach the window; drop hold-last. */
    gl_renderer_invalidate_present();
    /* §63: guest SIO came from the snap — drop Live pad-edge trackers. */
    psx_netplay_on_rb_snap_loaded();
}

static uint64_t smooth_60_frame_hash(const uint32_t* pixels, size_t count) {
    uint64_t hash = 1469598103934665603ull;
    const size_t step = std::max<size_t>(1, count / 4096u);
    for (size_t i = 0; i < count; i += step) {
        hash ^= pixels[i];
        hash *= 1099511628211ull;
    }
    hash ^= count;
    return hash * 1099511628211ull;
}

static bool smooth_60_scene_cut(const uint32_t* current,
                                const uint32_t* previous,
                                size_t count) {
    const size_t step = std::max<size_t>(1, count / 4096u);
    uint64_t difference = 0;
    size_t samples = 0;
    size_t large_changes = 0;
    for (size_t i = 0; i < count; i += step) {
        const uint32_t a = current[i];
        const uint32_t b = previous[i];
        const unsigned dr = (unsigned)std::abs((int)((a >> 16) & 0xFFu) -
                                               (int)((b >> 16) & 0xFFu));
        const unsigned dg = (unsigned)std::abs((int)((a >> 8) & 0xFFu) -
                                               (int)((b >> 8) & 0xFFu));
        const unsigned db = (unsigned)std::abs((int)(a & 0xFFu) -
                                               (int)(b & 0xFFu));
        const unsigned pixel_difference = dr + dg + db;
        difference += pixel_difference;
        if (pixel_difference > 320u) large_changes++;
        samples++;
    }
    return samples && large_changes * 4u > samples * 3u &&
           difference > (uint64_t)samples * 360u;
}

static void smooth_60_present(uint32_t* pixels, uint32_t width, uint32_t height,
                              bool eligible) {
    if (!eligible || !pixels || !g_smooth_60fps.load(std::memory_order_acquire)) {
        if (g_smooth_60_state.have_source) smooth_60_reset();
        return;
    }
    Smooth60State& state = g_smooth_60_state;
    const size_t count = (size_t)width * height;
    if (!count) { smooth_60_reset(); return; }
    const uint64_t hash = smooth_60_frame_hash(pixels, count);
    if (!state.have_source || state.width != width || state.height != height) {
        state.previous_source.assign(pixels, pixels + count);
        state.source_hash = hash;
        state.width = width;
        state.height = height;
        state.have_source = true;
        state.previous_was_duplicate = false;
        return;
    }
    const bool duplicate = hash == state.source_hash &&
        std::memcmp(pixels, state.previous_source.data(), count * sizeof(uint32_t)) == 0;
    if (duplicate) { state.previous_was_duplicate = true; return; }
    const bool interpolate = state.previous_was_duplicate &&
        !smooth_60_scene_cut(pixels, state.previous_source.data(), count);
    state.previous_was_duplicate = false;
    state.source_hash = hash;
    if (!interpolate) {
        std::copy(pixels, pixels + count, state.previous_source.begin());
        return;
    }
    if (!state.announced) {
        std::fprintf(stdout,
            "psxrecomp: 60 FPS smoothing active (guest timing preserved)\n");
        state.announced = true;
    }
    for (size_t i = 0; i < count; i++) {
        const uint32_t current = pixels[i];
        const uint32_t previous = state.previous_source[i];
        state.previous_source[i] = current;
        pixels[i] = 0xFF000000u |
            (((current & 0x00FEFEFEu) >> 1) + ((previous & 0x00FEFEFEu) >> 1));
    }
}

extern "C" void psx_smooth_60fps_set(int enabled) {
    g_smooth_60fps.store(enabled ? 1 : 0, std::memory_order_release);
}
#if defined(PSX_WEB)
extern "C" EMSCRIPTEN_KEEPALIVE void psx_web_set_smooth_60fps(int enabled) {
    psx_smooth_60fps_set(enabled);
}
#endif

/* [video] options, resolved from the game config (defaults: native + AA). */
static int           g_video_scale = 1;     /* internal-resolution SSAA factor */
static bool          g_video_aa    = true;  /* linear present filtering */
/* FMV present reconstruction (VIDEO_FMV_FILTER_*), pushed to the GL renderer
 * once the config is resolved. Only consulted while g_video_aa is on. */
static int           g_video_fmv_filter = PSXRecompV4::VIDEO_FMV_FILTER_DEFAULT;

/* recomp-ui stores this 1-based so a zero-initialized (older) host reads as
 * "unset" rather than pinning nearest; the config enum is 0-based. Convert at
 * the boundary, and treat anything out of range as the default. */
static inline int launcher_fmv_filter_to_cfg(int ls_value) {
    if (ls_value < 1 || ls_value > RECOMP_LAUNCHER_FMV_FILTER_COUNT)
        return PSXRecompV4::VIDEO_FMV_FILTER_DEFAULT;
    return ls_value - 1;
}
static inline int cfg_fmv_filter_to_launcher(int cfg_value) {
    if (cfg_value < 0 || cfg_value >= PSXRecompV4::VIDEO_FMV_FILTER_COUNT)
        cfg_value = PSXRecompV4::VIDEO_FMV_FILTER_DEFAULT;
    return cfg_value + 1;
}
static int           g_video_texfilter = 0; /* 0=nearest, 1=bilinear */
/* Sub-pixel vertex precision + perspective-correct UVs (PGXP-style). Visual
 * only: the PS1-visible GTE SXY FIFO stays integer, so guest-side culling and
 * SXY readback are untouched. Default off = the faithful floor. */
static int           g_video_geometry_correction   = 0;
static int           g_video_perspective_texturing = 0;
/* [video] pgxp_depth — depth-test using PGXP's recovered W instead of relying
 * solely on the ordering table. See gl_renderer_set_pgxp_depth. */
static int           g_video_pgxp_depth = 0;
extern "C" void gl_renderer_set_depth_always(int on);
static int           g_video_pgxp_cpu_mode         = 0;
static float         g_video_pgxp_tolerance        = 0.5f;
static int           g_video_renderer = PSXRecompV4::DEFAULT_VIDEO_RENDERER;
/* HD texture pack (tex_pack.cpp). Both default off: replacement needs a pack on
 * disk, and dumping is an authoring/verification tool that writes a PNG per
 * newly-seen texture. */
static int           g_hd_textures    = 0;
static int           g_hd_texture_dump = 0;
static std::string   g_hd_texture_dir;
static std::vector<std::string> g_hd_texture_exclude; /* [video] hd_texture_exclude */   /* relocates the whole convention (dev knob) */
static std::string   g_bezel_path;      /* [video] bezel -- margin artwork */
static std::string   g_hd_texture_pack;  /* the active pack folder itself (manager) */
static int           g_fullscreen     = 0;  /* tri-state: 0 windowed, 1 borderless (desktop)
                                              * fullscreen, 2 exclusive fullscreen */
static int           g_video_screen   = 0;  /* 0=raw,1=crt,2=composite,3=trinitron */
static int           g_video_win_w    = 0;    /* 0 = fit the display; see clamp_window_aspect */
static bool          g_video_win_w_explicit = false; /* user chose a width */
static bool          g_audio_spu_hq   = false; /* SPU float-shadow (env overrides) */
static int           g_audio_freq     = 44100; /* host device request */
static int           g_auto_skip_fmv  = 0;   /* skip FMVs the instant they're detected */
static int           g_rewind_depth  = 50;  /* local rewind snap count (50/100/150/200) */
static int           g_rewind_interval = 15; /* frames between snaps (1/4/8/12/15/30) */
static int           g_hotkey_pad_rewind = 1272;       /* select + r3 */
static int           g_hotkey_pad_save_state_menu = 2040;/* select + r1 */
static uint32_t      g_savestate_input_guard_min_until = 0;
static uint32_t      g_savestate_input_guard_max_until = 0;
static int           g_headless       = 0;   /* debug/CI frontend: no SDL window/audio */

/* FMV instant-skip via the game's OWN end-of-movie path. Tomba's MDEC player
 * (FUN_8001efe8) tears a movie down when the streamed frame number reaches that
 * movie's per-movie total minus 3; writing the current movie's total down to
 * g_fmv_skip_end_total makes the player end it on the next frame — a natural end
 * that reaches EVERY movie. g_fmv_skip_total_table = base of the per-movie u16
 * total table; g_fmv_skip_movie_id = guest addr of the current-movie-id byte.
 * 0 table => fall back to START injection. See [video] fmv_skip_* in game.toml. */
static uint32_t      g_fmv_skip_total_table = 0;
static uint32_t      g_fmv_skip_movie_id    = 0;
static int           g_fmv_skip_end_total   = 3;
/* fmv_skip_no_xa: detect FMVs on MDEC activity alone (silent RAM-preloaded
 * movies never stream XA, so the default MDEC+XA detector misses them —
 * Tomba2's Whoopee Camp logo). Presentation-side fast-forward only. */
static int           g_fmv_skip_no_xa       = 0;
static int           g_fmv_skip_no_xa_hold  = 4;
/* Low-latency present options (see [video] in config_loader). Measured on a
 * 60Hz box the dominant input->photon cost is NOT vsync at the swap (that
 * blocks ~tens of us) but that input is sampled ~one pacer-wait (~13.6ms)
 * before the frame the CPU then renders from it. g_low_latency_input re-samples
 * the pad AFTER the wall-clock pacer (just before present) so the next CPU frame
 * reads near-fresh input. g_video_vsync controls the GL swap interval
 * (1=vsync/tear-free, 0=immediate/lowest display latency+tearing, -1=adaptive);
 * it trims the display-side scanout latency the CPU-side ring can't see.
 *
 * Driver vsync and the wall-clock pacer are XOR. Waiting on both (pacer then
 * FIFO SwapBuffers) double-blocks on Linux compositors: ~16.7 ms + ~16.7 ms
 * = 30 Hz / 0.50x with the CPU idle. ~60 Hz panels may use vsync as the clock;
 * otherwise the pacer holds 59.94 Hz and present must not wait on the swap.
 * Wayland: never let driver vsync own cadence by default (same effective
 * policy as PSX_VSYNC=0); opt in with PSX_WAYLAND_ALLOW_VSYNC=1. */
static int           g_low_latency_input = 1;
static int           g_video_vsync        = 1;
static int           g_frame_interpolation = 0;
static int           g_frame_interpolation_fps = 0;
static int           g_frame_interpolation_blend =
    PSX_MOD_FRAME_INTERPOLATION_LINEAR;
static int           g_frame_interpolation_blend_default =
    PSX_MOD_FRAME_INTERPOLATION_LINEAR;
static int           g_mod_controller_mode_override[2] = { -1, -1 };
static_assert((int)PSX_MOD_CONTROLLER_HYBRID ==
              (int)PSXRecompV4::PAD_MODE_HYBRID);
static_assert((int)PSX_MOD_CONTROLLER_ANALOG ==
              (int)PSXRecompV4::PAD_MODE_ANALOG);
static_assert((int)PSX_MOD_CONTROLLER_DIGITAL ==
              (int)PSXRecompV4::PAD_MODE_DIGITAL);
static double        g_host_refresh_hz = 0.0;
static constexpr double PSX_FRAME_HZ_NTSC = 59.94;
static constexpr double PSX_FRAME_HZ_PAL  = 50.0;
static constexpr double PSX_FRAME_PERIOD_MS = 1000.0 / PSX_FRAME_HZ_NTSC;
static double        g_frame_period_ms = PSX_FRAME_PERIOD_MS;
static bool          g_mod_native_vblank_rate = false;
static bool          g_mod_native_vblank_active = false;
static uint32_t      g_mod_pending_crtc_multiplier = 1u;
static uint32_t      g_mod_native_vblank_fps = 0;

static double psx_crtc_frame_hz(void) {
    const uint32_t period = gpu_vblank_period_cycles();
    if (period == 0u)
        return PSX_FRAME_HZ_NTSC;
    /* 33.8688 MHz CPU clock / cycles-per-vblank. */
    return 33868800.0 / (double)period;
}

static void refresh_frame_pacer_period(void) {
    if (g_mod_native_vblank_rate && g_mod_native_vblank_active)
        return;
    const double psx_hz = psx_crtc_frame_hz();
    g_frame_period_ms = 1000.0 / psx_hz;
    if (g_host_refresh_hz > 0.0) {
        const double lo = psx_hz * 0.98;
        const double hi = psx_hz * 1.02;
        if (g_host_refresh_hz >= lo && g_host_refresh_hz <= hi)
            g_frame_period_ms = 1000.0 / g_host_refresh_hz;
    }
}
/* Activation-time request. -1 means no enabled mod owns load acceleration. */
static int           g_mod_load_wall_multiplier = -1;
static int           g_mod_load_release_frames = -1;
static int           g_mod_disc_speed_divisor = -1;
static int           g_mod_disc_instant_rate = -1;

static int present_vsync_owns_cadence(void);
static int present_effective_swap_interval(void);
static int present_should_wall_pace(void);
static void apply_present_cadence(void);

/* Map the configured tri-state fullscreen mode (g_fullscreen) to the SDL
 * window-fullscreen flag: used both to open the window in that mode and to
 * pick the hotkey's fullscreen target. */
static Uint32 psx_fullscreen_flag_for_mode(int mode) {
    if (mode == 2) return SDL_WINDOW_FULLSCREEN;         /* exclusive */
    if (mode == 1) return SDL_WINDOW_FULLSCREEN_DESKTOP; /* borderless */
    return 0;                                            /* windowed */
}

/* FMV auto-skip detection hooks (cdrom.c / mdec.c). */
extern "C" int      cdrom_xa_stream_active(void);
extern "C" uint32_t mdec_get_decode_count(void);

/* Debug observability: expose the RESOLVED FMV-skip + config state so the TCP
 * debug server can report the runtime truth (which game.toml path actually loaded,
 * and what auto_skip_fmv / the frame-count-table params resolved to) instead of us
 * inferring it from the .toml on disk. Set once in main() after the config path is
 * resolved. */
const char *g_active_config_path = nullptr;
extern "C" void debug_get_fmv_config(int *auto_skip, uint32_t *total_table,
                                      uint32_t *movie_id, int *no_xa_hold,
                                      const char **cfg_path) {
    if (auto_skip)   *auto_skip   = g_auto_skip_fmv;
    if (total_table) *total_table = g_fmv_skip_total_table;
    if (movie_id)    *movie_id    = g_fmv_skip_movie_id;
    if (no_xa_hold)  *no_xa_hold  = g_fmv_skip_no_xa_hold;
    if (cfg_path)    *cfg_path    = g_active_config_path ? g_active_config_path : "(null)";
}

/* Display aspect W:H (default 4:3 = native). Wider aspects enable the
 * widescreen hack: GTE X-squash + stretched present (see [video] aspect_ratio
 * in config_loader.h). */
static int           g_video_aspect_num = 4;
static int           g_video_aspect_den = 3;
/* Resize-driven widescreen. The user's fixed aspect is still used to shape the
 * initial window; after the game window exists these values follow its live
 * aspect, clamped to 4:3..the widest mode offered by the title. */
static bool          g_ws_adaptive_view = false;
static int           g_ws_adaptive_max_num = 16;
static int           g_ws_adaptive_max_den = 9;
/* game.toml [netplay] local_viewport = "vertical_split": during real netplay,
 * present only this peer's native split-screen half and stretch it to the
 * window. Presentation-only; the guest still renders the original framebuffer. */
static int g_netplay_local_viewport = 0; /* 0 off, 1 vertical split */
/* Optional aspect for netplay local-view extraction. Mirrors trusted mod aspect
 * activation, but remains game.toml opt-in so normal netplay stays vanilla. */
static int g_netplay_local_viewport_aspect = 0; /* 0 off, 1 16:9, 2 21:9, 3 adaptive */

extern "C" int psx_mod_set_fixed_display_aspect(
    uint32_t numerator, uint32_t denominator) {
    if (numerator == 0 || denominator == 0 ||
        numerator > 99 || denominator > 99 ||
        3u * numerator < 4u * denominator ||
        9u * numerator > 32u * denominator) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected invalid display aspect %u:%u\n",
            (unsigned)numerator, (unsigned)denominator);
        return 0;
    }
    g_video_aspect_num = (int)numerator;
    g_video_aspect_den = (int)denominator;
    gl_renderer_set_display_aspect(g_video_aspect_num, g_video_aspect_den);
    vk_renderer_set_display_aspect(g_video_aspect_num, g_video_aspect_den);
    g_ws_adaptive_view = false;
    std::fprintf(stdout, "psxrecomp: mod selected fixed display aspect %u:%u\n",
                 (unsigned)numerator, (unsigned)denominator);
    return 1;
}

extern "C" int psx_mod_set_adaptive_display_aspect(
    uint32_t max_numerator, uint32_t max_denominator) {
    if (max_numerator == 0 || max_denominator == 0 ||
        max_numerator > 99 || max_denominator > 99 ||
        3u * max_numerator < 4u * max_denominator ||
        9u * max_numerator > 32u * max_denominator) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected invalid adaptive display aspect %u:%u\n",
            (unsigned)max_numerator, (unsigned)max_denominator);
        return 0;
    }
    g_ws_adaptive_view = true;
    g_ws_adaptive_max_num = (int)max_numerator;
    g_ws_adaptive_max_den = (int)max_denominator;
    std::fprintf(stdout,
        "psxrecomp: mod selected adaptive display aspect "
        "(initial %d:%d, range 4:3 through %u:%u)\n",
        g_video_aspect_num, g_video_aspect_den,
        (unsigned)max_numerator, (unsigned)max_denominator);
    return 1;
}

extern "C" int psx_mod_set_native_vblank_rate(
    uint32_t frames_per_second) {
    if (frames_per_second != 0 &&
        (frames_per_second < 50 || frames_per_second > 1000)) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected invalid native VBlank rate %u FPS\n",
            (unsigned)frames_per_second);
        return 0;
    }
    g_mod_native_vblank_rate = true;
    g_mod_native_vblank_active = false;
    g_mod_native_vblank_fps = frames_per_second;
    /* Do not alter the live period during BIOS/boot.  The native rate is
     * armed below and becomes authoritative only when the PAL title CRTC
     * mode is observed by refresh_live_crtc_cadence(). */
    if (!g_mod_native_vblank_active)
        refresh_frame_pacer_period();
    /*
     * Above the physical panel rate (and in uncapped mode), swap-interval
     * blocking would silently replace the requested guest cadence with the
     * display cadence. The frame pacer owns fixed-rate timing here — UNLESS
     * the panel actually runs at the requested rate: then driver vsync is
     * the better cadence owner (it absorbs sub-frame jitter that the wall
     * pacer converts into debt/catch-up oscillation — for example, a
     * 99.7-122.5 fps seesaw at a 120 Hz guest and display cadence). The
     * present-cadence policy (present_vsync_owns_cadence) already contains
     * the matching band check and keeps the pacer/vsync XOR intact; only
     * force vsync off when the panel genuinely cannot match. */
    if (frames_per_second == 0)
        g_video_vsync = 0;
    /* fps > 60 no longer forces vsync off: the present-cadence policy is
     * already an XOR — present_effective_swap_interval() returns the
     * configured vsync ONLY when present_vsync_owns_cadence() (which
     * requires the panel to match the CRTC rate within 2%), and 0
     * otherwise, with the wall pacer engaged exactly when vsync does not
     * own. On a panel that matches a non-stock guest rate (for example,
     * a 120 Hz guest on a 120.0 Hz display), driver vsync owns cadence and
     * absorbs the sub-frame jitter the wall pacer converts into debt/catch-up
     * oscillation (measured: 99.7-122.5 fps seesaw). On a non-matching
     * panel the swap interval resolves to 0 and the pacer owns, same as
     * the old forced-off behavior. */
    if (frames_per_second) {
        std::fprintf(stdout,
            "psxrecomp: mod selected native guest VBlank pacing at %u FPS "
            "(%.4f ms/frame)\n",
            (unsigned)frames_per_second, g_frame_period_ms);
    } else {
        std::fprintf(stdout,
            "psxrecomp: mod selected uncapped native guest VBlank pacing\n");
    }
    return 1;
}

extern "C" int psx_mod_set_crtc_refresh_multiplier(
    uint32_t multiplier) {
    if (multiplier < 1u || multiplier > 8u) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected invalid CRTC refresh multiplier %u\n",
            (unsigned)multiplier);
        return 0;
    }
    if (g_mod_native_vblank_rate && !g_mod_native_vblank_active) {
        g_mod_pending_crtc_multiplier = multiplier;
    } else {
        gpu_set_crtc_refresh_multiplier(multiplier);
    }
    return 1;
}

extern "C" int psx_mod_set_crtc_vblank_hz(uint32_t hz) {
    if (hz == 0u) {
        gpu_set_crtc_vblank_period_override(0u);
        std::fprintf(stdout,
            "psxrecomp: mod cleared CRTC VBlank period override "
            "(follow GP1 video mode)\n");
        return 1;
    }
    if (hz != 50u && hz != 60u) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected invalid CRTC VBlank rate %u Hz "
            "(use 50, 60, or 0)\n",
            (unsigned)hz);
        return 0;
    }
    const uint32_t cycles =
        (hz == 50u) ? PSX_VBLANK_CYCLES_PAL : PSX_VBLANK_CYCLES_NTSC;
    gpu_set_crtc_vblank_period_override(cycles);
    std::fprintf(stdout,
        "psxrecomp: mod forced CRTC VBlank to %u Hz (%u guest cycles)\n",
        (unsigned)hz, (unsigned)cycles);
    return 1;
}

extern "C" int psx_mod_set_frame_interpolation(
    uint32_t frames_per_second) {
    if (frames_per_second != 0 &&
        (frames_per_second < 60 || frames_per_second > 1000)) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected invalid interpolated frame rate %u FPS\n",
            (unsigned)frames_per_second);
        return 0;
    }

    /*
     * Interpolation is a presentation feature of the OpenGL renderer. Mods are
     * activated before the game window and renderer are created, so selecting
     * it here is deterministic and does not require a live backend switch.
     */
    g_video_renderer = 1;
    g_video_vsync = 0;
    g_frame_interpolation = 1;
    g_frame_interpolation_fps =
        frames_per_second ? (int)frames_per_second : 0;

    if (frames_per_second) {
        std::fprintf(stdout,
            "psxrecomp: mod selected presentation-only interpolation at "
            "%u FPS; guest timing remains stock\n",
            (unsigned)frames_per_second);
    } else {
        std::fprintf(stdout,
            "psxrecomp: mod selected display-refresh presentation-only "
            "interpolation; guest timing remains stock\n");
    }
    return 1;
}

extern "C" int psx_mod_set_frame_interpolation_blend(
    uint32_t blend_mode) {
    if (blend_mode != PSX_MOD_FRAME_INTERPOLATION_LINEAR &&
        blend_mode != PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected invalid frame-interpolation blend %u\n",
            (unsigned)blend_mode);
        return 0;
    }
    g_frame_interpolation_blend = (int)blend_mode;
    std::fprintf(stdout, "psxrecomp: frame-interpolation blend = %s\n",
        blend_mode == PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE
            ? "motion-adaptive clarity" : "linear crossfade");
    return 1;
}

extern "C" int psx_mod_set_auto_skip_fmv(int enabled) {
    if (enabled != 0 && enabled != 1) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected invalid auto-skip-FMV value %d\n",
            enabled);
        return 0;
    }
    g_auto_skip_fmv = enabled;
    std::fprintf(stdout, "psxrecomp: mod %s automatic FMV skipping\n",
                 enabled ? "enabled" : "disabled");
    return 1;
}

extern "C" int psx_mod_set_load_acceleration(
    uint32_t wall_clock_multiplier, uint32_t release_frames) {
    /* Host pacing only changes how fast wall-clock time is fed to a load; every
     * guest frame, CD deadline, interrupt and callback still happens, so a high
     * multiplier cannot desync the guest -- it just approaches "as fast as the
     * host can". Ceiling raised from 16 for the same reason as disc speed:
     * players set this as an integer and want to find their own limit. */
    if (wall_clock_multiplier > PSX_MOD_LOAD_ACCEL_MAX || release_frames > 60) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected load acceleration "
            "(multiplier %u, release frames %u)\n",
            (unsigned)wall_clock_multiplier, (unsigned)release_frames);
        return 0;
    }
    g_mod_load_wall_multiplier = (int)wall_clock_multiplier;
    g_mod_load_release_frames = (int)release_frames;
    return 1;
}

extern "C" int psx_mod_set_disc_speed(
    uint32_t divisor, uint32_t instant_max_per_frame) {
    /* Any positive divisor is arithmetically safe: cdrom.c divides the sector
     * delay by it and floors the result at CDROM_MIN_DELAY, and XA streaming
     * keeps authentic timing regardless. The old 2-or-4 allowlist was policy,
     * not a hardware constraint, and it blocked players from finding the
     * highest speed their game tolerates (GH TombaRecomp#5). Games expose the
     * value as a player-set integer, so accept the full sane range and let the
     * mod's own description carry the risk warning. */
    if (divisor > PSX_MOD_DISC_SPEED_MAX ||
        (divisor == 0 &&
         (instant_max_per_frame < 1 || instant_max_per_frame > 256))) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected disc speed "
            "(divisor %u, instant budget %u)\n",
            (unsigned)divisor, (unsigned)instant_max_per_frame);
        return 0;
    }
    g_mod_disc_speed_divisor = (int)divisor;
    g_mod_disc_instant_rate =
        divisor == 0 ? (int)instant_max_per_frame : -1;
    return 1;
}

extern "C" int psx_mod_set_controller_mode_override(
    uint32_t player, uint32_t controller_mode) {
    if (player >= 2 ||
        controller_mode > (uint32_t)PSXRecompV4::PAD_MODE_DIGITAL) {
        std::fprintf(stderr,
            "psxrecomp: mod rejected controller-mode override "
            "(player %u, mode %u)\n",
            (unsigned)player, (unsigned)controller_mode);
        return 0;
    }
    g_mod_controller_mode_override[player] = (int)controller_mode;
    return 1;
}

/* [widescreen] per-game hooks (see config_loader.h): anchor scratch addr for
 * tagged sprite prims + HUD SPRT center-squash. Inert at 0/false. */
static uint32_t      g_ws_anchor_addr = 0;
static bool          g_ws_hud_sprt = false;
/* Runtime-only transition cleanup; kept out of gpu.h because generated game
 * units include that ABI header and do not need this frontend-only setter. */
extern "C" void gpu_ws_set_clear_reveal(int on);
extern "C" void gpu_ws_set_precise_nclip(int on);
extern "C" void gpu_ws_set_netplay_local_viewport(int enabled, int slot);
extern "C" int gpu_ws_netplay_local_viewport_base_x(void);
extern "C" int gpu_ws_netplay_local_viewport_width(void);
extern "C" void gpu_ws_set_nw_textured_edges(int on, int scale_pct);
extern "C" void gpu_ws_set_signed_x_bound_sites(const uint32_t*, const uint32_t*, int);
/* Widescreen content correction engages at game entry (fntrace_is_game_started):
 * the BIOS boot remains authentic 4:3 content with no GTE squash. A fixed outer
 * canvas, when configured, is already wide around that content. */
static bool          g_ws_engaged = true;
/* Fill the pillarbox margins of a 4:3-pinned present with the frame's own edge
 * columns instead of black (gl_renderer_set_pillarbox_edge_fill). Game-facing
 * look for 2D screens on a very wide display; live-toggled by the
 * ws_menu_edge_fill debug command for A/B. */
extern "C" { int g_ws_menu_edge_fill_flag = 1; }   /* ws_menu_edge_fill command */
/* Wide-aspect strategy: native-wide (render the wider FOV into a wider frame,
 * present 1:1 — the GTE is NOT squashed) vs. the legacy squash hack. Default
 * native-wide; toggle live via the ws_nw TCP command for A/B comparison. */
static int           g_ws_native_wide = 1;
/* [widescreen] fixed_outer_aspect: the configured aspect owns one stable outer
 * canvas while classification selects only the content transform inside it. */
static bool          g_ws_fixed_outer_aspect = false;
/* Logical present width for the SDL_Renderer (software) path; 640*scale at
 * 4:3, wider for wide aspects. Height is always 480*scale. Set at window
 * creation alongside SDL_RenderSetLogicalSize. */
static int           g_logical_w = 640;

/* Clamp a requested window width to the primary display's usable area so an
 * oversized choice (e.g. 1920 on a 1080p panel) still fits on screen. Keeps
 * the given aspect: height = width*den/num. */
static void clamp_window_aspect(int* w, int* h, int num, int den) {
    int width = *w;
    SDL_Rect bounds;
    const int have_bounds =
        (SDL_GetDisplayUsableBounds(0, &bounds) == 0 && bounds.w > 0 && bounds.h > 0);
    /* 0 = "fit the display". The old default was a hardcoded 1280, which on a
     * 4K or 8K panel opens a small window in the corner and, worse, makes the
     * image far smaller than the internal render resolution the user chose --
     * supersampling 16 rendering into a 1280-wide window throws almost all of
     * it away. Fitting the usable bounds keeps the window proportional to the
     * display it is actually on. An explicit width still wins. */
    if (width <= 0) width = have_bounds ? bounds.w : 1280;
    if (width < 640) width = 640;
    if (have_bounds) {
        if (width > bounds.w)             width = bounds.w;
        if (width * den / num > bounds.h) width = bounds.h * num / den;
    }
    *w = width;
    *h = width * den / num;
}

static int aspect_gcd(int a, int b) {
    while (b) { int t = a % b; a = b; b = t; }
    return a > 0 ? a : 1;
}

static int64_t aspect_gcd64(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a > 0 ? a : 1;
}

static void netplay_local_viewport_projection_aspect(
    int present_num, int present_den, int* proj_num, int* proj_den) {
    if (!proj_num || !proj_den) return;
    *proj_num = present_num;
    *proj_den = present_den;

    if (g_netplay_local_viewport != 1 ||
        !psx_netplay_active() ||
        !gpu_last_frame_vertical_split_screen() ||
        present_num * 3 <= present_den * 4) {
        return;
    }

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.depth24 || di.width < 2 || di.height == 0)
        return;

    /* The normal widescreen squash assumes a 4:3 source. A split-screen peer
     * source is only half the display width, so derive the equivalent aspect
     * that produces source_aspect / target_aspect as the X squash factor:
     *
     *   source = (display_w / 2) / display_h
     *   effective = (4:3) * target / source
     */
    int64_t n = (int64_t)present_num * 8 * (int64_t)di.height;
    int64_t d = (int64_t)present_den * 3 * (int64_t)di.width;
    if (n <= 0 || d <= 0) return;
    int64_t gcd = aspect_gcd64(n, d);
    n /= gcd;
    d /= gcd;
    if (n <= 0 || d <= 0 || n > INT32_MAX || d > INT32_MAX)
        return;

    *proj_num = (int)n;
    *proj_den = (int)d;
}

static int g_ws_projection_num = 4;
static int g_ws_projection_den = 3;
static int g_ws_projection_mode = -1;
extern "C" uint64_t psx_gpu_display_flip_count(void);  /* gpu.c: internal fps */

static void refresh_widescreen_projection() {
    if (!g_ws_engaged) return;

    const bool wide = g_video_aspect_num * 3 != g_video_aspect_den * 4;
    const bool local_native_wide =
        g_netplay_local_viewport == 1 && psx_netplay_active() &&
        gpu_last_frame_vertical_split_screen();
    const bool native_wide = (g_netplay_local_viewport == 1)
        ? local_native_wide
        : (g_ws_native_wide != 0);
    const int mode = wide ? (native_wide ? 2 : 1) : 0;
    int proj_num = g_video_aspect_num;
    int proj_den = g_video_aspect_den;
    if (mode == 1) {
        netplay_local_viewport_projection_aspect(
            g_video_aspect_num, g_video_aspect_den, &proj_num, &proj_den);
    }

    if (mode == g_ws_projection_mode &&
        proj_num == g_ws_projection_num &&
        proj_den == g_ws_projection_den) {
        return;
    }

    g_ws_projection_mode = mode;
    g_ws_projection_num = proj_num;
    g_ws_projection_den = proj_den;
    gte_set_display_aspect(mode == 1 ? proj_num : 4,
                           mode == 1 ? proj_den : 3);
    gpu_ws_configure(proj_num, proj_den, g_ws_anchor_addr,
                     g_ws_hud_sprt ? 1 : 0, mode);
}

/* Follow the host window without feeding its absolute pixel size into guest
 * rendering. Only the ratio matters: gpu_ws_configure derives the PSX-native
 * sidecar width from it, just as the fixed 16:9/21:9 modes do. */
static void update_adaptive_widescreen() {
    if (!g_ws_adaptive_view || !sdl_window) return;

    int width = 0, height = 0;
    SDL_GetWindowSize(sdl_window, &width, &height);
    if (width <= 0 || height <= 0) return;

    int num = width, den = height;
    if ((int64_t)width * 3 <= (int64_t)height * 4) {
        num = 4; den = 3;
    } else if ((int64_t)width * g_ws_adaptive_max_den >=
               (int64_t)height * g_ws_adaptive_max_num) {
        num = g_ws_adaptive_max_num;
        den = g_ws_adaptive_max_den;
    } else {
        int divisor = aspect_gcd(num, den);
        num /= divisor;
        den /= divisor;
    }
    if (num == g_video_aspect_num && den == g_video_aspect_den) return;

    g_video_aspect_num = num;
    g_video_aspect_den = den;
    gl_renderer_set_display_aspect(num, den);
    vk_renderer_set_display_aspect(num, den);
    if (sdl_renderer) {
        g_logical_w = 480 * num * g_video_scale / den;
        SDL_RenderSetLogicalSize(sdl_renderer, g_logical_w, 480 * g_video_scale);
    }

    refresh_widescreen_projection();
}

/* SDL GL attributes are global inputs to the next context creation.  Set the
 * runtime requirements immediately before every GL window, because launcher
 * teardown resets them and macOS otherwise supplies a legacy 2.1 context. */
static void configure_core_gl_context_attributes() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
}

/* Live native-wide vs squash toggle (ws_nw TCP command) for A/B comparison.
 * Re-engages with the chosen mode in place if widescreen is already running. */
extern "C" void psx_ws_set_native_wide(int on) {
    g_ws_native_wide = on ? 1 : 0;
    g_ws_projection_mode = -1;
    refresh_widescreen_projection();
}
extern "C" int psx_ws_get_native_wide(void) { return g_ws_native_wide; }

static bool          g_gl_active = false;    /* GL context live -> GL present path */
static bool          g_vk_active = false;    /* Vulkan context live -> VK present path */

/* present_shot — capture the COMPOSED present surface, i.e. the frame after the
 * backend has fitted the display buffer to the window, so the PNG carries the
 * aspect the player is actually looking at.
 *
 * screenshot / screenshot_file / screenshot_hires all resolve the display
 * buffer BEFORE that fit: on a 508x256 display in a 4:3 window they answer
 * 508x256 while the window shows 640x480. Correct for faithfulness work, wrong
 * for anything aspect-shaped — a widescreen change moves the GTE squash and the
 * present fit, which is exactly the stage those captures skip.
 *
 * Staged here and fulfilled by whichever backend owns the present: the SDL
 * software path reads back via SDL_RenderReadPixels, the GL path via
 * glReadPixels before SwapWindow (gpu_gl_renderer.c).
 *
 * THREADING: the request is written from a debug-server command handler on the
 * emu thread (debug_server_poll safe point), but GL fulfilment can run on the
 * frame-interpolation thread (gpu_gl_renderer.c interp_present -> gl_swap_with_osd),
 * so the path buffer and the pending flag are mutex-guarded and the counters are
 * atomic. present_shot_take() CLAIMS the request under the lock, so two present
 * paths can never fulfil the same shot.
 *
 * A staged shot is OVERWRITTEN by a later request rather than refused (the same
 * choice savestate's request_save_inner makes). A present that never arrives —
 * a static frame, a backend that stops presenting — therefore cannot wedge the
 * command: the next request simply replaces it. */
static std::mutex       s_present_shot_mtx;
static char             s_present_shot_path[512];
static bool             s_present_shot_pending = false;
static std::atomic<int> s_present_shot_seq{0};   /* bumps on every completion */
static std::atomic<int> s_present_shot_ok{0};    /* 1 = last completion wrote a PNG */

extern "C" int present_shot_request(const char *path)
{
    if (!path || !*path) return 0;
    if (g_headless)  return 0;   /* no present surface to read back */
    /* Vulkan owns its own swapchain present (see the g_vk_active branch in
     * sdl_vblank_present_body) and has no readback hook, so nothing would ever
     * fulfil the request. Refuse honestly instead of accepting a shot that can
     * never complete — CLAUDE.md rule 15. */
    if (g_vk_active) return 0;
    {
        std::lock_guard<std::mutex> lk(s_present_shot_mtx);
        std::snprintf(s_present_shot_path, sizeof(s_present_shot_path), "%s", path);
        s_present_shot_pending = true;
    }
    return 1;
}

/* Backend hook: atomically claim a staged shot (returns 1 and fills `out`),
 * then call present_shot_done(ok) once the PNG is written — or not written. */
extern "C" int present_shot_take(char *out, int n)
{
    if (!out || n <= 0) return 0;
    std::lock_guard<std::mutex> lk(s_present_shot_mtx);
    if (!s_present_shot_pending) return 0;
    std::snprintf(out, (size_t)n, "%s", s_present_shot_path);
    s_present_shot_pending = false;   /* claimed */
    return 1;
}

/* ok = 1 only when a PNG actually landed on disk. seq advances either way so a
 * client polling it always terminates; ok tells it whether the file exists. */
extern "C" void present_shot_done(int ok)
{
    s_present_shot_ok.store(ok ? 1 : 0, std::memory_order_release);
    s_present_shot_seq.fetch_add(1, std::memory_order_release);
}

extern "C" int present_shot_seq(void) { return s_present_shot_seq.load(std::memory_order_acquire); }
extern "C" int present_shot_ok(void)  { return s_present_shot_ok.load(std::memory_order_acquire); }
/* Present straight from the FBO (fast, no readback). Set PSX_GL_FORCE_CPU_PRESENT=1
 * to force the software readout path instead — a diagnostic/fallback that also
 * keeps CPU VRAM current every frame (so screenshots reflect the screen). */
static int           g_gl_fbo_present = 1;

/* Cleared on lobby soft-return so rematch can re-arm CPU-auth GPU lock. */
static int s_netplay_sw_gpu_locked;
/* Netplay + user OpenGL: dual-raster (SW@1× authority + GL@Nx present) or
 * fallback SW-only + CPU→GL present when dual could not arm. */
static int s_netplay_gl_present;
/* Netplay SW-only path: sim at scale 1 (no SW SSAA hi-res mirror).
 * Dual-raster OpenGL uses g_video_scale for the hr FBO; SW stays 1 via
 * glb_set_scale. Cleared on lobby soft-return. */
static int s_netplay_sim_native_scale;

/* True while netplay owns the GPU path: CPU VRAM is digest/snap authority. */
static int netplay_cpu_auth_gpu(void) {
    return s_netplay_sw_gpu_locked || s_netplay_gl_present ||
           s_netplay_sim_native_scale;
}

/* Dual-raster present quality: GL FBO at settings SSAA, SW@1× for snaps. */
static int netplay_gl_dual_quality(void) {
#ifndef PSX_SDL_NO_RENDER
    return s_netplay_gl_present && g_gl_active && g_gl_fbo_present &&
           gl_renderer_cpu_auth_dual();
#else
    return 0;
#endif
}

#ifndef PSX_SDL_NO_RENDER
/* Create SDL_Renderer + streaming texture for software present. Used when
 * netplay runs without a live GL context (software renderer selected, or GL
 * init failed). CPU-auth + OpenGL present does not need this. */
static int ensure_sw_sdl_present(void) {
    if (!sdl_window)
        return -1;
    if (!sdl_renderer) {
#ifdef _WIN32
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
        Uint32 rflags = SDL_RENDERER_ACCELERATED |
                        (present_effective_swap_interval() != 0
                             ? SDL_RENDERER_PRESENTVSYNC : 0u);
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, rflags);
        if (!sdl_renderer)
            sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
        if (!sdl_renderer) {
            std::fprintf(stderr,
                         "psxrecomp: netplay SW present: SDL_CreateRenderer failed: %s\n",
                         SDL_GetError());
            return -1;
        }
        /* Netplay CPU-auth: size logical/texture at 1× (sim has no hi-res
         * mirror). Offline SSAA preference stays in g_video_scale. */
        const int tex_scale = netplay_cpu_auth_gpu() ? 1 : g_video_scale;
        g_logical_w = 480 * g_video_aspect_num * tex_scale / g_video_aspect_den;
        if (g_logical_w < 1) g_logical_w = 640;
        SDL_RenderSetLogicalSize(sdl_renderer, g_logical_w, 480 * tex_scale);
    }
    if (!sdl_texture) {
        const int tex_scale = netplay_cpu_auth_gpu() ? 1 : g_video_scale;
        sdl_texture = SDL_CreateTexture(
            sdl_renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            640 * tex_scale, 512 * tex_scale);
        if (!sdl_texture) {
            std::fprintf(stderr,
                         "psxrecomp: netplay SW present: SDL_CreateTexture failed: %s\n",
                         SDL_GetError());
            return -1;
        }
        SDL_SetTextureScaleMode(sdl_texture,
                                g_video_aa ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    }
    if (!sdl_pixel_buf) {
        const int tex_scale = netplay_cpu_auth_gpu() ? 1 : g_video_scale;
        sdl_pixel_buf = (uint32_t*)std::malloc(
            (size_t)640 * tex_scale * 512 * tex_scale * sizeof(uint32_t));
        if (!sdl_pixel_buf) {
            std::fprintf(stderr, "psxrecomp: netplay SW present: staging alloc failed\n");
            return -1;
        }
    }
    return 0;
}
#endif

/* Rollback/delay netplay: guest VRAM must be CPU-authoritative. Prefer
 * dual-raster OpenGL: SW@1× writes snaps/digests/GPUREAD; GL@settings-scale
 * hr FBO is present-only (g_gl_fbo_present=1, never glReadPixels). Fallback
 * when no GL context: pure software SDL present at scale 1. */
extern "C" void psx_frontend_netplay_force_sw_gpu(void) {
#ifndef PSX_SDL_NO_RENDER
    /* Already locked on dual-raster GL quality present. */
    if (s_netplay_sw_gpu_locked && netplay_gl_dual_quality()) {
        int want = g_video_scale < 1 ? 1 : g_video_scale;
        if (want > SW_MAX_INTERNAL_SCALE) want = SW_MAX_INTERNAL_SCALE;
        gr_set_backend(GR_BACKEND_OPENGL);
        gl_renderer_set_cpu_auth_dual(1);
        /* FBO scale is fixed at context init — keep request in sync. */
        gr_set_scale(want);
        s_netplay_sim_native_scale = 1;
        return;
    }
    /* Already locked on legacy CPU→GL present (no dual). */
    if (s_netplay_sw_gpu_locked && g_gl_active && !g_gl_fbo_present) {
        gr_set_scale(1);
        s_netplay_sim_native_scale = 1;
        return;
    }
    /* Already locked on pure software SDL present. */
    if (s_netplay_sw_gpu_locked && !g_gl_active && !g_vk_active &&
        sdl_renderer && sdl_texture && sdl_pixel_buf) {
        gr_set_scale(1);
        s_netplay_sim_native_scale = 1;
        return;
    }
#endif
#ifndef PSX_SDL_NO_RENDER
    /* If we were on FBO-auth GL (offline), pull VRAM to CPU once before
     * arming dual. Caller must invoke this before g_np.active (ensure_cpu
     * no-ops while netplay is active). */
    if (g_gl_active && gr_backend() == GR_BACKEND_OPENGL &&
        !gl_renderer_cpu_auth_dual())
        gl_renderer_sync_cpu();
    if (g_vk_active) {
        /* Vulkan CPU-auth present not wired yet — tear down to SW SDL. */
        vk_renderer_sync_cpu();
        vk_renderer_shutdown();
        g_vk_active = false;
        s_netplay_gl_present = 0;
    }
#endif
#ifndef PSX_SDL_NO_RENDER
    if (g_gl_active) {
        /* Dual-raster: OPENGL backend, SW@1× + GPU@Nx, FBO present. */
        int want = g_video_scale < 1 ? 1 : g_video_scale;
        if (want > SW_MAX_INTERNAL_SCALE) want = SW_MAX_INTERNAL_SCALE;
        gr_set_backend(GR_BACKEND_OPENGL);
        gl_renderer_set_cpu_auth_dual(1);
        gr_set_scale(want);
        g_gl_fbo_present = true;
        s_netplay_gl_present = 1;
        s_netplay_sim_native_scale = 1;
        gl_renderer_restage_vram_after_savestate();
        gl_renderer_invalidate_present();
        s_netplay_sw_gpu_locked = 1;
        latency_ring_set_backend("opengl");
        fprintf(stderr,
                "psxrecomp: netplay dual-raster "
                "(SW@1x CPU-auth + OpenGL@%dx present; no FBO readback)\n",
                want);
        fflush(stderr);
        return;
    }
    /* Software present (user picked software, GL init failed, or Vulkan).
     * Do not clear g_video_renderer — that preference must survive rematch /
     * first settings.toml write so OpenGL stays the user's default. */
    g_gl_fbo_present = false;
    gl_renderer_set_cpu_auth_dual(0);
    gr_set_backend(GR_BACKEND_SOFTWARE);
    gr_set_scale(1);
    s_netplay_sim_native_scale = 1;
    s_netplay_gl_present = 0;
    if (ensure_sw_sdl_present() != 0) {
        s_netplay_sw_gpu_locked = 0;
        std::fprintf(stderr,
                     "psxrecomp: netplay SW GPU forced but present path failed "
                     "(expect black/empty window — will retry)\n");
        fflush(stderr);
        return;
    }
    s_netplay_sw_gpu_locked = 1;
#else
    gr_set_backend(GR_BACKEND_SOFTWARE);
    gr_set_scale(1);
    s_netplay_sim_native_scale = 1;
    s_netplay_sw_gpu_locked = 1;
#endif
    latency_ring_set_backend("software");
    fprintf(stderr,
            "psxrecomp: netplay software GPU "
            "(CPU-authoritative VRAM; sim scale 1)\n");
    fflush(stderr);
}

/* Vsync self-heal state (see SDL_RenderPresent wrapper in
 * sdl_vblank_present). C linkage: freeze_heartbeat.c includes both in
 * the freeze dump so a slow-frames wedge can be attributed to driver
 * present backpressure from the dump alone. */
extern "C" {
uint32_t g_present_slow_count = 0;     /* presents that blocked >250ms */
int      g_present_vsync_disabled = 0; /* 1 once self-heal tripped */
}

/* Turbo-through-loads (step 4). C linkage: debug_server.c reads/toggles the
 * enable and reports the frame counter. Enabled by game.toml [runtime]
 * turbo_loads (opt-in per game) or the turbo_loads TCP command. */
extern "C" {
int      g_turbo_loads_enabled = 0;
uint64_t g_turbo_loads_frames  = 0;   /* vblanks run unpaced (observability) */
int      g_turbo_audio_sink_enabled = 0;
int      g_turbo_audio_sink_active = 0;
uint64_t g_turbo_audio_sink_frames = 0; /* guest SPU frames advanced, not queued */
}
/* The CD predicate already excludes XA and holds across ordinary inter-file
 * gaps. A short engage debounce rejects a one-frame controller blip without
 * leaving a visible authentic-paced prefix on every real load. Once engaged,
 * a symmetric release debounce rides through tiny late-sector/IRQ gaps. This
 * changes host presentation/pacing only; guest cycles and input sampling keep
 * advancing normally. */
#define TURBO_LOADS_ENGAGE_FRAMES  4
#define TURBO_LOADS_RELEASE_FRAMES 6
/* Zero multiplier retains the historical uncapped turbo behavior. */
static int g_turbo_load_wall_multiplier = 0;
static int g_turbo_load_release_frames = TURBO_LOADS_RELEASE_FRAMES;
static SDL_AudioDeviceID sdl_audio_device;
static int16_t       sdl_audio_buf[2048 * 2];

/* DRC bridge. Producer (sdl_audio_pump) runs on the main loop thread under
 * SDL_LockAudioDevice; consumer (sdl_drc_callback) runs on the SDL audio
 * thread. */
static rab_bridge s_drc;
static bool       s_drc_ready = false;

/* Observability + A/B. PSXRECOMP_AUDIO_LEGACY=1 keeps the historical push model
 * (SDL_QueueAudio, no bridge) so the underrun baseline can be measured against
 * the bridge from a single build; default (unset) = bridge/pull model. Output
 * health is surfaced through the audio_stats TCP command (no stderr probe). */
static bool audio_legacy_mode(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("PSXRECOMP_AUDIO_LEGACY"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v != 0;
}
/* Legacy-mode underrun counter: incremented when the SDL queue is found empty
 * at pump time (the device was silence-filling = an audible gap). */
static uint64_t g_legacy_underruns = 0;
/* Set at unmute so the pump resyncs its bridge-underrun baseline past the
 * intentional mute-drain instead of reporting it as gaps. */
int g_audio_unmute_resync = 0;
/* Actual device rate the host opened at (bridge mode may differ from 44100;
 * the T3 tap ring runs at this rate and its WAV dump must say so). */
extern "C" {
int g_audio_host_rate = 44100;
/* Guest-clocked audio observability. These counters are diagnostics only; the
 * SPU still advances from psx_cycle_count and never from host presents. */
uint64_t g_audio_guest_cycles_total = 0;
uint64_t g_audio_guest_frames_budget = 0;
uint64_t g_audio_guest_frames_rendered = 0;
uint64_t g_audio_guest_pumps = 0;
uint64_t g_audio_guest_cycle_carry = 0;
}

extern "C" void psx_runtime_timing_snapshot(
    uint64_t *guest_cycles, uint64_t *guest_vblanks,
    uint32_t *crtc_period_cycles, uint32_t *crtc_multiplier,
    double *guest_hz, double *host_period_ms,
    int *native_rate_override, uint32_t *native_rate_fps,
    uint64_t *audio_guest_cycles, uint64_t *audio_frames_budget,
    uint64_t *audio_frames_rendered, uint64_t *audio_pumps,
    uint64_t *audio_cycle_carry)
{
    const uint32_t period = gpu_vblank_period_cycles();
    if (guest_cycles) *guest_cycles = psx_get_cycle_count();
    if (guest_vblanks) *guest_vblanks = s_frame_count;
    if (crtc_period_cycles) *crtc_period_cycles = period;
    if (crtc_multiplier) *crtc_multiplier = gpu_get_crtc_refresh_multiplier();
    if (guest_hz) {
        *guest_hz = period
            ? (double)PSX_GUEST_CPU_CLOCK_HZ / (double)period
            : 0.0;
    }
    if (host_period_ms) *host_period_ms = g_frame_period_ms;
    if (native_rate_override) *native_rate_override =
        (g_mod_native_vblank_rate && g_mod_native_vblank_active) ? 1 : 0;
    if (native_rate_fps) *native_rate_fps = g_mod_native_vblank_fps;
    if (audio_guest_cycles) *audio_guest_cycles = g_audio_guest_cycles_total;
    if (audio_frames_budget) *audio_frames_budget = g_audio_guest_frames_budget;
    if (audio_frames_rendered) *audio_frames_rendered = g_audio_guest_frames_rendered;
    if (audio_pumps) *audio_pumps = g_audio_guest_pumps;
    if (audio_cycle_carry) *audio_cycle_carry = g_audio_guest_cycle_carry;
}

static void sdl_drc_callback(void* /*user*/, Uint8* stream, int len) {
    if (!s_drc_ready) { std::memset(stream, 0, (size_t)len); return; }
    int frames = len / (int)(2 * sizeof(int16_t)); /* stereo S16 */
    rab_pull(&s_drc, reinterpret_cast<int16_t*>(stream), frames);
    /* T3 tap in bridge mode: the exact device-rate bytes the host consumes.
     * This callback is the tap's single writer while the bridge is active. */
    audio_trace_pcm(AUDIO_TAP_HOST, reinterpret_cast<const int16_t*>(stream),
                    frames);
}

static std::filesystem::path find_upward(std::filesystem::path start,
                                         const std::filesystem::path& marker) {
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec) start = std::filesystem::current_path();
    if (!std::filesystem::is_directory(start, ec)) start = start.parent_path();

    for (;;) {
        if (std::filesystem::exists(start / marker, ec)) return start;
        if (!start.has_parent_path() || start.parent_path() == start) break;
        start = start.parent_path();
    }
    return {};
}

// The directory the running executable lives in — the SINGLE anchor for every
// runtime file (game.toml, disc, BIOS, settings.toml, caches, memcards, sidecar
// .cfg). Deliberately NEVER the current working directory: a game must launch
// identically whether started from its own folder, a shortcut, a shell in some
// other directory, or a debugger. On Windows GetModuleFileNameW is the
// authoritative source (independent of argv[0], which can be relative/bare and
// would otherwise get resolved against cwd by fs::absolute). $APPIMAGE and
// argv[0] are non-Windows / fallback sources only.
static std::filesystem::path exe_dir_from_argv(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe_dir;
#ifdef _WIN32
    // Authoritative: the real image path, regardless of how we were invoked.
    {
        wchar_t buf[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        if (n > 0 && n < (DWORD)(sizeof(buf) / sizeof(buf[0]))) {
            exe_dir = fs::path(std::wstring(buf, buf + n)).parent_path();
        }
    }
#endif
    // Inside an AppImage the binary lives in a read-only mount and argv[0] can
    // be a bare name or symlink (launched via PATH / a .desktop file). $APPIMAGE
    // is the .AppImage's own path, so settings.toml anchors next to it.
    if (exe_dir.empty()) {
        if (const char* appimg = std::getenv("APPIMAGE"); appimg && appimg[0]) {
            exe_dir = fs::absolute(appimg, ec).parent_path();
            if (ec) exe_dir.clear();
        }
    }
    if (exe_dir.empty() && argv0 && argv0[0]) {
        exe_dir = fs::absolute(argv0, ec).parent_path();
        if (ec) exe_dir.clear();
    }
    // Last-ditch only (should be unreachable on a normal launch); a bare "." so
    // we never silently resolve against an unrelated cwd deeper in the tree.
    if (exe_dir.empty()) exe_dir = fs::path(".");
    return exe_dir;
}

static std::filesystem::path resolve_existing_runtime_path(const char* requested,
                                                           const char* argv0) {
    namespace fs = std::filesystem;
    if (!requested || !requested[0]) return {};

    std::error_code ec;
    fs::path p(requested);
    if (fs::exists(p, ec)) return fs::absolute(p, ec);
    if (p.is_absolute()) return {};

    // Anchor exclusively on the exe directory — never cwd (see exe_dir_from_argv).
    const fs::path root = exe_dir_from_argv(argv0);
    fs::path direct = root / p;
    if (fs::exists(direct, ec)) return fs::absolute(direct, ec);
    fs::path found = find_upward(root, p);
    if (!found.empty()) return fs::absolute(found / p, ec);
    return {};
}

static std::filesystem::path sidecar_cfg_path(const char* argv0, const char* filename) {
    return exe_dir_from_argv(argv0) / filename;
}

static std::filesystem::path read_cached_path(const char* argv0, const char* filename) {
    std::ifstream f(sidecar_cfg_path(argv0, filename));
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line.empty() ? std::filesystem::path{} : std::filesystem::path(line);
}

static void write_cached_path(const char* argv0, const char* filename,
                              const std::filesystem::path& path) {
    std::ofstream f(sidecar_cfg_path(argv0, filename), std::ios::trunc);
    if (f.is_open()) f << path.string() << "\n";
}

static void launcher_warning(const char* title, const std::string& msg) {
    std::fprintf(stderr, "%s: %s\n", title, msg.c_str());
#ifdef _WIN32
    // Headless (--headless / PSX_HEADLESS): NEVER pop a blocking modal — it would
    // hang an unattended/CI/scripted run forever waiting for a click.
    if (!g_headless) MessageBoxA(NULL, msg.c_str(), title, MB_OK | MB_ICONWARNING);
#endif
}

static void launcher_info(const char* title, const std::string& msg) {
    std::fprintf(stderr, "%s: %s\n", title, msg.c_str());
#ifdef _WIN32
    if (!g_headless) MessageBoxA(NULL, msg.c_str(), title, MB_OK | MB_ICONINFORMATION);
#endif
}

/* Game display name for picker dialogs ("Tomba!"); set after the game
 * config loads, before any interactive file resolution. */
static std::string s_picker_game_name = "PSXRecomp";

static bool pick_runtime_file(const char* title, const char* filter,
                              std::filesystem::path& out, const char* cli_flag) {
    // Headless: never open an interactive file dialog (it blocks). Fail the
    // resolve so boot aborts cleanly with the stderr message the caller printed.
    if (g_headless) {
        std::fprintf(stderr,
            "psxrecomp: headless — cannot prompt for '%s'.\n"
            "  Supply it on the command line:  %s <path>   (or set it in game.toml).\n",
            title, cli_flag);
        return false;
    }
#ifdef _WIN32
    char path_buf[4096];
    std::memset(path_buf, 0, sizeof(path_buf));

    OPENFILENAMEA ofn;
    std::memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path_buf;
    ofn.nMaxFile = (DWORD)sizeof(path_buf);
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameA(&ofn)) return false;
    out = path_buf;
    return true;
#else
    (void)filter;
    (void)out;
    // No native GUI file picker on this platform (macOS/Linux): the file must be
    // named on the command line. Tell the user the exact flag + a full example
    // instead of a dead-end "requires a command-line path".
    std::fprintf(stderr,
        "psxrecomp: no graphical file picker on this platform — '%s'\n"
        "  must be supplied on the command line:  %s <path>\n"
        "  Example:  ./<game-exe> --game game.toml "
        "--bios /path/to/SCPH1001.BIN --disc /path/to/game.cue\n"
        "  (or set the path in game.toml). See the game's README, \"Building From Source\".\n",
        title, cli_flag);
    return false;
#endif
}

static std::string uppercase_ascii(std::string s) {
    for (char& c : s) {
        c = (char)std::toupper((unsigned char)c);
    }
    return s;
}

// Region display label for the launcher, derived from the game-id serial
// prefix (e.g. "SCUS-94423" -> "(USA)"). Used only when game.toml [game]
// leaves `region` unset. Unknown/empty serials yield "" (no badge) rather
// than guessing.
static std::string region_label_from_serial(const std::string& serial) {
    if (serial.size() < 3) return std::string();
    const std::string up  = uppercase_ascii(serial);
    const std::string p4  = up.substr(0, std::min<size_t>(4, up.size()));
    const std::string p3  = up.substr(0, 3);
    if (p4 == "SCUS" || p4 == "SLUS" || p3 == "LSP") return "(USA)";
    if (p4 == "SCES" || p4 == "SLES")                return "(Europe)";
    if (p4 == "SCPS" || p4 == "SLPS" || p4 == "SLPM") return "(Japan)";
    return std::string();
}

static bool read_at(std::ifstream& f, uint64_t offset, uint8_t* out, size_t len) {
    f.clear();
    f.seekg((std::streamoff)offset, std::ios::beg);
    if (!f.good()) return false;
    f.read(reinterpret_cast<char*>(out), (std::streamsize)len);
    return f.gcount() == (std::streamsize)len;
}

struct DiscValidation {
    bool opened = false;
    bool has_header = false;
    bool id_matches = false;
    std::string detail;
};

static DiscValidation validate_disc_image(const std::filesystem::path& selected_path,
                                          const std::string& game_id) {
    // Delegate to the shared disc-identity module so the launch-time check and
    // the launcher badge can never drift apart. No CRC here — the launch check
    // only cares about openability / header / serial; the launcher does the
    // (slower) CRC pass when an expected CRC is configured.
    const PSXRecompV4::DiscIdentity id =
        PSXRecompV4::identify_disc(selected_path, game_id, /*expected_crc*/0,
                                   /*has_expected_crc*/false, /*compute_crc*/false);
    DiscValidation v;
    v.opened     = id.opened;
    v.has_header = id.has_header;
    v.id_matches = game_id.empty() ? true : id.serial_matches;
    v.detail     = id.detail;
    if (id.opened && id.has_header && !v.id_matches && v.detail.empty()) {
        v.detail = "The disc header is readable, but it does not contain the expected game ID " +
                   uppercase_ascii(game_id) + " in the early disc metadata.";
    }
    return v;
}

static bool validate_disc_for_launch(const std::filesystem::path& path,
                                     const std::string& game_id) {
    const DiscValidation v = validate_disc_image(path, game_id);
    if (!v.opened) {
        launcher_warning("Disc Image Not Found", v.detail + "\n\nSelected path:\n" + path.string());
        return false;
    }
    if (!v.has_header || !v.id_matches) {
        launcher_warning("Disc Image Warning",
            v.detail + "\n\nThis may be the wrong game or a corrupt image. The runtime will try to run it anyway.");
    }
    return true;
}

static std::filesystem::path normalize_disc_path_for_launch(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    if (ec) p = path;

    if (uppercase_ascii(p.extension().string()) == ".CUE") {
        fs::path bin = p;
        bin.replace_extension(".bin");
        if (fs::exists(bin, ec)) {
            fs::path abs = fs::absolute(bin, ec);
            return ec ? bin : abs;
        }
        ec.clear();
        bin.replace_extension(".BIN");
        if (fs::exists(bin, ec)) {
            fs::path abs = fs::absolute(bin, ec);
            return ec ? bin : abs;
        }
    }

    return p;
}

/* BIOS selection state (docs/BIOS_SELECTION.md). s_openbios_allowed is the
 * game's [runtime] openbios; s_bundled_bios_rel is where the shipped
 * redistributable image lives relative to the executable. */
static bool        s_openbios_allowed  = true;
static std::string s_bundled_bios_rel  = PSX_BUNDLED_BIOS_PATH;

/* Identity-match a file against a linked backend. Size+CRC must agree —
 * bytes is a guaranteed wild jump. Identity therefore decides WHICH backend
 * may run, not merely whether to warn. Null = no linked image matches.
 */
static const PsxBiosBackend* bios_backend_for_file(const std::filesystem::path& path,
                                                   uint32_t* out_crc,
                                                   uint64_t* out_size) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return nullptr;
    const std::streamoff size = f.tellg();
    if (out_size) *out_size = (uint64_t)size;
    if (size <= 0) return nullptr;
    std::vector<uint8_t> data((size_t)size);
    if (!read_at(f, 0, data.data(), data.size())) return nullptr;
    const uint32_t crc = crc32_compute(data.data(), data.size());
    if (out_crc) *out_crc = crc;
    for (uint32_t i = 0; i < psx_bios_registry_count; i++) {
        const PsxBiosBackend* b = psx_bios_registry[i];
        if (!b || !b->image) continue;
        if ((uint64_t)size == (uint64_t)b->image->image_size &&
            crc == b->image->image_crc32)
            return b;
    }
    return nullptr;
}

/* What a player may supply, for mismatch and picker copy. The bundled image is
 * excluded: it is never something to go and find. */
static std::string bios_accepted_images() {
    std::string s;
    for (uint32_t i = 0; i < psx_bios_registry_count; i++) {
        const PsxBiosBackend* b = psx_bios_registry[i];
        if (!b || !b->image || b->image->image_bundled) continue;
        if (!s.empty()) s += ", ";
        s += b->image->image_id;
        s += " (" + std::to_string(b->image->image_size / 1024u) + " KB)";
    }
    return s.empty() ? std::string("(this build ships its own BIOS)") : s;
}

/* Identity-gate a player-chosen BIOS and activate its backend on success. */
static bool validate_bios_for_launch(const std::filesystem::path& path) {
    uint32_t crc = 0; uint64_t size = 0;
    const PsxBiosBackend* b = bios_backend_for_file(path, &crc, &size);
    if (b) return psx_bios_activate(b) != 0;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "That BIOS (%llu bytes, CRC32 %08X) is not an image this build was "
        "compiled from.\n\nThis build accepts: %s\n\nThe compiled-in code "
        "would execute against mismatched data and crash.",
        (unsigned long long)size, crc, bios_accepted_images().c_str());
    launcher_warning("BIOS Mismatch", buf);
    return false;
}

static std::filesystem::path resolve_bios_path(const char* requested, const char* argv0);

/* Resolve which BIOS image file to load, and activate its backend.
 *
 * Policy (docs/BIOS_SELECTION.md):
 *   no explicit player choice  -> bundled OpenBIOS, silently
 *   explicit choice, matching  -> that image
 *   explicit choice, mismatched-> explained; falls back to OpenBIOS if allowed
 *   openbios disabled for this title -> a retail image is required
 *
 * "Explicit" means --bios or a remembered launcher/settings/`bios.cfg` pick.
 * First-run setup may seed bios.cfg from a validated SCPH1001 beside the
 * install; after that, Play never invents a BIOS from random on-disk files.
 */
static std::filesystem::path resolve_bios_for_runtime(const char* requested,
                                                      const char* argv0,
                                                      bool requested_is_explicit) {
    const bool openbios_allowed = s_openbios_allowed;
    const PsxBiosBackend* bundled = psx_bios_bundled();
    const bool player_bios_selectable = psx_bios_has_selectable() != 0;
    const bool bundled_only =
        openbios_allowed && bundled && !player_bios_selectable;

    /* 1. An explicit choice: --bios, else a remembered pick. A product build
     * with only its bundled backend has no meaningful player choice: ignore
     * stale settings/bios.cfg paths instead of validating an image the hidden
     * launcher row cannot clear. Setup hosts (registry_count == 0) retain their
     * picker/generation flow. */
    std::filesystem::path chosen;
    if (!bundled_only) {
        if (requested_is_explicit && requested && requested[0]) {
            chosen = resolve_bios_path(requested, argv0);
        } else {
            std::filesystem::path cached = read_cached_path(argv0, "bios.cfg");
            if (!cached.empty() && std::filesystem::exists(cached)) chosen = cached;
        }
    }
    if (!chosen.empty() && std::filesystem::exists(chosen)) {
        if (validate_bios_for_launch(chosen)) return chosen;   /* activates it */
        if (!(openbios_allowed && bundled)) return {};
    }

    /* 2. No usable explicit choice -> bundled OpenBIOS, if this title allows it. */
    if (openbios_allowed && bundled) {
        std::filesystem::path img = resolve_bios_path(
            s_bundled_bios_rel.empty() ? nullptr : s_bundled_bios_rel.c_str(), argv0);
        if (!img.empty() && std::filesystem::exists(img) &&
            bios_backend_for_file(img, nullptr, nullptr) == bundled &&
            psx_bios_activate(bundled)) {
            return img;
        }
        launcher_warning("Bundled BIOS Missing",
            std::string("This build ships its own BIOS (") +
            bundled->image->image_id + "), but the bundled image is missing "
            "or does not match.\n\nExpected next to the executable:\n" +
            (s_bundled_bios_rel.empty() ? "(default path)" : s_bundled_bios_rel) +
            "\n\nReinstall or rebuild.");
        return {};
    }

    /* Setup host (CI zip root): no BIOS backends linked yet. Play belongs to
     * the product binary under build-release/ after Generate & rebuild. */
    if (psx_bios_registry_count == 0) {
        launcher_warning("Setup host — finish Generate & rebuild",
            "This executable is the first-run setup host (no game/BIOS code "
            "linked).\n\n"
            "Use Generate & rebuild in the launcher. After that succeeds, open "
            "this same shortcut again — it starts the game from build-release/ "
            "(where bios/, mods/, and settings live).\n\n"
            "Or run build-release/<game>.exe directly.");
        return {};
    }

    /* 3. This title requires a retail BIOS: ask for one. */
    const std::string accepted = bios_accepted_images();
    launcher_info((s_picker_game_name + " — PlayStation BIOS needed").c_str(),
        s_picker_game_name + " requires a PlayStation BIOS.\n\n"
        "Step 1 of 2 — PlayStation BIOS\n\n"
        "In the next window, select your PlayStation BIOS dump. This build "
        "requires the exact image it was compiled from: " + accepted + ". "
        "Usually named SCPH1001.BIN and exactly 512 KB. Dump from your own "
        "console or otherwise legally obtain it.\n\n"
        "(This is NOT the game disc — that is asked for next.)");
    std::string bios_title =
        s_picker_game_name + " — Step 1 of 2: select PlayStation BIOS (" +
        accepted + ")";
    for (;;) {
        std::filesystem::path picked;
        if (!pick_runtime_file(
                bios_title.c_str(),
                "PlayStation BIOS (*.bin)\0*.bin\0All Files (*.*)\0*.*\0",
                picked, "--bios")) {
            return {};
        }
        if (validate_bios_for_launch(picked)) {
            write_cached_path(argv0, "bios.cfg", picked);
            return picked;
        }
    }
}

static std::filesystem::path resolve_disc_for_runtime(const std::filesystem::path& config_disc,
                                                      const char* disc_override,
                                                      const std::string& game_id,
                                                      const char* argv0) {
    if (disc_override && disc_override[0]) {
        std::filesystem::path p = normalize_disc_path_for_launch(disc_override);
        return validate_disc_for_launch(p, game_id) ? p : std::filesystem::path{};
    }

    // A relative disc path in game.toml resolves against the exe dir (never cwd);
    // resolve_existing_runtime_path also walks up from there (build/ -> game dir),
    // so an out-of-tree build layout still finds the bundled disc.
    if (!config_disc.empty()) {
        std::filesystem::path cd = config_disc;
        if (cd.is_relative()) {
            std::filesystem::path r = resolve_existing_runtime_path(config_disc.string().c_str(), argv0);
            if (!r.empty()) cd = r;
        }
        cd = normalize_disc_path_for_launch(cd);
        if (std::filesystem::exists(cd) && validate_disc_for_launch(cd, game_id)) {
            return cd;
        }
    }

    std::filesystem::path cached = read_cached_path(argv0, "disc.cfg");
    if (!cached.empty()) {
        cached = normalize_disc_path_for_launch(cached);
    }
    if (!cached.empty() && std::filesystem::exists(cached) &&
        validate_disc_for_launch(cached, game_id)) {
        return cached;
    }

    launcher_info((s_picker_game_name + " — game disc image needed").c_str(),
        "Step 2 of 2 — game disc image\n\n"
        "In the next window, select your " + s_picker_game_name +
        (game_id.empty() ? std::string() : " (" + game_id + ")") +
        " disc image ripped from your own disc.\n\n"
        "Accepted formats: .cue (preferred, with its .bin next to it), "
        ".bin, .img, .iso, .car (Steam), or .chd.\n\n"
        "(This is NOT the BIOS — the BIOS was already chosen.)");
    std::string disc_title =
        s_picker_game_name + " — Step 2 of 2: select " + s_picker_game_name +
        " disc image (.cue / .bin / .img / .iso / .car / .chd)";
    for (;;) {
        std::filesystem::path picked;
        if (!pick_runtime_file(
                disc_title.c_str(),
                "PS1 Disc Images (*.cue;*.bin;*.img;*.iso;*.car;*.chd)\0*.cue;*.bin;*.img;*.iso;*.car;*.chd\0All Files (*.*)\0*.*\0",
                picked, "--disc")) {
            return {};
        }
        picked = normalize_disc_path_for_launch(picked);
        if (validate_disc_for_launch(picked, game_id)) {
            write_cached_path(argv0, "disc.cfg", picked);
            return picked;
        }
    }
}

static std::filesystem::path resolve_bios_path(const char* requested, const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!requested || !requested[0]) return {};
    fs::path p(requested);
    if (fs::exists(p, ec)) {
        fs::path abs = fs::absolute(p, ec);
        return ec ? p : abs;
    }
    // Either BIOS filename convention is acceptable: a dump folder holding
    // "US-PSX-SCPH1001.BIN" satisfies a request for "SCPH1001.BIN" and vice
    // versa (see recompiler/include/bios_rom_alias.h).
    if (fs::path aliased = PSXRecompV4::resolve_bios_rom(p); aliased != p) {
        fs::path abs = fs::absolute(aliased, ec);
        return ec ? aliased : abs;
    }
    if (p.is_absolute()) return p;

    // Anchor on the exe directory — never cwd (see exe_dir_from_argv).
    fs::path found = find_upward(exe_dir_from_argv(argv0), p);
    if (!found.empty()) return found / p;
    // Same walk, accepting the other naming convention at each rung: the
    // literal name is absent but a region-qualified sibling may be present.
    for (fs::path dir = fs::absolute(exe_dir_from_argv(argv0), ec);
         !dir.empty(); dir = dir.parent_path()) {
        const fs::path aliased = PSXRecompV4::resolve_bios_rom(dir / p);
        if (aliased != dir / p && fs::exists(aliased, ec)) return aliased;
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    }

    // Dev-checkout rung: game projects keep the framework at
    // <game root>/psxrecomp-v4 (junction/worktree), so a relative default like
    // "bios/SCPH1001.BIN" lives under that prefix rather than at the game root.
    // A user install has no psxrecomp-v4 directory, so this rung cannot
    // resolve a build-machine BIOS there — it falls through to bios.cfg or the
    // interactive picker.
    const fs::path dev_marker = fs::path("psxrecomp-v4") / p;
    found = find_upward(exe_dir_from_argv(argv0), dev_marker);
    if (!found.empty()) return found / dev_marker;
    return p;
}

/* First-run / setup discovery of a retail BIOS the player already dumped next
 * to the game (docs/BIOS_SELECTION.md). Only size+CRC identity counts — wrong
 * dumps are skipped silently. Empty result → keep OpenBIOS (no prompt). */
static bool retail_bios_file_ok(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    if (psx_bios_registry_count > 0) {
        const PsxBiosBackend* b = bios_backend_for_file(path, nullptr, nullptr);
        return b && b->image && !b->image->image_bundled;
    }
    /* Setup host (no backends linked yet): accept validated SCPH-1001 only. */
    constexpr uint64_t kSize = 512u * 1024u;
    constexpr uint32_t kScph1001Crc = 0x37157331u;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const auto size = static_cast<uint64_t>(f.tellg());
    if (size != kSize) return false;
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!read_at(f, 0, data.data(), data.size())) return false;
    return crc32_compute(data.data(), data.size()) == kScph1001Crc;
}

static std::filesystem::path discover_retail_bios_near(const char* argv0) {
    namespace fs = std::filesystem;
    static const char* kNames[] = {
        "SCPH1001.BIN", "scph1001.bin", "SCPH-1001.BIN", "scph-1001.bin",
        "SCPH1001.bin", "scph1001.BIN",
    };
    static const char* kSubdirs[] = {
        "bios", "", "system", "firmware", "psxrecomp/bios", "psxrecomp-v4/bios",
    };
    const fs::path exe_dir = exe_dir_from_argv(argv0);
    std::error_code ec;
    for (fs::path root = exe_dir; !root.empty(); root = root.parent_path()) {
        for (const char* sub : kSubdirs) {
            const fs::path dir = (sub && sub[0]) ? (root / sub) : root;
            for (const char* name : kNames) {
                const fs::path cand = dir / name;
                if (retail_bios_file_ok(cand)) {
                    auto abs = fs::weakly_canonical(cand, ec);
                    if (ec) abs = fs::absolute(cand, ec);
                    return abs;
                }
            }
        }
        if (!root.has_parent_path() || root == root.root_path()) break;
    }
    return {};
}

/* Match-only BIOS from lobby `session_bios`. Never writes bios.cfg / settings.
 * Returns true when session_bios is a known settle token.
 * *out_path empty ⇒ OpenBIOS for this match; otherwise a validated retail dump. */
static bool resolve_match_session_bios_path(
    const char* session_bios,
    const std::filesystem::path& preferred_hint,
    const char* launcher_bios_path,
    const char* argv0,
    std::filesystem::path* out_path) {
    if (!out_path || !session_bios || !session_bios[0])
        return false;
    if (std::strcmp(session_bios, "openbios") == 0) {
        out_path->clear();
        return true;
    }
    if (std::strcmp(session_bios, "scph1001") != 0)
        return false;

    std::filesystem::path retail;
    std::error_code ec;
    auto try_retail = [&](const std::filesystem::path& p) {
        if (p.empty() || !std::filesystem::exists(p, ec))
            return;
        const PsxBiosBackend* b = bios_backend_for_file(p, nullptr, nullptr);
        if (b && b->image && !b->image->image_bundled)
            retail = p;
    };
    try_retail(preferred_hint);
    if (retail.empty() && launcher_bios_path && launcher_bios_path[0])
        try_retail(resolve_bios_path(launcher_bios_path, argv0));
    if (retail.empty())
        try_retail(read_cached_path(argv0, "bios.cfg"));
    if (retail.empty())
        try_retail(discover_retail_bios_near(argv0));
    *out_path = std::move(retail); /* empty ⇒ caller falls back to OpenBIOS */
    return true;
}

// Fallback memcard directory used when no game config (or its [runtime]
// block) specifies one: the executable's directory (authoritative, never cwd —
// see exe_dir_from_argv), so saves always live next to the binary.
static std::filesystem::path default_memcard_dir(const char* argv0) {
    return exe_dir_from_argv(argv0);
}

static void close_controller(void);
static void shutdown_runtime(void);

/* Set when this process started netplay from the lobby room — soft-exit returns
 * there instead of killing the process. */
static int g_netplay_from_lobby = 0;
static int g_netplay_vsync_forced_off = 0;
/* Manual fast-forward drops the swap interval to 0 for its duration. With
 * driver vsync on, every present blocks ~one refresh, so turbo was capped at
 * (present_every x refresh) regardless of CPU headroom — present_every is 2 at
 * the default 4x multiplier, i.e. a hard 2x ceiling, and even
 * PSX_FAST_FORWARD_SPEED=max could not exceed 4x. Deliberately consulted only
 * by present_effective_swap_interval(), NOT by present_vsync_owns_cadence():
 * vsync must keep "owning cadence" so present_should_wall_pace() stays false
 * and the 60 Hz wall pacer does not replace the ceiling it just removed. The
 * turbo block's own frame_pacer_wait(period / mult) remains the limiter. */
static int g_turbo_vsync_forced_off = 0;

static void apply_netplay_local_viewport_aspect(bool netplay_enabled) {
    if (!netplay_enabled ||
        g_netplay_local_viewport != 1 ||
        g_netplay_local_viewport_aspect == 0) {
        gpu_ws_set_netplay_local_viewport(0, 0);
        return;
    }

    gpu_ws_set_netplay_local_viewport(1, psx_netplay_local_slot());
    switch (g_netplay_local_viewport_aspect) {
        case 1:
            (void)psx_mod_set_fixed_display_aspect(16u, 9u);
            break;
        case 2:
            (void)psx_mod_set_fixed_display_aspect(21u, 9u);
            break;
        case 3:
            (void)psx_mod_set_fixed_display_aspect(16u, 9u);
            (void)psx_mod_set_adaptive_display_aspect(21u, 9u);
            break;
        default:
            break;
    }
}

/* Host-only: lockstep already couples peers. Driver vsync on top of the
 * wall-clock pacer double-blocks the vblank callback (present is before the
 * guest resumes), which shows up as MotK FMV ~30–40 FPS in netplay vs ~50+
 * offline. Force immediate swaps for the session; restore on soft-exit. */
static int g_present_half_rate_healed = 0; /* 1 once 0.50x heal tripped */
/* Wayland: driver vsync + wall pacer (or compositor-forced FIFO with pacer)
 * lands at ~0.50x. Empirically PSX_VSYNC=0 (pacer + immediate swap) holds
 * 60 fps; so never let driver vsync own cadence on Wayland unless opted in. */
static int g_wayland_allow_vsync = 0;

static int host_video_is_wayland(void) {
    const char *driver = SDL_GetCurrentVideoDriver();
    return driver && std::strcmp(driver, "wayland") == 0;
}

/* Plausible panel rates only. SDL/Wayland has stuffed pixel width (e.g. 3840
 * for 4K) into refresh_rate; treating that as Hz selects the wrong cadence
 * path and pairs badly with compositor present sync. */
static double sanitize_host_refresh_hz(double hz, int mode_w, int mode_h,
                                       double raw_hz, const char **reject_why) {
    if (reject_why) *reject_why = nullptr;
    if (!(hz > 0.0))
        return 0.0;
    if (hz < 20.0 || hz > 500.0) {
        if (reject_why) *reject_why = "out of range";
        return 0.0;
    }
    if (mode_w > 0 && std::fabs(hz - (double)mode_w) < 0.5) {
        if (reject_why) *reject_why = "matches mode width";
        return 0.0;
    }
    if (mode_h > 0 && std::fabs(hz - (double)mode_h) < 0.5) {
        if (reject_why) *reject_why = "matches mode height";
        return 0.0;
    }
    (void)raw_hz;
    return hz;
}

static double display_mode_refresh_hz(const SDL_DisplayMode &dm,
                                      const char **reject_why) {
    double hz = 0.0;
#if defined(PSX_SDL3)
    /* Prefer rational rate: float refresh_rate is what Wayland has corrupted. */
    if (dm.refresh_rate_numerator > 0 && dm.refresh_rate_denominator > 0)
        hz = (double)dm.refresh_rate_numerator /
             (double)dm.refresh_rate_denominator;
#endif
    if (!(hz >= 20.0 && hz <= 500.0) && dm.refresh_rate > 0)
        hz = (double)dm.refresh_rate;
    return sanitize_host_refresh_hz(hz, dm.w, dm.h, (double)dm.refresh_rate,
                                    reject_why);
}

static int host_refresh_matches_crtc(void) {
    const double psx_hz = psx_crtc_frame_hz();
    return g_host_refresh_hz >= psx_hz * 0.98 &&
           g_host_refresh_hz <= psx_hz * 1.02;
}

static int present_vsync_owns_cadence(void) {
    if (g_video_vsync == 0 || g_present_vsync_disabled)
        return 0;
    if (g_frame_period_ms <= 0.0)
        return 0;
    if (g_frame_interpolation)
        return 0;
    if (g_netplay_vsync_forced_off || psx_netplay_active())
        return 0;
    /* An explicit native guest rate is a simulation contract, not merely a
     * request to match the panel.  In particular, PAL x2 must keep the
     * wall-clock pacer as the authority: the observed 100-Hz swap path can
     * miss presentation deadlines and then feed that debt back into guest
     * progress.  Swap interval 0 lets the pacer enforce the exact target. */
    if (g_mod_native_vblank_rate && g_mod_native_vblank_active)
        return 0;
    /* Wayland default: wall-clock pacer + swap interval 0 (same as PSX_VSYNC=0). */
    if (host_video_is_wayland() && !g_wayland_allow_vsync)
        return 0;
    return host_refresh_matches_crtc();
}

static int present_effective_swap_interval(void) {
    if (g_netplay_vsync_forced_off || psx_netplay_active())
        return 0;
    if (g_turbo_vsync_forced_off)
        return 0;
    if (g_frame_interpolation)
        return 0;
    if (g_frame_period_ms <= 0.0)
        return 0;
    if (present_vsync_owns_cadence())
        return g_video_vsync;
    return 0;
}

static int present_should_wall_pace(void) {
    return g_frame_period_ms > 0.0 && !present_vsync_owns_cadence();
}

/* PAL x2 owns the guest/presentation cadence. FMV presentation is reduced to
 * one of every two depth-24 frames below, while the guest remains at 100 Hz.
 * This is presentation-only: guest cycles, CD timing, and SPU production are
 * never rescaled for movies. */
static double present_frame_period_ms(void) {
    return g_frame_period_ms;
}

/* Live query for the GL presenter: nonzero while the blocking Swap is the
 * game's frame clock. While true, unchanged-frame present skips must be
 * disabled — a skipped swap is an unpaced frame (the "fps readout off makes
 * it stupid fast" bug). Evaluated at each skip decision so no cached copy
 * can go stale across init order or cadence changes. */
extern "C" int psx_present_swap_paces(void) {
    return present_vsync_owns_cadence() &&
           present_effective_swap_interval() != 0;
}

static void apply_present_cadence(void) {
#ifndef PSX_SDL_NO_RENDER
    const int interval = present_effective_swap_interval();
    if (g_gl_active)
        gl_renderer_set_swap_interval(interval);
    if (g_vk_active)
        vk_renderer_set_present_mode(interval);
    if (sdl_renderer)
        (void)SDL_RenderSetVSync(sdl_renderer, interval != 0 ? 1 : 0);
    latency_ring_set_present_mode(interval);
#endif
}

static void log_present_cadence(void) {
    if (present_vsync_owns_cadence()) {
        std::printf("psxrecomp: present cadence: driver vsync (%.1f Hz panel, "
                    "wall-clock pacer skipped)\n",
                    g_host_refresh_hz);
    } else if (g_frame_period_ms > 0.0) {
        if (host_video_is_wayland() && !g_wayland_allow_vsync &&
            g_video_vsync != 0 && !g_frame_interpolation &&
            !g_netplay_vsync_forced_off) {
            std::printf("psxrecomp: present cadence: wall-clock pacer "
                        "(%.4f ms/frame); Wayland forces swap interval 0 "
                        "(set PSX_WAYLAND_ALLOW_VSYNC=1 to opt into driver vsync)\n",
                        g_frame_period_ms);
        } else if (g_video_vsync != 0 && !g_frame_interpolation &&
                   !g_netplay_vsync_forced_off) {
            if (g_host_refresh_hz > 0.0) {
                std::printf("psxrecomp: present cadence: wall-clock pacer "
                            "(%.4f ms/frame); driver vsync off on %.0f Hz panel\n",
                            g_frame_period_ms, g_host_refresh_hz);
            } else {
                std::printf("psxrecomp: present cadence: wall-clock pacer "
                            "(%.4f ms/frame); driver vsync off "
                            "(host refresh unknown)\n",
                            g_frame_period_ms);
            }
        } else {
            std::printf("psxrecomp: present cadence: wall-clock pacer "
                        "(%.4f ms/frame)\n",
                        g_frame_period_ms);
        }
    } else {
        std::printf("psxrecomp: present cadence: uncapped "
                    "(no pacer, no vsync)\n");
    }
}

/* GP1(08h) changes the GPU's live VBlank period before the next VBlank edge is
 * raised. Reconcile the host side at that edge, before either the wall pacer or
 * SwapWindow can wait. The exact integer period is the transition key: stable
 * modes are no-ops, while PAL/NTSC, a restored mode, or a plugin timing change
 * is applied once. An explicit native-VBlank mod remains the pacing authority. */
static void refresh_live_crtc_cadence(void) {
    const uint32_t period = gpu_vblank_period_cycles();
    const int is_pal = gpu_display_is_pal() ? 1 : 0;
    const bool period_changed = period != s_present_crtc_period_cycles;
    const bool standard_changed = is_pal != s_present_crtc_is_pal;

    /* PAL x2 is a title-mode contract. Keep BIOS/boot on its GP1-selected
     * cadence and arm the native override only once the PAL game display is
     * actually programmed. */
    const bool native_was_active = g_mod_native_vblank_active;
    if (g_mod_native_vblank_rate && !g_mod_native_vblank_active &&
        gpu_display_mode_is_programmed() && is_pal) {
        gpu_set_crtc_refresh_multiplier(g_mod_pending_crtc_multiplier);
        g_mod_native_vblank_active = true;
        g_frame_period_ms = g_mod_native_vblank_fps
            ? 1000.0 / (double)g_mod_native_vblank_fps : 0.0;
    }
    const bool native_active_changed =
        native_was_active != g_mod_native_vblank_active;

    if (period_changed) {
        s_present_crtc_period_cycles = period;
        if (!(g_mod_native_vblank_rate && g_mod_native_vblank_active)) {
            refresh_frame_pacer_period();
            /* A new period must not inherit debt from the old clock. */
            s_frame_pacer = FramePacer{ 0 };
        }
        /* Reapply the same live policy used by the pacing query so the
         * renderer's actual swap interval cannot remain startup-latched.
         * This is still required when a mod explicitly owns the wall target:
         * that override does not own or alter the live CRTC period. */
        apply_present_cadence();
    }
    if (native_active_changed) {
        s_frame_pacer = FramePacer{ 0 };
        apply_present_cadence();
    }
    s_present_crtc_is_pal = is_pal;

    if (gpu_display_mode_is_programmed() &&
        (!s_present_crtc_banner_logged || period_changed || standard_changed)) {
        std::printf("psxrecomp: CRTC mode %s: %s x%u = %.3f Hz\n",
                    s_present_crtc_banner_logged ? "changed" : "programmed",
                    is_pal ? "PAL" : "NTSC",
                    (unsigned)gpu_get_crtc_refresh_multiplier(),
                    psx_crtc_frame_hz());
        log_present_cadence();
        std::fflush(stdout);
        s_present_crtc_banner_logged = true;
    }
}

static void netplay_host_present_uncap(void) {
    g_netplay_vsync_forced_off = 1;
    apply_present_cadence();
}

static void netplay_host_present_restore(void) {
    if (!g_netplay_vsync_forced_off) return;
    g_netplay_vsync_forced_off = 0;
    apply_present_cadence();
}

static void netplay_soft_exit(const char *origin) {
    psx_crash_trace_set_exit_origin(origin);
    netplay_host_present_restore();
    psx_netplay_shutdown(); /* sends BYE so the peer soft-exits too */
    if (g_netplay_from_lobby) {
        std::printf("psxrecomp: netplay ended (%s) — returning to lobby\n",
                    origin ? origin : "?");
        std::fflush(stdout);
        psx_request_return_to_lobby();
        return;
    }
    shutdown_runtime();
    std::exit(0);
}

static void shutdown_runtime(void) {
    /* (sljit removed 2026-07-15: overlay_compile_worker_stop joined the
     * off-thread JIT worker here; the worker no longer exists.) */
    psx_netplay_shutdown();
    psx_rewind_shutdown();
    memcard_flush_all();
    /* Stop and join the active external compiler before capture/debug teardown.
     * Otherwise closing the window can leave cmd/python/gcc running against
     * cache and capture files while the main thread performs synchronous
     * shutdown work, making the window appear frozen until that tree exits. */
    autocompile_shutdown();
    overlay_autocapture_shutdown();
    overlay_capture_wait_pending();
    overlay_capture_write_json();
    if (sdl_audio_device) {
        psx_sdl_audio_clear(sdl_audio_device);
        psx_sdl_audio_close(sdl_audio_device);   /* stops the pull callback */
        sdl_audio_device = 0;
    }
    if (s_drc_ready) { rab_free(&s_drc); s_drc_ready = false; }
    close_controller();
    debug_server_shutdown();
}

/* Tear down the game window/GL/audio after a lobby soft-return. Leaves SDL
 * subsystems and the lobby WebSocket intact for the next launcher session. */
static void teardown_game_session_keep_lobby(void) {
    netplay_host_present_restore();
    psx_netplay_shutdown();
    psx_rewind_shutdown();
    memcard_flush_all();
    if (sdl_audio_device) {
        psx_sdl_audio_clear(sdl_audio_device);
        psx_sdl_audio_close(sdl_audio_device);
        sdl_audio_device = 0;
    }
    if (s_drc_ready) { rab_free(&s_drc); s_drc_ready = false; }
    close_controller();
    if (g_vk_active) {
        vk_renderer_shutdown();
        g_vk_active = false;
    }
    if (g_gl_active) {
        gl_renderer_shutdown();
        g_gl_active = false;
    }
    if (sdl_texture) { SDL_DestroyTexture(sdl_texture); sdl_texture = nullptr; }
    if (sdl_renderer) { SDL_DestroyRenderer(sdl_renderer); sdl_renderer = nullptr; }
    if (sdl_window) { SDL_DestroyWindow(sdl_window); sdl_window = nullptr; }
    if (sdl_pixel_buf) { std::free(sdl_pixel_buf); sdl_pixel_buf = nullptr; }
    /* Rematch must re-run force_sw (and prefer CPU-auth GL present again). */
    s_netplay_sw_gpu_locked = 0;
    s_netplay_gl_present = 0;
    s_netplay_sim_native_scale = 0;
    gl_renderer_set_cpu_auth_dual(0);
    psx_lobby_set_ready(0);
    psx_lobby_clear_launch_pending();
#if defined(PSX_HAS_LOBBY_CLIENT)
    /* Drop prior-match ICE SDP/candidates; ignore new ones until next launch. */
    psx_lobby_clear_signals();
    psx_lobby_set_ice_signal_accept(0);
#endif
    psx_clear_return_to_lobby();
}

/* Linear gain ramp g0 -> g1 across a block of interleaved stereo frames. */
static void sdl_audio_gain_ramp(int16_t* buf, int frames, float g0, float g1) {
    if (frames <= 0) return;
    const float step = (g1 - g0) / (float)frames;
    float g = g0;
    for (int f = 0; f < frames; f++, g += step) {
        buf[f * 2 + 0] = (int16_t)((float)buf[f * 2 + 0] * g);
        buf[f * 2 + 1] = (int16_t)((float)buf[f * 2 + 1] * g);
    }
}

/* Fade-in state: samples of rising ramp still to apply after an unmute.
 * Consumed by sdl_audio_pump across however many pump calls it spans.
 * MUST fit sdl_audio_buf (2048 frames): the fade-out tail renders this many
 * frames in one spu_render call. 1764 frames = 40 ms.
 * sdl_audio_fadein_left is declared with present_session_reset. */
static const int sdl_audio_fade_samples = 44100 * 40 / 1000;  /* 40 ms */

static void sdl_audio_pump(bool discard_output = false) {
    /* Guest-cycle SPU advance must not depend on host audio device/backpressure.
     * Win↔Linux rollback forked on aux/spu when one peer skipped spu_render
     * (queue full / !drc / no device) while the other kept advancing. */
    const uint32_t bytes_per_frame = sizeof(int16_t) * 2u;
    const bool legacy = audio_legacy_mode();
    const int netplay = psx_netplay_active();
    static int had_audio = 0;
    uint32_t queued = 0;   /* RENDER event b: bytes (legacy) / fill ms (bridge) */
    int host_queue_ok = 0;

    if (discard_output) {
        /* Host-only sink: skip all queue/bridge interaction, but continue to
         * the guest-cycle sample budget and spu_render below. */
    } else if (sdl_audio_device && legacy) {
        /* Historical push model + baseline measurement: a drained (==0) queue
         * means the device was silence-filling since the last pump = a gap.
         * Only meaningful after the first audio has been queued. */
        const uint32_t max_queue_bytes = 44100u * bytes_per_frame / 5u;
        queued = psx_sdl_audio_queued_size(sdl_audio_device);
        if (queued == 0 && had_audio) {
            g_legacy_underruns++;
            audio_trace_event(AUDIO_EV_UNDERRUN, 0, 0);
        }
        if (queued > max_queue_bytes) {
            audio_trace_event(AUDIO_EV_PUMP_SKIP, queued, 0);
            /* Still advance SPU below; only skip host enqueue. */
        } else {
            host_queue_ok = 1;
        }
    } else if (sdl_audio_device && !legacy && s_drc_ready) {
        /* Surface bridge underruns (counted on the SDL audio thread) into the
         * event ring from this thread — the event ring is single-writer.
         * Across a turbo mute the ring intentionally runs dry (the pump stops
         * while the callback keeps pulling); those dry pulls are the mute,
         * not gaps — resync past them instead of reporting them. */
        rab_stats st;
        rab_get_stats(&s_drc, &st);
        static uint64_t prev_underruns = 0;
        extern int g_audio_unmute_resync;
        if (g_audio_unmute_resync) {
            prev_underruns = st.underrun_events;
            g_audio_unmute_resync = 0;
        } else if (st.underrun_events > prev_underruns) {
            audio_trace_event(AUDIO_EV_UNDERRUN,
                              (uint32_t)(st.underrun_events - prev_underruns), 1);
            prev_underruns = st.underrun_events;
        }
        queued = (uint32_t)st.last_fill_ms;
        host_queue_ok = 1;
    }

    /* Faithful sample budget: the SPU is clocked by the GUEST, not by host
     * presents. 33.8688 MHz / 44100 Hz = exactly 768 guest cycles per output
     * frame, so production tracks guest time precisely — including the real
     * NTSC 59.94 Hz vblank — instead of assuming 60.00 Hz per present, which
     * built in a systematic -0.1% production deficit (measured: 43950/s
     * produced vs 44100/s consumed = recurring ring underruns no +/-0.5%
     * DRC trim could absorb during jitter spikes). */
    extern uint64_t psx_cycle_count;
    static uint64_t last_cycles = 0;
    static uint64_t cycle_carry = 0;
    const uint64_t now_cycles = psx_cycle_count;
    if (g_audio_cycle_resync) {
        last_cycles = now_cycles;
        cycle_carry = 0;
        g_audio_cycle_resync = 0;
        if (legacy && sdl_audio_device)
            psx_sdl_audio_clear(sdl_audio_device);
        else
            g_audio_unmute_resync = 1; /* skip mute-drain underrun reports */
        return;
    }
    if (last_cycles == 0) last_cycles = now_cycles;
    uint64_t delta = (now_cycles - last_cycles) + cycle_carry;
    last_cycles = now_cycles;
    g_audio_guest_cycles_total += delta;
    g_audio_guest_pumps++;
    int frames = (int)psx_guest_audio_frames_for_cycles(delta, &cycle_carry);
    g_audio_guest_frames_budget += (uint64_t)frames;
    g_audio_guest_cycle_carry = cycle_carry;
    if (frames <= 0) return;
    if (frames > 2048 && !netplay) {
        /* Offline: a burst beyond one buffer (e.g. right after an unmute or a
         * long stall) renders one full buffer and DROPs the remainder of the
         * debt — mute-model freeze rather than time-compressing a backlog.
         * Netplay never drops: both peers must consume the same guest debt. */
        frames = 2048;
        cycle_carry = 0;
        g_audio_guest_cycle_carry = 0;
    }

    audio_trace_event(AUDIO_EV_RENDER, (uint32_t)frames, queued);

    /* Catch up in ≤2048-frame chunks (sdl_audio_buf capacity). Only the last
     * chunk may be handed to the host queue — earlier chunks advance state
     * only (avoids dumping seconds of catch-up into the device ring). */
    int remaining = frames;
    int host_frames = 0;
    while (remaining > 0) {
        const int chunk = remaining > 2048 ? 2048 : remaining;
        spu_render(sdl_audio_buf, chunk);
        g_audio_guest_frames_rendered += (uint64_t)chunk;
        remaining -= chunk;
        host_frames = chunk;
        if (discard_output) {
            g_turbo_audio_sink_frames += (uint64_t)chunk;
            audio_trace_event(AUDIO_EV_SINK_DROP, (uint32_t)chunk, 0);
        }
    }
    if (discard_output)
        return;
    if (!host_queue_ok || !sdl_audio_device)
        return;

    frames = host_frames;
    if (sdl_audio_fadein_left > 0) {
        const float g0 = 1.0f - (float)sdl_audio_fadein_left
                                / (float)sdl_audio_fade_samples;
        int ramp = sdl_audio_fadein_left < frames ? sdl_audio_fadein_left : frames;
        const float g1 = 1.0f - (float)(sdl_audio_fadein_left - ramp)
                                / (float)sdl_audio_fade_samples;
        sdl_audio_gain_ramp(sdl_audio_buf, ramp, g0, g1);
        sdl_audio_fadein_left -= ramp;
    }
    /* Host master volume (launcher / numpad +/-). Applied after fade so mute
     * edges stay continuous and volume steps take effect immediately. */
    {
        const int vol = host_volume_get();
        if (vol < 100) {
            const float g = (float)vol / 100.0f;
            sdl_audio_gain_ramp(sdl_audio_buf, frames, g, g);
        }
    }
    if (legacy) {
        /* T3 tap: the exact post-fade bytes handed to the host audio queue. */
        audio_trace_pcm(AUDIO_TAP_HOST, sdl_audio_buf, frames);
        psx_sdl_audio_queue(sdl_audio_device, sdl_audio_buf, (uint32_t)frames * bytes_per_frame);
        had_audio = 1;
    } else {
        /* Hand to the bridge (band-limited resample + DRC) instead of
         * SDL_QueueAudio. Lock guards the SPSC ring against the pull callback.
         * The T3 tap moves to sdl_drc_callback: what the device actually
         * receives is the bridge's device-rate output, not this buffer. */
        psx_sdl_audio_lock(sdl_audio_device);
        rab_push(&s_drc, sdl_audio_buf, frames);
        psx_sdl_audio_unlock(sdl_audio_device);
    }
}


/* Audio gating across turbo-loads transitions.
 *
 * The mute model stays: during turbo the guest runs at host speed, so
 * rendered SPU audio is time-compressed garble — we stop pumping, the queue
 * drains, voice positions freeze, and music resumes in place afterward.
 * What changes is the EDGES:
 *   - entering turbo: render one short tail of the current voice state,
 *     ramp it to silence, and queue it — the drain ends in a fade instead
 *     of a hard cut;
 *   - leaving turbo: hold the mute for a short hangover first (loads often
 *     re-trigger within a few frames; without the debounce the mute would
 *     flicker audibly), then resume pumping with a rising ramp applied
 *     across the first ~50 ms of samples (sdl_audio_pump above). */
/* Bridge/legacy output health, surfaced through the audio_stats TCP command
 * (debug_server.c) — no stderr probe; rule 3. */
extern "C" int psx_audio_out_stats(double *fill_ms, double *target_ms,
                                   uint64_t *underruns,
                                   uint64_t *overflow_drops, double *correction,
                                   int *legacy, int *host_rate)
{
    *legacy = audio_legacy_mode() ? 1 : 0;
    *host_rate = g_audio_host_rate;
    if (*legacy || !s_drc_ready) {
        *fill_ms = sdl_audio_device
                   ? (double)psx_sdl_audio_queued_size(sdl_audio_device)
                     / (44100.0 * 4.0) * 1000.0
                   : 0.0;
        *target_ms = 0.0; /* push queue — no DRC fill target */
        *underruns = g_legacy_underruns;
        *overflow_drops = 0;
        *correction = 0.0;
        return sdl_audio_device != 0;
    }
    rab_stats st;
    rab_get_stats(&s_drc, &st);
    *fill_ms = st.last_fill_ms;
    *target_ms = s_drc.cfg.target_ms;
    *underruns = st.underrun_events;
    *overflow_drops = st.overflow_drops;
    *correction = st.last_correction;
    return 1;
}

/* Lightweight production-safe cadence probe. Unlike the TCP debug build this
 * adds no per-block or per-instruction recording. All counter/timer work below
 * is opt-in via PSX_RUNTIME_PERF_DIAG; the normal Release path pays only the
 * existing once-per-vblank disabled checks. */
struct RuntimePerfSnapshot {
    uint64_t counter = 0;
    uint64_t frame = 0;
    uint64_t guest_work_ticks = 0;
    uint64_t pacer_ticks = 0;
    uint64_t autocapture_ticks = 0;
    uint64_t provider_poll_ticks = 0;
    uint64_t dirty_insns = 0;
    uint64_t dirty_dispatches = 0;
    uint32_t overlay_loads = 0;
    uint32_t overlay_invalidations = 0;
    uint32_t overlay_unregistered = 0;
    uint64_t overlay_native = 0;
    uint64_t overlay_interp = 0;
    uint64_t overlay_stale = 0;
    uint32_t overlay_revalidations = 0;
    uint32_t overlay_hot_native_pc = 0;
    uint64_t overlay_hot_native_calls = 0;
    uint64_t overlay_shadow_calls = 0;
    uint64_t overlay_shadow_divergences = 0;
    uint32_t overlay_first_divergence_pc = 0;
    uint32_t capture_triggers = 0;
    uint64_t capture_last_dispatch_delta = 0;
    int capture_overlays = 0;
};

struct RuntimePerfState {
    bool initialized = false;
    bool enabled = false;
    uint32_t interval_ms = 5000;
    uint64_t frequency = 0;
    uint64_t last_frame_exit_counter = 0;
    uint64_t guest_work_ticks = 0;
    uint64_t pacer_ticks = 0;
    uint64_t autocapture_ticks = 0;
    uint64_t provider_poll_ticks = 0;
    bool bench_configured = false;
    bool bench_started = false;
    bool bench_reported = false;
    uint64_t bench_start_frame = 0;
    uint64_t bench_end_frame = 0;
    RuntimePerfSnapshot bench_start;
};

static RuntimePerfState g_runtime_perf;

static bool runtime_perf_parse_window(const char *text, uint64_t *start,
                                      uint64_t *end) {
    if (!text || !text[0]) return false;
    char *middle = nullptr;
    unsigned long long first = std::strtoull(text, &middle, 10);
    if (middle == text || *middle != ':') return false;
    char *tail = nullptr;
    unsigned long long last = std::strtoull(middle + 1, &tail, 10);
    if (tail == middle + 1 || *tail != '\0' || first == 0 || last <= first)
        return false;
    *start = (uint64_t)first;
    *end = (uint64_t)last;
    return true;
}

static void runtime_perf_init() {
    if (g_runtime_perf.initialized) return;
    g_runtime_perf.initialized = true;
    const char *enabled = std::getenv("PSX_RUNTIME_PERF_DIAG");
    g_runtime_perf.enabled = enabled && enabled[0] && enabled[0] != '0';
    if (!g_runtime_perf.enabled) return;

    g_runtime_perf.frequency = SDL_GetPerformanceFrequency();
    if (!g_runtime_perf.frequency) g_runtime_perf.frequency = 1;
    if (const char *interval = std::getenv("PSX_RUNTIME_PERF_DIAG_MS")) {
        char *tail = nullptr;
        long requested = std::strtol(interval, &tail, 10);
        if (tail != interval && *tail == '\0') {
            if (requested < 250) requested = 250;
            if (requested > 600000) requested = 600000;
            g_runtime_perf.interval_ms = (uint32_t)requested;
        } else {
            std::fprintf(stderr,
                "psxrecomp: ignoring invalid PSX_RUNTIME_PERF_DIAG_MS='%s'\n",
                interval);
        }
    }
    if (const char *window = std::getenv("PSX_BENCH_WINDOW")) {
        if (runtime_perf_parse_window(window,
                                      &g_runtime_perf.bench_start_frame,
                                      &g_runtime_perf.bench_end_frame)) {
            g_runtime_perf.bench_configured = true;
        } else {
            std::fprintf(stderr,
                "psxrecomp: ignoring invalid PSX_BENCH_WINDOW='%s' "
                "(expected start:end, start >= 1, end > start)\n", window);
        }
    }
    std::fprintf(stdout, "psxrecomp: runtime perf diagnostics enabled: interval=%u ms",
                 g_runtime_perf.interval_ms);
    if (g_runtime_perf.bench_configured)
        std::fprintf(stdout, ", bench=%llu:%llu",
                     (unsigned long long)g_runtime_perf.bench_start_frame,
                     (unsigned long long)g_runtime_perf.bench_end_frame);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

static RuntimePerfSnapshot runtime_perf_snapshot(uint64_t now) {
    RuntimePerfSnapshot s;
    s.counter = now;
    s.frame = s_frame_count;
    s.guest_work_ticks = g_runtime_perf.guest_work_ticks;
    s.pacer_ticks = g_runtime_perf.pacer_ticks;
    s.autocapture_ticks = g_runtime_perf.autocapture_ticks;
    s.provider_poll_ticks = g_runtime_perf.provider_poll_ticks;
    s.dirty_insns = g_dirty_ram_insns_run;
    s.dirty_dispatches = g_dirty_window_dispatches;
    overlay_loader_get_counters(&s.overlay_loads, &s.overlay_invalidations,
                                &s.overlay_unregistered, &s.overlay_native,
                                &s.overlay_interp, &s.overlay_stale,
                                nullptr, nullptr, nullptr, nullptr,
                                &s.overlay_revalidations);
    overlay_loader_take_hot_native(&s.overlay_hot_native_pc,
                                   &s.overlay_hot_native_calls);
    overlay_loader_get_shadow_summary(&s.overlay_shadow_calls,
                                      &s.overlay_shadow_divergences,
                                      &s.overlay_first_divergence_pc);
    int capture_enabled = 0;
    overlay_autocapture_get_status(&capture_enabled, &s.capture_triggers,
                                   &s.capture_last_dispatch_delta);
    s.capture_overlays = overlay_capture_count();
    return s;
}

static double runtime_perf_ticks_ms(uint64_t ticks) {
    return (double)ticks * 1000.0 / (double)g_runtime_perf.frequency;
}

static void runtime_perf_bench_tick(uint64_t now) {
    if (!g_runtime_perf.bench_configured || g_runtime_perf.bench_reported) return;
    if (!g_runtime_perf.bench_started) {
        if (s_frame_count == g_runtime_perf.bench_start_frame) {
            g_runtime_perf.bench_start = runtime_perf_snapshot(now);
            g_runtime_perf.bench_started = true;
        }
        return;
    }
    if (s_frame_count != g_runtime_perf.bench_end_frame) return;

    RuntimePerfSnapshot end = runtime_perf_snapshot(now);
    const RuntimePerfSnapshot &start = g_runtime_perf.bench_start;
    std::fprintf(stdout,
        "[BENCH] window=%llu:%llu frames=%llu wall_ms=%.3f "
        "guest_work_ms=%.3f pacer_ms=%.3f autocapture_main_ms=%.3f "
        "provider_poll_ms=%.3f "
        "dirty_insns=+%llu dirty_dispatches=+%llu "
        "overlay_native=+%llu overlay_interp=+%llu overlay_loads=+%u "
        "overlay_invalidations=+%u overlay_unregistered=+%u "
        "overlay_stale=+%llu overlay_revalidations=+%u "
        "capture_triggers=+%u capture_overlays=+%d "
        "capture_last_dispatch_delta=%llu\n",
        (unsigned long long)start.frame, (unsigned long long)end.frame,
        (unsigned long long)(end.frame - start.frame),
        runtime_perf_ticks_ms(end.counter - start.counter),
        runtime_perf_ticks_ms(end.guest_work_ticks - start.guest_work_ticks),
        runtime_perf_ticks_ms(end.pacer_ticks - start.pacer_ticks),
        runtime_perf_ticks_ms(end.autocapture_ticks - start.autocapture_ticks),
        runtime_perf_ticks_ms(end.provider_poll_ticks - start.provider_poll_ticks),
        (unsigned long long)(end.dirty_insns - start.dirty_insns),
        (unsigned long long)(end.dirty_dispatches - start.dirty_dispatches),
        (unsigned long long)(end.overlay_native - start.overlay_native),
        (unsigned long long)(end.overlay_interp - start.overlay_interp),
        end.overlay_loads - start.overlay_loads,
        end.overlay_invalidations - start.overlay_invalidations,
        end.overlay_unregistered - start.overlay_unregistered,
        (unsigned long long)(end.overlay_stale - start.overlay_stale),
        end.overlay_revalidations - start.overlay_revalidations,
        end.capture_triggers - start.capture_triggers,
        end.capture_overlays - start.capture_overlays,
        (unsigned long long)end.capture_last_dispatch_delta);
    std::fflush(stdout);
    g_runtime_perf.bench_reported = true;
}

static void runtime_perf_frame_begin() {
    runtime_perf_init();
    if (!g_runtime_perf.enabled) return;
    uint64_t now = SDL_GetPerformanceCounter();
    if (g_runtime_perf.last_frame_exit_counter &&
        now >= g_runtime_perf.last_frame_exit_counter) {
        g_runtime_perf.guest_work_ticks +=
            now - g_runtime_perf.last_frame_exit_counter;
    }
    runtime_perf_bench_tick(now);
}

static void runtime_perf_frame_end() {
    if (!g_runtime_perf.enabled) return;
    g_runtime_perf.last_frame_exit_counter = SDL_GetPerformanceCounter();
}

class RuntimePerfFrameScope {
public:
    ~RuntimePerfFrameScope() { runtime_perf_frame_end(); }
};

static uint64_t runtime_perf_section_begin() {
    return g_runtime_perf.enabled ? SDL_GetPerformanceCounter() : 0;
}

static void runtime_perf_section_end(uint64_t start, uint64_t *total) {
    if (!start) return;
    uint64_t end = SDL_GetPerformanceCounter();
    if (end >= start) *total += end - start;
}

static void runtime_perf_diag_tick() {
    static bool have_last = false;
    static RuntimePerfSnapshot last;
    static uint64_t last_spu = 0, last_underruns = 0, last_overflows = 0;
    static uint64_t last_up[6] = {0};
    static uint64_t last_overlay_load_us = 0;
    if (!g_runtime_perf.enabled) return;

    uint64_t now = SDL_GetPerformanceCounter();
    const uint64_t interval_ticks =
        g_runtime_perf.frequency * (uint64_t)g_runtime_perf.interval_ms / 1000u;
    if (have_last && now - last.counter < interval_ticks) return;

    RuntimePerfSnapshot current = runtime_perf_snapshot(now);
    AudioTraceStats audio;
    audio_trace_get_stats(&audio);
    double fill_ms = 0.0, target_ms = 0.0, correction = 0.0;
    uint64_t underruns = 0, overflows = 0;
    int legacy = 0, host_rate = 0;
    psx_audio_out_stats(&fill_ms, &target_ms, &underruns, &overflows, &correction,
                        &legacy, &host_rate);
    (void)target_ms;
    uint64_t up[6] = {0};
    gl_renderer_runtime_diag(up);
    uint64_t overlay_load_us = 0, overlay_load_max_us = 0, overlay_load_last_us = 0;
    overlay_loader_get_load_timing(&overlay_load_us, &overlay_load_max_us,
                                   &overlay_load_last_us);
    if (!have_last) {
        last = current;
        last_spu = audio.tap_frames[AUDIO_TAP_SPU_OUT];
        last_underruns = underruns;
        last_overflows = overflows;
        last_overlay_load_us = overlay_load_us;
        for (int i = 0; i < 6; i++) last_up[i] = up[i];
        have_last = true;
        return;
    }

    const double dt = (double)(current.counter - last.counter) /
                      (double)g_runtime_perf.frequency;
    std::fprintf(stdout,
        "psxrecomp: runtime cadence: guest=%.2f Hz, spu=%.1f Hz, "
        "audio_fill=%.1f ms, underruns=+%llu, overflows=+%llu, corr=%+.5f; "
        "GL upload=%.1f calls/s %.1f rect/s %.2f Mpix/s, "
        "cpu=%.1f tex=%.1f draw=%.1f ms/s; "
        "work guest=%.1f pacer=%.1f autocapture=%.1f provider_poll=%.1f ms/s, "
        "dirty=%.0f insn/s %.0f dispatch/s; "
        "overlay native=+%llu interp=+%llu hot_native=0x%08X/+%llu "
        "shadow=+%llu div=+%llu first_div=0x%08X "
        "loads=+%u revalidations=+%u "
        "load_wall=%.1f ms max=%.1f last=%.1f ms; "
        "capture triggers=+%u overlays=+%d last_dispatch_delta=%llu\n",
        (double)(current.frame - last.frame) / dt,
        (double)(audio.tap_frames[AUDIO_TAP_SPU_OUT] - last_spu) / dt,
        fill_ms, (unsigned long long)(underruns - last_underruns),
        (unsigned long long)(overflows - last_overflows), correction,
        (double)(up[0] - last_up[0]) / dt,
        (double)(up[1] - last_up[1]) / dt,
        (double)(up[2] - last_up[2]) / dt / 1.0e6,
        (double)(up[3] - last_up[3]) * 1000.0 /
            (double)g_runtime_perf.frequency / dt,
        (double)(up[4] - last_up[4]) * 1000.0 /
            (double)g_runtime_perf.frequency / dt,
        (double)(up[5] - last_up[5]) * 1000.0 /
            (double)g_runtime_perf.frequency / dt,
        runtime_perf_ticks_ms(current.guest_work_ticks - last.guest_work_ticks) / dt,
        runtime_perf_ticks_ms(current.pacer_ticks - last.pacer_ticks) / dt,
        runtime_perf_ticks_ms(current.autocapture_ticks - last.autocapture_ticks) / dt,
        runtime_perf_ticks_ms(current.provider_poll_ticks - last.provider_poll_ticks) / dt,
        (double)(current.dirty_insns - last.dirty_insns) / dt,
        (double)(current.dirty_dispatches - last.dirty_dispatches) / dt,
        (unsigned long long)(current.overlay_native - last.overlay_native),
        (unsigned long long)(current.overlay_interp - last.overlay_interp),
        current.overlay_hot_native_pc,
        (unsigned long long)current.overlay_hot_native_calls,
        (unsigned long long)(current.overlay_shadow_calls - last.overlay_shadow_calls),
        (unsigned long long)(current.overlay_shadow_divergences -
                             last.overlay_shadow_divergences),
        current.overlay_first_divergence_pc,
        current.overlay_loads - last.overlay_loads,
        current.overlay_revalidations - last.overlay_revalidations,
        (double)(overlay_load_us - last_overlay_load_us) / 1000.0,
        (double)overlay_load_max_us / 1000.0,
        (double)overlay_load_last_us / 1000.0,
        current.capture_triggers - last.capture_triggers,
        current.capture_overlays - last.capture_overlays,
        (unsigned long long)current.capture_last_dispatch_delta);
    std::fflush(stdout);
    last = current;
    last_spu = audio.tap_frames[AUDIO_TAP_SPU_OUT];
    last_underruns = underruns;
    last_overflows = overflows;
    last_overlay_load_us = overlay_load_us;
    for (int i = 0; i < 6; i++) last_up[i] = up[i];
}

/* Audio gate state, shared with the mid-frame (VBlank-edge) pump.
 *
 * sdl_audio_update() below is the sole authority on whether a pump should emit
 * audio, discard it, or not run at all. The mid-frame pump exists to keep SPU
 * time flowing across guest busy-waits that never present a frame, but it must
 * NOT bypass that authority: pumping unconditionally would push real audio
 * during a turbo-load hard mute, defeating the mute model (the queue is
 * supposed to drain and voice positions freeze in place, so music resumes where
 * it left off rather than replaying time-compressed garble), and would emit to
 * the device during the discard-only turbo sink.
 *
 * So the mid-frame pump mirrors whatever the last frame decided. */
enum AudioGate { AUDIO_GATE_NORMAL = 0, AUDIO_GATE_MUTED = 1, AUDIO_GATE_SINK = 2 };
static AudioGate s_audio_gate = AUDIO_GATE_NORMAL;

/* Invoked from the guest-derived VBlank edge (interrupts.c). */
static void sdl_audio_pump_midframe(void) {
    /* Device optional: SPU still advances from guest cycles (netplay-safe). */
    switch (s_audio_gate) {
    case AUDIO_GATE_MUTED:
        /* Deliberately nothing. This preserves the existing, user-validated
         * freeze-in-place mute semantics exactly. NOTE a real tension here: the
         * SPU is autonomous on hardware and never freezes, so a game that
         * busy-waits on an SPU-generated condition *during* a turbo load would
         * still stall. No title in our suite is known to do that; recording it
         * rather than guessing a fix that would change validated mute audio. */
        return;
    case AUDIO_GATE_SINK:
        sdl_audio_pump(true);   /* advance SPU time, discard output */
        return;
    case AUDIO_GATE_NORMAL:
    default:
        sdl_audio_pump(false);
        return;
    }
}

static void sdl_audio_update(int hard_mute_active, int turbo_sink_active) {
    {   /* Tag audio events with the vblank frame counter. */
        extern uint64_t s_frame_count;
        audio_trace_note_frame((uint32_t)s_frame_count);
    }
    const int HANGOVER_FRAMES = 8;  /* ~133 ms at 60 fps */
    static int muted = 0;
    static int hangover = 0;
    static int sink_was_active = 0;

    if (hard_mute_active) {
        if (!muted) {
            int tail = sdl_audio_fade_samples;
            const int buf_cap = (int)(sizeof(sdl_audio_buf) / (2 * sizeof(int16_t)));
            if (tail > buf_cap) tail = buf_cap;
            /* An unmute ramp may still be in progress; start the down-ramp
             * from its current gain so the edge stays continuous. */
            const float vol = (float)host_volume_get() / 100.0f;
            const float g0 = (1.0f - (float)sdl_audio_fadein_left
                                     / (float)sdl_audio_fade_samples) * vol;
            sdl_audio_fadein_left = 0;
            spu_render(sdl_audio_buf, tail);
            sdl_audio_gain_ramp(sdl_audio_buf, tail, g0, 0.0f);
            audio_trace_event(AUDIO_EV_MUTE, (uint32_t)tail, 0);
            if (sdl_audio_device && audio_legacy_mode()) {
                audio_trace_pcm(AUDIO_TAP_HOST, sdl_audio_buf, tail);
                psx_sdl_audio_queue(sdl_audio_device, sdl_audio_buf, (uint32_t)tail * sizeof(int16_t) * 2u);
            } else if (sdl_audio_device && s_drc_ready) {
                psx_sdl_audio_lock(sdl_audio_device);
                rab_push(&s_drc, sdl_audio_buf, tail);
                psx_sdl_audio_unlock(sdl_audio_device);
            }
            muted = 1;
        }
        s_audio_gate = AUDIO_GATE_MUTED;
        hangover = HANGOVER_FRAMES;
        return;
    }
    if (muted) {
        /* Still inside the post-mute hangover: the gate stays MUTED so the
         * mid-frame pump does not sneak audio out ahead of the unmute ramp. */
        if (hangover > 0) { hangover--; return; }
        muted = 0;
        sdl_audio_fadein_left = sdl_audio_fade_samples;
        audio_trace_event(AUDIO_EV_UNMUTE, (uint32_t)sdl_audio_fadein_left, 0);
        extern int g_audio_unmute_resync;
        g_audio_unmute_resync = 1;
    }
    if (turbo_sink_active) {
        if (!sink_was_active) {
            sink_was_active = 1;
            audio_trace_event(AUDIO_EV_MUTE, 0, 2); /* b=2: discard-only sink */
        }
        g_turbo_audio_sink_active = 1;
        s_audio_gate = AUDIO_GATE_SINK;
        sdl_audio_pump(true);
        return;
    }
    g_turbo_audio_sink_active = 0;
    if (sink_was_active) {
        sink_was_active = 0;
        sdl_audio_fadein_left = sdl_audio_fade_samples;
        audio_trace_event(AUDIO_EV_UNMUTE,
                          (uint32_t)sdl_audio_fadein_left, 2);
        g_audio_unmute_resync = 1;
    }
    s_audio_gate = AUDIO_GATE_NORMAL;
    sdl_audio_pump(false);
}

/* PS1 digital pad button bits (active-low: 0=pressed, 1=released).
 * Bit 0 = SELECT, Bit 1 = L3, Bit 2 = R3, Bit 3 = START,
 * Bit 4 = UP, Bit 5 = RIGHT, Bit 6 = DOWN, Bit 7 = LEFT,
 * Bit 8 = L2, Bit 9 = R2, Bit 10 = L1, Bit 11 = R1,
 * Bit 12 = TRIANGLE, Bit 13 = CIRCLE, Bit 14 = CROSS, Bit 15 = SQUARE.
 * L3/R3 (stick clicks) exist on a DualShock only; the wire reports them like
 * Beetle's dualshock.cpp does — straight from the button word, no mode mask. */
#define PAD_SELECT   (1 << 0)
#define PAD_L3       (1 << 1)
#define PAD_R3       (1 << 2)
#define PAD_START    (1 << 3)
#define PAD_UP       (1 << 4)
#define PAD_RIGHT    (1 << 5)
#define PAD_DOWN     (1 << 6)
#define PAD_LEFT     (1 << 7)
#define PAD_L2       (1 << 8)
#define PAD_R2       (1 << 9)
#define PAD_L1       (1 << 10)
#define PAD_R1       (1 << 11)
#define PAD_TRIANGLE (1 << 12)
#define PAD_CIRCLE   (1 << 13)
#define PAD_CROSS    (1 << 14)
#define PAD_SQUARE   (1 << 15)

struct ControllerSource {
    enum class Kind {
        None,
        Button,
        AxisPositive,
        AxisNegative,
    };

    Kind kind = Kind::None;
    int id = -1;
};

struct PsxButtonMap {
    uint16_t bit;               /* 0 = stick-direction slot (no digital bit alone) */
    const char* ini_name;
    std::vector<ControllerSource> sources;
    /* When set, a pressed stick-direction source also contributes this d-pad
     * bit in digital mode (ls_* -> Up/Down/Left/Right). */
    uint16_t fold_bit = 0;
};

static int controller_device_index = 0;
/* Default ~10% of SDL axis range (32767). Overridden per-player via settings. */
static int controller_deadzone = 3277;
/* [controller] anti_deadzone (game.toml). 0 = off, the historical behaviour. */
static int controller_anti_deadzone = 0;
static constexpr int kDefaultDeadzoneRaw = 3277;
static constexpr int kControllerMapN = 24;
using ControllerMap = std::array<PsxButtonMap, kControllerMapN>;
static ControllerMap controller_map = {{
    { PAD_UP,       "up",       {}, 0 },
    { PAD_DOWN,     "down",     {}, 0 },
    { PAD_LEFT,     "left",     {}, 0 },
    { PAD_RIGHT,    "right",    {}, 0 },
    { PAD_CROSS,    "cross",    {}, 0 },
    { PAD_CIRCLE,   "circle",   {}, 0 },
    { PAD_SQUARE,   "square",   {}, 0 },
    { PAD_TRIANGLE, "triangle", {}, 0 },
    { PAD_L1,       "l1",       {}, 0 },
    { PAD_R1,       "r1",       {}, 0 },
    { PAD_L2,       "l2",       {}, 0 },
    { PAD_R2,       "r2",       {}, 0 },
    { PAD_L3,       "l3",       {}, 0 },
    { PAD_R3,       "r3",       {}, 0 },
    { PAD_START,    "start",    {}, 0 },
    { PAD_SELECT,   "select",   {}, 0 },
    /* Stick directions (analog axes by default; fold onto d-pad digitally). */
    { 0, "ls_up",    {}, PAD_UP },
    { 0, "ls_down",  {}, PAD_DOWN },
    { 0, "ls_left",  {}, PAD_LEFT },
    { 0, "ls_right", {}, PAD_RIGHT },
    { 0, "rs_up",    {}, 0 },
    { 0, "rs_down",  {}, 0 },
    { 0, "rs_left",  {}, 0 },
    { 0, "rs_right", {}, 0 },
}};
/* Per-GUID overrides from input.ini [mapping.<guid>]. Absent GUID => global. */
static std::unordered_map<std::string, ControllerMap> controller_maps_by_guid;

static const ControllerMap& controller_map_for(const PlayerInput& p) {
    if (p.guid[0]) {
        char lower[40]{};
        for (size_t i = 0; i < sizeof(lower) - 1 && p.guid[i]; ++i)
            lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(p.guid[i])));
        auto it = controller_maps_by_guid.find(lower);
        if (it != controller_maps_by_guid.end()) return it->second;
        // Legacy files may have stored mixed-case GUIDs before normalize.
        it = controller_maps_by_guid.find(p.guid);
        if (it != controller_maps_by_guid.end()) return it->second;
    }
    return controller_map;
}

static std::string trim_copy(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace((unsigned char)s[first])) first++;
    size_t last = s.size();
    while (last > first && std::isspace((unsigned char)s[last - 1])) last--;
    return s.substr(first, last - first);
}

/* Resolve the durable additive overlay-capture store. Explicit process env is
 * authoritative. For local development, parse only the two known capture keys
 * from a gitignored root .psxrecomp.env (or .env); do not import unrelated
 * variables/secrets into the game process. Relative paths are root-relative. */
static std::filesystem::path resolve_overlay_capture_path(
        const std::filesystem::path& project_root,
        const std::filesystem::path& exe_dir,
        const std::string& game_id) {
    std::string direct;
    std::string store_root;
    std::filesystem::path value_base = project_root;
    if (const char* e = std::getenv("PSX_OVERLAY_CAPTURES")) direct = e;
    if (const char* e = std::getenv("PSX_OVERLAY_CAPTURE_ROOT")) store_root = e;

    if (direct.empty() && store_root.empty()) {
        std::vector<std::filesystem::path> configs = {
            project_root / ".psxrecomp.env", project_root / ".env"
        };
        /* Framework developers commonly build a game whose config root is a
         * sibling repository. Walk upward from the executable as well so the
         * framework worktree's private config remains the durable authority. */
        std::filesystem::path cur = exe_dir;
        for (int i = 0; i < 8; ++i) {
            configs.push_back(cur / ".psxrecomp.env");
            const auto parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
        for (const auto& config : configs) {
            std::ifstream in(config);
            if (!in) continue;
            std::string line;
            while (std::getline(in, line)) {
                line = trim_copy(line);
                if (line.empty() || line[0] == '#') continue;
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = trim_copy(line.substr(0, eq));
                std::string value = trim_copy(line.substr(eq + 1));
                if (value.size() >= 2 &&
                    ((value.front() == '"' && value.back() == '"') ||
                     (value.front() == '\'' && value.back() == '\'')))
                    value = value.substr(1, value.size() - 2);
                if (key == "PSX_OVERLAY_CAPTURES") {
                    direct = value;
                    value_base = config.parent_path();
                } else if (key == "PSX_OVERLAY_CAPTURE_ROOT") {
                    store_root = value;
                    value_base = config.parent_path();
                }
            }
            if (!direct.empty() || !store_root.empty()) break;
        }
    }

    auto root_relative = [&](const std::string& raw) {
        std::filesystem::path p(raw);
        return p.is_absolute() ? p : value_base / p;
    };
    std::filesystem::path result;
    if (!direct.empty()) result = root_relative(direct);
    else if (!store_root.empty())
        result = root_relative(store_root) / game_id / "overlay_captures.json";
    else result = exe_dir / "overlay_captures.json";

    std::error_code ec;
    std::filesystem::create_directories(result.parent_path(), ec);
    if (ec) {
        std::fprintf(stderr, "psxrecomp: cannot create overlay capture store %s: %s\n",
                     result.parent_path().string().c_str(), ec.message().c_str());
        return exe_dir / "overlay_captures.json";
    }
    return result.lexically_normal();
}

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool parse_bool_value(const std::string& value, bool fallback) {
    std::string v = lower_copy(trim_copy(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

static ControllerSource parse_controller_source(const std::string& raw) {
    std::string s = lower_copy(trim_copy(raw));
    ControllerSource out;
    if (s.empty() || s == "none" || s == "disabled") return out;

    // recomp-ui pad capture persists axes as "name+" / "name-" (see
    // source_from_bind). Defaults historically omit the suffix for triggers
    // ("lefttrigger"). Accept both; strip the sign before name lookup.
    int dir = 0; // -1, 0 (unspecified), +1
    if (s.size() >= 2) {
        const char last = s.back();
        const char prev = s[s.size() - 2];
        if ((last == '+' || last == '-') &&
            (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')) {
            dir = (last == '+') ? +1 : -1;
            s.pop_back();
        }
    }

    auto as_button = [&](SDL_GameControllerButton b) -> ControllerSource {
        out.kind = ControllerSource::Kind::Button;
        out.id = b;
        return out;
    };
    auto as_axis = [&](SDL_GameControllerAxis a, int d) -> ControllerSource {
        // Unspecified direction → positive (triggers / capture default).
        out.kind = (d < 0) ? ControllerSource::Kind::AxisNegative
                           : ControllerSource::Kind::AxisPositive;
        out.id = a;
        return out;
    };

    if (s == "a") return as_button(SDL_CONTROLLER_BUTTON_A);
    if (s == "b") return as_button(SDL_CONTROLLER_BUTTON_B);
    if (s == "x") return as_button(SDL_CONTROLLER_BUTTON_X);
    if (s == "y") return as_button(SDL_CONTROLLER_BUTTON_Y);
    if (s == "back" || s == "view" || s == "select")
        return as_button(SDL_CONTROLLER_BUTTON_BACK);
    if (s == "start" || s == "menu")
        return as_button(SDL_CONTROLLER_BUTTON_START);
    if (s == "guide") return as_button(SDL_CONTROLLER_BUTTON_GUIDE);
    if (s == "leftstick") return as_button(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    if (s == "rightstick") return as_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    // Shoulders are digital buttons. A trailing +/- from axis-style capture is
    // ignored so "leftshoulder+" still maps to L1 instead of becoming unbound.
    if (s == "leftshoulder" || s == "lb" || s == "l1")
        return as_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    if (s == "rightshoulder" || s == "rb" || s == "r1")
        return as_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    if (s == "dpup" || s == "dpadup")
        return as_button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    if (s == "dpdown" || s == "dpaddown")
        return as_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    if (s == "dpleft" || s == "dpadleft")
        return as_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    if (s == "dpright" || s == "dpadright")
        return as_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    // Triggers: default "lefttrigger" and capture "lefttrigger+" both work.
    if (s == "lefttrigger" || s == "lt" || s == "l2")
        return as_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, dir == 0 ? +1 : dir);
    if (s == "righttrigger" || s == "rt" || s == "r2")
        return as_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, dir == 0 ? +1 : dir);

    // Stick axes (suffix required unless using a directional alias).
    if (s == "leftx") {
        if (dir == 0) return out;
        return as_axis(SDL_CONTROLLER_AXIS_LEFTX, dir);
    }
    if (s == "lefty") {
        if (dir == 0) return out;
        return as_axis(SDL_CONTROLLER_AXIS_LEFTY, dir);
    }
    if (s == "rightx") {
        if (dir == 0) return out;
        return as_axis(SDL_CONTROLLER_AXIS_RIGHTX, dir);
    }
    if (s == "righty") {
        if (dir == 0) return out;
        return as_axis(SDL_CONTROLLER_AXIS_RIGHTY, dir);
    }
    if (s == "lsright") return as_axis(SDL_CONTROLLER_AXIS_LEFTX, +1);
    if (s == "lsleft") return as_axis(SDL_CONTROLLER_AXIS_LEFTX, -1);
    if (s == "lsdown") return as_axis(SDL_CONTROLLER_AXIS_LEFTY, +1);
    if (s == "lsup") return as_axis(SDL_CONTROLLER_AXIS_LEFTY, -1);
    if (s == "rsright") return as_axis(SDL_CONTROLLER_AXIS_RIGHTX, +1);
    if (s == "rsleft") return as_axis(SDL_CONTROLLER_AXIS_RIGHTX, -1);
    if (s == "rsdown") return as_axis(SDL_CONTROLLER_AXIS_RIGHTY, +1);
    if (s == "rsup") return as_axis(SDL_CONTROLLER_AXIS_RIGHTY, -1);

    SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(s.c_str());
    if (button != SDL_CONTROLLER_BUTTON_INVALID)
        return as_button(button);

    SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(s.c_str());
    if (axis != SDL_CONTROLLER_AXIS_INVALID)
        return as_axis(axis, dir == 0 ? +1 : dir);

    return out;
}

static std::vector<ControllerSource> parse_source_list(const std::string& value) {
    std::vector<ControllerSource> sources;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        ControllerSource source = parse_controller_source(item);
        if (source.kind != ControllerSource::Kind::None) {
            sources.push_back(source);
        }
    }
    return sources;
}

static void apply_sources_to_map(ControllerMap& map, const char* name,
                                 const char* sources) {
    for (auto& entry : map) {
        if (std::strcmp(entry.ini_name, name) == 0) {
            entry.sources = parse_source_list(sources);
            return;
        }
    }
}

static void set_default_controller_mapping_into(ControllerMap& map) {
    for (auto& entry : map) entry.sources.clear();
    /* D-pad and sticks are separate (launcher Gamepad Bindings layout). Digital
     * mode still folds ls_* onto d-pad bits via fold_bit. */
    apply_sources_to_map(map, "up",       "dpup");
    apply_sources_to_map(map, "down",     "dpdown");
    apply_sources_to_map(map, "left",     "dpleft");
    apply_sources_to_map(map, "right",    "dpright");
    apply_sources_to_map(map, "cross",    "a");
    apply_sources_to_map(map, "circle",   "b");
    apply_sources_to_map(map, "square",   "x");
    apply_sources_to_map(map, "triangle", "y");
    apply_sources_to_map(map, "l1",       "leftshoulder");
    apply_sources_to_map(map, "r1",       "rightshoulder");
    apply_sources_to_map(map, "l2",       "lefttrigger");
    apply_sources_to_map(map, "r2",       "righttrigger");
    apply_sources_to_map(map, "l3",       "leftstick");
    apply_sources_to_map(map, "r3",       "rightstick");
    apply_sources_to_map(map, "start",    "start");
    apply_sources_to_map(map, "select",   "back");
    apply_sources_to_map(map, "ls_up",    "lefty-");
    apply_sources_to_map(map, "ls_down",  "lefty+");
    apply_sources_to_map(map, "ls_left",  "leftx-");
    apply_sources_to_map(map, "ls_right", "leftx+");
    apply_sources_to_map(map, "rs_up",    "righty-");
    apply_sources_to_map(map, "rs_down",  "righty+");
    apply_sources_to_map(map, "rs_left",  "rightx-");
    apply_sources_to_map(map, "rs_right", "rightx+");
}

static void set_default_controller_mapping(void) {
    set_default_controller_mapping_into(controller_map);
    controller_maps_by_guid.clear();
}

static std::string default_input_ini_text(void) {
    return
        "; PSXRecomp input mapping. PSX buttons are active when any listed source is pressed.\n"
        "; Sources use SDL/Xbox names: a,b,x,y,back,start,leftshoulder,rightshoulder,\n"
        "; lefttrigger[/+],righttrigger[/+],leftstick,rightstick (stick clicks -> L3/R3),\n"
        "; dpup,dpdown,dpleft,dpright,leftx-/leftx+/lefty-/lefty+.\n"
        "; Axis capture may append +/−; both forms are accepted. PSX slots are digital.\n"
        "; Optional per-device overrides: [mapping.<sdl-guid>].\n"
        "\n"
        "[controller]\n"
        "enabled = true\n"
        "device = 0\n"
        "deadzone = 3277\n"
        "\n"
        "[mapping]\n"
        "up = dpup\n"
        "down = dpdown\n"
        "left = dpleft\n"
        "right = dpright\n"
        "cross = a\n"
        "circle = b\n"
        "square = x\n"
        "triangle = y\n"
        "l1 = leftshoulder\n"
        "r1 = rightshoulder\n"
        "l2 = lefttrigger\n"
        "r2 = righttrigger\n"
        "l3 = leftstick\n"
        "r3 = rightstick\n"
        "start = start\n"
        "select = back\n"
        "ls_up = lefty-\n"
        "ls_down = lefty+\n"
        "ls_left = leftx-\n"
        "ls_right = leftx+\n"
        "rs_up = righty-\n"
        "rs_down = righty+\n"
        "rs_left = rightx-\n"
        "rs_right = rightx+\n";
}

static void load_input_config(const char* argv0) {
    set_default_controller_mapping();

    namespace fs = std::filesystem;
    fs::path config_path = exe_dir_from_argv(argv0) / "input.ini";
    std::error_code ec;
    if (!fs::exists(config_path, ec)) {
        std::ofstream out(config_path, std::ios::binary);
        if (out) out << default_input_ini_text();
        psx_keybinds_init(argv0);
        return;
    }

    std::ifstream in(config_path);
    if (!in) {
        psx_keybinds_init(argv0);
        return;
    }

    bool controller_enabled = true;
    std::string section;
    std::string line;
    ControllerMap* guid_target = nullptr;
    while (std::getline(in, line)) {
        size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.resize(comment);
        line = trim_copy(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower_copy(trim_copy(line.substr(1, line.size() - 2)));
            guid_target = nullptr;
            if (section.rfind("mapping.", 0) == 0) {
                const std::string guid = section.substr(8);
                if (!guid.empty()) {
                    /* Seed from the current global map (defaults + any
                     * [mapping] keys already parsed), then overlay GUID keys. */
                    if (!controller_maps_by_guid.count(guid))
                        controller_maps_by_guid[guid] = controller_map;
                    guid_target = &controller_maps_by_guid[guid];
                }
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = lower_copy(trim_copy(line.substr(0, eq)));
        std::string value = trim_copy(line.substr(eq + 1));
        if (section == "controller") {
            if (key == "enabled") {
                controller_enabled = parse_bool_value(value, controller_enabled);
            } else if (key == "device") {
                controller_device_index = std::max(0, std::atoi(value.c_str()));
            } else if (key == "deadzone") {
                controller_deadzone = std::max(0, std::min(32767, std::atoi(value.c_str())));
            }
        } else if (section == "mapping") {
            for (auto& entry : controller_map) {
                if (key == entry.ini_name) {
                    std::vector<ControllerSource> parsed = parse_source_list(value);
                    const std::string vlow = lower_copy(trim_copy(value));
                    const bool explicit_none =
                        vlow.empty() || vlow == "none" || vlow == "disabled";
                    /* Keep defaults when the value is garbage (typo) so a bad
                     * line cannot silently unbind a working control. */
                    if (!parsed.empty() || explicit_none)
                        entry.sources = std::move(parsed);
                    break;
                }
            }
        } else if (guid_target) {
            for (auto& entry : *guid_target) {
                if (key == entry.ini_name) {
                    std::vector<ControllerSource> parsed = parse_source_list(value);
                    const std::string vlow = lower_copy(trim_copy(value));
                    const bool explicit_none =
                        vlow.empty() || vlow == "none" || vlow == "disabled";
                    if (!parsed.empty() || explicit_none)
                        entry.sources = std::move(parsed);
                    break;
                }
            }
        }
    }

    if (!controller_enabled) {
        for (auto& entry : controller_map) entry.sources.clear();
        for (auto& kv : controller_maps_by_guid)
            for (auto& entry : kv.second) entry.sources.clear();
    }

    /* Configurable KEYBOARD keybinds (keybinds.ini, next to the exe) — separate
     * from input.ini's gamepad map. Loads the user's map (or writes defaults on
     * first run). The launcher may have just edited+saved this file; we re-read
     * it here so the runtime always reflects the current bindings. */
    psx_keybinds_init(argv0);
}

static void close_player(PlayerInput& p) {
    if (p.handle) {
        if (p.rumble_small || p.rumble_large)
            (void)SDL_GameControllerRumble(p.handle, 0, 0, 0);
        SDL_GameControllerClose(p.handle);
        p.handle = nullptr;
        p.instance = -1;
    }
    p.rumble_small = 0;
    p.rumble_large = 0;
    p.rumble_known = false;
    p.rumble_warned = false;
}

static void close_controller(void) {
    for (int s = 0; s < PSX_MAX_PLAYERS; s++)
        close_player(g_players[s]);
}

static int device_claimed_by_other(int self_slot, SDL_JoystickID inst) {
    for (int o = 0; o < PSX_MAX_PLAYERS; o++) {
        if (o == self_slot) continue;
        if (g_players[o].handle && g_players[o].instance == inst) return 1;
    }
    return 0;
}

/* Open the SDL controller whose GUID matches p.guid. If no exact GUID match
 * exists (e.g. a different physical unit of the same model, or Steam's virtual
 * pad at an unpredictable slot), fall back to the first controller not already
 * claimed by another player. */
static void open_player(PlayerInput& p, int self_slot) {
    if (p.kind != 2 || p.handle) return;

    int chosen = -1, fallback = -1;
    const int joysticks = SDL_NumJoysticks();
    for (int i = 0; i < joysticks; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
        char buf[40] = {0};
        SDL_JoystickGetGUIDString(g, buf, sizeof(buf));
        /* Skip a device already opened by another player. */
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        if (device_claimed_by_other(self_slot, inst)) continue;
        if (p.guid[0] && lower_copy(buf) == lower_copy(p.guid)) { chosen = i; break; }
        if (fallback < 0) fallback = i;
    }
    if (chosen < 0) chosen = fallback;
    if (chosen < 0) return;

    p.handle = SDL_GameControllerOpen(chosen);
    if (p.handle) {
        SDL_Joystick* joy = SDL_GameControllerGetJoystick(p.handle);
        p.instance = joy ? SDL_JoystickInstanceID(joy) : -1;
        /* Persist the GUID of the pad we actually opened so per-device
         * [mapping.<guid>] lookups match the live hardware (including the
         * first-available fallback when the saved GUID is offline). */
        {
            SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(chosen);
            SDL_JoystickGetGUIDString(g, p.guid, (int)sizeof(p.guid));
            for (char* c = p.guid; *c; ++c)
                *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        }
        const char* name = SDL_GameControllerName(p.handle);
        std::fprintf(stdout, "psxrecomp runtime: opened controller for slot: %s\n",
                     name ? name : "(unnamed)");
        p.rumble_known = false;
        p.rumble_warned = false;
    }
}

/* Forward the emulated DualShock's two motors to the SDL controller assigned
 * to the same PSX port. The large motor is variable-strength/low-frequency;
 * the small motor is fixed-strength/high-frequency. Active effects are renewed
 * every VBlank with a short lifetime so a crash or unplug cannot leave a host
 * controller vibrating indefinitely. */
static void update_controller_rumble(void) {
    static const bool trace = [] {
        const char* e = std::getenv("PSX_RUMBLE_TRACE");
        return e && e[0] && e[0] != '0';
    }();
    for (int s = 0; s < PSX_MAX_PLAYERS; s++) {
        PlayerInput& p = g_players[s];
        uint8_t small = 0, large = 0;
        sio_get_pad_rumble(s, &small, &large);
        if (!p.handle) {
            p.rumble_small = small;
            p.rumble_large = large;
            p.rumble_known = true;
            continue;
        }

        const bool changed = !p.rumble_known || p.rumble_small != small ||
                             p.rumble_large != large;
        if (small || large || (changed && p.rumble_known)) {
            const Uint16 low_frequency = (Uint16)((unsigned)large * 257u);
            const Uint16 high_frequency = small ? 0xFFFFu : 0u;
#if defined(PSX_SDL3)
            const int rc = SDL_GameControllerRumble(
                p.handle, low_frequency, high_frequency,
                (small || large) ? 100u : 0u) ? 0 : -1;
#else
            const int rc = SDL_GameControllerRumble(
                p.handle, low_frequency, high_frequency,
                (small || large) ? 100u : 0u);
#endif
            if (rc != 0 && !p.rumble_warned) {
                std::fprintf(stderr,
                    "psxrecomp runtime: controller for slot %d rejected rumble: %s\n",
                    s + 1, SDL_GetError());
                p.rumble_warned = true;
            }
        }
        if (trace && changed) {
            std::fprintf(stdout,
                "psxrecomp rumble: slot=%d small=%u large=%u\n",
                s + 1, (unsigned)small, (unsigned)large);
        }
        p.rumble_small = small;
        p.rumble_large = large;
        p.rumble_known = true;
    }
}

/* The pad type a mode reports before any input has been sampled (boot /
 * hotplug). analog pins DualShock; digital and hybrid both start as a digital
 * pad (hybrid only flips to DualShock once the player nudges the stick). */
static int pad_mode_boot_analog(int mode) {
    /* Hybrid boots ANALOG-on (matches a DualShock powered up with the analog LED
     * lit): the seamless auto-switch then drops to digital only if the player
     * reaches for the d-pad. Pinned-analog also boots analog; digital boots off. */
    return (mode == PSXRecompV4::PAD_MODE_ANALOG ||
            mode == PSXRecompV4::PAD_MODE_HYBRID) ? 1 : 0;
}

/* Keyboard has no DualShock sticks/config handshake — always present as a
 * plain digital pad (SCPH-1080). Leaving keyboard slots in ANALOG/HYBRID made
 * P2–P5 show as connected while games that expect digital multitap pads never
 * saw usable button input. */
static int effective_player_mode(const PlayerInput& p) {
    if (p.kind == 1) return (int)PSXRecompV4::PAD_MODE_DIGITAL;
    return p.mode;
}

/* Pad mode for the SIO seat this sample will drive. Multitap taps stay digital
 * unless the DualShock-on-tap hack (sio_get_multitap_analog) is armed. */
static int effective_player_mode_for_sio(const PlayerInput& p, int sio_slot) {
    if (sio_pad_on_multitap(sio_slot) && !sio_get_multitap_analog())
        return (int)PSXRecompV4::PAD_MODE_DIGITAL;
    return effective_player_mode(p);
}

/* Open/close SDL handles so they match g_players, and (re)assert each slot's
 * PSX connection + pad type. Safe to call repeatedly (hotplug, boot).
 * While delay-sync netplay is active, SIO connection/type are owned by
 * psx_netplay (session slots stay plugged); only refresh host SDL handles. */
static void refresh_player_devices(void) {
    const int netplay = psx_netplay_active();
    for (int s = 0; s < PSX_MAX_PLAYERS; s++) {
        PlayerInput& p = g_players[s];
        if (p.kind != 2) close_player(p);           /* keyboard/none: no handle */
        else open_player(p, s);
        if (netplay) continue;
        const int mode = effective_player_mode_for_sio(p, s);
        sio_set_pad_connected(s, p.kind != 0 ? 1 : 0);
        sio_set_pad_analog(s, pad_mode_boot_analog(mode), 0x80, 0x80, 0x80, 0x80);
        /* DIGITAL mode == a plain digital controller that ignores the DualShock
         * config-mode commands (real SCPH-1080 behaviour); ANALOG/HYBRID == a
         * config-capable DualShock. A digital pad that wrongly answered 0x43
         * sent Tomba 2's pad driver down the config path -> phantom 0x00 reads.
         * Multitap taps are always digital (see sio_pad_on_multitap). */
        sio_set_pad_config_capable(s, mode != PSXRecompV4::PAD_MODE_DIGITAL);
    }
}

/* Parse a [controller] device string into a player slot:
 *   "none" -> no pad; "keyboard" -> keyboard map; otherwise an SDL GUID. */
static void set_player_device(PlayerInput& p, const std::string& dev, int mode) {
    p.mode = mode;
    /* Hybrid starts in ANALOG (analog LED on); the auto-switch drops to digital
     * only when the player uses the d-pad. */
    p.hybrid_analog = true;
    p.guid[0] = '\0';
    std::string d = lower_copy(trim_copy(dev));
    if (d.empty() || d == "none") { p.kind = 0; }
    else if (d == "keyboard")     { p.kind = 1; }
    else if (d == "auto" || d == "gamepad" || d == "controller") {
        /* First available SDL game controller (guid empty -> open_player falls
         * back to the first connected pad). Lets a user default to "my
         * controller" without pinning a specific GUID. */
        p.kind = 2;  /* p.guid already cleared above */
    }
    else {
        p.kind = 2;
        std::snprintf(p.guid, sizeof(p.guid), "%s", trim_copy(dev).c_str());
    }
}

/* Stick→button (Digital D-Pad fold): per-axis deadzone. A strong left/right
 * push with a few units of Y drift must NOT fire Up/Down (jump/crouch). Pure
 * radial+sign did that — once mag cleared the circle, every non-zero axis
 * component became a D-Pad bit. Analog stick bytes still use radial rescale
 * in axes_to_pad_pair; this path is digital buttons only. Triggers share the
 * same per-axis threshold. */
static bool controller_source_pressed_h(SDL_GameController* h, const ControllerSource& source,
                                        int deadzone_raw) {
    if (!h) return false;
    const int dz = deadzone_raw > 0 ? deadzone_raw : controller_deadzone;

    switch (source.kind) {
    case ControllerSource::Kind::Button:
        return SDL_GameControllerGetButton(
            h, (SDL_GameControllerButton)source.id) != 0;
    case ControllerSource::Kind::AxisPositive:
    case ControllerSource::Kind::AxisNegative: {
        const int16_t v = SDL_GameControllerGetAxis(h, (SDL_GameControllerAxis)source.id);
        if (source.kind == ControllerSource::Kind::AxisPositive)
            return v > dz;
        return v < -dz;
    }
    case ControllerSource::Kind::None:
    default:
        return false;
    }
}

/* Keyboard -> PSX button word for `player` (1 or 2), via the configurable
 * keybinds.ini map (psx_keybinds). Defaults reproduce the old hardcoded layout
 * (arrows=d-pad, X/S/Z/A=Cross/Circle/Square/Triangle, Q/W/E/R=L1/R1/L2/R2,
 * Return=Start, RShift=Select) plus T/Y=L3/R3 stick clicks. */
static uint16_t pad_from_keyboard(int player) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    return psx_keybinds_pad_word(keys, player);
}

/* A left/right ANALOG-STICK axis source (LEFTX/LEFTY/RIGHTX/RIGHTY), as opposed
 * to a trigger axis (L2/R2) or a button. The default map folds the left stick
 * onto the D-pad bits (up=dpup,lefty- … right=dpright,leftx+) so the stick works
 * as a D-pad for DIGITAL games. In ANALOG mode that fold is WRONG: the stick
 * already drives the analog axes, and on a real DualShock the stick and D-pad
 * are independent — so a dual-analog game that uses the D-pad as its own control
 * (e.g. Ape Escape's camera rotate) would see phantom D-pad presses from every
 * stick movement, and constant rotation from centre drift. controller_pad_buttons
 * suppresses these sources when the pad presents as analog. Trigger axes and
 * button sources are never suppressed. */
static bool source_is_stick_axis(const ControllerSource& s) {
    if (s.kind != ControllerSource::Kind::AxisPositive &&
        s.kind != ControllerSource::Kind::AxisNegative) return false;
    return s.id == SDL_CONTROLLER_AXIS_LEFTX  || s.id == SDL_CONTROLLER_AXIS_LEFTY ||
           s.id == SDL_CONTROLLER_AXIS_RIGHTX || s.id == SDL_CONTROLLER_AXIS_RIGHTY;
}

/* Build the active-low PSX button word from a controller's input.ini map. When
 * suppress_stick_axes is set (the pad is presenting as analog this frame), the
 * left/right analog-stick axes do NOT contribute button bits — see
 * source_is_stick_axis above. Digital mode passes false, so ls_* stick
 * directions still fold onto the D-pad via fold_bit. */
static uint16_t controller_pad_buttons(const ControllerMap& map,
                                       SDL_GameController* h,
                                       bool suppress_stick_axes,
                                       int deadzone_raw) {
    uint16_t buttons = 0xFFFF;  /* all released */
    if (!h) return buttons;
    for (const auto& entry : map) {
        for (const auto& source : entry.sources) {
            if (suppress_stick_axes && source_is_stick_axis(source)) continue;
            if (!controller_source_pressed_h(h, source, deadzone_raw)) continue;
            if (entry.bit)
                buttons &= (uint16_t)~entry.bit;
            if (entry.fold_bit && !suppress_stick_axes)
                buttons &= (uint16_t)~entry.fold_bit;
            break;
        }
    }
    return buttons;
}

/* Radial deadzone: process a stick's X and Y together so the dead region is a
 * small CIRCLE, not a per-axis square. Travel within controller_deadzone reads
 * centred (0x80/0x80); beyond it the vector MAGNITUDE is rescaled to the full
 * range along the stick's true direction, so diagonals are preserved (a
 * per-axis deadzone snaps diagonals toward the cardinals and feels notchy —
 * bad for directional analog like Ape Escape's net swing). The magnitude is
 * capped at 32767 before rescale so a full-diagonal push (raw mag ~46341) maps
 * to ~0x9E/0x9E per axis — the circular gate a real DualShock stick reports,
 * not 0xFF/0xFF. At dz==0 it reduces to a plain magnitude-preserving map. */
/* Radial deadzone + anti-deadzone in ONE place: psx_stick_to_dualshock
 * (runtime/src/psx_stick.c), the shared implementation master calls. The
 * per-player deadzone is PR #110's; anti_deadzone is master's [controller]
 * setting, which an inlined copy of the maths here silently dropped. */
static void axes_to_pad_pair(int16_t vx, int16_t vy, uint8_t* obx, uint8_t* oby,
                             int deadzone_raw) {
    psx_stick_to_dualshock(vx, vy,
                           deadzone_raw > 0 ? deadzone_raw : controller_deadzone,
                           controller_anti_deadzone, obx, oby);
}

/* Buttons for a player's selected device (0xFFFF = none pressed). `player` is
 * 1..5 — selects which keybinds.ini section drives a keyboard port. */
static uint16_t pad_buttons_for(const PlayerInput& p, int player, bool suppress_stick_axes) {
    if (p.kind == 1) return pad_from_keyboard(player);
    if (p.kind == 2)
        return controller_pad_buttons(controller_map_for(p), p.handle,
                                      suppress_stick_axes, p.deadzone);
    return 0xFFFF;
}

/* Analog stick bytes (lx,ly,rx,ry) for a player; centred if no live source.
 *
 * The host left stick maps to the LEFT analog axes (variable). When fold_dpad
 * is set, the physical D-pad (and, for a keyboard player, the arrow keys) is
 * ALSO folded onto the left axes at full deflection, so an analog-mode game
 * whose movement reads only the stick magnitude (e.g. Tomba) still responds to
 * the D-pad/keyboard — the faithful "seamless" behaviour: the stick gives
 * variable speed, the D-pad gives full speed, both at once. Only the PHYSICAL
 * D-pad is folded in (not the stick->d-pad button mapping), so the stick keeps
 * its true variable magnitude. fold_dpad is false in HYBRID mode (there the
 * D-pad instead flips the pad to digital, so the game runs its own d-pad path)
 * and true in pinned-ANALOG mode. The keyboard branch always folds the arrows
 * — they are that player's only stick source. */
static void pad_sticks_for(const PlayerInput& p, int player, uint8_t out[4], bool fold_dpad) {
    out[0] = out[1] = out[2] = out[3] = 0x80;
    if (p.kind == 1) {
        /* Keyboard analog: the configurable left/right stick-direction binds
         * (default = arrow keys on the LEFT stick; RIGHT stick unbound), so the
         * old keyboard analog behaviour is preserved unless the user rebinds. */
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        psx_keybinds_sticks(keys, player, out);
        return;
    }
    if (p.kind == 2 && p.handle) {
        const ControllerMap& map = controller_map_for(p);
        auto find_entry = [&](const char* name) -> const PsxButtonMap* {
            for (const auto& e : map)
                if (std::strcmp(e.ini_name, name) == 0) return &e;
            return nullptr;
        };
        /* Default left/right stick axis layout → smooth radial deadzone path.
         * Any remapped stick direction falls back to discrete full deflection. */
        auto sticks_default_axes = [&](const char* up, const char* down,
                                       const char* left, const char* right,
                                       SDL_GameControllerAxis ax,
                                       SDL_GameControllerAxis ay) -> bool {
            auto is_axis = [&](const char* n, int axis_id, ControllerSource::Kind k) {
                const PsxButtonMap* e = find_entry(n);
                if (!e || e->sources.size() != 1) return false;
                const auto& s = e->sources[0];
                return s.kind == k && s.id == axis_id;
            };
            return is_axis(up, ay, ControllerSource::Kind::AxisNegative) &&
                   is_axis(down, ay, ControllerSource::Kind::AxisPositive) &&
                   is_axis(left, ax, ControllerSource::Kind::AxisNegative) &&
                   is_axis(right, ax, ControllerSource::Kind::AxisPositive);
        };
        auto apply_discrete_stick = [&](const char* up, const char* down,
                                        const char* left, const char* right,
                                        uint8_t* bx, uint8_t* by) {
            *bx = *by = 0x80;
            auto pressed = [&](const char* n) {
                const PsxButtonMap* e = find_entry(n);
                if (!e) return false;
                for (const auto& s : e->sources)
                    if (controller_source_pressed_h(p.handle, s, p.deadzone))
                        return true;
                return false;
            };
            if (pressed(left))  *bx = 0x00;
            if (pressed(right)) *bx = 0xFF;
            if (pressed(up))    *by = 0x00;
            if (pressed(down))  *by = 0xFF;
        };

        if (sticks_default_axes("ls_up", "ls_down", "ls_left", "ls_right",
                                SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY)) {
            axes_to_pad_pair(SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTX),
                             SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTY),
                             &out[0], &out[1], p.deadzone);
        } else {
            apply_discrete_stick("ls_up", "ls_down", "ls_left", "ls_right",
                                 &out[0], &out[1]);
        }
        if (sticks_default_axes("rs_up", "rs_down", "rs_left", "rs_right",
                                SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY)) {
            axes_to_pad_pair(SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_RIGHTX),
                             SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_RIGHTY),
                             &out[2], &out[3], p.deadzone);
        } else {
            apply_discrete_stick("rs_up", "rs_down", "rs_left", "rs_right",
                                 &out[2], &out[3]);
        }
        if (fold_dpad) {
            /* Fold physical D-pad (mapped "up"/etc. button sources that are
             * d-pad buttons) — prefer the mapped sources over hardcoded buttons
             * so a remapped d-pad still folds onto the left stick. */
            auto dpad_pressed = [&](const char* n) {
                const PsxButtonMap* e = find_entry(n);
                if (!e) return false;
                for (const auto& s : e->sources) {
                    if (source_is_stick_axis(s)) continue;
                    if (controller_source_pressed_h(p.handle, s, p.deadzone))
                        return true;
                }
                return false;
            };
            if (dpad_pressed("left"))  out[0] = 0x00;
            if (dpad_pressed("right")) out[0] = 0xFF;
            if (dpad_pressed("up"))    out[1] = 0x00;
            if (dpad_pressed("down"))  out[1] = 0xFF;
        }
    }
}

/* HYBRID-mode auto-switch detectors (mirror the DualShock analog LED toggling).
 * hybrid_stick_active: the LEFT stick is outside the radial deadzone — the
 * player has reached for analog. hybrid_dpad_active: any D-pad direction (or,
 * for the keyboard, an arrow key) is held — the player wants classic digital.
 * The keyboard has no analog stick, so a keyboard player stays digital. */
static bool controller_stick_active(SDL_GameController* handle, int deadzone) {
    if (!handle) return false;
    const double lx =
        SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_LEFTX);
    const double ly =
        SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_LEFTY);
    const double dz = (double)(deadzone > 0 ? deadzone : controller_deadzone);
    return std::sqrt(lx * lx + ly * ly) > dz;
}
static bool controller_dpad_active(SDL_GameController* handle) {
    return handle &&
        (SDL_GameControllerGetButton(handle, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
         SDL_GameControllerGetButton(handle, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
         SDL_GameControllerGetButton(handle, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
         SDL_GameControllerGetButton(handle, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
}
/* Which input sources may drive ONE pad slot this frame.
 *
 * This exists because the answer is consumed in three places — the button
 * merge, the HYBRID analog/digital auto-switch, and the analog stick fold —
 * and those three used to compute it independently. Whenever they disagreed,
 * a source could assert a button without the hybrid state machine seeing it,
 * leaving the pad reporting D-pad presses while still presenting ANALOG: the
 * exact failure the hybrid logic exists to prevent, and silent when it
 * happens. Deriving all three from one predicate makes that desync
 * structurally impossible rather than merely fixed once.
 *
 * Changing input-routing POLICY is therefore a change to this function alone
 * (e.g. whether keybinds stay live alongside a routed gamepad), not a change
 * to three separate conditions that must be kept in step by hand. */
struct PadSources {
    bool device;    /* the slot's own assigned device (keyboard or controller) */
    bool keybinds;  /* keybinds.ini keyboard/mouse binds for this player       */
    bool all_pads;  /* every connected controller (dev-any-input)              */
};

static PadSources pad_sources_for(const PlayerInput& p, bool dev_here) {
    PadSources s;
    s.device   = (p.kind != 0);
    /* Keybinds are ALWAYS live, including alongside a routed gamepad: that is
     * the whole point of binding a mouse button for aiming while holding a
     * pad. Routing a player to a controller used to discard every
     * keybinds.ini/mouse bind silently.
     *
     * Widening this ONE line is safe precisely because every consumer reads
     * it — the button merge, the HYBRID auto-switch detectors and the stick
     * fold all learn about the keyboard in the same instant. Widening the
     * button merge alone (the original shape of this change) let a keyboard
     * D-pad press assert the D-pad bits while hybrid_dpad_active never saw
     * them, leaving a mod-driven hybrid pad reporting D-pad input while still
     * presenting ANALOG.
     *
     * The PSX pad word is active-low and the merge is an AND, so an unpressed
     * source is a no-op: a pad-only player is unaffected. kind==1 already
     * consumes the binds through pad_buttons_for/pad_sticks_for, and applying
     * them twice is idempotent. */
    (void)dev_here;
    s.keybinds = true;
    s.all_pads = dev_here;
    return s;
}

static bool hybrid_stick_active(const PlayerInput& p, const PadSources& src) {
    if (src.device && p.kind == 2 &&
        controller_stick_active(p.handle, p.deadzone)) return true;
    if (src.all_pads) {
        const int n = SDL_NumJoysticks();
        for (int i = 0; i < n; i++) {
            if (!SDL_IsGameController(i)) continue;
            const SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
            SDL_GameController* handle =
                SDL_GameControllerFromInstanceID(inst);
            if (!handle) handle = SDL_GameControllerOpen(i);
            if (controller_stick_active(handle, controller_deadzone)) return true;
        }
    }
    return false;
}
static bool hybrid_dpad_active(const PlayerInput& p, int player,
                               const PadSources& src) {
    if (src.device && p.kind == 2 && controller_dpad_active(p.handle)) return true;
    if (src.all_pads) {
        const int n = SDL_NumJoysticks();
        for (int i = 0; i < n; i++) {
            if (!SDL_IsGameController(i)) continue;
            const SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
            SDL_GameController* handle =
                SDL_GameControllerFromInstanceID(inst);
            if (!handle) handle = SDL_GameControllerOpen(i);
            if (controller_dpad_active(handle)) return true;
        }
    }
    if (src.keybinds) {
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        if (psx_keybinds_dpad_active(keys, player)) return true;
    }
    return false;
}

/* Sample each player's live device state into the matching SIO pad slot.
 * override >= 0 forces port-1 buttons (debug-server input injection). Called
 * once at cycle start (covers the turbo/FMV-skip paths that present nothing)
 * and, when g_low_latency_input is set, AGAIN after the pacer wait so the next
 * CPU frame reads near-fresh input instead of input ~one frame stale.
 *
 * HYBRID mode auto-switches DualShock<->digital from the most-recent input
 * (stick -> analog, D-pad -> digital) so the game runs its own analog or
 * digital input path exactly as on hardware (mirrors the inline block this
 * helper was extracted from for the low-latency re-sample). */
/* Dev input mode: while enabled, Player 1 is driven by the keyboard AND EVERY
 * connected game controller, all merged together (active-low AND) onto the P1 pad
 * word — so a tester can navigate P1 from whatever is plugged in (keyboard, the
 * launcher-assigned pad, or any other controller) with no reconfiguration. The P1
 * pad TYPE is left unchanged: a launcher-assigned analog DualShock still presents
 * as analog (so the game's analog input path / SIO handshake cadence is preserved
 * exactly), and merged sources only contribute button/stick STATE, never a type
 * downgrade. This diagnostic convenience must never alter normal player input:
 * strict single-device-per-port routing is the default, and PSX_DEV_INPUT must
 * be explicitly set to 1/true/yes/on to enable the merge. */
static bool dev_any_input_enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* e = std::getenv("PSX_DEV_INPUT");
        const std::string value = lower_copy(trim_copy(e ? e : ""));
        cached = (value == "1" || value == "true" ||
                  value == "yes" || value == "on") ? 1 : 0;
    }
    return cached != 0;
}

/* Merge (active-low AND) the button words of every connected game controller.
 * Handles are resolved from SDL's already-open set when possible and opened once
 * on first sight otherwise (kept open for the process lifetime; if a slot later
 * closes a shared device this self-heals by reopening next frame). This does NOT
 * disturb per-slot routing — SDL returns the same handle for an already-open
 * device, so reads are shared and harmless. */
static uint16_t dev_all_controllers_buttons(bool suppress_stick_axes) {
    uint16_t btn = 0xFFFF;
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        SDL_GameController* h = SDL_GameControllerFromInstanceID(inst);
        if (!h) h = SDL_GameControllerOpen(i);   /* open once; SDL keeps it */
        if (!h) continue;
        PlayerInput tmp;
        tmp.kind = 2;
        {
            SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
            SDL_JoystickGetGUIDString(g, tmp.guid, (int)sizeof(tmp.guid));
        }
        btn &= controller_pad_buttons(controller_map_for(tmp), h,
                                      suppress_stick_axes, controller_deadzone);
    }
    return btn;
}

/* Fold the first live (non-neutral) left/right stick from ANY connected
 * controller into st[] (lx,ly,rx,ry). Lets an analog-mode P1 steer from
 * whatever pad is plugged in, not only the launcher-assigned one. Only axes
 * that read past the deadzone override; a neutral pad leaves st[] untouched. */
static void dev_any_controller_sticks(uint8_t st[4]) {
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        SDL_GameController* h = SDL_GameControllerFromInstanceID(inst);
        if (!h) h = SDL_GameControllerOpen(i);
        if (!h) continue;
        uint8_t lx, ly, rx, ry;
        axes_to_pad_pair(SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_LEFTX),
                         SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_LEFTY),
                         &lx, &ly, controller_deadzone);
        axes_to_pad_pair(SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_RIGHTX),
                         SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_RIGHTY),
                         &rx, &ry, controller_deadzone);
        if (lx != 0x80 || ly != 0x80) { st[0] = lx; st[1] = ly; }
        if (rx != 0x80 || ry != 0x80) { st[2] = rx; st[3] = ry; }
    }
}

static void savestate_input_guard_arm(void) {
    uint32_t now = (uint32_t)SDL_GetTicks();
    g_savestate_input_guard_min_until = now + 90u;
    g_savestate_input_guard_max_until = now + 700u;
}

static int any_controller_button_down(void) {
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        SDL_GameController* h = SDL_GameControllerFromInstanceID(inst);
        if (!h) h = SDL_GameControllerOpen(i);
        if (!h) continue;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            if (SDL_GameControllerGetButton(h, (SDL_GameControllerButton)b))
                return 1;
        }
    }
    return 0;
}

static int savestate_resume_inputs_held(void) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    if (keys) {
        if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER] ||
            keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_L])
            return 1;
        if (pad_from_keyboard(1) != 0xFFFFu)
            return 1;
    }
    return any_controller_button_down();
}

static int savestate_input_guard_active(void) {
    uint32_t now;
    if (g_savestate_input_guard_max_until == 0)
        return 0;
    now = (uint32_t)SDL_GetTicks();
    if ((int32_t)(now - g_savestate_input_guard_max_until) >= 0) {
        g_savestate_input_guard_min_until = 0;
        g_savestate_input_guard_max_until = 0;
        return 0;
    }
    if ((int32_t)(now - g_savestate_input_guard_min_until) >= 0 &&
        !savestate_resume_inputs_held()) {
        g_savestate_input_guard_min_until = 0;
        g_savestate_input_guard_max_until = 0;
        return 0;
    }
    return 1;
}

/* Debug-server input injection: drive the SAME pad model as a physical
 * device. The old path set only the button word and returned, which left the
 * pad type/sticks at whatever the real device last was — a hybrid P1 stayed
 * analog, so injected d-pad bits never moved games that read the stick in
 * analog mode (menus reacted, walking didn't). Injected d-pad reads as d-pad
 * activity (hybrid drops to digital), an injected stick override reads as
 * stick activity (hybrid rises to analog), and the resolved type goes through
 * the same coherent request channel as real sampling — never slammed mid-
 * handshake (the v0.5.0 phantom-input lesson). */
static void apply_input_override_to_sio(int override_word) {
    PlayerInput& p = g_players[0];
    const uint16_t w = (uint16_t)override_word;
    sio_set_pad_state_slot(0, w);

    uint8_t st[4] = { 0x80, 0x80, 0x80, 0x80 };
    int axes = 0;
#ifndef PSX_NO_DEBUG_TOOLS
    axes = debug_server_get_axis_override(st);
#endif
    const bool stick_live = axes != 0 && (st[0] != 0x80 || st[1] != 0x80 ||
                                          st[2] != 0x80 || st[3] != 0x80);
    const bool dpad_live  = ((uint16_t)~w & 0x00F0u) != 0;   /* up/right/down/left */

    /* Prefer the configured seat mode (incl. game.toml lock_mode / settings
     * p1_mode). Falling back to DIGITAL when kind==0 made DualShock-locked
     * titles (Ape Escape) ignore debug-server injection in headless runs. */
    int mode;
    if (p.kind != 0)                  mode = effective_player_mode(p);
    else if (dev_any_input_enabled()) mode = (int)PSXRecompV4::PAD_MODE_ANALOG;
    else                              mode = p.mode;

    int eff_analog;
    if (mode == (int)PSXRecompV4::PAD_MODE_DIGITAL) {
        eff_analog = 0;
    } else if (mode == (int)PSXRecompV4::PAD_MODE_ANALOG) {
        eff_analog = 1;
    } else { /* HYBRID: most-recent input source wins, like the real sampler */
        if (stick_live)     p.hybrid_analog = true;
        else if (dpad_live) p.hybrid_analog = false;
        eff_analog = p.hybrid_analog ? 1 : 0;
    }
    /* Pinned-ANALOG folds injected D-pad onto the left stick (same as a real
     * DualShock seat). Without this, stick-only menu/move paths ignore
     * button-bit injection even though pad_status shows the bits pressed. */
    if (eff_analog && mode == (int)PSXRecompV4::PAD_MODE_ANALOG && !stick_live) {
        if ((uint16_t)(~w & 0x0010u)) st[1] = 0x00; /* Up */
        if ((uint16_t)(~w & 0x0040u)) st[1] = 0xFF; /* Down */
        if ((uint16_t)(~w & 0x0080u)) st[0] = 0x00; /* Left */
        if ((uint16_t)(~w & 0x0020u)) st[0] = 0xFF; /* Right */
    }
    if (!eff_analog) { st[0] = st[1] = st[2] = st[3] = 0x80; }
    sio_set_pad_sticks(0, st[0], st[1], st[2], st[3]);
    sio_request_pad_type(0, eff_analog);
    psx_selfcheck_note_pad(0, w, st[0], st[1], st[2], st[3],
                           (uint8_t)(eff_analog ? 1 : 0));
}

/* Capture one SIO slot's host pad into a netplay/local blob. Returns 1 if a
 * device (or dev-any P1) is present; 0 leaves *out as released/disconnected. */
static int capture_pad_slot(int s, PsxNetPad* out) {
    if (!out) return 0;
    out->buttons = 0xFFFFu;
    out->lx = out->ly = out->rx = out->ry = 0x80u;
    out->analog = 0;
    out->connected = 0;

    PlayerInput& p = g_players[s];
    const int  player  = s + 1;             /* keybinds.ini section (1..5) */
    /* Opt-in dev merge: P1 is driven by the keyboard AND every connected
     * controller (PSX_DEV_INPUT=1). Default is strict per-slot routing. */
    const bool dev_here = (dev_any_input_enabled() && s == 0);
    if (p.kind == 0 && !dev_here) return 0;  /* no device in this port */

    /* Resolve the pad type this frame FIRST — the effective analog/digital
     * state gates how the left stick is read for BOTH the button word and the
     * analog axes below. An assigned device keeps its configured mode (a
     * launcher-selected analog DualShock stays analog, so its input path / SIO
     * handshake cadence is preserved exactly). A P1 with no assigned device
     * keeps the game's resolved mode while dev-any-input merges the keyboard and
     * all connected controllers. */
    /* One source set, consumed by the hybrid switch, the button merge and the
     * stick fold below — see pad_sources_for(). */
    const PadSources src = pad_sources_for(p, dev_here);

    const int mode = effective_player_mode_for_sio(p, s);
    int eff_analog;
    if (mode == PSXRecompV4::PAD_MODE_DIGITAL) {
        eff_analog = 0;
    } else if (mode == PSXRecompV4::PAD_MODE_ANALOG) {
        eff_analog = 1;
    } else { /* HYBRID */
        if (hybrid_stick_active(p, src))             p.hybrid_analog = true;
        else if (hybrid_dpad_active(p, player, src)) p.hybrid_analog = false;
        eff_analog = p.hybrid_analog ? 1 : 0;
    }
    if (savestate_input_guard_active()) {
        out->buttons = 0xFFFFu;
        out->lx = out->ly = out->rx = out->ry = 0x80u;
        out->analog = eff_analog ? 1u : 0u;
        out->connected = 1;
        return 1;
    }

    /* Buttons: merge the assigned device with the keyboard (PSX pad word is
     * active-low, so AND combines "pressed on either source"). In dev mode P1
     * also folds in the keyboard binds and EVERY connected controller. When the
     * pad presents as ANALOG this frame (eff_analog), the left/right analog-
     * stick axes are suppressed as button sources so the stick drives ONLY the
     * analog axes — the D-pad bits then come solely from the physical D-pad,
     * exactly as on a real DualShock. This is what stops a dual-analog game's
     * D-pad control (Ape Escape's camera rotate) from being spun by stick
     * movement or centre drift. Digital mode keeps the stick->D-pad fold. */
    const bool suppress_stick = (eff_analog != 0);
    uint16_t btn = src.device ? pad_buttons_for(p, player, suppress_stick)
                              : (uint16_t)0xFFFF;
    /* kind==1 already consumed the binds inside pad_buttons_for — ANDing the
     * same word twice is idempotent, so this stays a plain source check. */
    if (src.keybinds)
        btn &= pad_from_keyboard(player);
    if (src.all_pads)
        btn &= dev_all_controllers_buttons(suppress_stick);

    /* Analog axes. Pinned-ANALOG folds the physical D-pad onto the left axes
     * (fold_dpad) so the D-pad still moves stick-only games; HYBRID feeds the
     * raw stick when currently analog (no fold — the D-pad drives its own
     * digital path there); DIGITAL leaves the axes centred. */
    uint8_t st[4] = { 0x80, 0x80, 0x80, 0x80 };
    if (mode == PSXRecompV4::PAD_MODE_ANALOG) {
        pad_sticks_for(p, player, st, /*fold_dpad=*/true);
    } else if (eff_analog) {  /* HYBRID, currently presenting analog */
        pad_sticks_for(p, player, st, /*fold_dpad=*/false);
    }
    /* Stick fold, driven by the SAME source set as the buttons above: whatever
     * may press a button may also steer. kind==1 already folded its binds
     * inside pad_sticks_for; psx_keybinds_sticks only widens a deflection, so
     * applying it twice is idempotent. */
    if (eff_analog) {
        if (src.keybinds) {
            const Uint8* keys = SDL_GetKeyboardState(NULL);
            psx_keybinds_sticks(keys, player, st);
        }
        if (src.all_pads)
            dev_any_controller_sticks(st);
    }

    out->buttons = btn;
    out->lx = st[0]; out->ly = st[1]; out->rx = st[2]; out->ry = st[3];
    out->analog = eff_analog ? 1u : 0u;
    out->connected = 1;
    return 1;
}

/* Netplay-only capture: assigned PlayerInput for this slot only. Never merges
 * keyboard-all / all-controllers (dev_any_input). Same pad-mode / stick rules
 * as capture_pad_slot otherwise. present_sio_slot is the delay-sync seat this
 * blob will publish to (may differ from host card `s` on guests). */
static int capture_pad_slot_exclusive(int s, PsxNetPad* out, int present_sio_slot) {
    if (!out) return 0;
    out->buttons = 0xFFFFu;
    out->lx = out->ly = out->rx = out->ry = 0x80u;
    out->analog = 0;
    out->connected = 0;

    PlayerInput& p = g_players[s];
    const int  player  = s + 1;             /* keybinds.ini section (1..5) */
    const bool dev_here = false;
    if (p.kind == 0) return 0;  /* no device in this port */

    /* Same predicate as capture_pad_slot, with dev-any-input disabled:
     * netplay must stay exclusive so peers hash-agree. */
    const PadSources src = pad_sources_for(p, dev_here);
    const int sio_slot = (present_sio_slot >= 0) ? present_sio_slot : s;
    int mode = effective_player_mode_for_sio(p, sio_slot);
    int eff_analog;
    if (mode == PSXRecompV4::PAD_MODE_DIGITAL) {
        eff_analog = 0;
    } else if (mode == PSXRecompV4::PAD_MODE_ANALOG) {
        eff_analog = 1;
    } else { /* HYBRID */
        if (hybrid_stick_active(p, src))             p.hybrid_analog = true;
        else if (hybrid_dpad_active(p, player, src)) p.hybrid_analog = false;
        eff_analog = p.hybrid_analog ? 1 : 0;
    }

    const bool suppress_stick = (eff_analog != 0);
    uint16_t btn = pad_buttons_for(p, player, suppress_stick);

    uint8_t st[4] = { 0x80, 0x80, 0x80, 0x80 };
    if (mode == PSXRecompV4::PAD_MODE_ANALOG) {
        pad_sticks_for(p, player, st, /*fold_dpad=*/true);
    } else if (eff_analog) {  /* HYBRID, currently presenting analog */
        pad_sticks_for(p, player, st, /*fold_dpad=*/false);
    }

    out->buttons = btn;
    out->lx = st[0]; out->ly = st[1]; out->rx = st[2]; out->ry = st[3];
    out->analog = eff_analog ? 1u : 0u;
    out->connected = 1;
    return 1;
}

static void apply_pad_slot_to_sio(int s, const PsxNetPad& pad) {
    if (sio_pad_on_multitap(s) && !sio_get_multitap_analog())
        sio_set_pad_config_capable(s, 0);
    sio_set_pad_state_slot(s, pad.buttons);
    sio_set_pad_sticks(s, pad.lx, pad.ly, pad.rx, pad.ry);
    sio_request_pad_type(s, pad.analog ? 1 : 0);
    /* Solo resim self-check records exactly what was applied this boundary. */
    psx_selfcheck_note_pad(s, pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry,
                           pad.analog);
}

/* Raw SDL Start face-button (ignores remaps) — pad-trace only. */
static int netplay_sdl_start_held(int card) {
    if (card < 0 || card >= PSX_MAX_PLAYERS) return 0;
    const PlayerInput& p = g_players[card];
    if (p.kind == 2 && p.handle)
        return SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_START) != 0;
    if (p.kind == 1) {
        /* Keyboard: Start bit in keybinds pad word (active-low clear = pressed). */
        return (pad_from_keyboard(card + 1) & 0x0008u) == 0;
    }
    return 0;
}

/* Local human pad for delay-sync: sample the host PlayerInput selected for
 * this peer (see --net-input-player / auto), then recomp-net maps that blob
 * onto local_slot (lobby seat → sim P1/P2/…). Never writes SIO. Exclusive
 * capture — no keyboard-all / all-controllers merge — so peers hash-agree. */
static void capture_local_human_pad(PsxNetPad* out) {
    int idx = psx_netplay_input_player();
    if (idx < 0 || idx >= PSX_MAX_PLAYERS) idx = 0;
    /* Present as the lobby seat (multitap taps → digital), not the host card. */
    const int seat = psx_netplay_local_slot();
    const int present = (seat >= 0) ? seat : idx;
    int card = idx;
    int fallback = 0;
    auto bisect_cap = [&](int c, int fb) {
        if (!psx_start_bisect_enabled()) return;
        const int sdl = netplay_sdl_start_held(c);
        const int cap = ((uint16_t)(~out->buttons) & 0x0008u) != 0;
        psx_start_bisect_log("cap", psx_netplay_sim_tick(), sdl, cap, -1, -1,
                             0, psx_netplay_rb_tip_holding(),
                             psx_netplay_is_resimulating());
        (void)fb;
    };
    if (capture_pad_slot_exclusive(idx, out, present)) {
        out->connected = 1;
        psx_netplay_normalize_pad(out);
        psx_netplay_pad_trace_dev(card, fallback, netplay_sdl_start_held(card),
                                  out->buttons);
        bisect_cap(card, fallback);
        return;
    }
    /* Fallbacks: NETPLAY/P1 card, lobby seat card, then any assigned device. */
    if (idx != 0 && capture_pad_slot_exclusive(0, out, present)) {
        out->connected = 1;
        psx_netplay_normalize_pad(out);
        card = 0;
        fallback = 1;
        psx_netplay_pad_trace_dev(card, fallback, netplay_sdl_start_held(card),
                                  out->buttons);
        bisect_cap(card, fallback);
        return;
    }
    if (seat >= 0 && seat < PSX_MAX_PLAYERS && seat != idx && seat != 0 &&
        capture_pad_slot_exclusive(seat, out, present)) {
        out->connected = 1;
        psx_netplay_normalize_pad(out);
        card = seat;
        fallback = 1;
        psx_netplay_pad_trace_dev(card, fallback, netplay_sdl_start_held(card),
                                  out->buttons);
        bisect_cap(card, fallback);
        return;
    }
    for (int s = 0; s < PSX_MAX_PLAYERS; ++s) {
        if (s == idx || s == 0 || s == seat) continue;
        if (capture_pad_slot_exclusive(s, out, present)) {
            out->connected = 1;
            psx_netplay_normalize_pad(out);
            card = s;
            fallback = 1;
            psx_netplay_pad_trace_dev(card, fallback, netplay_sdl_start_held(card),
                                      out->buttons);
            bisect_cap(card, fallback);
            return;
        }
    }
    out->buttons = 0xFFFFu;
    out->lx = out->ly = out->rx = out->ry = 0x80u;
    out->analog = 1;
    out->connected = 1;
    psx_netplay_normalize_pad(out);
    psx_netplay_pad_trace_dev(idx, 1, netplay_sdl_start_held(idx), out->buttons);
    bisect_cap(idx, 1);
}

/* Build a netplay pad blob from a debug-server override without writing SIO. */
static void capture_override_pad(int override_word, PsxNetPad* out) {
    PlayerInput& p = g_players[0];
    const uint16_t w = (uint16_t)override_word;
    uint8_t st[4] = { 0x80, 0x80, 0x80, 0x80 };
    int axes = 0;
#ifndef PSX_NO_DEBUG_TOOLS
    axes = debug_server_get_axis_override(st);
#endif
    const bool stick_live = axes != 0 && (st[0] != 0x80 || st[1] != 0x80 ||
                                          st[2] != 0x80 || st[3] != 0x80);
    const bool dpad_live  = ((uint16_t)~w & 0x00F0u) != 0;

    int mode;
    if (p.kind != 0)                  mode = effective_player_mode(p);
    else if (dev_any_input_enabled()) mode = (int)PSXRecompV4::PAD_MODE_ANALOG;
    else                              mode = p.mode;

    int eff_analog;
    if (mode == (int)PSXRecompV4::PAD_MODE_DIGITAL) {
        eff_analog = 0;
    } else if (mode == (int)PSXRecompV4::PAD_MODE_ANALOG) {
        eff_analog = 1;
    } else {
        if (stick_live)     p.hybrid_analog = true;
        else if (dpad_live) p.hybrid_analog = false;
        eff_analog = p.hybrid_analog ? 1 : 0;
    }
    if (eff_analog && mode == (int)PSXRecompV4::PAD_MODE_ANALOG && !stick_live) {
        if ((uint16_t)(~w & 0x0010u)) st[1] = 0x00;
        if ((uint16_t)(~w & 0x0040u)) st[1] = 0xFF;
        if ((uint16_t)(~w & 0x0080u)) st[0] = 0x00;
        if ((uint16_t)(~w & 0x0020u)) st[0] = 0xFF;
    }
    if (!eff_analog) { st[0] = st[1] = st[2] = st[3] = 0x80; }

    out->buttons = w;
    out->lx = st[0]; out->ly = st[1]; out->rx = st[2]; out->ry = st[3];
    out->analog = eff_analog ? 1u : 0u;
    out->connected = 1;
}

/* Opt-in netplay barrier split: guest quantum vs admit wait (PSX_NETPLAY_TIMING=1).
 * Printed on the existing [FPS] line — no separate log files. */
static int      s_np_timing_enabled = -1;
static uint64_t s_np_admit_ticks = 0;
static uint64_t s_np_guest_ticks = 0;
static uint64_t s_np_last_admit_end = 0;
static uint64_t s_np_timing_frames = 0;
/* §27: wall-clock gaps between successful presents (player-visible starvation). */
static uint64_t s_present_last_ms = 0;
static uint32_t s_present_gaps_ms[128];
static unsigned s_present_gaps_n = 0;
/* §74: highest sim tick ever shown on screen. Resim frames above it are new
 * forward progress (treadmill chain) and present live; at/below is a rewound
 * re-play and keeps §63 hold-last. Reset when a netplay session starts. */
static uint32_t s_netplay_present_sim_watermark = 0;
static int netplay_timing_on(void) {
    if (s_np_timing_enabled < 0) {
        const char *e = std::getenv("PSX_NETPLAY_TIMING");
        s_np_timing_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_np_timing_enabled;
}

static void netplay_note_present(void);
static void netplay_hold_last_present_tick(void);

static void netplay_note_present(void) {
    uint64_t now;
    uint32_t gap;
    if (!psx_netplay_active() || !netplay_timing_on())
        return;
    now = SDL_GetTicks64();
    if (s_present_last_ms != 0ull && now >= s_present_last_ms) {
        gap = (uint32_t)(now - s_present_last_ms);
        if (s_present_gaps_n < (unsigned)(sizeof(s_present_gaps_ms) / sizeof(s_present_gaps_ms[0])))
            s_present_gaps_ms[s_present_gaps_n++] = gap;
    }
    s_present_last_ms = now ? now : 1ull;
}

static int netplay_local_viewport_slot(void) {
    if (g_netplay_local_viewport != 1 || !psx_netplay_active())
        return -1;
    if (!gpu_last_frame_vertical_split_screen())
        return -1;
    int slot = psx_netplay_local_slot();
    return (slot == 0 || slot == 1) ? slot : -1;
}

static int crop_present_to_netplay_local_viewport(uint32_t* pixels,
                                                  int* width,
                                                  int height) {
    if (!pixels || !width || *width < 2 || height <= 0)
        return 0;
    const int slot = netplay_local_viewport_slot();
    if (slot < 0)
        return 0;

    const int src_w = *width;
    const int crop_w = src_w / 2;
    const int crop_x = slot == 0 ? 0 : (src_w - crop_w);
    for (int y = 0; y < height; ++y) {
        uint32_t* dst = pixels + (size_t)y * (size_t)crop_w;
        uint32_t* src = pixels + (size_t)y * (size_t)src_w + crop_x;
        memmove(dst, src, (size_t)crop_w * sizeof(uint32_t));
    }
    *width = crop_w;
    return 1;
}

static void netplay_present_gap_stats(uint32_t *p95_out, uint32_t *max_out) {
    unsigned n = s_present_gaps_n;
    unsigned idx;
    if (p95_out) *p95_out = 0;
    if (max_out) *max_out = 0;
    if (n == 0)
        return;
    std::sort(s_present_gaps_ms, s_present_gaps_ms + n);
    idx = (n > 1u) ? (unsigned)((95u * (n - 1u)) / 100u) : 0u;
    if (p95_out) *p95_out = s_present_gaps_ms[idx];
    if (max_out) *max_out = s_present_gaps_ms[n - 1u];
    s_present_gaps_n = 0;
}

/* Stage local pad + poll admit until published. Parks the guest fiber when
 * called from the vblank callback (or before scheduler entry). Latches one
 * sample per sim tick; stalls on INPUT_CONFIRM desync. */
static void netplay_barrier_admit(int override) {
    if (!psx_netplay_active()) return;
    /* Launcher/game window teardown can leave a queued SDL_QUIT; draining it
     * prevents an instant soft-return before the first frame. */
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_QUIT);
    static int desync_logged = 0;
    const Uint64 barrier_t0 = SDL_GetTicks64();
    /* Rematch session_reboot is asymmetric: the faster peer reaches admit
     * while the slower is still in BIOS/GL bring-up. Do not arm the 20s
     * admit-stall / 1.5s silence clocks until RUNNING (HELLO exchanged).
     * progress_t0 is set on the first RUNNING sample. */
    Uint64 progress_t0 = 0;
    Uint64 last_stall_log_ms = barrier_t0;
    const uint64_t admit_t0 =
        netplay_timing_on() ? SDL_GetPerformanceCounter() : 0;
    /* Guest quantum = time since previous admit returned (fiber ran). */
    if (admit_t0 && s_np_last_admit_end && admit_t0 >= s_np_last_admit_end) {
        s_np_guest_ticks += admit_t0 - s_np_last_admit_end;
        s_np_timing_frames++;
    }
    int liveness_rearamed = 0;
    for (;;) {
        uint32_t dt = 0, lh = 0, rh = 0;
        const Uint64 now_ms = SDL_GetTicks64();
        const int running = psx_netplay_is_running();
        if (running && progress_t0 == 0)
            progress_t0 = now_ms;
        /* Pump before liveness: free-run between vblanks (and tick-0 dig CRCs)
         * does not call poll_admit, so last_peer_rx can age past 1.5s while
         * peer FRAME_COMMIT/INPUT sit in the UDP socket. Checking disconnect
         * first false-killed rematch right after enter-guest. */
        psx_netplay_pump();
        if (!liveness_rearamed) {
            psx_netplay_touch_peer_liveness();
            liveness_rearamed = 1;
        }
        /* Load apply/ready suppresses INPUT (and can sit silent for seconds on
         * a hash-match .pst). timeout=0 still honors BYE (peer_gone) but does
         * not treat rx silence as disconnect — that was kicking both peers to
         * the lobby mid-load. Same BYE-only policy while LINKING so a rematch
         * peer still booting cannot be false-disconnected at 15s. */
        if (psx_netplay_peer_disconnected(
                (psx_netplay_in_load_barrier() || !running)
                    ? 0u
                    : psx_netplay_running_liveness_timeout_ms())) {
            netplay_soft_exit("netplay_peer_disconnect");
            if (psx_return_to_lobby_requested()) return;
        }
        /* Staged .pst rejected (stale codegen / BIOS / missing) — do not wait
         * out the 90s load barrier with stall=load_apply_done. */
        if (psx_netplay_consume_load_apply_failed()) {
            netplay_soft_exit("netplay_load_failed");
            if (psx_return_to_lobby_requested()) return;
        }
        /* Mutual INPUT/CONFIRM stall still refreshes last_peer_rx — detect
         * "no sim progress" separately (common rematch + TURN loss mode).
         * Save/load/memcard probe+chunk xfer uses a longer budget (TURN +
         * ~1.4MB .pst). The old 20s admit timeout killed SAVE mid-transfer.
         * Link phase (not RUNNING yet) uses its own 90s backstop so a dead
         * peer still returns to lobby without blaming admit progress. */
        if (psx_netplay_in_load_barrier() && now_ms - barrier_t0 >= 90000u) {
            char stall[96];
            uint32_t sim = 0;
            int lead = 0;
            psx_netplay_admit_wait_info(stall, sizeof(stall), &sim, &lead);
            std::fprintf(stderr,
                         "psxrecomp: netplay state barrier timeout sim=%u "
                         "stall=%s lead=%d — returning to lobby\n",
                         (unsigned)sim, stall[0] ? stall : "?", lead);
            netplay_soft_exit("netplay_load_stall");
            if (psx_return_to_lobby_requested()) return;
        } else if (!psx_netplay_in_load_barrier() && !running &&
                   now_ms - barrier_t0 >= 90000u) {
            char stall[64];
            uint32_t sim = 0;
            int lead = 0;
            psx_netplay_admit_wait_info(stall, sizeof(stall), &sim, &lead);
            std::fprintf(stderr,
                         "psxrecomp: netplay link stall timeout sim=%u "
                         "stall=%s lead=%d — returning to lobby\n",
                         (unsigned)sim, stall[0] ? stall : "?", lead);
            netplay_soft_exit("netplay_link_stall");
            if (psx_return_to_lobby_requested()) return;
        } else if (!psx_netplay_in_load_barrier() && running &&
                   progress_t0 != 0 && now_ms - progress_t0 >= 20000u) {
            char stall[64];
            uint32_t sim = 0;
            int lead = 0;
            psx_netplay_admit_wait_info(stall, sizeof(stall), &sim, &lead);
            std::fprintf(stderr,
                         "psxrecomp: netplay admit stall timeout sim=%u "
                         "stall=%s lead=%d — returning to lobby\n",
                         (unsigned)sim, stall[0] ? stall : "?", lead);
            netplay_soft_exit("netplay_admit_stall");
            if (psx_return_to_lobby_requested()) return;
        } else if (now_ms - last_stall_log_ms >= 2000u) {
            char stall[96];
            uint32_t sim = 0;
            int lead = 0;
            const Uint64 clock0 = running && progress_t0 ? progress_t0 : barrier_t0;
            psx_netplay_admit_wait_info(stall, sizeof(stall), &sim, &lead);
            std::fprintf(stderr,
                         "psxrecomp: netplay admit waiting sim=%u stall=%s "
                         "lead=%d (%llums%s)\n",
                         (unsigned)sim, stall[0] ? stall : "?", lead,
                         (unsigned long long)(now_ms - clock0),
                         running ? "" : ", linking");
            last_stall_log_ms = now_ms;
        }
        psx_lobby_pump();
        if (psx_netplay_input_desync(&dt, &lh, &rh)) {
            if (!desync_logged) {
                std::printf("psxrecomp: netplay INPUT desync tick=%u local=%08x remote=%08x — stalled\n",
                            (unsigned)dt, (unsigned)lh, (unsigned)rh);
                std::fflush(stdout);
                desync_logged = 1;
            }
            SDL_Delay(16);
#ifndef PSX_NO_DEBUG_TOOLS
            debug_server_poll();
#endif
            continue;
        }
        /* §36: TipHold parks sim, so needs_local_sample stays 0 after latch.
         * Still capture every spin so live_pad_buttons can see a real release
         * (SAFETY deferred must not ride ABSOLUTE 2000ms on a frozen peek).
         * PSX_START_BISECT_NO_TIPHOLD_CAPTURE=1: only sample when latching a
         * new sim tip (Stage-3 isolate TipHold refresh). */
        const int tip_hold = psx_netplay_rb_tip_holding();
        const int need_sample =
            psx_netplay_needs_local_sample() ||
            (tip_hold && !psx_start_bisect_no_tiphold_capture());
        if (need_sample) {
            PsxNetPad local{};
            if (override >= 0 && !g_headless) {
                capture_override_pad(override, &local);
            } else if (g_headless) {
                local.buttons = 0xFFFFu;
                local.lx = local.ly = local.rx = local.ry = 0x80u;
                local.analog = 1;
                local.connected = 1;
            } else {
                capture_local_human_pad(&local);
            }
            psx_netplay_stage_local(&local);
        } else if (psx_start_bisect_spin_log() && !g_headless) {
            /* Dense SDL-only samples while admit waits without capture. */
            const int card = psx_netplay_input_player();
            const int sdl = netplay_sdl_start_held(card >= 0 ? card : 0);
            psx_start_bisect_log("spin", psx_netplay_sim_tick(), sdl, -1, -1,
                                 -1, tip_hold ? 0 : 1, tip_hold,
                                 psx_netplay_is_resimulating());
        }
        if (psx_netplay_poll_admit()) {
            desync_logged = 0;
            if (admit_t0) {
                const uint64_t t1 = SDL_GetPerformanceCounter();
                if (t1 >= admit_t0) s_np_admit_ticks += t1 - admit_t0;
                s_np_last_admit_end = t1;
            }
            return;
        }
        /* Episode snap may have been applied during pump/try_admit without
         * longjmp — flush here (no present-body C++ RAII) before spinning.
         * try_admit refuses to arm needs_advance while resume is pending. */
        psx_netplay_rb_flush_resume();
#ifndef PSX_NO_DEBUG_TOOLS
        debug_server_poll();
#endif
        if (!g_headless) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    netplay_soft_exit("sdl_window_close");
                    if (psx_return_to_lobby_requested()) return;
                }
                if (ev.type == SDL_KEYDOWN) {
#if defined(PSX_SDL3)
                    const SDL_Keycode key = ev.key.key;
#else
                    const SDL_Keycode key = ev.key.keysym.sym;
#endif
                    if (key == SDLK_ESCAPE) {
                        netplay_soft_exit("netplay_barrier_escape");
                        if (psx_return_to_lobby_requested()) return;
                    }
                }
                if (ev.type == SDL_CONTROLLERDEVICEADDED ||
                    ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    refresh_player_devices();
                }
            }
            /* Stage-3: PSX_START_BISECT_NO_GC_UPDATE_IN_ADMIT skips the dense
             * Update that advances SDL pad state between sim publishes. */
            if (!psx_start_bisect_no_gc_update_in_admit())
                SDL_GameControllerUpdate();
        }
        if (psx_return_to_lobby_requested()) return;
        /* §35: TipHold invent-cap stall freezes guest (no vblank present).
         * Keep Swap alive on the last Live frame so SAFETY/held waits do not
         * open a ~250ms present gap.
         * §100: MEDIA-KF probe/xfer (~3.7 MB) also parks sim in admit — hold
         * last FMV frame so intro does not freeze for the transfer window. */
        if (psx_netplay_rb_tip_holding() ||
            psx_netplay_rb_media_kf_busy() ||
            (psx_netplay_rb_fmv_media_active() && psx_netplay_rb_active() &&
             !psx_netplay_rb_is_resimulating()))
            netplay_hold_last_present_tick();
        /* Wake on peer UDP (or 1ms timeout). SDL_Delay(1) under dual FMV load
         * often stretches multi-ms and cut MotK netplay intro ~59→~36; Delay(0)
         * busy-spins and can starve the peer (tick-0 hang). */
        psx_netplay_wait_recv(1);
        /* Admit barriers (save/load sync) can last seconds without guest cycles. */
        starvation_watchdog_heartbeat();
    }
}

static void sample_pad_into_sio(int override) {
    /* Selfcheck fighter mash owns P1 when enabled (headless-safe). */
    if (override < 0) {
        uint16_t mash = 0xFFFFu;
        if (psx_selfcheck_mash_override(&mash))
            override = (int)mash;
    }
    if (override >= 0) {
        apply_input_override_to_sio(override);
        return;
    }
    int n = g_offline_pad_count;
    if (n < 1) n = 1;
    if (n > PSX_MAX_PLAYERS) n = PSX_MAX_PLAYERS;
    const uint32_t consumer_sim =
        psx_start_consumer_enabled() ? psx_start_consumer_offline_frame() : 0u;
    for (int s = 0; s < n; s++) {
        PsxNetPad pad;
        if (!capture_pad_slot(s, &pad)) continue;  /* no device in this port */
        /* Push sticks every frame; request the pad type (digital/analog) through
         * the coherent channel so a hybrid stick<->d-pad flip is applied only at
         * an idle, non-config bus boundary (never mid-poll / mid-handshake). This
         * is the fix for the v0.5.0 phantom-input regression: slamming the type
         * each frame raced Tomba's DualShock config handshake -> garbage button
         * reads. eff_analog still reflects this frame's mode (digital / analog /
         * hybrid auto-switch). */
        apply_pad_slot_to_sio(s, pad);
        if (psx_start_consumer_enabled())
            psx_start_consumer_note(s, consumer_sim, pad.buttons);
        if (psx_start_bisect_enabled() && s == 0) {
            const int sdl = netplay_sdl_start_held(s);
            const int cap = ((uint16_t)(~pad.buttons) & 0x0008u) != 0;
            const int sio =
                ((uint16_t)(~sio_get_pad_buttons_slot(s)) & 0x0008u) != 0;
            psx_start_bisect_log("offline", consumer_sim, sdl, cap, cap, sio, 1,
                                 0, 0);
        }
    }
}

static void sample_headless_pad_into_sio(int override) {
    if (override < 0) {
        uint16_t mash = 0xFFFFu;
        if (psx_selfcheck_mash_override(&mash))
            override = (int)mash;
    }
    if (override >= 0) {
        apply_input_override_to_sio(override);
        return;
    }
#ifdef PSX_COSIM
    /* Input-driven cosim (EXPERIMENTAL): the coordinator sets a HELD pad state via the
     * `setpad` TCP command; the headless sampler applies it every frame so the same
     * button input is fed to BOTH lockstep instances at the same guest cycle. If the
     * two instances desync under input, this path is proven nondeterministic and gets
     * stubbed out (see cosim.c). Default 0xFFFF = all released (PSX pad is active-low). */
    { extern volatile int g_cosim_pad_hold[2];
      sio_set_pad_state_slot(0, (uint16_t)g_cosim_pad_hold[0]);
      sio_set_pad_state_slot(1, (uint16_t)g_cosim_pad_hold[1]);
      return; }
#endif
    sio_set_pad_state_slot(0, 0xFFFFu);
    sio_set_pad_state_slot(1, 0xFFFFu);
}

/* PSX native vblank cadence: NTSC ≈ 59.94 Hz. Wall-clock target keeps
 * audio sample generation (735 samples/vblank * 60 = 44100/sec) matched
 * to the SDL audio device drain rate, eliminating queue overflow drops
 * and underruns. Uncapped, the host runs the simulation at whatever
 * speed it can — typically several × realtime — and audio glitches.
 *
 * The wall-clock pacer target; nudged to the host display refresh at
 * window-creation time when the panel is ~60 Hz. Driver vsync and this pacer
 * are XOR (see present_vsync_owns_cadence): both at once double-blocks on
 * Linux compositors to 30 Hz. See g_frame_period_ms. */
/* Live pacer period (ms). Defaults to the PSX rate; set to the host refresh
 * period when the panel is within ~2% of 60 Hz. Left at the PSX rate on
 * non-~60Hz / unknown-refresh panels so vsync cannot run the sim fast.
 * Declared with the early video/mod globals. */

/* §33/§35/§47: re-present last Live frame on a wall-clock cadence while guest
 * sim is frozen (short resim) or TipHold invent-cap stall (admit spin). */
static void netplay_hold_last_present_tick(void) {
    static uint64_t s_hold_last_ms;
    uint64_t now = SDL_GetTicks64();
    uint32_t period = (uint32_t)(g_frame_period_ms + 0.5);
    int did = 0;
    if (period < 8u)
        period = 8u;
    if (period > 33u)
        period = 33u;
#ifndef PSX_SDL_NO_RENDER
    if (g_gl_active)
        gl_renderer_set_interpolation_suspended(1);
#endif
    if (s_hold_last_ms != 0ull && now >= s_hold_last_ms &&
        (uint32_t)(now - s_hold_last_ms) < period)
        return;
#ifndef PSX_SDL_NO_RENDER
    if (g_gl_active) {
        did = gl_renderer_present_hold_last();
    } else if (!g_vk_active && sdl_renderer && sdl_texture && s_sw_hold_valid) {
        SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer);
        SDL_RenderCopy(sdl_renderer, sdl_texture, &s_sw_hold_src, &s_sw_hold_dst);
        host_osd_draw_sdl(sdl_renderer);
        SDL_RenderPresent(sdl_renderer);
        did = 1;
    }
#endif
    if (did) {
        netplay_note_present();
        s_hold_last_ms = now ? now : 1ull;
    }
}

/* §47/§51/§63 Layer 4: long Replay catch-up — wall-clock gate for a real VRAM
 * present. Independent of SPAN chunk boundaries (those are bookkeeping, not
 * present events). Returns 1 if this guest vblank should fall through.
 *
 * §63: tip episodes are ≤ tip_runway (24). Live-presenting those frames
 * re-showed edge-trigger menu inputs that Live had already displayed before
 * the rewind (pad-log DUP_SIO Start/dpad). Hold-last through a full SPAN;
 * only ownership-chain catch-up deeper than that shows live progress. */
static int netplay_replay_catchup_should_live_present(uint32_t remaining) {
    static uint64_t s_catchup_present_ms;
    uint64_t now = SDL_GetTicks64();
    /* ~6–7 Hz: enough to show progress without flashing every resim frame. */
    uint32_t period = 150u;
    if (remaining <= 24u)
        return 0;
    if (s_catchup_present_ms != 0ull && now >= s_catchup_present_ms &&
        (uint32_t)(now - s_catchup_present_ms) < period)
        return 0;
    s_catchup_present_ms = now ? now : 1ull;
    {
        static uint32_t s_log_sim;
        uint32_t sim = psx_netplay_sim_tick();
        if (s_log_sim != sim) {
            fprintf(stderr,
                    "psxrecomp: rb catchup frontier_rem=%u sim=%u target=%u "
                    "present=live\n",
                    (unsigned)remaining, (unsigned)sim,
                    (unsigned)psx_netplay_rb_episode_target());
            fflush(stderr);
            s_log_sim = sim;
        }
    }
    return 1;
}

/* ── Host-stack-usage profile (RECURSION_BUG.md §17) ──────────────────────────
 * The decisive instrument for the long-run freeze. The guest call graph mirrors
 * onto the 1MB guest fiber stack (traps.c), and the native-stack guard trips at
 * ~768KB used. The open question is the SHAPE of host-stack usage over frames:
 *   - LINEAR climb from gameplay start  => a per-frame interp<->compiled boundary
 *     leak: every game tick leaks one un-unwound host call chain (~13 frames),
 *     overflowing the fiber after ~50k frames. 50k is a CAPACITY limit, not a
 *     trigger. (Model A — predicted.)
 *   - FLAT, then a cliff at one frame   => a within-one-frame runaway re-entry at
 *     a real ~frame-50k trigger. (Model W — the old §15 premise.)
 * (TEB StackBase - rsp), sampled on the guest fiber each vblank, IS the leaked
 * depth. Sampled in sdl_vblank_present — a gpu_vblank_tick callback off
 * psx_check_interrupts, i.e. ON the guest fiber, every vblank, in both builds.
 * Decimated 1-per-STRIDE frames so the ring spans the whole run; "now" carries
 * the exact latest sample. ALWAYS-ON: read live via the `stack_profile` TCP
 * command while the game is still responsive (no need to reach the overflow),
 * and dumped in the crash report. Query the ring; never arm-and-capture. */
#if defined(_WIN32)
#include <intrin.h>   /* __readgsqword — fiber TEB StackBase */
static size_t host_stack_used(void) {
    char probe;
    uintptr_t base = (uintptr_t)__readgsqword(0x08);   /* TEB StackBase (high) */
    uintptr_t sp   = (uintptr_t)&probe;
    return (base > sp) ? (size_t)(base - sp) : 0;
}
#else
static size_t host_stack_used(void) { return 0; }
#endif

#define STACK_PROFILE_CAP    512u
#define STACK_PROFILE_STRIDE 128u   /* 512 * 128 = 65536 frames of coverage */
typedef struct { uint32_t frame; uint32_t used_kb; } StackSample;
static StackSample g_stack_prof[STACK_PROFILE_CAP];
static uint32_t    g_stack_prof_seq    = 0;
static uint32_t    g_stack_used_now_kb = 0;
static uint32_t    g_stack_frame_now   = 0;
static uint32_t    g_stack_used_max_kb = 0;

static void stack_profile_sample(void) {
    extern uint64_t s_frame_count;
    uint32_t f  = (uint32_t)s_frame_count;
    uint32_t kb = (uint32_t)(host_stack_used() >> 10);
    g_stack_frame_now   = f;
    g_stack_used_now_kb = kb;
    if (kb > g_stack_used_max_kb) g_stack_used_max_kb = kb;
    if ((f % STACK_PROFILE_STRIDE) == 0) {
        StackSample *e = &g_stack_prof[g_stack_prof_seq++ & (STACK_PROFILE_CAP - 1u)];
        e->frame = f; e->used_kb = kb;
    }
}

/* JSON dump of the profile (callable from C: crash report + TCP command). */
extern "C" int stack_profile_json(char *out, int cap) {
    uint32_t total = g_stack_prof_seq;
    uint32_t avail = total < STACK_PROFILE_CAP ? total : STACK_PROFILE_CAP;
    uint32_t start = total - avail;
    int n = snprintf(out, cap,
        "{\"now\":{\"f\":%u,\"kb\":%u},\"max_kb\":%u,\"stride\":%u,\"count\":%u,\"samples\":[",
        g_stack_frame_now, g_stack_used_now_kb, g_stack_used_max_kb,
        STACK_PROFILE_STRIDE, avail);
    for (uint32_t i = 0; i < avail && n < cap - 48; i++) {
        StackSample *e = &g_stack_prof[(start + i) & (STACK_PROFILE_CAP - 1u)];
        n += snprintf(out + n, cap - n, "%s{\"f\":%u,\"kb\":%u}",
                      i ? "," : "", e->frame, e->used_kb);
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

/* ---- Present-classification ring (see present_ring.h) -----------------
 * Always-on record of how each present was classified, for every backend.
 * A native-wide present that silently falls back to the canonical width is
 * indistinguishable on screen from a deliberate stretch, so the decision
 * has to live in a queryable ring rather than be re-derived after the fact. */
#define PRES_RING_CAP 4096u   /* power of two; ~68 s at 60 Hz */
static PresRingEntry s_pres_ring[PRES_RING_CAP];
static uint64_t      s_pres_ring_total = 0;

extern "C" uint64_t present_ring_total(void) { return s_pres_ring_total; }
extern "C" int present_ring_get(uint64_t seq, PresRingEntry* out) {
    if (seq >= s_pres_ring_total) return 0;
    if (s_pres_ring_total - seq > PRES_RING_CAP) return 0;
    *out = s_pres_ring[seq & (PRES_RING_CAP - 1u)];
    return 1;
}

/* Commit this present's classification. Returns the stored entry so the
 * software wide-fallback (the only post-decision mutation) can amend it. */
static PresRingEntry* present_ring_commit(uint8_t path, uint16_t disp_w,
                                          uint16_t disp_h, uint16_t present_w) {
    GpuWsDebug ws;
    gpu_ws_get_debug(&ws);
    PresRingEntry* e = &s_pres_ring[s_pres_ring_total & (PRES_RING_CAP - 1u)];
    s_pres_ring_total++;
    e->frame         = (uint32_t)ws.cur_frame;
    e->disp_w        = disp_w;
    e->disp_h        = disp_h;
    e->present_w     = present_w;
    e->nw_extra      = (uint16_t)(ws.nw_extra < 0 ? 0 : ws.nw_extra);
    e->path          = path;
    e->wide_fellback = 0;
    e->game_mode     = (uint8_t)ws.game_mode;
    e->native_43     = (uint8_t)ws.present_native_43;
    int64_t d = (int64_t)ws.cur_frame - (int64_t)ws.last_tag_frame;
    e->tag_delta     = (d > INT32_MAX || d < INT32_MIN) ? INT32_MAX : (int32_t)d;
    e->gte_verts     = (uint16_t)(ws.gte_verts > 0xFFFF ? 0xFFFF : ws.gte_verts);
    e->ovh_prims     = (uint16_t)(ws.ovh_prims > 0xFFFF ? 0xFFFF : ws.ovh_prims);
    return e;
}

/* ---- Load-transition ring ---------------------------------------------
 * State edges only, rather than one record per frame. This makes the exact
 * CD-read -> bridged-load -> turbo -> paced transitions cheap and queryable
 * without changing any runtime behavior. */
#define LOAD_TRANSITION_CAP 512u
static LoadTransitionEntry s_load_transitions[LOAD_TRANSITION_CAP];
static uint64_t s_load_transition_total = 0;

extern "C" uint64_t load_transition_total(void) {
    return s_load_transition_total;
}
extern "C" int load_transition_get(uint64_t seq, LoadTransitionEntry *out) {
    if (seq >= s_load_transition_total) return 0;
    if (s_load_transition_total - seq > LOAD_TRANSITION_CAP) return 0;
    *out = s_load_transitions[seq & (LOAD_TRANSITION_CAP - 1u)];
    return 1;
}

static void load_transition_note(int read_active, int load_active,
                                 int turbo_active, int load_run) {
    static int prev_read = -1, prev_load = -1, prev_turbo = -1;
    if (read_active == prev_read && load_active == prev_load &&
        turbo_active == prev_turbo) return;
    extern uint64_t s_frame_count;
    LoadTransitionEntry *e =
        &s_load_transitions[s_load_transition_total & (LOAD_TRANSITION_CAP - 1u)];
    s_load_transition_total++;
    e->frame = (uint32_t)s_frame_count;
    e->host_ms = (uint32_t)SDL_GetTicks();
    e->load_run = (uint16_t)(load_run > 0xFFFF ? 0xFFFF : load_run);
    e->read_active = (uint8_t)(read_active != 0);
    e->load_active = (uint8_t)(load_active != 0);
    e->turbo_active = (uint8_t)(turbo_active != 0);
    /* Pacing-state audit trail: any unpaced/present-skip mode engaging or
     * releasing mid-play is exactly the class of bug users report as "the
     * speed is busted", and the ring above is only reachable through the
     * debug server. One line per transition is cheap and self-documents. */
    if (turbo_active != prev_turbo)
        std::fprintf(stdout, "[pace] turbo_loads %s (frame %u, host %ums)\n",
                     turbo_active ? "ENGAGED" : "released",
                     e->frame, e->host_ms);
    prev_read = read_active;
    prev_load = load_active;
    prev_turbo = turbo_active;
}

/* Tick depth24 cutover state once per present. Arm a short full-frame blank
 * when MDEC returns after leaving depth24, or after a *long* intra-depth24
 * idle (real inter-movie gap) — not after every short gap between ~15fps
 * MDEC frames. With SIMD IDCT a movie frame often finishes in one vblank;
 * treating !mdec_recently_active(3) as a cutover re-armed a 2-present full
 * black blank on every decode burst (BPE FMV flicker). Do not regress to
 * short-gap arming for MotK merges — MotK cutovers still arm via depth24
 * exit and long idle. */
static void depth24_cutover_tick(int depth24) {
    const int mdec_on = depth24 && mdec_recently_active(3);
    /* ~0.5s @60Hz — longer than inter-frame MDEC idle in a live movie. */
    const int mdec_long_idle = depth24 && !mdec_recently_active(30);
    if (!depth24) {
        s_d24_prev_mdec = 0;
        s_d24_cutover_blank = 0;
        s_d24_saw_gap = 1; /* next depth24+MDEC is a fresh movie cutover */
        return;
    }
    if (mdec_long_idle)
        s_d24_saw_gap = 1;
    if (s_d24_saw_gap && mdec_on && !s_d24_prev_mdec) {
        /* Hide the transitional present(s) that still show stale RGB888 junk. */
        s_d24_cutover_blank = 2;
        s_d24_saw_gap = 0;
    }
    s_d24_prev_mdec = mdec_on;
}

/* Depth24 FMV: CRTC width stays full (e.g. MotK 512) while MDEC uploads may
 * not cover the right side yet — leftover VRAM read as RGB888 flashes junk.
 * Present width is never shrunk; black-fill only the trailing uncovered cols.
 *
 * Match origin/master's span policy: when upload span is unknown (lim==0),
 * only blank the last ~8 columns. Blanking [0..w) on lim==0 (an older tip
 * path) turned MotK FMV into a permanent black screen after GP1(07h) hold
 * reset the span — hold ticks skip Swap, then the next present saw lim=0
 * and wiped the whole frame every vblank.
 *
 * Optional short cutover blank: full-frame black for 1–2 presents on movie
 * start / long idle resume only (see depth24_cutover_tick). */
static void depth24_fix_trailing_margin(uint32_t *buf, uint32_t w, uint32_t h,
                                          uint32_t display_x) {
    if (!buf || w < 8u || h == 0u) return;

    if (s_d24_cutover_blank > 0) {
        s_d24_cutover_blank--;
        const uint32_t n = w * h;
        for (uint32_t i = 0; i < n; i++)
            buf[i] = 0xFF000000u;
        return;
    }

    /* Default: last 8 columns. If the upload span is known and ends earlier
     * inside that margin, start blanking from the span edge instead. */
    uint32_t start = w - 8u;
    uint32_t lim = gpu_depth24_rgb_limit(display_x, w);
    if (lim > 0u && lim < w && lim < start)
        start = lim;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = start; x < w; x++)
            buf[y * w + x] = 0xFF000000u;
    }
}

enum {
    PSX_ASSIST_BIND_REWIND = 0,
    PSX_ASSIST_BIND_SAVE_STATE_MENU,
    PSX_ASSIST_BIND_COUNT
};

#define PSX_HOTKEY_PAD_BUTTON(code) (1 + (int)(code))
#define PSX_HOTKEY_PAD_BUTTON_COMBO(mask) (1000 + (int)(mask))
#define PSX_HOTKEY_PAD_IS_BUTTON(value) ((value) > 0 && (value) < 100)
#define PSX_HOTKEY_PAD_IS_AXIS(value) ((value) >= 100 && (value) < 1000)
#define PSX_HOTKEY_PAD_IS_BUTTON_COMBO(value) ((value) >= 1000)
#define PSX_HOTKEY_PAD_BUTTON_CODE(value) ((value) - 1)
#define PSX_HOTKEY_PAD_AXIS_CODE(value) (((value) - 100) / 2)
#define PSX_HOTKEY_PAD_AXIS_POSITIVE(value) (((value) - 100) & 1)
#define PSX_HOTKEY_PAD_BUTTON_COMBO_MASK(value) ((value) - 1000)
#define PSX_HOTKEY_PAD_SELECT_R3 \
    PSX_HOTKEY_PAD_BUTTON_COMBO(((uint32_t)1u << SDL_CONTROLLER_BUTTON_BACK) | \
                                ((uint32_t)1u << SDL_CONTROLLER_BUTTON_RIGHTSTICK))
#define PSX_HOTKEY_PAD_SELECT_R1 \
    PSX_HOTKEY_PAD_BUTTON_COMBO(((uint32_t)1u << SDL_CONTROLLER_BUTTON_BACK) | \
                                ((uint32_t)1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))

static int normalize_hotkey_pad_binding(int binding, int fallback) {
    if (PSX_HOTKEY_PAD_IS_BUTTON(binding)) {
        int code = PSX_HOTKEY_PAD_BUTTON_CODE(binding);
        if (code >= 0 && code < SDL_CONTROLLER_BUTTON_MAX)
            return binding;
    } else if (PSX_HOTKEY_PAD_IS_BUTTON_COMBO(binding)) {
        int mask = PSX_HOTKEY_PAD_BUTTON_COMBO_MASK(binding);
        if (mask > 0)
            return binding;
    } else if (PSX_HOTKEY_PAD_IS_AXIS(binding)) {
        int code = PSX_HOTKEY_PAD_AXIS_CODE(binding);
        if (code >= 0 && code < SDL_CONTROLLER_AXIS_MAX)
            return binding;
    } else if (binding == 0) {
        return 0;
    }
    return fallback;
}

static int hotkey_pad_binding_down(int binding) {
    SDL_GameController *h = g_players[0].handle;
    if (!h || binding == 0)
        return 0;
    if (PSX_HOTKEY_PAD_IS_BUTTON_COMBO(binding)) {
        uint32_t mask = (uint32_t)PSX_HOTKEY_PAD_BUTTON_COMBO_MASK(binding);
        if (!mask)
            return 0;
        for (int code = 0; code < SDL_CONTROLLER_BUTTON_MAX && code < 32; ++code) {
            if ((mask & ((uint32_t)1u << code)) == 0)
                continue;
            if (!SDL_GameControllerGetButton(
                    h, (SDL_GameControllerButton)code))
                return 0;
        }
        return 1;
    }
    if (!SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_BACK))
        return 0;
    if (PSX_HOTKEY_PAD_IS_BUTTON(binding)) {
        int code = PSX_HOTKEY_PAD_BUTTON_CODE(binding);
        if (code < 0 || code >= SDL_CONTROLLER_BUTTON_MAX)
            return 0;
        return SDL_GameControllerGetButton(h, (SDL_GameControllerButton)code) != 0;
    }
    if (PSX_HOTKEY_PAD_IS_AXIS(binding)) {
        int code = PSX_HOTKEY_PAD_AXIS_CODE(binding);
        Sint16 v;
        if (code < 0 || code >= SDL_CONTROLLER_AXIS_MAX)
            return 0;
        v = SDL_GameControllerGetAxis(h, (SDL_GameControllerAxis)code);
        return PSX_HOTKEY_PAD_AXIS_POSITIVE(binding)
            ? (v > 16000)
            : (v < -16000);
    }
    return 0;
}

static int savestate_menu_open = 0;
static int savestate_menu_slot = 0;
static int savestate_menu_ignore_toggle_release = 0;
static SDL_Keycode savestate_menu_open_key = 0;

static void savestate_menu_sync_overlay(void) {
    psx_savestate_menu_set_state(savestate_menu_open, savestate_menu_slot);
}

static void savestate_menu_close(void) {
    savestate_menu_open = 0;
    savestate_menu_sync_overlay();
    host_osd_push("Save states closed", 800);
}

static void savestate_menu_toggle(SDL_Keycode opened_by_key) {
    if (psx_rewind_is_open())
        return;
    if (savestate_menu_open) {
        savestate_menu_close();
        return;
    }
    savestate_menu_open = 1;
    savestate_menu_ignore_toggle_release = 1;
    savestate_menu_open_key = opened_by_key;
    savestate_menu_sync_overlay();
}

static void savestate_menu_move(int delta) {
    savestate_menu_slot += delta;
    while (savestate_menu_slot < 0)
        savestate_menu_slot += 12;
    while (savestate_menu_slot >= 12)
        savestate_menu_slot -= 12;
    savestate_menu_sync_overlay();
}

static int savestate_submit_slot(int slot, int save) {
    if (!save && !savestate_slot_exists(slot)) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Slot %d is empty", slot + 1);
        host_osd_push(msg, 1200);
        return 0;
    }
    if (!save)
        savestate_input_guard_arm();
    if (psx_netplay_active()) {
        if (!psx_netplay_is_host()) {
            host_osd_push("Save states are host-only in netplay", 1500);
            return 0;
        }
        if (save)
            (void)psx_netplay_request_save(slot);
        else
            (void)psx_netplay_request_load(slot);
    } else if (save) {
        (void)savestate_request_save(slot);
    } else {
        (void)savestate_request_load(slot);
    }
    return 1;
}

static void savestate_menu_submit(int save) {
    if (savestate_submit_slot(savestate_menu_slot, save) && savestate_menu_open) {
        savestate_menu_open = 0;
        savestate_menu_sync_overlay();
    }
}

static int savestate_menu_slot_from_key(SDL_Keycode key) {
    if (key >= SDLK_1 && key <= SDLK_9)
        return (int)(key - SDLK_1);
    if (key == SDLK_0)
        return 9;
    if (key == SDLK_MINUS)
        return 10;
    if (key == SDLK_EQUALS)
        return 11;
    return -1;
}

static void savestate_menu_handle_key(SDL_Keycode key, int mod, int repeat) {
    int slot;
    if (repeat)
        return;
    if (savestate_menu_open_key && key == savestate_menu_open_key)
        return;
    slot = savestate_menu_slot_from_key(key);
    if (slot >= 0) {
        savestate_menu_slot = slot;
        savestate_menu_sync_overlay();
        return;
    }
    if (host_keymap_match(HOST_KEYMAP_SAVE_STATE_MENU, (int)key, mod) ||
        key == SDLK_ESCAPE || key == SDLK_BACKSPACE) {
        savestate_menu_close();
    } else if (key == SDLK_LEFT || key == SDLK_UP) {
        savestate_menu_move(-1);
    } else if (key == SDLK_RIGHT || key == SDLK_DOWN) {
        savestate_menu_move(+1);
    } else if (key == SDLK_s) {
        savestate_menu_submit(1);
    } else if (key == SDLK_l) {
        savestate_menu_submit(0);
    } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
        savestate_menu_submit((mod & KMOD_SHIFT) != 0);
    }
}

static void savestate_menu_poll_nav(uint32_t now_ms) {
    static int prev_load, prev_save, prev_cancel, prev_toggle;
    static int held_dir, last_step_ms;
    int prev = 0, next = 0, load = 0, save = 0, cancel = 0;
    int dir = 0;

    SDL_GameController *h = g_players[0].handle;
    if (h) {
        const Sint16 lx =
            SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_LEFTX);
        const int dz = 16000;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
            SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
            lx < -dz)
            prev = 1;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
            SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
            lx > dz)
            next = 1;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_A))
            load = 1;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_X))
            save = 1;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_B))
            cancel = 1;
    }

    const int toggle = hotkey_pad_binding_down(g_hotkey_pad_save_state_menu);
    if (savestate_menu_ignore_toggle_release) {
        if (!toggle)
            savestate_menu_ignore_toggle_release = 0;
    } else if (toggle && !prev_toggle) {
        cancel = 1;
    }
    prev_toggle = toggle;

    if (load && !prev_load)
        savestate_menu_submit(0);
    if (save && !prev_save)
        savestate_menu_submit(1);
    if (cancel && !prev_cancel)
        savestate_menu_close();
    prev_load = load;
    prev_save = save;
    prev_cancel = cancel;

    if (!savestate_menu_open)
        return;

    dir = next ? +1 : prev ? -1 : 0;
    if (!dir) {
        held_dir = 0;
        return;
    }
    if (dir != held_dir || (int32_t)(now_ms - (uint32_t)last_step_ms) >= 160) {
        savestate_menu_move(dir);
        held_dir = dir;
        last_step_ms = (int)now_ms;
    }
}

static int rewind_toggle_buttons_down(void) {
    return hotkey_pad_binding_down(g_hotkey_pad_rewind);
}

static void rewind_poll_toggle_buttons(void) {
    static int was_down;
    int down = rewind_toggle_buttons_down();
    if (down && !was_down && !psx_rewind_is_open())
        psx_rewind_toggle();
    was_down = down;
}

static void savestate_menu_poll_toggle_buttons(void) {
    static int was_down;
    int down = hotkey_pad_binding_down(g_hotkey_pad_save_state_menu);
    if (down && !was_down && !psx_rewind_is_open())
        savestate_menu_toggle(0);
    was_down = down;
}

static void rewind_poll_nav(uint32_t now_ms) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int left = keys[SDL_SCANCODE_LEFT] ? 1 : 0;
    int right = keys[SDL_SCANCODE_RIGHT] ? 1 : 0;
    /* Overlay: A/Cross load, B/Circle close. Also Enter/Space/Esc. */
    int acc = (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE] ||
               keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_A]) ? 1 : 0;
    int can = (keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_BACKSPACE] ||
               keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_B]) ? 1 : 0;
    /* Honor remapped Cross/Circle (and Select/R3) via the same pad path as
     * gameplay — GameController A/B alone miss keyboard-as-pad and remaps. */
    uint16_t btn = pad_buttons_for(g_players[0], 1, true);
    if ((btn & PAD_LEFT) == 0)
        left = 1;
    if ((btn & PAD_RIGHT) == 0)
        right = 1;
    if ((btn & PAD_CROSS) == 0)
        acc = 1;
    if ((btn & PAD_CIRCLE) == 0 || (btn & PAD_SELECT) == 0 ||
        (btn & PAD_R3) == 0)
        can = 1;
    SDL_GameController *h = g_players[0].handle;
    if (h) {
        const Sint16 lx =
            SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_LEFTX);
        const int dz = 16000;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
            lx < -dz)
            left = 1;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
            lx > dz)
            right = 1;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_A))
            acc = 1;
        if (SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_B) ||
            SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_BACK) ||
            SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
            can = 1;
    }
    psx_rewind_nav_held(left, right, acc, can, now_ms);
}

static void rewind_pause_present(void) {
    psx_rewind_present_tick((uint32_t)SDL_GetTicks());
#ifndef PSX_SDL_NO_RENDER
    if (g_gl_active) {
        gl_renderer_set_interpolation_suspended(1);
        (void)gl_renderer_present_hold_last();
    } else if (g_vk_active) {
        vk_renderer_present_blank();
    } else if (sdl_renderer) {
        SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer);
        if (sdl_texture && s_sw_hold_valid)
            SDL_RenderCopy(sdl_renderer, sdl_texture, &s_sw_hold_src,
                           &s_sw_hold_dst);
        host_osd_draw_sdl(sdl_renderer);
        SDL_RenderPresent(sdl_renderer);
    }
#endif
}

/* Freeze guest in vblank present while the rewind filmstrip is open. */
static void rewind_host_pause_loop(void) {
    while (psx_rewind_is_open()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                psx_crash_trace_set_exit_origin("sdl_window_close");
                shutdown_runtime();
                std::exit(0);
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                refresh_player_devices();
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                close_controller();
                refresh_player_devices();
            } else if (ev.type == SDL_KEYDOWN) {
#if defined(PSX_SDL3)
                const SDL_Keymod mod = ev.key.mod;
                const SDL_Keycode key = ev.key.key;
                const int repeat = ev.key.repeat ? 1 : 0;
#else
                const Uint16 mod = ev.key.keysym.mod;
                const SDL_Keycode key = ev.key.keysym.sym;
                const int repeat = ev.key.repeat ? 1 : 0;
#endif
                if (!repeat &&
                    host_keymap_match(HOST_KEYMAP_REWIND, (int)key, (int)mod)) {
                    psx_rewind_toggle();
                }
            }
        }
        rewind_poll_nav((uint32_t)SDL_GetTicks());
        rewind_pause_present();
        SDL_Delay(8);
    }
}

/* Freeze guest in vblank present while the save-state slot menu is open. */
static void savestate_menu_host_pause_loop(void) {
    while (savestate_menu_open) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                psx_crash_trace_set_exit_origin("sdl_window_close");
                shutdown_runtime();
                std::exit(0);
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                refresh_player_devices();
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                close_controller();
                refresh_player_devices();
            } else if (ev.type == SDL_KEYDOWN) {
#if defined(PSX_SDL3)
                const SDL_Keymod mod = ev.key.mod;
                const SDL_Keycode key = ev.key.key;
                const int repeat = ev.key.repeat ? 1 : 0;
#else
                const Uint16 mod = ev.key.keysym.mod;
                const SDL_Keycode key = ev.key.keysym.sym;
                const int repeat = ev.key.repeat ? 1 : 0;
#endif
                savestate_menu_handle_key(key, (int)mod, repeat);
            } else if (ev.type == SDL_KEYUP) {
#if defined(PSX_SDL3)
                const SDL_Keycode key = ev.key.key;
#else
                const SDL_Keycode key = ev.key.keysym.sym;
#endif
                if (savestate_menu_open_key == key)
                    savestate_menu_open_key = 0;
            }
        }
        savestate_menu_poll_nav((uint32_t)SDL_GetTicks());
        rewind_pause_present();
        SDL_Delay(8);
    }
}

/* Epilogue for netplay admit/pace AFTER all C++ RAII in the present body
 * is destroyed — episode snap load longjmps via psx_netplay_rb_flush_resume and
 * must not cross non-trivial destructors (UB / guest crash). */
struct NetplayVblankEpilogue {
    int do_epilogue = 0;
    int skip_pace = 0;
    int override = -1;
};

/* Called from gpu_vblank_tick() at each simulated vblank. */
static NetplayVblankEpilogue sdl_vblank_present_body(void) {
    NetplayVblankEpilogue ep{};
    /* Guest quantum for this vblank is complete. Drop top-level-resume armed
     * by any resume_at (savestate / selfcheck / RB) during that quantum — the
     * next tick has a live native chain under its dispatch again. */
    psx_scheduler_top_level_resume_clear();
    refresh_live_crtc_cadence();
    int probe_turbo = 0;
    int probe_reached = 0;
    struct PostLoadProbeScope {
        int *turbo;
        int *reached;
        ~PostLoadProbeScope() {
            post_load_probe_on_vblank(*turbo, *reached);
        }
    } probe_scope{&probe_turbo, &probe_reached};

#ifndef PSX_NO_DEBUG_TOOLS
    debug_server_set_fmv_quiet(mdec_recently_active(2));
    /* Debug server: pause gate, poll commands, record frame, check watchpoints. */
    debug_server_wait_if_paused();
    debug_server_poll();
    debug_server_record_frame();
    debug_server_check_watchpoints();

    /* Check debug server input override. */
    int override = debug_server_get_input_override();
#else
    /* Production: skip debug server. Still need to advance frame counter
     * locally so anything else that reads it continues to work. */
    extern uint64_t s_frame_count;
    s_frame_count++;
    int override = -1;
#endif

    runtime_perf_frame_begin();
    RuntimePerfFrameScope runtime_perf_frame_scope;
    runtime_perf_diag_tick();

    /* PSX_HITCH_DIAG=<ms>: per-frame hitch tracer. The 1 s FPS average hides
     * single-frame spikes ("fps is fine but weapon impacts stutter"), so log
     * one line whenever the wall time between vblanks exceeds the threshold,
     * with the emu/pacer split and deltas of the usual burst suspects. The
     * wall - guest - pacer remainder is host-side work (present, GL, audio). */
    {
        static double hitch_thresh_ms = -1.0;
        if (hitch_thresh_ms < 0.0) {
            const char *e = std::getenv("PSX_HITCH_DIAG");
            hitch_thresh_ms = (e && *e) ? atof(e) : 0.0;
        }
        if (hitch_thresh_ms > 0.0) {
            extern uint64_t g_dirty_ram_insns_run;
            extern uint32_t g_dirty_ram_code_gen;
            extern uint64_t s_frame_count;
            static Uint64 h_last_pc = 0;
            static uint64_t h_guest = 0, h_pace = 0, h_dirty = 0, h_ldus = 0;
            static uint32_t h_cg = 0;
            uint64_t ld_tot = 0, ld_max = 0, ld_last = 0;
            overlay_loader_get_load_timing(&ld_tot, &ld_max, &ld_last);
            Uint64 pc_now = SDL_GetPerformanceCounter();
            if (h_last_pc) {
                /* g_runtime_perf.frequency is only initialized under
                 * PSX_RUNTIME_PERF_DIAG; fall back to the SDL clock so the
                 * split never prints NaN when the tracer runs alone. */
                double tick_freq = (double)g_runtime_perf.frequency;
                if (tick_freq <= 0.0)
                    tick_freq = (double)SDL_GetPerformanceFrequency();
                double wall = (double)(pc_now - h_last_pc) * 1000.0 /
                              (double)SDL_GetPerformanceFrequency();
                double guest = (double)(g_runtime_perf.guest_work_ticks -
                                        h_guest) * 1000.0 / tick_freq;
                double pace = (double)(g_runtime_perf.pacer_ticks -
                                       h_pace) * 1000.0 / tick_freq;
                if (wall > hitch_thresh_ms) {
                    std::fprintf(stderr,
                        "[HITCH] f=%llu wall=%.1fms guest=%.1f pacer=%.1f "
                        "host=%.1f | dirty_insns=+%llu ovl_load=+%.2fms "
                        "code_gen=+%u\n",
                        (unsigned long long)s_frame_count, wall, guest, pace,
                        wall - guest - pace,
                        (unsigned long long)(g_dirty_ram_insns_run - h_dirty),
                        (double)(ld_tot - h_ldus) / 1000.0,
                        g_dirty_ram_code_gen - h_cg);
                }
            }
            h_last_pc = pc_now;
            h_guest = g_runtime_perf.guest_work_ticks;
            h_pace  = g_runtime_perf.pacer_ticks;
            h_dirty = g_dirty_ram_insns_run;
            h_ldus  = ld_tot;
            h_cg    = g_dirty_ram_code_gen;
        }
    }

    /* Lightweight frontend telemetry from PR #13. Count simulated vblanks rather
     * than presents so turbo and skipped-frame modes still report game speed.
     * Skip during netplay post-load barrier — admit is stalled and the window
     * is not updating, so a climbing FPS line is misleading. */
    if (fps_telemetry_enabled() && !psx_netplay_in_load_barrier()) {
        extern uint64_t s_frame_count;
        const Uint64 now = SDL_GetPerformanceCounter();
        const Uint64 frequency = SDL_GetPerformanceFrequency();
        if (!s_fps_last_time) {
            s_fps_last_time = now;
            s_fps_last_frame = s_frame_count;
            if (sdl_window && s_fps_base_title.empty()) {
                const char *title = SDL_GetWindowTitle(sdl_window);
                if (title) s_fps_base_title = title;
            }
        } else if (frequency && now - s_fps_last_time >= frequency) {
            const double seconds = (double)(now - s_fps_last_time) / (double)frequency;
            const double fps = (double)(s_frame_count - s_fps_last_frame) / seconds;
            const double speed = fps / psx_crtc_frame_hz();
            /* Internal rendered-frame rate (display-origin flips): the game
             * actually finishing frames, as distinct from the vblank rate —
             * the two diverge under load and users need both on screen. */
            static uint64_t s_fps_last_flips = 0;
            const uint64_t flips_now = psx_gpu_display_flip_count();
            const double render_fps =
                (double)(flips_now - s_fps_last_flips) / seconds;
            s_fps_last_flips = flips_now;
            double display_fps = 0.0;
            if (g_frame_interpolation && g_gl_active) {
                display_fps = g_frame_interpolation_fps > 0
                    ? (double)g_frame_interpolation_fps
                    : g_host_refresh_hz;
            }
            if (!g_headless && sdl_window) {
                char title[256];
                if (display_fps > 0.0) {
                    snprintf(title, sizeof(title),
                             "%s  [VBlank %.0f/s %.2fx | Render %.0f fps | Display %.0f fps]",
                             s_fps_base_title.c_str(), fps, speed, render_fps,
                             display_fps);
                } else {
                    snprintf(title, sizeof(title),
                             "%s  [VBlank %.0f/s %.2fx | Render %.0f fps]",
                             s_fps_base_title.c_str(), fps, speed, render_fps);
                }
                SDL_SetWindowTitle(sdl_window, title);
            }
            if (!g_headless) {
                char osd[96];
                if (display_fps > 0.0) {
                    snprintf(osd, sizeof(osd),
                             "VBl %.0f/s %.2fx  Render %.0f FPS  Disp %.0f",
                             fps, speed, render_fps, display_fps);
                } else {
                    snprintf(osd, sizeof(osd),
                             "VBl %.0f/s %.2fx  Render %.0f FPS",
                             fps, speed, render_fps);
                }
                host_osd_set_status(osd);
            }
            if (netplay_timing_on() && s_np_timing_frames > 0) {
                const double invf = 1000.0 / (double)frequency;
                const double admit_ms =
                    (double)s_np_admit_ticks * invf / (double)s_np_timing_frames;
                const double guest_ms =
                    (double)s_np_guest_ticks * invf / (double)s_np_timing_frames;
                /* Fraction of this window's ticks spent resimulating (Replay)
                 * vs running forward normally (Live). Lets a soak confirm
                 * "the game was chain-episoding almost the whole time" (near
                 * 100%) instead of inferring it from raw episode density in
                 * the log — see docs/ROLLBACK_MOTK_HOOKUP.md 2026-08-01. */
                const uint64_t replay_ticks = psx_netplay_rb_take_replay_ticks();
                const double replay_pct =
                    100.0 * (double)replay_ticks / (double)s_np_timing_frames;
                uint32_t gap_p95 = 0, gap_max = 0;
                /* §27: presentation starvation — eye cares about gaps more
                 * than replay%. Pass: post-settle present_gap_p95 ≤ 33ms. */
                netplay_present_gap_stats(&gap_p95, &gap_max);
                std::fprintf(stderr,
                    "[FPS] game: %.1f fps (%.2fx) render: %.1f | frames: %llu | "
                    "guest=%.2f ms/f admit=%.2f ms/f replay=%.0f%% "
                    "present_gap_p95=%u ms max=%u ms (n=%llu)\n",
                    fps, speed, render_fps, (unsigned long long)s_frame_count,
                    guest_ms, admit_ms, replay_pct,
                    (unsigned)gap_p95, (unsigned)gap_max,
                    (unsigned long long)s_np_timing_frames);
                s_np_admit_ticks = 0;
                s_np_guest_ticks = 0;
                s_np_timing_frames = 0;
            } else {
                std::fprintf(stderr,
                             "[FPS] game: %.1f fps (%.2fx) render: %.1f | frames: %llu\n",
                             fps, speed, render_fps,
                             (unsigned long long)s_frame_count);
            }
            std::fflush(stderr);

            /* Half-rate (~0.50x) with driver vsync still requested: force the
             * proven Wayland-safe policy (PSX_VSYNC=0) — wall-clock pacer +
             * immediate swap. Do NOT hand cadence to driver vsync. */
            {
                static int s_half_rate_windows = 0;
                const int half = (speed >= 0.45 && speed <= 0.55) ? 1 : 0;
                if (!g_present_half_rate_healed &&
                    !g_frame_interpolation &&
                    !psx_netplay_active() &&
                    g_video_vsync != 0 &&
                    half) {
                    s_half_rate_windows++;
                    if (s_half_rate_windows >= 2) {
                        g_present_half_rate_healed = 1;
                        g_video_vsync = 0;
                        g_wayland_allow_vsync = 0;
                        apply_present_cadence();
                        std::printf("psxrecomp: present half-rate self-heal: "
                                    "forcing PSX_VSYNC=0 (wall-clock pacer + "
                                    "immediate swap); driver vsync was "
                                    "halving the frame rate\n");
                        log_present_cadence();
                        std::fflush(stdout);
                        s_half_rate_windows = 0;
                    }
                } else if (!half) {
                    s_half_rate_windows = 0;
                }
            }

            s_fps_last_time = now;
            s_fps_last_frame = s_frame_count;
        }
    }

    /* Host-stack-usage profile sample — frame counter is now current, and we are
     * on the guest fiber (see §17 block above). BEFORE the turbo/fast-boot early
     * returns so the curve is captured even when presents are skipped. */
    stack_profile_sample();

    /* Step 2.8 automation ticks — BEFORE the turbo/fast-boot early returns
     * below, so capture detection and compile pickup keep running while the
     * frontend is skipping presents. Both are cheap and emu-thread-only. */
    {
        uint64_t perf_start = runtime_perf_section_begin();
        overlay_autocapture_tick();
        runtime_perf_section_end(perf_start,
                                 &g_runtime_perf.autocapture_ticks);
    }
    {   /* Apply a finished batch compile via the active code provider (gcc:
         * cache rescan on done). */
        uint64_t perf_start = runtime_perf_section_begin();
        const CodeProvider *cp = code_provider_active();
        if (cp->poll_main) cp->poll_main();
        runtime_perf_section_end(perf_start,
                                 &g_runtime_perf.provider_poll_ticks);
    }

    if (!g_headless) {
        /* Pump SDL events to prevent window freeze. */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                if (psx_netplay_active()) {
                    netplay_soft_exit("sdl_window_close");
                    return ep;
                }
                psx_crash_trace_set_exit_origin("sdl_window_close");
                shutdown_runtime();
                std::exit(0);
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                refresh_player_devices();
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                bool ours = false;
                for (int s = 0; s < PSX_MAX_PLAYERS; s++) {
#if defined(PSX_SDL3)
                    if (ev.gdevice.which == g_players[s].instance) { ours = true; break; }
#else
                    if (ev.cdevice.which == g_players[s].instance) { ours = true; break; }
#endif
                }
                if (ours) {
                    close_controller();
                    refresh_player_devices();
                }
            } else if (ev.type == SDL_KEYDOWN) {
#if defined(PSX_SDL3)
                const SDL_Keymod mod = ev.key.mod;
                const SDL_Keycode key = ev.key.key;
                const int key_repeat = ev.key.repeat ? 1 : 0;
#else
                const Uint16 mod = ev.key.keysym.mod;
                const SDL_Keycode key = ev.key.keysym.sym;
                const int key_repeat = ev.key.repeat ? 1 : 0;
#endif
                if (key == SDLK_ESCAPE && psx_netplay_active()) {
                    netplay_soft_exit("netplay_escape");
                    return ep;
                }
                if (!key_repeat &&
                    host_keymap_match(HOST_KEYMAP_REWIND, (int)key, (int)mod)) {
                    psx_rewind_toggle();
                }
                else if (!key_repeat &&
                         host_keymap_match(HOST_KEYMAP_SAVE_STATE_MENU,
                                           (int)key, (int)mod)) {
                    savestate_menu_toggle(key);
                }
                else if (key == SDLK_c && (mod & KMOD_CTRL)) {
                    std::fprintf(stdout, "[DEBUG] Forzando reinserción de CD...\n");
                    debug_force_cd_reinsert();
                    host_osd_push("CD reinsert", 1500);
                }
                else if (!key_repeat &&
                         host_keymap_match(HOST_KEYMAP_DISPLAY_PERF, (int)key,
                                           (int)mod)) {
                    fps_telemetry_toggle();
                }
                /* Host volume: config.ini [KeyMap] VolumeUp/VolumeDown
                 * (defaults: keypad +/-). 5% steps; shows right-side bar. */
                else if (host_keymap_match(HOST_KEYMAP_VOLUME_UP, (int)key,
                                           (int)mod)) {
                    host_volume_adjust(+5);
                } else if (host_keymap_match(HOST_KEYMAP_VOLUME_DOWN, (int)key,
                                             (int)mod)) {
                    host_volume_adjust(-5);
                }
                /* Fullscreen toggle: Alt+Enter or Cmd/Ctrl+F. Toggles between
                 * windowed and the CONFIGURED tri-state mode (g_fullscreen: 1
                 * borderless desktop fullscreen keeping the desktop resolution
                 * and letterboxing the image, or 2 exclusive fullscreen — a
                 * real display-mode change). SDL_WINDOW_FULLSCREEN's bit is
                 * set in both SDL_WINDOW_FULLSCREEN and
                 * SDL_WINDOW_FULLSCREEN_DESKTOP, so testing just that bit
                 * detects "currently fullscreen, either mode". */
                else if (!key_repeat &&
                         host_keymap_match(HOST_KEYMAP_FULLSCREEN, (int)key,
                                           (int)mod)) {
                    Uint32 is_fs = SDL_GetWindowFlags(sdl_window) &
                                   SDL_WINDOW_FULLSCREEN;
                    if (is_fs) {
                        SDL_SetWindowFullscreen(sdl_window, 0);
                        host_osd_push("Windowed", 1500);
                    } else {
                        /* If the configured mode is "off", the hotkey still
                         * needs a mode to switch INTO — default to borderless,
                         * matching the historical (pre-tri-state) behaviour. */
                        Uint32 target = psx_fullscreen_flag_for_mode(g_fullscreen);
                        if (target == 0) target = SDL_WINDOW_FULLSCREEN_DESKTOP;
                        SDL_SetWindowFullscreen(sdl_window, target);
                        host_osd_push("Fullscreen", 1500);
                    }
                }
            }
        }
        savestate_menu_poll_toggle_buttons();
        rewind_poll_toggle_buttons();
        psx_rewind_note_frame();
        psx_rewind_present_tick((uint32_t)SDL_GetTicks());
        if (savestate_menu_open)
            savestate_menu_host_pause_loop();
        if (psx_rewind_is_open())
            rewind_host_pause_loop();
    }

    /* Sample each player's device and feed the matching SIO pad slot.
     * Debug server input override (when active) drives port 1 only. With
     * g_low_latency_input this early sample is re-done after the pacer wait
     * (below) for the interactive present path; it still covers the turbo /
     * FMV-skip paths that early-return before pacing.
     *
     * Delay-sync netplay host FPS (MotK FMV): finish → present → admit/pace.
     * Admit/pace run in sdl_vblank_present() AFTER this body returns so episode
     * resume longjmp does not cross C++ destructors. Early returns still set
     * ep.do_epilogue so admit runs every tick (including skip-present paths).
     * Offline keeps pace-before-present. Barrier wait stays UDP poll — do
     * not pair this order with SDL_Delay(0) busy-spin (tick-0 hang). */
    ep.do_epilogue = psx_netplay_active() != 0 ? 1 : 0;
    ep.override = override;

    /* Turbo-active / multitap arming share game-started detection. */
    extern int fntrace_is_game_started(void);

    if (psx_netplay_active()) {
        psx_netplay_finish_frame();
    } else {
        /* Offline N-pad: enable multitap only once the game EXE is running.
         * Port comes from game.toml [controller] multitap_port (default Port 1;
         * BPE uses Port 2). Empty tap slots are OK — P1 may be the standalone
         * pad on the other port. */
        if (g_offline_pad_count >= 3 && fntrace_is_game_started() &&
            !sio_get_multitap()) {
            sio_set_multitap(1);
            /* Tap seats drop to plain digital unless multitap_analog is on. */
            for (int s = 0; s < PSX_MAX_PLAYERS; ++s) {
                if (!sio_pad_on_multitap(s)) continue;
                if (sio_get_multitap_analog()) continue;
                sio_set_pad_config_capable(s, 0);
                sio_set_pad_analog(s, 0, 0x80, 0x80, 0x80, 0x80);
            }
        }
        /* Solo resim self-check replay republishes the recorded rows itself —
         * live sampling must not touch SIO during the replay window. */
        if (!psx_selfcheck_replay_input()) {
            if (g_headless)
                sample_headless_pad_into_sio(override);
            else
                sample_pad_into_sio(override);
        }
        /* Offline vblank boundary: record/replay/compare (PSX_RB_SELFCHECK).
         * Defer opening a window while multitap arming is still pending —
         * the arm writes SIO once, host-latched, and would not replay. */
        psx_selfcheck_finish_frame(
            (g_offline_pad_count >= 3 && !sio_get_multitap()) ? 1 : 0);
    }
    if (!g_headless) update_controller_rumble();

    /* Latency ring: open this present cycle's slot, stamping when input was
     * sampled into SIO.  Always-on; queried via the debug server "latency". */
    latency_ring_frame_begin();

    /* Post-savestate CD delay boost (ReadTOC/seek clamp window).
     * Skip during rollback resim — boost is host-local and not in the CD snap. */
    if (!psx_netplay_is_resimulating() && !psx_selfcheck_resim_active())
        cdrom_savestate_boost_vblank();

    /* Turbo-active test shared by the pacing/present gate below. */
    int turbo_loads_active = 0;
    int logical_load_active = fntrace_is_game_started() &&
        (cdrom_load_in_progress() || cdrom_savestate_cd_wait_active());
    int load_run_value = 0;
    static int load_run = 0;
    static int release_run = 0;
    if (g_turbo_loads_enabled && !psx_netplay_active() &&
        !psx_selfcheck_resim_active()) {
        /* Require a short sustained predicate on entry, then retain turbo over
         * a short false gap after it has engaged. Netplay / selfcheck keep
         * wall-pacing off via their own gates; turbo engage hysteresis must
         * not leak host ambient across resim passes. */
        if (logical_load_active) {
            if (load_run < (1 << 20)) load_run++;
            if (load_run >= TURBO_LOADS_ENGAGE_FRAMES || release_run > 0) {
                turbo_loads_active = 1;
                release_run = g_turbo_load_release_frames;
            }
        } else {
            load_run = 0;
            if (release_run > 0) {
                turbo_loads_active = 1;
                release_run--;
            }
        }
        load_run_value = load_run;
    } else {
        load_run = 0;
        release_run = 0;
    }
    /* HLE boot-skip window: from reset until the game entry PC first
     * dispatches, run unpaced so the (shell-skipped) BIOS kernel init +
     * SYSTEM.CNF + game EXE load compress to host speed. This replaces the
     * old fast_boot snapshot restore; all guest timing is authentic. */
    if (psx_bios_hle_boot_turbo_active())
        turbo_loads_active = 1;
    probe_turbo = turbo_loads_active;

    load_transition_note(cdrom_data_read_active(), logical_load_active,
                         turbo_loads_active, load_run_value);



    /* FMV auto-skip ([video] auto_skip_fmv). A streaming FMV is XA audio + MDEC
     * video together. Detect "MDEC produced a frame since the last vblank AND XA
     * is streaming"; a short hold rides out brief inter-frame gaps. The present/
     * pacing safety-net + audio mute below hide the one or two transition frames.
     *
     * End the movie the GAME's own way, chosen per game:
     *  - fmv_skip_total_table set (Tomba): write the CURRENT movie's per-movie
     *    frame-total down to fmv_skip_end_total, so the player's MDEC loop
     *    (FUN_8001efe8: teardown when streamed frame# >= total - 3) terminates on
     *    its next frame — a NATURAL end that reaches EVERY movie, including ones
     *    whose caller never polls the skip button. Only the active movie's table
     *    entry is touched (via the movie-id byte), so nothing else is disturbed.
     *  - no table configured (generic): hold START so a movie whose handler polls
     *    the pad aborts itself (can't reach unskippable movies). */
    int fmv_skip_active = 0;
    /* Never inject START / poke movie totals under netplay — host-side skip
     * forks peers (and stomps SIO after sealed publish during resim). */
    if (g_auto_skip_fmv && !psx_netplay_active() &&
        !psx_selfcheck_resim_active()) {
        uint32_t mc = mdec_get_decode_count();
        int mdec_decoding = (mc != s_fmv_skip_last_mdec);
        s_fmv_skip_last_mdec = mc;
        int xa = cdrom_xa_stream_active();
        /* fmv_skip_no_xa: silent RAM-preloaded movies (no XA stream) never
         * satisfy the MDEC+XA detector; with the per-game opt-in, MDEC
         * activity alone qualifies. */
        int detected = mdec_decoding && (xa || g_fmv_skip_no_xa);
        if (detected) s_fmv_skip_hold = (!xa && g_fmv_skip_no_xa)
                                     ? g_fmv_skip_no_xa_hold : 4;
        else if (s_fmv_skip_hold > 0) s_fmv_skip_hold--;
        fmv_skip_active = (s_fmv_skip_hold > 0) && (xa || g_fmv_skip_no_xa);
        if (fmv_skip_active) {
            if (g_fmv_skip_total_table) {
                /* End the active movie via its own frame-count teardown. */
                uint8_t mid = psx_read_byte(g_fmv_skip_movie_id);
                psx_write_half(g_fmv_skip_total_table + (uint32_t)mid * 2u,
                               (uint16_t)g_fmv_skip_end_total);
            } else {
                /* Generic fallback: hold START (PSX pad word is active-low; START
                 * is bit 3) so the game's FMV handler aborts the movie itself. */
                sio_set_pad_state_slot(0, (uint16_t)~(1u << 3));
            }
        }
    }

#ifndef PSX_SDL_NO_AUDIO
    /* Optional turbo host sink advances the canonical SPU on the exact guest
     * sample budget but drops accelerated output before SDL. This is distinct
     * from the old mute/freeze model: voice and CD state never pause. FMV skip
     * retains the hard mute because it deliberately fast-forwards a movie.
     * Rollback resim: skip host audio pump (uncapped frames) so SPU/CD audio
     * paths cannot fork peers during the episode window. */
    if (!psx_netplay_is_resimulating() && !psx_selfcheck_resim_active()) {
        sdl_audio_update(fmv_skip_active,
                         turbo_loads_active && g_turbo_audio_sink_enabled);
    }
#endif

    if (g_headless) {
        ep.skip_pace = 1;
        return ep;
    }

    /* TCP turbo is for automated validation and trace capture. It keeps the
     * simulation advancing and the debug server polling, but removes frontend
     * presentation and wall-clock pacing. */
#ifndef PSX_NO_DEBUG_TOOLS
    if (debug_server_turbo_enabled()) {
        ep.skip_pace = 1;
        return ep;
    }
#endif

    bool manual_turbo_active = false;
    bool turbo_load_paced = false;

    /* Manual fast-forward: bounded by default so the game visibly advances and
     * audio is less hostile. PSX_FAST_FORWARD_SPEED=2..16 changes the cap;
     * PSX_FAST_FORWARD_SPEED=max restores the old unbounded simulation rate. */
    {
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        static int turbo_skip = 0;
        static int turbo_was_down = 0;
        /* PSX_TURBO_HOLD=1: benchmarking override — behave as if the turbo key
         * were held for the whole session. Headless fixtures (PSX_LOAD_SLOT)
         * have no keyboard, and measuring the fast-forward ceiling / profiling
         * under turbo requires the exact production code path, not a synthetic
         * unpaced mode. */
        static int turbo_hold_env = -1;
        if (turbo_hold_env < 0) {
            const char *th = std::getenv("PSX_TURBO_HOLD");
            turbo_hold_env = (th && th[0] == '1') ? 1 : 0;
        }
        if (turbo_hold_env ||
            host_keymap_down(HOST_KEYMAP_TURBO, keys, (int)SDL_GetModState())) {
            const int mult = manual_fast_forward_multiplier();
            const int present_every = (mult < 0) ? 4 : (mult <= 4 ? 2 : 4);
            manual_turbo_active = true;
            if (!turbo_was_down) {
                char msg[40];
                if (mult < 0)
                    snprintf(msg, sizeof(msg), "Fast forward: max");
                else
                    snprintf(msg, sizeof(msg), "Fast forward: %dx", mult);
                host_osd_push(msg, 900);
                std::fprintf(stdout,
                             "[pace] manual fast-forward ENGAGED (%s, host %ums)\n",
                             msg, (unsigned)SDL_GetTicks());
                /* Release the vsync ceiling for the duration of the hold. */
                g_turbo_vsync_forced_off = 1;
                apply_present_cadence();
            }
            turbo_was_down = 1;
            if (mult >= 2 && g_frame_period_ms > 0.0) {
                uint64_t perf_start = runtime_perf_section_begin();
                frame_pacer_wait(&s_frame_pacer,
                                 g_frame_period_ms / (double)mult);
                runtime_perf_section_end(perf_start,
                                         &g_runtime_perf.pacer_ticks);
                latency_ring_mark(LAT_PACED);
            }
            turbo_skip = (turbo_skip + 1) % present_every;
            if (turbo_skip != 0) {
                ep.skip_pace = 1;
                return ep;  /* skip render this frame */
            }
        } else {
            turbo_skip = 0;
            if (turbo_was_down && g_turbo_vsync_forced_off) {
                /* Restore the configured present cadence on release. */
                g_turbo_vsync_forced_off = 0;
                apply_present_cadence();
                std::fprintf(stdout,
                             "[pace] manual fast-forward released (host %ums)\n",
                             (unsigned)SDL_GetTicks());
            }
            turbo_was_down = 0;
        }
    }

    /* Turbo-through-loads (step 4, OPT-IN via game.toml [runtime]
     * turbo_loads): while the game is loading — CD data stream active,
     * XA/FMV excluded, post-BIOS-handoff only — skip wall-clock pacing and
     * most presents so the guest runs at host speed. Step-3 measurement
     * showed loads are paced by the game's own per-sector processing at
     * real time (2.2-4.8 sectors/frame against a 32-256 IRQ budget), so
     * host-speed execution is the lever that compresses load wall-time.
     * Presents 1-in-30 so visual progress stays visible. */
    if (turbo_loads_active) {
        g_turbo_loads_frames++;
        /* A mod may request a bounded wall-clock multiplier instead of the
         * legacy unpaced host-speed path. Pace every simulated guest frame at
         * the divided period; the presentation skip below remains independent
         * so rendering cost does not distort the selected multiplier. */
        if (g_turbo_load_wall_multiplier >= 2 &&
            g_frame_period_ms > 0.0) {
            uint64_t perf_start = runtime_perf_section_begin();
            frame_pacer_wait(
                &s_frame_pacer,
                g_frame_period_ms / (double)g_turbo_load_wall_multiplier);
            runtime_perf_section_end(
                perf_start, &g_runtime_perf.pacer_ticks);
            latency_ring_mark(LAT_PACED);
            turbo_load_paced = true;
        }
        const int TL_PRESENT_EVERY = 30;
        s_turbo_present_skip = (s_turbo_present_skip + 1) % TL_PRESENT_EVERY;
        if (s_turbo_present_skip != 0) {
            ep.skip_pace = 1;
            return ep;
        }
    }

    /* FMV auto-skip: run uncapped (no wall-clock pacing) and suppress nearly all
     * presents so the fast-forwarded movie is invisible. Present 1-in-30 only so
     * the window keeps pumping and never looks hung during the brief skip. */
    if (fmv_skip_active) {
        const int FMV_PRESENT_EVERY = 30;
        s_fmv_skip_present_skip = (s_fmv_skip_present_skip + 1) % FMV_PRESENT_EVERY;
        if (s_fmv_skip_present_skip != 0) {
            ep.skip_pace = 1;
            return ep;
        }
    }

    /* FMV: under PAL x2, present 1 of every two depth24 vblanks while MDEC is
     * hot. This keeps the guest/audio clock at 100 Hz while retaining the
     * authored PAL 50-Hz movie cadence. Netplay uses the same skip point,
     * with its configurable divisor.
     * §98: default N=2 (was 4). 1/4 made present_gap_p95≈80–100 ms look like
     * hitches; SW scanout is batched so 1/2 is affordable. Admit + wall pace
     * still run every tick. During MDEC-idle cutover present every frame.
     * Override: PSX_NET_FMV_PRESENT_DIV (1=every frame, 2=default, 4=legacy). */
    if ((psx_netplay_active() ||
         (g_mod_native_vblank_rate && g_mod_native_vblank_active)) &&
        gpu_display_is_depth24() &&
        mdec_recently_active(8)) {
        static int s_fmv_present_div = -1;
        int div;
        if (g_mod_native_vblank_rate && g_mod_native_vblank_active) {
            div = 2;
        } else if (s_fmv_present_div < 0) {
            const char *e = getenv("PSX_NET_FMV_PRESENT_DIV");
            unsigned v = 2u;
            if (e && e[0] && sscanf(e, "%u", &v) == 1 && v >= 1u && v <= 8u)
                s_fmv_present_div = (int)v;
            else
                s_fmv_present_div = 2;
            div = s_fmv_present_div;
        } else {
            div = s_fmv_present_div;
        }
        if (div <= 1) {
            s_netplay_depth24_present_skip = 0;
        } else if (s_netplay_depth24_present_skip > 0) {
            s_netplay_depth24_present_skip--;
            /* Offline PAL x2 still paces every guest tick; only the movie
             * presentation is skipped. Netplay performs its epilogue pace
             * separately and keeps the historical skip behavior. */
            if (!psx_netplay_active() && !psx_selfcheck_resim_active()) {
                refresh_frame_pacer_period();
                frame_pacer_wait(&s_frame_pacer, present_frame_period_ms());
            }
            return ep;
        } else {
            s_netplay_depth24_present_skip = div - 1;
        }
    } else {
        s_netplay_depth24_present_skip = 0;
    }

    /* Offline wall-clock pacing before present. Skipped when driver vsync
     * owns cadence (panel near the live CRTC rate) so the two waits cannot
     * double-block.
     * Netplay paces in the epilogue AFTER present so Swap overlaps the
     * peer's guest quantum. Self-check replay runs uncapped like netplay
     * resim. */
    if (!psx_netplay_active() && !psx_selfcheck_resim_active()) {
        uint64_t perf_start = runtime_perf_section_begin();
        refresh_frame_pacer_period();
        if (!manual_turbo_active && !turbo_load_paced && present_should_wall_pace())
            frame_pacer_wait(&s_frame_pacer, present_frame_period_ms());
        runtime_perf_section_end(perf_start, &g_runtime_perf.pacer_ticks);
        latency_ring_mark(LAT_PACED);

        /* Low-latency input: the early sample above is now ~one pacer-wait old.
         * Refresh the device state and re-sample right before present so the next
         * CPU frame reads near-fresh input (the dominant input->photon cost on a
         * vsync-light box). Re-stamp the ring's input mark to measure from here.
         * Self-check record keeps pads boundary-latched (netplay tick
         * semantics) — the mid-frame re-sample would fork the resim. */
        if (g_low_latency_input && !psx_selfcheck_input_locked()) {
            SDL_GameControllerUpdate();  /* refresh pad state after the wait */
            SDL_PumpEvents();            /* refresh keyboard state */
            sample_pad_into_sio(override);
            latency_ring_restamp_input();
        }
    }

    /* Mod hooks. Run after all normal input sampling. */
    mod_call_frame_hooks();

    /* Resize-driven mode updates the pending aspect during BIOS boot too, but
     * the actual wide compositor remains disengaged until game entry. */
    update_adaptive_widescreen();

    /* Depth24 GP1(07h) retarget (MotK intro→crawl): keep the prior Swap for a
     * few vblanks so stale trailing VRAM never flashes on the right edge.
     * Must tick every present — gpu.c arms s_d24_present_hold and also
     * freezes upload-span tracking while hold > 0; without this call the hold
     * sticks and depth24_fix_trailing_margin blanks the whole FMV forever. */
    if (gpu_depth24_present_hold_tick())
        return ep;
    /* Engage widescreen at game entry: BIOS boot stays authentic 4:3. */
    if (!g_ws_engaged) {
        extern int fntrace_is_game_started(void);
        if (fntrace_is_game_started()) {
            g_ws_engaged = true;
            g_ws_projection_mode = -1;
            refresh_widescreen_projection();
        }
    } else {
        refresh_widescreen_projection();
    }

    /* Rollback resim (§33/§47): short catch-up keeps hold-last; long catch-up
     * periodically presents live Replay VRAM so the display shows progress
     * while sim stays uncapped. TipHold invent-cap stall uses hold-last from
     * the admit spin (no guest vblank).
     *
     * §74: ownership-chain Replay treadmills at wire pace during fights
     * (target extends per wire arrival, remaining ≤ 24 forever), so the
     * blanket hold-last froze the display 135–212 ms per ~12-tick episode
     * and then skipped ahead ("delay-sync burst" feel). Split by a display
     * watermark: frames the user has NEVER seen (resim sim beyond the
     * highest sim ever presented) are new forward progress — present them
     * live every vblank. Frames at/below the watermark are a rewound
     * re-play; hold-last exactly as before so §63 edge-input re-show
     * (pad-log DUP_SIO) stays impossible. */
    if (psx_netplay_is_resimulating()) {
        uint32_t rem = psx_netplay_rb_confirmed_remaining();
        uint32_t sim = psx_netplay_sim_tick();
        int beyond_shown = sim != 0u && sim != 0xffffffffu &&
                           sim > s_netplay_present_sim_watermark;
        /* §99: FMV media Replay is CD/MDEC-bound — skip dual-raster present
         * entirely (hold-last). Live FMV present + catchup gate still apply
         * once media ends; near-tip media loads are short enough that a frozen
         * frame for a few resim ticks is preferable to 1–3s GL hitch. */
        if (psx_netplay_rb_fmv_media_active()) {
            netplay_hold_last_present_tick();
            return ep;
        }
        if (!beyond_shown && !netplay_replay_catchup_should_live_present(rem)) {
            netplay_hold_last_present_tick();
            return ep;
        }
        /* Fall through to real VRAM present (new frame, or wall-clock gate). */
    }
    {
        /* Raise the display watermark for any frame that falls through to a
         * real present (live or Replay). Hold-last paths return above and
         * never raise it. */
        uint32_t sim = psx_netplay_sim_tick();
        if (sim != 0u && sim != 0xffffffffu &&
            sim > s_netplay_present_sim_watermark)
            s_netplay_present_sim_watermark = sim;
    }

    /* ---- Display from our VRAM ---- */
    probe_reached = 1;
    uint32_t w = 0, h = 0;
    uint32_t present_w = 0;  /* display width actually presented (w + native-wide EXTRA) */
    int active_scale = 1;   /* hi-res mirror used only for 15-bit display */
    bool scene_content_4_3 = false; /* menu/FMV/boot: native content correction */
    bool content_4_3 = false;       /* final inner-content policy after fallback */
    bool depth24_frame = false;
    uint32_t depth24_display_x = 0;
    bool local_viewport_crop_applied = false;
    if (s_force_present_after_load && g_gl_active)
        gl_renderer_flush_cpu_uploads();
    {
        GpuDisplayInfo di;
        gpu_get_display_info(&di);
        depth24_frame = di.depth24 != 0;
        depth24_cutover_tick(depth24_frame ? 1 : 0);
        if (di.disabled || di.width == 0 || di.height == 0) {
            smooth_60_present(nullptr, 0, 0, false);
            present_ring_commit(PRES_PATH_BLANK, (uint16_t)di.width,
                                (uint16_t)di.height, 0);
#ifndef PSX_SDL_NO_RENDER
            /* After savestate load, always restage once even if we blanked
             * earlier in the session — otherwise the window keeps a stale
             * pre-load frame while vblanks (and FPS) keep advancing. */
            if (!s_disabled_frame_presented || s_force_present_after_load) {
                s_disabled_frame_presented = true;
                s_force_present_after_load = false;
                if (g_gl_active) {
                    gl_renderer_present_blank();
                } else if (g_vk_active) {
                    vk_renderer_present_blank();
                } else {
                    if (!sdl_renderer && ensure_sw_sdl_present() != 0)
                        return ep;
                    if (sdl_renderer) {
                        SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
                        SDL_RenderClear(sdl_renderer);
                        host_osd_draw_sdl(sdl_renderer);
                        SDL_RenderPresent(sdl_renderer);
                    }
                }
            }
#endif
            return ep;
        }
        s_disabled_frame_presented = false;
        s_force_present_after_load = false;
        w = di.width; h = di.height;
        depth24_display_x = di.display_x;
        /* Scene classification controls content correction only. BIOS, FMV and
         * full-2D screens retain an undistorted 4:3 safe area; it must never
         * override the user-configured outer presentation canvas. */
        scene_content_4_3 = di.depth24 || !g_ws_engaged ||
                            gpu_ws_content_native_43() != 0;
        /* 2D screens (title, menus, loading) are deliberately kept at 4:3 by the
         * 3D-gated widescreen layer, which on a 32:9 display leaves them as a
         * small island in a mostly black window. Extend their own edge columns
         * across the margins instead. Never for MDEC video: those edge pixels
         * are real picture content and smear.
         *
         * Deliberately NOT gated on g_ws_engaged. Engagement means "the GTE
         * squash is running on gameplay frames", and it is false during boot,
         * the memory-card load and the title screen -- precisely the screens
         * this is for. gl_renderer_present only fills when the present is
         * actually pillarboxed, so a 4:3 window has no margins to fill and this
         * costs nothing there. */
        if (g_gl_active)
            gl_renderer_set_pillarbox_edge_fill(
                g_ws_menu_edge_fill_flag && !di.depth24);
        /* MDEC movies are already decoded at their authored cadence and are
         * CPU/upload heavy. High-refresh crossfades only contend with decoding
         * and can starve audio, so present native-4:3/MDEC phases directly.
         * The classification catches the transition frame before the first
         * decode; the activity stamp also covers authentic 4:3 configurations. */
        if (g_gl_active)
            gl_renderer_set_interpolation_suspended(
                scene_content_4_3 || mdec_recently_active(2));
        const int local_viewport_slot = netplay_local_viewport_slot();
        const bool local_viewport_crop = local_viewport_slot >= 0;
        bool local_viewport_wide =
            local_viewport_crop && g_ws_engaged && ws_native_wide_active() &&
            gr_wide_supported() && gpu_ws_netplay_local_viewport_width() > 0;

        /* Canonical present width. Native-wide does NOT widen the canonical read
         * (that bled across adjacent framebuffers); it composites into a separate
         * wide surface and presents from there instead (see gpu wide compositor). */
        present_w = w;

        /* Native-wide present: on a game frame, if the active backend has the
         * wide compositor, present the wider surface (canonical width + EXTRA)
         * from the displayed buffer's surface. FMV/menu frames stay 4:3. */
        bool wide_present = (!scene_content_4_3 && !di.depth24 && g_ws_engaged &&
                             ws_native_wide_active() && gr_wide_supported() &&
                             (!local_viewport_crop || local_viewport_wide));
        if (wide_present) {
            present_w = local_viewport_wide
                ? (uint32_t)gpu_ws_netplay_local_viewport_width()
                : (w + (uint32_t)ws_nw_extra());
        }

        /* Native-wide invariant: canonical (320-wide) content is NEVER
         * stretched across the wide window — a game frame that cannot present
         * wide (compositor unsupported, or the surface fallback below)
         * pillarboxes 4:3 like FMV/menus instead. Only squash mode (1) may
         * stretch: its canonical content is pre-squashed FOR the stretch. */
        const bool nw_requires_safe_fallback =
            g_ws_engaged && g_ws_native_wide;
        content_4_3 = scene_content_4_3 ||
                      (nw_requires_safe_fallback && !wide_present);

        /* Ring the classification now that it's final (only the software/CPU
         * wide path below can still fall back — it amends this entry). A
         * CANONICAL game frame on a widescreen window IS the stretch. */
        PresRingEntry* pres_entry = present_ring_commit(
            scene_content_4_3 ? PRES_PATH_CONTENT_43
                      : (wide_present ? PRES_PATH_WIDE : PRES_PATH_CANONICAL),
            (uint16_t)w, (uint16_t)h, (uint16_t)present_w);

        /* OpenGL: 15-bit frames ALWAYS present straight from the authoritative
         * VRAM FBO — one deterministic path (the old per-frame FBO-vs-CPU
         * alternation caused the menu seam/jitter). 24-bit (FMV) frames and
         * the PSX_GL_FORCE_CPU_PRESENT diagnostic use the CPU readout below.
         * depth24: do NOT sync_cpu (FBO readback can clobber packed RGB888) and
         * do NOT flush_cpu_uploads (MDEC already wrote the CPU mirror; forcing
         * FBO uploads every frame cut MotK intro from ~50 to ~30 FPS). */
#ifndef PSX_SDL_NO_RENDER
        if (g_gl_active && g_gl_fbo_present && !di.depth24 &&
            !local_viewport_crop) {
            if (wide_present) {
                /* GPU-direct native-wide present: blit the displayed buffer's
                 * wide FBO straight to the window (GPU-side, like the canonical
                 * present_vram). This avoids the glFinish + glReadPixels CPU
                 * round-trip the old path did EVERY frame — that readback was the
                 * GL-only slowdown (SW's wide path is pure CPU, no GPU sync, so
                 * SW stayed smooth). Falls through to the CPU readout path only if
                 * the wide surface for this buffer doesn't exist yet. */
                if (gl_renderer_present_wide_fbo((int)di.display_x, (int)di.display_y,
                                                 (int)h, g_video_aa ? 1 : 0)) {
                    netplay_note_present();
                    return ep;
                }
            } else {
                gl_renderer_present_vram((int)di.display_x, (int)di.display_y,
                                         (int)present_w, (int)h, g_video_aa ? 1 : 0,
                                         content_4_3 ? 1 : 0);
                netplay_note_present();
                return ep;
            }
        }
        if (g_gl_active && !di.depth24) gl_renderer_sync_cpu();
        /* Vulkan owns every frame: 15-bit frames present straight from the GPU
         * VRAM image (deterministic blit, no readback), mirroring the GL path;
         * 24-bit (FMV) frames go through the CPU present (Phase 3). The Vulkan
         * window has no SDL_Renderer, so we must never fall through below. */
        if (g_vk_active && !local_viewport_crop) {
            if (di.depth24) {
                /* 24-bit (FMV): packed RGB lives in the CPU mirror — do NOT
                 * sync_cpu (FBO readback clobbers RGB888). Batch per-scanline
                 * (see gpu_depth24_present_row) instead of a per-pixel call
                 * chain — this loop was the FMV present-side cost. */
                for (uint32_t y = 0; y < h; y++)
                    gpu_depth24_present_row(&di, y, sdl_pixel_buf + (size_t)y * present_w,
                                            present_w);
                /* Trailing / cutover blank: black-fill uncovered RGB cols
                 * inside the full-width buffer — never shrink present width. */
                depth24_fix_trailing_margin(sdl_pixel_buf, present_w, h,
                                             di.display_x);
                vk_renderer_present_cpu(sdl_pixel_buf, (int)present_w, (int)h,
                                        0 /* nearest */, content_4_3 ? 1 : 0);
            } else if (wide_present &&
                       vk_renderer_present_wide((int)di.display_x, (int)di.display_y,
                                                (int)h, g_video_aa ? 1 : 0)) {
                /* presented wide */
            } else {
                vk_renderer_present_vram((int)di.display_x, (int)di.display_y,
                                         (int)present_w, (int)h, g_video_aa ? 1 : 0,
                                         (content_4_3 ||
                                          nw_requires_safe_fallback) ? 1 : 0);
            }
            netplay_note_present();
            return ep;
        }
#endif

        /* The hi-res mirror is a 15-bit copy of VRAM; 24-bit display (FMV)
         * reads packed bytes the mirror can't represent, so fall back to the
         * native path for those frames (the present filter still upscales).
         * SW-only netplay present: native scanout. Dual-raster FBO present
         * already returned above at GL SSAA. */
        if (netplay_cpu_auth_gpu() && !netplay_gl_dual_quality())
            active_scale = 1;
        else
            active_scale = (g_video_scale > 1 && !di.depth24) ? g_video_scale : 1;

        if (wide_present) {
            /* The wide compositor surface is at the renderer's REAL internal
             * scale, which gr_render_wide_display uses for its readback pitch.
             * Use gr_scale() (== that internal scale) rather than g_video_scale:
             * on the GL backend g_video_scale can be stale (1) while the renderer
             * supersamples at 2, and a mismatch makes the present read the wide
             * buffer at the wrong width (a magnified top-left slice). gr_scale()
             * is guaranteed to match the readback. Falls back to the canonical
             * hires path if the displayed buffer has no surface yet. */
            int s = gr_scale();
            int sw = (int)present_w * s;
            int base_x = local_viewport_wide
                ? gpu_ws_netplay_local_viewport_base_x()
                : (int)di.display_x;
            int n = gr_render_wide_display(sdl_pixel_buf, (int)(sw * sizeof(uint32_t)),
                                           base_x, (int)di.display_y, (int)h);
            if (n > 0) {
                active_scale = s;
            } else {
                wide_present = false;
                local_viewport_wide = false;
                present_w = w;
                pres_entry->path          = PRES_PATH_CANONICAL;
                pres_entry->wide_fellback = 1;
                pres_entry->present_w     = (uint16_t)present_w;
            }
        }
        /* Re-evaluate after a CPU wide-surface fallback. The outer canvas is
         * unchanged; only the inner source becomes the centered 4:3 fallback. */
        content_4_3 = scene_content_4_3 || di.depth24 ||
                      (nw_requires_safe_fallback && !wide_present);
        if (!wide_present) {
            /* Never hires-present under netplay CPU-auth (even if settings say
             * 4× — that preference is offline-only). */
            if (active_scale > 1 && !netplay_cpu_auth_gpu()) {
                int sw = (int)present_w * active_scale;
                gr_render_display_hires(sdl_pixel_buf, (int)(sw * sizeof(uint32_t)),
                                        (int)di.display_x, (int)di.display_y,
                                        (int)present_w, (int)h);
            } else if (di.depth24) {
                /* Batch per-scanline (see gpu_depth24_present_row) — the
                 * per-pixel call chain was the dominant FMV present cost
                 * (3x the VRAM touches of the 16-bit path below). */
                for (uint32_t y = 0; y < h; y++)
                    gpu_depth24_present_row(&di, y, sdl_pixel_buf + (size_t)y * present_w,
                                            present_w);
            } else {
                for (uint32_t y = 0; y < h; y++) {
                    for (uint32_t x = 0; x < present_w; x++) {
                        sdl_pixel_buf[y * present_w + x] = gpu_display_pixel_argb(&di, x, y);
                    }
                }
            }
        }

        /* Depth24 trailing / cutover blank: MotK CRTC is 512 RGB but uploads
         * may not cover the right side yet (colorful junk flash). Fix pixels
         * in-place at full present_w — never crop the GL/SDL draw width. */
        if (di.depth24 && active_scale == 1 && !wide_present)
            depth24_fix_trailing_margin(sdl_pixel_buf, present_w, h,
                                         di.display_x);

        int present_px_w = (int)present_w * active_scale;
        int present_px_h = (int)h * active_scale;
        if (!local_viewport_wide &&
            crop_present_to_netplay_local_viewport(sdl_pixel_buf,
                                                   &present_px_w,
                                                   present_px_h)) {
            content_4_3 = false;
            local_viewport_crop_applied = true;
        }

        smooth_60_present(sdl_pixel_buf,
                          (uint32_t)present_px_w,
                          (uint32_t)present_px_h,
                          !g_gl_active && !g_vk_active && !di.depth24 &&
                              !scene_content_4_3);

        /* Frame blending (CRT-persistence masker for 30fps double-buffered
         * content). Some games (e.g. Crash Bash menus/characters) leave a
         * dynamic object in only one of the two display buffers per 30fps cycle,
         * so it strobes on/off as the display alternates buffers — the static
         * background, present in both, stays put. Real CRTs hid this via phosphor
         * persistence; accurate emulators either time it away (cycle accuracy) or
         * offer a "blend frames" option that averages the last two presented
         * frames. This is the latter: presentation only, game logic untouched.
         * Guarded to the plain software present path (no GL/VK/wide/hires/FMV) so
         * the buffer is a simple present_w*h ARGB grid. PSX_FRAME_BLEND=0 off. */
        {
            static int blend_cfg = -1;   /* -1 unresolved, 0 off, 1 on */
            if (blend_cfg < 0) {
                const char* e = getenv("PSX_FRAME_BLEND");
                blend_cfg = (e && e[0] == '1') ? 1 : 0;   /* default off (masker only) */
            }
            const bool simple_sw = !g_gl_active && !g_vk_active && !wide_present &&
                                   !di.depth24 && active_scale == 1;
            if (blend_cfg && simple_sw &&
                !g_smooth_60fps.load(std::memory_order_acquire)) {
                static uint32_t prev_buf[640 * 512];
                static uint32_t prev_px = 0;
                const uint32_t npx = local_viewport_crop_applied
                                       ? (uint32_t)(present_px_w * present_px_h)
                                       : present_w * h;
                if (npx <= (uint32_t)(640 * 512)) {
                    if (prev_px == npx) {
                        for (uint32_t i = 0; i < npx; i++) {
                            const uint32_t a = sdl_pixel_buf[i];
                            const uint32_t b = prev_buf[i];
                            prev_buf[i] = a;   /* store this frame (pre-blend) */
                            sdl_pixel_buf[i] =
                                0xFF000000u |
                                (((a & 0xFEFEFEu) >> 1) + ((b & 0xFEFEFEu) >> 1));
                        }
                    } else {
                        /* Resolution changed / first frame: seed, no blend. */
                        for (uint32_t i = 0; i < npx; i++) prev_buf[i] = sdl_pixel_buf[i];
                        prev_px = npx;
                    }
                }
            }
        }
    }

    /* Update only the active display rectangle. The backing texture is sized
     * 640x512 (times the supersampling factor), while games can switch to
     * smaller modes such as 320x224 for FMV; presenting the full texture would
     * leave the active image stuck in the upper-left portion of the window. */
#ifndef PSX_SDL_NO_RENDER
    int src_w = (int)present_w * active_scale;
    int src_h = (int)h * active_scale;
    if (local_viewport_crop_applied && src_w >= 2)
        src_w /= 2;
    if (g_gl_active) {
        /* OpenGL present: upload the active display rect and draw a full-screen
         * quad. Either SwapWindow vsync OR the wall-clock pacer owns timing,
         * never both. 24-bit (FMV) frames pin to native 4:3. */
        /* FMV follows the same AA setting as everything else. It used to be
         * pinned to nearest because plain GL_LINEAR fringed the right edge of
         * low-res 24-bit scanouts into adjacent (often garbage) texels — but
         * gl_renderer_present now half-texel-insets its UV rect (and crops the
         * depth24 margin via content_w), which is exactly that bleed, and it
         * reconstructs a filtered low-res source properly (PSX_FMV_FILTER,
         * default bicubic) instead of smearing whole texels. Measured: the
         * right-edge jump the old comment describes is 0 with filtering on.
         * Set video AA off to get nearest back. */
        gl_renderer_set_fmv_filter(g_video_fmv_filter);
        const uint32_t depth24_w = depth24_frame
            ? gpu_depth24_rgb_limit(depth24_display_x, present_w)
            : 0u;
        gl_renderer_present(sdl_pixel_buf, src_w, src_h,
                            g_video_aa ? 1 : 0,
                            content_4_3 ? 1 : 0,
                            depth24_w ? (int)depth24_w : 0);
        netplay_note_present();
    } else {
    if ((!sdl_renderer || !sdl_texture) && ensure_sw_sdl_present() != 0)
        return ep;
    if (!sdl_renderer || !sdl_texture)
        return ep;
    SDL_Rect src = { 0, 0, src_w, src_h };
    SDL_UpdateTexture(sdl_texture, &src, sdl_pixel_buf,
                      (int)(src_w * sizeof(uint32_t)));
    /* Short FMV bands only update [0..src_h). Linear sampling at the bottom
     * edge blends with uninitialized texels below (X11 SDL backends often
     * show that as a thin white strip; Wayland may not). Pad one black row
     * so any residual linear fringe is black, matching the letterbox. */
    const int tex_scale = netplay_cpu_auth_gpu() ? 1 : g_video_scale;
    const int tex_h = 512 * tex_scale;
    if (src_w > 0 && src_h > 0 && src_h < tex_h) {
        static uint32_t s_black_pad[640 * 4]; /* covers g_video_scale <= 4 */
        const int pad_cap = (int)(sizeof(s_black_pad) / sizeof(s_black_pad[0]));
        const int pad_w = (src_w <= pad_cap) ? src_w : pad_cap;
        for (int i = 0; i < pad_w; i++)
            s_black_pad[i] = 0xFF000000u;
        SDL_Rect pad = { 0, src_h, pad_w, 1 };
        SDL_UpdateTexture(sdl_texture, &pad, s_black_pad,
                          (int)(pad_w * sizeof(uint32_t)));
    }

    /* Native 4:3 content is centered inside the configured SDL logical canvas;
     * the logical canvas itself always keeps the selected outer aspect. */
    int dst_w = content_4_3 ? 640 * tex_scale : g_logical_w;
    int dst_h = 480 * tex_scale;
    SDL_Rect dst = { (g_logical_w - dst_w) / 2, 0, dst_w, dst_h };
    /* Match GL: short display bands letterbox inside the 4:3 rect. */
    if (content_4_3 && h > 0 && h < 240) {
        int content_h = (dst_h * (int)h) / 240;
        if (content_h < 1) content_h = 1;
        dst.y = (dst_h - content_h) / 2;
        dst.h = content_h;
    }
    /* Match GL depth24 nearest present — linear AA fringes short FMV bands
     * into the pad/unwritten row. Restore AA scale mode for 15-bit frames. */
    {
        static int s_sw_scale_nearest = -1;
        const int want_nearest = depth24_frame || !g_video_aa;
        if (s_sw_scale_nearest != want_nearest) {
            SDL_SetTextureScaleMode(sdl_texture,
                                    want_nearest ? SDL_ScaleModeNearest
                                                 : SDL_ScaleModeLinear);
            s_sw_scale_nearest = want_nearest;
        }
    }
    /* Always clear black: hot-path RenderClear used to inherit the backend
     * default draw color (often white), so letterbox margins under short FMV
     * bands flashed as a bright strip on some X11 SDL drivers. */
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, &src, &dst);
    host_osd_draw_sdl(sdl_renderer);
    /* Fulfil a staged present_shot here: after RenderCopy + OSD, before
     * RenderPresent, the renderer holds exactly the composed frame the player
     * sees — including the logical-size aspect fit that every buffer-level
     * capture misses. Reading back costs a GPU sync, so it only runs when a
     * shot was explicitly requested. */
    char shot_path[512];
    if (present_shot_take(shot_path, (int)sizeof(shot_path))) {
        int ow = 0, oh = 0;
        uint8_t *packed = NULL;
#if defined(PSX_SDL3)
        SDL_Surface *surf = SDL_RenderReadPixels(sdl_renderer, NULL);
        if (surf) {
            SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGB24);
            SDL_DestroySurface(surf);
            if (conv) {
                ow = conv->w; oh = conv->h;
                packed = (uint8_t *)std::malloc((size_t)ow * oh * 3);
                if (packed) {
                    /* SDL rows are pitch-aligned; png_write_rgb wants packed. */
                    for (int y = 0; y < oh; y++)
                        std::memcpy(packed + (size_t)y * ow * 3,
                                    (const uint8_t *)conv->pixels + (size_t)y * conv->pitch,
                                    (size_t)ow * 3);
                }
                SDL_DestroySurface(conv);
            }
        }
#else
        /* Size the buffer from the renderer's REAL output, not from a
         * viewport*scale reconstruction: SDL_RenderReadPixels(NULL) fills the
         * current viewport, and deriving that region from
         * SDL_RenderGetViewport x SDL_RenderGetScale rounds independently of
         * what SDL actually writes. The output size is a hard upper bound, so
         * a full-output allocation cannot be overrun whatever SDL picks. */
        int rw = 0, rh = 0;
        if (SDL_GetRendererOutputSize(sdl_renderer, &rw, &rh) == 0 && rw > 0 && rh > 0) {
            SDL_Rect vp; SDL_RenderGetViewport(sdl_renderer, &vp);
            float sx = 1.0f, sy = 1.0f; SDL_RenderGetScale(sdl_renderer, &sx, &sy);
            ow = (int)(vp.w * sx); oh = (int)(vp.h * sy);
            if (ow <= 0 || ow > rw) ow = rw;
            if (oh <= 0 || oh > rh) oh = rh;
            packed = (uint8_t *)std::malloc((size_t)rw * rh * 3);   /* upper bound */
            if (packed && SDL_RenderReadPixels(sdl_renderer, NULL,
                                               SDL_PIXELFORMAT_RGB24,
                                               packed, ow * 3) != 0) {
                std::free(packed); packed = NULL;
            }
        }
#endif
        int wrote = 0;
        if (packed && ow > 0 && oh > 0) {
            FILE *pf = std::fopen(shot_path, "wb");
            if (pf) {
                wrote = png_write_rgb(pf, packed, (uint32_t)ow, (uint32_t)oh);
                std::fclose(pf);
            }
        }
        std::free(packed);
        present_shot_done(wrote);
    }
    /* §33: remember active rect for resim hold-last (not full 640x512). */
    s_sw_hold_src = src;
    s_sw_hold_dst = dst;
    s_sw_hold_valid = 1;

    /* Vsync self-heal. PRESENTVSYNC is only armed when driver vsync owns
     * cadence; the wall-clock pacer otherwise holds 59.94 Hz. Under some
     * driver states (observed: NVIDIA GL with the swap queue wedged)
     * SwapBuffers blocks ~1.5 s per present, dragging the whole
     * emulation to ~0.7 fps for minutes (freeze dump 1781045865:
     * 8/8 main-thread samples inside wglSwapBuffers). If presents
     * block pathologically several times in a row, drop driver vsync
     * for the rest of the session; wall-clock pacing takes over. */
    {
        latency_ring_mark(LAT_SWAP_BEGIN);
        const Uint64 t0 = SDL_GetPerformanceCounter();
        SDL_RenderPresent(sdl_renderer);
        const Uint64 t1 = SDL_GetPerformanceCounter();
        latency_ring_mark(LAT_SWAP_END);
        netplay_note_present();
        const Uint64 freq = SDL_GetPerformanceFrequency();
        const Uint64 present_ms = (t1 >= t0 && freq) ? ((t1 - t0) * 1000u) / freq : 0;
        if (!g_present_vsync_disabled && present_ms > 250) {
            g_present_slow_count++;
            if (g_present_slow_count >= 3 &&
                SDL_RenderSetVSync(sdl_renderer, 0) == 0) {
                g_present_vsync_disabled = 1;
            }
        }
    }
    }
#endif
    return ep;
}

static void sdl_vblank_present(void) {
    NetplayVblankEpilogue ep = sdl_vblank_present_body();
    /* Selfcheck span-end rewind: after present-body C++ RAII, before any
     * further guest progress. Longjmps on success — keeps every resim load
     * on the same VBlank boundary (BB fast-poll tails forked #2 vs #3). */
    psx_selfcheck_flush_load();
    if (!ep.do_epilogue)
        return;
    if (!psx_return_to_lobby_requested())
        netplay_barrier_admit(ep.override);
    /* Episode baseline applied during admit without longjmp — resume now that
     * present-body C++ destructors have run (mirrors savestate BB-edge path). */
    psx_netplay_rb_flush_resume();
    if (ep.skip_pace || psx_return_to_lobby_requested())
        return;
    if (psx_netplay_is_resimulating() || psx_netplay_rb_tip_holding())
        return;
    if (!gpu_display_is_depth24() && psx_netplay_catchup_budget() > 0) {
        psx_netplay_catchup_consume_frame();
        return;
    }
    uint64_t perf_start = runtime_perf_section_begin();
    refresh_frame_pacer_period();
    if (present_should_wall_pace())
        frame_pacer_wait(&s_frame_pacer, present_frame_period_ms());
    runtime_perf_section_end(perf_start, &g_runtime_perf.pacer_ticks);
    latency_ring_mark(LAT_PACED);
}

/* game.toml [netplay] + last TOC fingerprint — shared by launcher verify and
 * the pre-psx_netplay_start gate (works with or without RECOMP_LAUNCHER). */
static PSXRecompV4::NetplayDiscExpect g_netplay_disc_expect{};
static std::string g_session_disc_fp;
static bool        g_session_netplay_disc_ok = false;

#if defined(RECOMP_LAUNCHER)
// Host verification/inspection callbacks for the shared recomp-ui launcher.
// The launcher re-runs these on every disc/memory-card change so the "Disc
// verified" verdict + memcard block grids reflect the real images. The expected
// serial/CRC are passed via these file-scope statics (set just before
// recomp_launcher_run_window) because the C-ABI callback can't capture locals.
namespace {
    std::string g_lnch_expected_serial;
    uint32_t    g_lnch_expected_crc  = 0;
    bool        g_lnch_has_crc       = false;
    const char* g_lnch_argv0         = nullptr;
    bool        g_lnch_netplay_available = false;

    int ae_bios_verify(const char* bios_path, RecompLauncherCBiosVerify* out) {
        if (!out) return 0;
        std::memset(out, 0, sizeof(*out));
        /* Empty path = use bundled OpenBIOS when this title allows it.
         * Never needs_regen: switching to OpenBIOS is always a hot-swap. */
        if (!bios_path || !bios_path[0]) {
            out->needs_regen = 0;
            if (s_openbios_allowed && psx_bios_bundled()) {
                out->ok = 1;
                std::snprintf(out->detail, sizeof(out->detail),
                              "Using bundled OpenBIOS.");
                return 1;
            }
            /* Setup host: no BIOS backends linked yet — first Generate for
             * game C will also emit OpenBIOS from redistributable openbios.bin.
             * Selecting OpenBIOS itself still does not require a BIOS regen. */
            if (s_openbios_allowed && psx_bios_registry_count == 0) {
                /* Wizard may Continue; Play is blocked in resolve_bios_for_runtime
                 * until the product build under build-release/ is linked. */
                out->ok = 1;
                std::snprintf(out->detail, sizeof(out->detail),
                              "OpenBIOS will be emitted on Generate & rebuild "
                              "(optional: pick SCPH1001). Play uses "
                              "build-release/ after rebuild.");
                return 1;
            }
            std::snprintf(out->detail, sizeof(out->detail),
                          "PlayStation BIOS required (SCPH1001.BIN).");
            return 1;
        }
        /* Match runtime resolve: relative picks like bios/SCPH1001.BIN must not
         * depend on process cwd (build/ vs project root). Guard filesystem
         * exceptions so a bad picker path cannot take down the setup wizard. */
        try {
            std::filesystem::path resolved =
                resolve_bios_path(bios_path, g_lnch_argv0 ? g_lnch_argv0 : "");
            const std::string open_path =
                (!resolved.empty() ? resolved : std::filesystem::path(bios_path))
                    .string();
            std::ifstream f(open_path, std::ios::binary | std::ios::ate);
            if (!f.is_open()) {
                std::snprintf(out->detail, sizeof(out->detail),
                              "BIOS file not found.");
                return 1;
            }
            const std::streamoff size = f.tellg();
            if (size != 512 * 1024) {
                std::snprintf(out->detail, sizeof(out->detail),
                              "BIOS must be exactly 512 KiB (got %lld). Use "
                              "SCPH1001.BIN.",
                              (long long)size);
                return 1;
            }
            std::vector<uint8_t> data((size_t)size);
            if (!read_at(f, 0, data.data(), data.size())) {
                std::snprintf(out->detail, sizeof(out->detail),
                              "Failed to read BIOS file.");
                return 1;
            }
            const uint32_t crc = crc32_compute(data.data(), data.size());
            if (crc != 0x37157331u) {
                out->warn = 1;
                std::snprintf(out->detail, sizeof(out->detail),
                              "CRC32 %08X (validated dump is SCPH1001 CRC32 "
                              "37157331).",
                              crc);
            } else {
                std::snprintf(out->detail, sizeof(out->detail),
                              "SCPH1001.BIN (CRC OK).");
            }
            /* Setup host (no backends yet): file is fine for first Generate. */
            if (psx_bios_registry_count == 0) {
                out->ok = 1;
                return 1;
            }
            /* Built host: only accept images that have a linked backend.
             * Otherwise Play would load mismatched dispatch and crash. */
            uint32_t match_crc = 0;
            uint64_t match_size = 0;
            if (bios_backend_for_file(open_path, &match_crc, &match_size)) {
                out->ok = 1;
                return 1;
            }
            out->ok = 0;
            out->needs_regen = 1;
            std::snprintf(out->detail, sizeof(out->detail),
                          "This BIOS is not compiled into the current build. "
                          "Generate & rebuild to switch (or use OpenBIOS).");
            return 1;
        } catch (const std::exception& e) {
            std::snprintf(out->detail, sizeof(out->detail),
                          "BIOS check failed: %s", e.what());
            return 1;
        } catch (...) {
            std::snprintf(out->detail, sizeof(out->detail),
                          "BIOS check failed.");
            return 1;
        }
    }

    int ae_prepare_disc(const char* source_path, char* out_disc_path, size_t out_cap,
                        char* err_msg, size_t err_cap) {
        if (!source_path || !source_path[0] || !out_disc_path || out_cap == 0) return 0;
        out_disc_path[0] = '\0';
        if (err_msg && err_cap) err_msg[0] = '\0';
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_regular_file(source_path, ec)) {
            if (err_msg && err_cap)
                std::snprintf(err_msg, err_cap, "Source dump not found.");
            return 0;
        }
        const fs::path exe_dir = exe_dir_from_argv(g_lnch_argv0 ? g_lnch_argv0 : "");
        const fs::path root = find_upward(exe_dir, "tools/prepare_disc.py");
        if (root.empty()) {
            if (err_msg && err_cap)
                std::snprintf(err_msg, err_cap,
                              "tools/prepare_disc.py not found near the executable.");
            return 0;
        }
        const fs::path script = root / "tools" / "prepare_disc.py";
        const fs::path out_dir = root / "motk";
        fs::create_directories(out_dir, ec);
        /* Prefer python3; fall back to python (Windows). */
        const char* py = "python3";
#if defined(_WIN32)
        /* On Windows `python` is the usual launcher; python3 may be absent. */
        py = "python";
#endif
        std::string cmd = std::string(py) + " \"" + script.string() + "\" \"" +
                          source_path + "\" --out-dir \"" + out_dir.string() + "\"";
#if !defined(_WIN32)
        /* If python3 missing, retry with python. */
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            cmd = std::string("python \"") + script.string() + "\" \"" + source_path +
                  "\" --out-dir \"" + out_dir.string() + "\"";
            rc = std::system(cmd.c_str());
        }
#else
        int rc = std::system(cmd.c_str());
#endif
        if (rc != 0) {
            if (err_msg && err_cap)
                std::snprintf(err_msg, err_cap,
                              "prepare_disc.py failed (exit %d). Check the dump is "
                              "2448 bytes/sector.",
                              rc);
            return 0;
        }
        const fs::path cue =
            out_dir / "Star Wars - Masters of Teras Kasi (USA).cue";
        const fs::path bin =
            out_dir / "Star Wars - Masters of Teras Kasi (USA).bin";
        fs::path playable = fs::exists(cue, ec) ? cue : bin;
        if (!fs::exists(playable, ec)) {
            if (err_msg && err_cap)
                std::snprintf(err_msg, err_cap,
                              "prepare_disc finished but no .cue/.bin was written.");
            return 0;
        }
        playable = normalize_disc_path_for_launch(playable);
        std::snprintf(out_disc_path, out_cap, "%s", playable.string().c_str());
        return 1;
    }

    int ae_disc_verify(const char* disc_path, RecompLauncherCDiscVerify* out) {
        if (!disc_path || !disc_path[0] || !out) return 0;
        std::memset(out, 0, sizeof(*out));
        PSXRecompV4::DiscIdentity id = PSXRecompV4::identify_disc(
            disc_path, g_lnch_expected_serial, g_lnch_expected_crc,
            g_lnch_has_crc, /*compute_crc*/ g_lnch_has_crc,
            g_lnch_netplay_available ? &g_netplay_disc_expect : nullptr);
        const std::string& serial = !id.detected_serial.empty()
            ? id.detected_serial : g_lnch_expected_serial;
        std::snprintf(out->serial, sizeof(out->serial), "%s", serial.c_str());
        std::snprintf(out->region, sizeof(out->region), "%s", id.region.c_str());
        out->iso_ok = id.has_header ? 1 : 0;
        if (g_lnch_netplay_available) {
            out->track_count = id.track_count;
            out->netplay_ok = id.netplay_ok ? 1 : 0;
            std::snprintf(out->disc_fp, sizeof(out->disc_fp), "%s",
                          id.disc_fp.c_str());
            std::snprintf(out->netplay_detail, sizeof(out->netplay_detail), "%s",
                          id.netplay_detail.c_str());
            g_session_disc_fp = id.disc_fp;
            g_session_netplay_disc_ok = id.netplay_ok && !id.disc_fp.empty();
            psx_lobby_set_disc_fp(id.disc_fp.c_str());
        } else {
            out->netplay_ok = 1;
            g_session_disc_fp.clear();
            g_session_netplay_disc_ok = false;
        }
        // Verdict shown by the launcher. TOC / [netplay] failures only affect
        // netplay-capable titles; ordinary offline disc verification is
        // strictly serial/header/optional-CRC based.
        if (!id.opened || !id.has_header)                            out->verdict = 3; // bad
        else if (id.expected_serial_given && !id.serial_matches)     out->verdict = 3; // wrong disc
        else if (id.expected_crc_given && id.crc_computed && !id.crc_matches) out->verdict = 2; // warn
        else if (g_lnch_netplay_available && !id.netplay_ok)         out->verdict = 2; // TOC/cue
        else                                                          out->verdict = 1; // ok
        return 1;
    }

    int ae_memcard_inspect(const char* card_path, RecompLauncherCMemcard* out) {
        if (!card_path || !card_path[0] || !out) return 0;
        MemcardSummary s;
        if (memcard_summary_path(card_path, &s) != 0) return 0;
        out->valid       = s.valid;
        out->used_blocks = s.used_blocks;
        for (int i = 0; i < 15; ++i) out->block_used[i] = s.block_used[i];
        return 1;
    }

    std::string g_lnch_netplay_game_name;
    int g_lnch_game_players = 2; /* from game.toml; lobby max_slots default */
    std::filesystem::path g_lnch_settings_path;
    std::string g_lnch_lobby_url;
    RecompLauncherCNetplayLaunch g_lnch_pending_direct_launch{};
    int g_lnch_lobby_input_delay = 2;
    int g_lnch_lobby_input_prediction = 4;
    /* §108: online lobbies always SFU; force_input_relay is set from launch
     * relay_endpoint. force_turn is a rollback delay-floor hint only. */
    int g_lnch_force_input_relay = 0;
    int g_lnch_force_turn = 0;
    /* Lobby default on; host “Disable Rollback” clears this → delay_sync. */
    int g_lnch_rollback = 1;
    int g_lnch_multitap_analog = 1;
    int g_lnch_host_max_slots = 2;

    /* Delay-sync READY/START waits for every seat in slot_count. Use seated
     * players (not lobby max_slots) so a 3/5 room can start. Sparse seats
     * (moved) still need width covering the highest occupied index. */
    static int ae_np_session_slot_count(int player_count, int max_slots,
                                       int local_slot, int game_fallback) {
        int slots = player_count >= 2 ? player_count
                    : (max_slots >= 2 ? max_slots
                       : (game_fallback >= 2 ? game_fallback : 2));
        if (local_slot + 1 > slots) slots = local_slot + 1;
        if (slots < 2) slots = 2;
        if (slots > PSX_MAX_PLAYERS) slots = PSX_MAX_PLAYERS;
        return slots;
    }
    bool g_lnch_hosting_lan = false;
    bool g_lnch_joined_lan = false;
    /* Join Direct / cross-machine: membership via UDP, not the local file. */
    bool g_lnch_remote_lan = false;
    std::string g_lnch_lan_endpoint;
    uint32_t g_lnch_lan_session_id = 1;

    static constexpr int kAeLanMaxSlots = RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
    struct AeLanLobbyState {
        std::string name;
        std::string game;
        std::string endpoint;
        std::string host_name;
        std::string joiner_name; /* legacy / first guest mirror */
        std::string password;
        bool started = false;
        int host_slot = 0;
        int max_slots = 2;
        uint32_t session_id = 1;
        std::string slot_name[kAeLanMaxSlots];
        std::string slot_id[kAeLanMaxSlots]; /* stable per-seat player id */
    };
    AeLanLobbyState g_lnch_remote_lan_state{};
    int g_lnch_lan_my_slot = -1;
    std::string g_lnch_lan_player_id; /* local process identity for LAN seats */

    /* Per-seat BIOS offers for LAN settle (mirrors online bios_offer). */
    struct AeLanSlotBios {
        int valid = 0;
        int prefer_openbios = 1;
        int can_openbios = 1;
        int can_scph1001 = 0;
    };
    AeLanSlotBios g_lnch_lan_slot_bios[kAeLanMaxSlots]{};
    /* Match-only BIOS token from lobby settle or LAN START ("openbios"|"scph1001"). */
    char g_lnch_session_bios[16]{};

    static int ae_np_lan_occupied(const AeLanLobbyState& state);
    static int ae_np_lan_endpoint_port(const std::string& endpoint);
    static bool ae_np_read_lan_file_state(AeLanLobbyState* state);
    static void ae_np_set_session_bios_token(const char* token);
    static void ae_np_clear_session_bios_token(void);
    static void ae_np_lan_clear_slot_bios(int slot);
    static void ae_np_lan_store_slot_bios(int slot, int prefer_open, int can_open,
                                         int can_scph);
    static void ae_np_lan_sync_local_slot_bios(void);
    static int ae_np_lan_settle_session_bios(char* out, size_t out_cap);
    static void ae_np_append_lan_bios_join(char* msg, size_t msg_cap, int* io_off);
    static int ae_np_parse_lan_bios_tail(char* p, int* prefer_open, int* can_open,
                                        int* can_scph);

#ifdef _WIN32
    using AeLanSock = SOCKET;
    static constexpr AeLanSock kAeLanSockInvalid = INVALID_SOCKET;
#else
    using AeLanSock = int;
    static constexpr AeLanSock kAeLanSockInvalid = -1;
#endif
    static void ae_np_lan_sock_close(AeLanSock* s);
    static bool ae_np_lan_set_nonblock(AeLanSock s);
    AeLanSock g_lnch_lan_udp = kAeLanSockInvalid;
    sockaddr_in g_lnch_lan_peers[kAeLanMaxSlots]{};
    bool g_lnch_lan_peer_ok[kAeLanMaxSlots]{};
    uint32_t g_lnch_lan_join_pulse_ms = 0;
    /* LAN list latency from last Refresh probe; -1 unknown. */
    int g_lnch_lan_latency_ms = -1;

    /* Cross-machine LAN lobby discovery (UDP broadcast browse/beacon).
     * The on-disk netplay_lan_lobby.txt registry only works for same-cwd /
     * same-machine; remote peers discover hosts via MOTK1 BROWSE→BEACON. */
    static constexpr int kAeLanDiscoverMax = 16;
    static constexpr int kAeLanBrowsePortLo = 7777;
    static constexpr int kAeLanBrowsePortHi = 7808; /* inclusive */
    static constexpr uint32_t kAeLanDiscoverStaleMs = 8000u;
    struct AeLanDiscovered {
        char endpoint[64];
        char name[64];
        char game[64];
        int player_count = 0;
        int max_slots = 2;
        int has_password = 0;
        int latency_ms = -1;
        int input_delay = 2;
        int input_prediction = 4;
        int rollback = 1;
        uint32_t session_id = 1;
        uint32_t last_seen_ms = 0;
    };
    static AeLanDiscovered g_lnch_lan_discovered[kAeLanDiscoverMax];
    static int g_lnch_lan_discovered_n = 0;
    static uint32_t g_lnch_lan_beacon_announce_ms = 0;

    std::filesystem::path ae_np_lan_file() {
        return std::filesystem::current_path() / "netplay_lan_lobby.txt";
    }

    static void ae_np_lan_sock_set_broadcast(AeLanSock s) {
        if (s == kAeLanSockInvalid) return;
#ifdef _WIN32
        BOOL on = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof(on));
#else
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#endif
    }

    static void ae_np_lan_prune_discovered(void) {
        const uint32_t now = SDL_GetTicks();
        int w = 0;
        for (int i = 0; i < g_lnch_lan_discovered_n; ++i) {
            if ((uint32_t)(now - g_lnch_lan_discovered[i].last_seen_ms) >
                kAeLanDiscoverStaleMs)
                continue;
            if (w != i)
                g_lnch_lan_discovered[w] = g_lnch_lan_discovered[i];
            ++w;
        }
        g_lnch_lan_discovered_n = w;
    }

    static int ae_np_lan_discovered_find(const char* endpoint) {
        if (!endpoint || !endpoint[0]) return -1;
        for (int i = 0; i < g_lnch_lan_discovered_n; ++i) {
            if (std::strcmp(g_lnch_lan_discovered[i].endpoint, endpoint) == 0)
                return i;
        }
        return -1;
    }

    static void ae_np_lan_discovered_upsert(const AeLanDiscovered& in) {
        if (!in.endpoint[0]) return;
        const int idx = ae_np_lan_discovered_find(in.endpoint);
        if (idx >= 0) {
            AeLanDiscovered& e = g_lnch_lan_discovered[idx];
            e = in;
            return;
        }
        if (g_lnch_lan_discovered_n >= kAeLanDiscoverMax) {
            /* Drop oldest. */
            int oldest = 0;
            for (int i = 1; i < g_lnch_lan_discovered_n; ++i) {
                if (g_lnch_lan_discovered[i].last_seen_ms <
                    g_lnch_lan_discovered[oldest].last_seen_ms)
                    oldest = i;
            }
            g_lnch_lan_discovered[oldest] = in;
            return;
        }
        g_lnch_lan_discovered[g_lnch_lan_discovered_n++] = in;
    }

    static int ae_np_lan_format_beacon(char* buf, size_t cap,
                                      const AeLanLobbyState& st) {
        if (!buf || cap < 32) return -1;
        const int players = ae_np_lan_occupied(st);
        const int max_slots = st.max_slots >= 2 ? st.max_slots : 2;
        const int has_pw = st.password.empty() ? 0 : 1;
        const uint32_t sid = st.session_id ? st.session_id : 1u;
        int delay = g_lnch_lobby_input_delay;
        int pred = g_lnch_lobby_input_prediction;
        if (delay < 2) delay = 2;
        if (delay > 20) delay = 20;
        if (pred < 2) pred = 2;
        if (pred > 16) pred = 16;
        return std::snprintf(
            buf, cap,
            "MOTK1 BEACON\n%s\n%s\n%s\n%d\n%d\n%d\n%u\n%d\n%d\n%d\n",
            st.name.empty() ? "LAN Lobby" : st.name.c_str(),
            st.game.empty() ? "PSX" : st.game.c_str(),
            st.endpoint.c_str(),
            players, max_slots, has_pw, (unsigned)sid,
            delay, pred, g_lnch_rollback ? 1 : 0);
    }

    /* Parse BEACON; prefer reply source IP + advertised port for joinability. */
    static int ae_np_lan_parse_beacon(char* buf, const sockaddr_in* from,
                                     int rtt_ms) {
        if (!buf || std::strncmp(buf, "MOTK1 BEACON\n", 13) != 0) return -1;
        char* p = buf + 13;
        char* lines[10] = {};
        for (int i = 0; i < 10; ++i) {
            lines[i] = p;
            char* nl = std::strchr(p, '\n');
            if (!nl) {
                if (i < 6) return -1;
                break;
            }
            *nl = '\0';
            p = nl + 1;
        }
        if (!lines[0] || !lines[1] || !lines[2]) return -1;

        AeLanDiscovered d{};
        std::snprintf(d.name, sizeof(d.name), "%s", lines[0]);
        std::snprintf(d.game, sizeof(d.game), "%s", lines[1]);
        const int adv_port = ae_np_lan_endpoint_port(lines[2]);
        int port = adv_port > 0 ? adv_port : 7777;
        if (from) {
            char ip[64];
            if (inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip)))
                std::snprintf(d.endpoint, sizeof(d.endpoint), "%s:%d", ip, port);
            else
                std::snprintf(d.endpoint, sizeof(d.endpoint), "%s", lines[2]);
        } else {
            std::snprintf(d.endpoint, sizeof(d.endpoint), "%s", lines[2]);
        }
        d.player_count = lines[3] ? std::atoi(lines[3]) : 0;
        d.max_slots = lines[4] ? std::atoi(lines[4]) : 2;
        if (d.max_slots < 2) d.max_slots = 2;
        if (d.max_slots > kAeLanMaxSlots) d.max_slots = kAeLanMaxSlots;
        d.has_password = lines[5] ? (std::atoi(lines[5]) != 0) : 0;
        d.session_id = lines[6]
            ? (uint32_t)std::strtoul(lines[6], nullptr, 10) : 1u;
        if (!d.session_id) d.session_id = 1u;
        d.input_delay = lines[7] ? std::atoi(lines[7]) : 2;
        d.input_prediction = lines[8] ? std::atoi(lines[8]) : 4;
        d.rollback = lines[9] ? (std::atoi(lines[9]) != 0) : 1;
        if (d.input_delay < 2) d.input_delay = 2;
        if (d.input_delay > 20) d.input_delay = 20;
        if (d.input_prediction < 2) d.input_prediction = 2;
        if (d.input_prediction > 16) d.input_prediction = 16;
        d.latency_ms = rtt_ms;
        d.last_seen_ms = SDL_GetTicks();
        ae_np_lan_discovered_upsert(d);
        return 0;
    }

    static void ae_np_lan_broadcast_msg(AeLanSock s, int port, const char* msg) {
        if (s == kAeLanSockInvalid || !msg || port <= 0 || port > 65535) return;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)port);
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        const int n = (int)std::strlen(msg);
#ifdef _WIN32
        sendto(s, msg, n, 0, (const sockaddr*)&to, sizeof(to));
#else
        sendto(s, msg, (size_t)n, 0, (const sockaddr*)&to, sizeof(to));
#endif
    }

    /* Async LAN browse: broadcast once, collect BEACONs across frames in
     * ae_np_lan_browse_pump (no SDL_Delay busy-wait on the UI thread). */
    static AeLanSock g_lan_browse_sock = kAeLanSockInvalid;
    static uint32_t g_lan_browse_t0 = 0;
    static uint32_t g_lan_browse_deadline = 0;
    static int g_lan_browse_file_pending = 0;

    static void ae_np_lan_browse_close(void) {
        if (g_lan_browse_sock != kAeLanSockInvalid)
            ae_np_lan_sock_close(&g_lan_browse_sock);
        g_lan_browse_deadline = 0;
        g_lan_browse_t0 = 0;
    }

    static void ae_np_lan_browse_start(uint32_t wait_ms) {
        if (g_lan_browse_sock != kAeLanSockInvalid)
            return; /* already collecting */
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        AeLanSock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kAeLanSockInvalid) return;
        if (!ae_np_lan_set_nonblock(s)) {
            ae_np_lan_sock_close(&s);
            return;
        }
        ae_np_lan_sock_set_broadcast(s);
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(0);
        if (bind(s, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
            ae_np_lan_sock_close(&s);
            return;
        }

        const char browse[] = "MOTK1 BROWSE\n";
        for (int port = kAeLanBrowsePortLo; port <= kAeLanBrowsePortHi; ++port)
            ae_np_lan_broadcast_msg(s, port, browse);

        g_lan_browse_sock = s;
        g_lan_browse_t0 = SDL_GetTicks();
        g_lan_browse_deadline =
            g_lan_browse_t0 + (wait_ms ? wait_ms : 250u);
    }

    static void ae_np_lan_file_promote_without_rtt(void) {
        AeLanLobbyState st;
        if (!ae_np_read_lan_file_state(&st) || st.started) {
            g_lnch_lan_latency_ms = -1;
            return;
        }
        g_lnch_lan_latency_ms = -1;
        if (ae_np_lan_discovered_find(st.endpoint.c_str()) >= 0)
            return;
        AeLanDiscovered d{};
        std::snprintf(d.endpoint, sizeof(d.endpoint), "%s", st.endpoint.c_str());
        std::snprintf(d.name, sizeof(d.name), "%s",
                      st.name.empty() ? "LAN Lobby" : st.name.c_str());
        std::snprintf(d.game, sizeof(d.game), "%s",
                      st.game.empty() ? "PSX" : st.game.c_str());
        d.player_count = ae_np_lan_occupied(st);
        d.max_slots = st.max_slots >= 2 ? st.max_slots : 2;
        d.has_password = st.password.empty() ? 0 : 1;
        d.latency_ms = -1;
        d.input_delay = g_lnch_lobby_input_delay;
        d.input_prediction = g_lnch_lobby_input_prediction;
        d.rollback = g_lnch_rollback ? 1 : 0;
        d.session_id = st.session_id ? st.session_id : 1u;
        d.last_seen_ms = SDL_GetTicks();
        ae_np_lan_discovered_upsert(d);
    }

    static void ae_np_lan_browse_pump(void) {
        if (g_lan_browse_sock == kAeLanSockInvalid) {
            if (g_lan_browse_file_pending) {
                g_lan_browse_file_pending = 0;
                if (!g_lnch_hosting_lan && !g_lnch_joined_lan)
                    ae_np_lan_file_promote_without_rtt();
            }
            return;
        }

        for (;;) {
            char buf[1024];
            sockaddr_in from{};
#ifdef _WIN32
            int fromlen = (int)sizeof(from);
            const int n = recvfrom(g_lan_browse_sock, buf, (int)sizeof(buf) - 1, 0,
                                   (sockaddr*)&from, &fromlen);
#else
            socklen_t fromlen = sizeof(from);
            const int n = (int)recvfrom(g_lan_browse_sock, buf, sizeof(buf) - 1, 0,
                                        (sockaddr*)&from, &fromlen);
#endif
            if (n <= 0)
                break;
            buf[n] = '\0';
            const int rtt = (int)(SDL_GetTicks() - g_lan_browse_t0);
            (void)ae_np_lan_parse_beacon(buf, &from, rtt);
        }

        if ((int32_t)(g_lan_browse_deadline - SDL_GetTicks()) > 0)
            return;

        ae_np_lan_browse_close();
        ae_np_lan_prune_discovered();
        if (g_lan_browse_file_pending) {
            g_lan_browse_file_pending = 0;
            if (!g_lnch_hosting_lan && !g_lnch_joined_lan)
                ae_np_lan_file_promote_without_rtt();
        }
    }

    static void ae_np_lan_apply_discovered_caps(const char* endpoint) {
        const int idx = ae_np_lan_discovered_find(endpoint);
        if (idx < 0) return;
        const AeLanDiscovered& d = g_lnch_lan_discovered[idx];
        g_lnch_lobby_input_delay = d.input_delay;
        g_lnch_lobby_input_prediction = d.input_prediction;
        g_lnch_rollback = d.rollback ? 1 : 0;
    }

    static int ae_np_lan_fill_lobby_from_discovered(
        int discovered_index, RecompLauncherCNetplayLobby* out) {
        if (!out || discovered_index < 0 ||
            discovered_index >= g_lnch_lan_discovered_n)
            return 0;
        const AeLanDiscovered& d = g_lnch_lan_discovered[discovered_index];
        std::snprintf(out->lobby_id, sizeof(out->lobby_id), "lan:%s", d.endpoint);
        std::snprintf(out->name, sizeof(out->name), "LAN - %s",
                      d.name[0] ? d.name : "Lobby");
        std::snprintf(out->game_name, sizeof(out->game_name), "%s",
                      d.game[0] ? d.game : "PSX");
        out->game_version[0] = '\0';
        out->player_count = d.player_count;
        out->max_slots = d.max_slots;
        out->has_password = d.has_password;
        out->latency_ms = d.latency_ms;
        return 1;
    }

    static void ae_np_lan_sock_close(AeLanSock* s) {
        if (!s || *s == kAeLanSockInvalid) return;
#ifdef _WIN32
        closesocket(*s);
#else
        close(*s);
#endif
        *s = kAeLanSockInvalid;
    }

    static void ae_np_lan_udp_close(void) {
        ae_np_lan_sock_close(&g_lnch_lan_udp);
        for (int i = 0; i < kAeLanMaxSlots; ++i)
            g_lnch_lan_peer_ok[i] = false;
    }

    static void ae_np_lan_sync_legacy_names(AeLanLobbyState& state) {
        if (state.max_slots < 2) state.max_slots = 2;
        if (state.max_slots > kAeLanMaxSlots) state.max_slots = kAeLanMaxSlots;
        if (state.host_slot < 0 || state.host_slot >= state.max_slots)
            state.host_slot = 0;
        if (!state.slot_name[state.host_slot].empty())
            state.host_name = state.slot_name[state.host_slot];
        else if (!state.host_name.empty())
            state.slot_name[state.host_slot] = state.host_name;
        state.joiner_name.clear();
        for (int i = 0; i < state.max_slots; ++i) {
            if (i == state.host_slot) continue;
            if (!state.slot_name[i].empty()) {
                state.joiner_name = state.slot_name[i];
                break;
            }
        }
    }

    static int ae_np_lan_occupied(const AeLanLobbyState& state) {
        int n = 0;
        for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots; ++i)
            if (!state.slot_name[i].empty()) ++n;
        return n;
    }

    static int ae_np_lan_find_free_slot(const AeLanLobbyState& state) {
        for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots; ++i) {
            if (i == state.host_slot) continue;
            if (state.slot_name[i].empty()) return i;
        }
        return -1;
    }

    /* Guest seat by display name. Never match host_slot — same-machine
     * instances often share settings.toml player_name, and reclaiming the
     * host seat made JOIN "succeed" with no visible joiner. */
    static int ae_np_lan_find_guest_slot_by_name(const AeLanLobbyState& state,
                                                const char* name) {
        if (!name || !name[0]) return -1;
        for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots; ++i) {
            if (i == state.host_slot) continue;
            if (state.slot_name[i] == name) return i;
        }
        return -1;
    }

    static int ae_np_lan_find_slot_by_id(const AeLanLobbyState& state,
                                        const char* player_id) {
        if (!player_id || !player_id[0]) return -1;
        for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots; ++i) {
            if (state.slot_id[i] == player_id) return i;
        }
        return -1;
    }

    /* Stable per-process LAN identity (prefer WS player_id when connected). */
    static const char* ae_np_lan_local_player_id(void) {
        if (!g_lnch_lan_player_id.empty()) return g_lnch_lan_player_id.c_str();
        const char* ws = psx_lobby_player_id();
        if (ws && ws[0]) {
            g_lnch_lan_player_id = ws;
            return g_lnch_lan_player_id.c_str();
        }
        unsigned char b[16];
        bool ok = false;
#if !defined(_WIN32)
        FILE* ur = std::fopen("/dev/urandom", "rb");
        if (ur) {
            ok = std::fread(b, 1, sizeof(b), ur) == sizeof(b);
            std::fclose(ur);
        }
#endif
        if (!ok) {
            uint64_t t = (uint64_t)SDL_GetTicks()
                ^ ((uint64_t)(uintptr_t)&g_lnch_lan_player_id << 32)
                ^ ((uint64_t)SDL_GetPerformanceCounter() << 17);
            for (size_t i = 0; i < sizeof(b); ++i) {
                t = t * 6364136223846793005ull + 1ull;
                b[i] = (unsigned char)(t >> 56);
            }
        }
        char hex[40];
        std::snprintf(hex, sizeof(hex),
                      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                      b[10], b[11], b[12], b[13], b[14], b[15]);
        g_lnch_lan_player_id = hex;
        return g_lnch_lan_player_id.c_str();
    }

    /* Exact-name uniqueness among seats: Alex, Alex (2), Alex (3), … */
    static std::string ae_np_lan_unique_display_name(const AeLanLobbyState& state,
                                                    const char* requested,
                                                    int skip_slot) {
        std::string base = (requested && requested[0]) ? requested : "Player";
        auto taken = [&](const std::string& n) {
            for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots; ++i) {
                if (i == skip_slot) continue;
                if (state.slot_name[i] == n) return true;
            }
            return false;
        };
        if (!taken(base)) return base;
        for (int n = 2; n <= 64; ++n) {
            char cand[96];
            std::snprintf(cand, sizeof(cand), "%s (%d)", base.c_str(), n);
            if (!taken(cand)) return cand;
        }
        return base + " (" + std::string(ae_np_lan_local_player_id()).substr(0, 8) + ")";
    }

    /* Seat or reconnect a guest by player_id; uniquify display name on insert. */
    static int ae_np_lan_seat_guest(AeLanLobbyState& st, const char* player_id,
                                   const char* requested_name) {
        if (!player_id || !player_id[0]) return -1;
        int slot = ae_np_lan_find_slot_by_id(st, player_id);
        if (slot >= 0) {
            if (slot == st.host_slot) return -1;
            st.slot_name[slot] =
                ae_np_lan_unique_display_name(st, requested_name, slot);
            return slot;
        }
        slot = ae_np_lan_find_free_slot(st);
        if (slot < 0) return -1;
        st.slot_id[slot] = player_id;
        st.slot_name[slot] =
            ae_np_lan_unique_display_name(st, requested_name, -1);
        return slot;
    }

    static void ae_np_lan_clear_peer_slot(int slot) {
        if (slot < 0 || slot >= kAeLanMaxSlots) return;
        g_lnch_lan_peer_ok[slot] = false;
    }

    static void ae_np_lan_set_peer_slot(int slot, const sockaddr_in& addr) {
        if (slot < 0 || slot >= kAeLanMaxSlots) return;
        g_lnch_lan_peers[slot] = addr;
        g_lnch_lan_peer_ok[slot] = true;
    }

    static int ae_np_lan_endpoint_port(const std::string& endpoint) {
        const size_t colon = endpoint.rfind(':');
        if (colon == std::string::npos) return 7777;
        const int p = std::atoi(endpoint.c_str() + colon + 1);
        return (p > 0 && p <= 65535) ? p : 7777;
    }

    static bool ae_np_lan_endpoint_host(const std::string& endpoint, char* host,
                                       size_t host_len) {
        if (!host || host_len == 0) return false;
        const size_t colon = endpoint.rfind(':');
        if (colon == std::string::npos || colon == 0) return false;
        if (colon >= host_len) return false;
        std::memcpy(host, endpoint.data(), colon);
        host[colon] = '\0';
        return host[0] != '\0';
    }

    static bool ae_np_lan_set_nonblock(AeLanSock s) {
#ifdef _WIN32
        u_long mode = 1;
        return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
        const int fl = fcntl(s, F_GETFL, 0);
        return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
    }

    /* Exclusive bind probe (no SO_REUSEADDR) so a busy port is detected. */
    static bool ae_np_udp_port_available(int port) {
        if (port <= 0 || port > 65535) return false;
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        AeLanSock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kAeLanSockInvalid) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)port);
        const bool ok = (bind(s, (sockaddr*)&addr, sizeof(addr)) == 0);
        ae_np_lan_sock_close(&s);
        return ok;
    }

    /* Online create: try preferred, then the next few ports. */
    static int ae_np_find_free_udp_port(int preferred) {
        if (preferred <= 0 || preferred > 65535) preferred = 7777;
        for (int i = 0; i < 32; ++i) {
            const int p = preferred + i;
            if (p > 65535) break;
            if (ae_np_udp_port_available(p)) return p;
        }
        return -1;
    }

    static bool ae_np_endpoint_replace_port(char* endpoint, size_t cap, int port) {
        if (!endpoint || cap < 4 || port <= 0 || port > 65535) return false;
        char host[64];
        if (!ae_np_lan_endpoint_host(endpoint, host, sizeof(host))) {
            if (endpoint[0] == ':' || !endpoint[0])
                std::snprintf(host, sizeof(host), "0.0.0.0");
            else
                return false;
        }
        const int n = std::snprintf(endpoint, cap, "%s:%d", host, port);
        return n > 0 && (size_t)n < cap;
    }

    static bool ae_np_lan_udp_ensure(bool bind_port, int port) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        if (g_lnch_lan_udp == kAeLanSockInvalid) {
            g_lnch_lan_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (g_lnch_lan_udp == kAeLanSockInvalid) return false;
            if (!ae_np_lan_set_nonblock(g_lnch_lan_udp)) {
                ae_np_lan_udp_close();
                return false;
            }
            /* Do not SO_REUSEADDR on the host lobby port — a second host on the
             * same port must fail so we can surface "port in use". */
            if (bind_port) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_ANY);
                addr.sin_port = htons((uint16_t)port);
                if (bind(g_lnch_lan_udp, (sockaddr*)&addr, sizeof(addr)) != 0) {
                    ae_np_lan_udp_close();
                    return false;
                }
            }
        }
        return true;
    }

    static void ae_np_lan_udp_sendto(const sockaddr_in& to, const char* msg) {
        if (g_lnch_lan_udp == kAeLanSockInvalid || !msg) return;
        const int n = (int)std::strlen(msg);
#ifdef _WIN32
        sendto(g_lnch_lan_udp, msg, n, 0, (const sockaddr*)&to, sizeof(to));
#else
        sendto(g_lnch_lan_udp, msg, (size_t)n, 0, (const sockaddr*)&to, sizeof(to));
#endif
    }

    bool ae_np_read_lan_state(AeLanLobbyState* state) {
        if (!state) return false;
        if (g_lnch_remote_lan) {
            *state = g_lnch_remote_lan_state;
            ae_np_lan_sync_legacy_names(*state);
            return !state->endpoint.empty();
        }
        std::ifstream f(ae_np_lan_file());
        if (!f) return false;
        std::string started, host_slot, session, max_slots_s;
        std::getline(f, state->name);
        std::getline(f, state->game);
        std::getline(f, state->endpoint);
        std::getline(f, state->host_name);
        std::getline(f, state->joiner_name);
        std::getline(f, started);
        std::getline(f, host_slot);
        std::getline(f, state->password);
        std::getline(f, session);
        state->started = started == "1";
        state->host_slot = std::atoi(host_slot.c_str());
        if (state->host_slot < 0) state->host_slot = 0;
        state->session_id = 1;
        if (!session.empty()) {
            const unsigned v = (unsigned)std::strtoul(session.c_str(), nullptr, 10);
            if (v) state->session_id = (uint32_t)v;
        }
        state->max_slots = 2;
        for (int i = 0; i < kAeLanMaxSlots; ++i) {
            state->slot_name[i].clear();
            state->slot_id[i].clear();
        }
        if (std::getline(f, max_slots_s)) {
            int ms = std::atoi(max_slots_s.c_str());
            if (ms >= 2 && ms <= kAeLanMaxSlots) state->max_slots = ms;
            for (int i = 0; i < state->max_slots; ++i)
                std::getline(f, state->slot_name[i]);
            for (int i = 0; i < state->max_slots; ++i) {
                if (!std::getline(f, state->slot_id[i]))
                    state->slot_id[i].clear();
            }
        } else {
            /* Legacy 2P file: host + single joiner. */
            state->slot_name[state->host_slot == 1 ? 1 : 0] = state->host_name;
            const int guest = state->host_slot == 1 ? 0 : 1;
            state->slot_name[guest] = state->joiner_name;
        }
        ae_np_lan_sync_legacy_names(*state);
        return !state->endpoint.empty();
    }

    bool ae_np_write_lan_state(const AeLanLobbyState& state_in) {
        AeLanLobbyState state = state_in;
        ae_np_lan_sync_legacy_names(state);
        if (g_lnch_remote_lan) {
            g_lnch_remote_lan_state = state;
            return true;
        }
        std::ofstream f(ae_np_lan_file(), std::ios::trunc);
        if (!f) return false;
        const uint32_t sid = state.session_id ? state.session_id : 1u;
        f << state.name << "\n"
          << state.game << "\n"
          << state.endpoint << "\n"
          << state.host_name << "\n"
          << state.joiner_name << "\n"
          << (state.started ? "1" : "0") << "\n"
          << state.host_slot << "\n"
          << state.password << "\n"
          << sid << "\n"
          << state.max_slots << "\n";
        for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots; ++i)
            f << state.slot_name[i] << "\n";
        for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots; ++i)
            f << state.slot_id[i] << "\n";
        return (bool)f;
    }

    static void ae_np_lan_send_update_to_peers(const AeLanLobbyState& state_in) {
        AeLanLobbyState state = state_in;
        ae_np_lan_sync_legacy_names(state);
        if (g_lnch_hosting_lan) ae_np_lan_sync_local_slot_bios();
        char msg3[1536];
        int off3 = std::snprintf(msg3, sizeof(msg3),
                                 "MOTK3 UPDATE\n%d\n%d\n%d\n%u\n",
                                 state.max_slots, state.host_slot,
                                 state.started ? 1 : 0,
                                 (unsigned)(state.session_id ? state.session_id : 1u));
        for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots && off3 > 0 &&
             off3 < (int)sizeof(msg3) - 128; ++i) {
            off3 += std::snprintf(msg3 + off3, sizeof(msg3) - (size_t)off3,
                                  "%s\n%s\n", state.slot_id[i].c_str(),
                                  state.slot_name[i].c_str());
        }
        /* MOTK4 carries per-seat bios_offer so guests can preview settle. */
        char msg4[2048];
        int off4 = std::snprintf(msg4, sizeof(msg4),
                                 "MOTK4 UPDATE\n%d\n%d\n%d\n%u\n",
                                 state.max_slots, state.host_slot,
                                 state.started ? 1 : 0,
                                 (unsigned)(state.session_id ? state.session_id : 1u));
        for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots && off4 > 0 &&
             off4 < (int)sizeof(msg4) - 160; ++i) {
            const AeLanSlotBios& b = g_lnch_lan_slot_bios[i];
            off4 += std::snprintf(
                msg4 + off4, sizeof(msg4) - (size_t)off4, "%s\n%s\n%d\n%d\n%d\n%d\n",
                state.slot_id[i].c_str(), state.slot_name[i].c_str(),
                b.valid ? 1 : 0, b.prefer_openbios ? 1 : 0, b.can_openbios ? 1 : 0,
                b.can_scph1001 ? 1 : 0);
        }
        if (off3 <= 0 && off4 <= 0) return;
        for (int i = 0; i < kAeLanMaxSlots; ++i) {
            if (!g_lnch_lan_peer_ok[i]) continue;
            if (off3 > 0) ae_np_lan_udp_sendto(g_lnch_lan_peers[i], msg3);
            if (off4 > 0) ae_np_lan_udp_sendto(g_lnch_lan_peers[i], msg4);
        }
    }

    static void ae_np_lan_atexit_cleanup(void) {
        if (!g_lnch_hosting_lan) return;
        std::error_code ec;
        std::filesystem::remove(ae_np_lan_file(), ec);
        g_lnch_hosting_lan = false;
    }

    /* Returns false if the lobby UDP port cannot be bound (in use). */
    bool ae_np_write_lan_lobby(const char* name, const char* endpoint,
                               const char* password, int max_slots) {
        ae_np_lan_udp_close();
        g_lnch_remote_lan = false;
        g_lnch_remote_lan_state = {};
        g_lnch_lan_my_slot = 0;
        AeLanLobbyState state;
        state.name = name && name[0] ? name : "LAN Lobby";
        state.game = g_lnch_netplay_game_name.empty() ? "PSX" : g_lnch_netplay_game_name;
        state.endpoint = endpoint && endpoint[0] ? endpoint : "127.0.0.1:7777";
        state.host_name = psx_lobby_display_name();
        if (state.host_name.empty()) state.host_name = "Host";
        state.password = password ? password : "";
        if (max_slots < 2) max_slots = 2;
        if (max_slots > kAeLanMaxSlots) max_slots = kAeLanMaxSlots;
        state.max_slots = max_slots;
        state.host_slot = 0;
        state.slot_name[0] = state.host_name;
        state.slot_id[0] = ae_np_lan_local_player_id();
        const int port = ae_np_lan_endpoint_port(state.endpoint);
        if (!ae_np_udp_port_available(port) ||
            !ae_np_lan_udp_ensure(true, port)) {
            ae_np_lan_udp_close();
            return false;
        }
        if (!ae_np_write_lan_state(state)) {
            ae_np_lan_udp_close();
            return false;
        }
        g_lnch_hosting_lan = true;
        g_lnch_joined_lan = false;
        g_lnch_lan_endpoint = state.endpoint;
        for (int i = 0; i < kAeLanMaxSlots; ++i) g_lnch_lan_slot_bios[i] = {};
        ae_np_clear_session_bios_token();
        ae_np_lan_sync_local_slot_bios();
        static bool atexit_hooked = false;
        if (!atexit_hooked) {
            std::atexit(ae_np_lan_atexit_cleanup);
            atexit_hooked = true;
        }
        return true;
    }

    int ae_np_read_lan_lobby(RecompLauncherCNetplayLobby* out) {
        if (!out) return 0;
        AeLanLobbyState state;
        if (!ae_np_read_lan_state(&state)) return 0;
        std::snprintf(out->lobby_id, sizeof(out->lobby_id), "lan:%s", state.endpoint.c_str());
        std::snprintf(out->name, sizeof(out->name), "LAN - %s",
                      state.name.empty() ? "Lobby" : state.name.c_str());
        std::snprintf(out->game_name, sizeof(out->game_name), "%s",
                      state.game.empty() ? "PSX" : state.game.c_str());
        out->player_count = ae_np_lan_occupied(state);
        out->max_slots = state.max_slots >= 2 ? state.max_slots
            : (g_lnch_host_max_slots >= 2 ? g_lnch_host_max_slots
               : (g_lnch_game_players >= 2 ? g_lnch_game_players : 2));
        if (out->max_slots > PSX_MAX_PLAYERS) out->max_slots = PSX_MAX_PLAYERS;
        if (out->max_slots > kAeLanMaxSlots) out->max_slots = kAeLanMaxSlots;
        out->has_password = state.password.empty() ? 0 : 1;
        out->latency_ms = g_lnch_hosting_lan ? 0 : g_lnch_lan_latency_ms;
        return 1;
    }

    static bool ae_np_remote_has_same_name_as_lan(const char* lan_display_name) {
        static constexpr const char kLanPrefix[] = "LAN - ";
        const char* base = lan_display_name;
        if (base && std::strncmp(base, kLanPrefix, sizeof(kLanPrefix) - 1) == 0)
            base += sizeof(kLanPrefix) - 1;
        if (!base || !base[0]) return false;
        for (int i = 0; i < psx_lobby_list_count(); ++i) {
            PsxLobbyRow row{};
            if (!psx_lobby_list_get(i, &row)) continue;
            if (std::strcmp(row.name, base) == 0) return true;
        }
        return false;
    }

    static bool ae_np_read_lan_file_state(AeLanLobbyState* state) {
        if (!state) return false;
        const bool prior = g_lnch_remote_lan;
        g_lnch_remote_lan = false;
        const bool ok = ae_np_read_lan_state(state);
        g_lnch_remote_lan = prior;
        return ok;
    }

    /* Probe LAN host; returns RTT ms, or -1 on timeout/failure. */
    static int ae_np_lan_probe_rtt_ms(const std::string& endpoint, uint32_t timeout_ms) {
        char host[64];
        if (!ae_np_lan_endpoint_host(endpoint, host, sizeof(host))) return -1;
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        AeLanSock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kAeLanSockInvalid) return -1;
        if (!ae_np_lan_set_nonblock(s)) {
            ae_np_lan_sock_close(&s);
            return -1;
        }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)ae_np_lan_endpoint_port(endpoint));
        if (inet_pton(AF_INET, host, &to.sin_addr) != 1) {
            ae_np_lan_sock_close(&s);
            return -1;
        }
        const char ping[] = "MOTK1 PING\n";
        const uint32_t t0 = SDL_GetTicks();
#ifdef _WIN32
        sendto(s, ping, (int)sizeof(ping) - 1, 0, (const sockaddr*)&to, sizeof(to));
#else
        sendto(s, ping, sizeof(ping) - 1, 0, (const sockaddr*)&to, sizeof(to));
#endif
        const uint32_t deadline = t0 + timeout_ms;
        int rtt = -1;
        while ((int32_t)(deadline - SDL_GetTicks()) > 0) {
            char buf[64];
            sockaddr_in from{};
#ifdef _WIN32
            int fromlen = (int)sizeof(from);
            const int n = recvfrom(s, buf, (int)sizeof(buf) - 1, 0,
                                   (sockaddr*)&from, &fromlen);
#else
            socklen_t fromlen = sizeof(from);
            const int n = (int)recvfrom(s, buf, sizeof(buf) - 1, 0,
                                        (sockaddr*)&from, &fromlen);
#endif
            if (n > 0) {
                buf[n] = '\0';
                if (std::strncmp(buf, "MOTK1 PONG", 10) == 0) {
                    rtt = (int)(SDL_GetTicks() - t0);
                    break;
                }
            }
            SDL_Delay(5);
        }
        ae_np_lan_sock_close(&s);
        return rtt;
    }

    static bool ae_np_lan_probe_host_ms(const std::string& endpoint, uint32_t timeout_ms) {
        return ae_np_lan_probe_rtt_ms(endpoint, timeout_ms) >= 0;
    }

    static bool ae_np_lan_probe_host(const std::string& endpoint) {
        return ae_np_lan_probe_host_ms(endpoint, 200u);
    }


    static int ae_np_lan_parse_motk2_update(char* body, AeLanLobbyState* out) {
        if (!body || !out) return -1;
        char* lines[8] = {};
        char* p = body;
        for (int i = 0; i < 4; ++i) {
            lines[i] = p;
            char* nl = std::strchr(p, '\n');
            if (!nl) return -1;
            *nl = '\0';
            p = nl + 1;
        }
        int max_slots = std::atoi(lines[0]);
        int host_slot = std::atoi(lines[1]);
        int started = std::atoi(lines[2]);
        unsigned sid = (unsigned)std::strtoul(lines[3], nullptr, 10);
        if (max_slots < 2) max_slots = 2;
        if (max_slots > kAeLanMaxSlots) max_slots = kAeLanMaxSlots;
        if (host_slot < 0 || host_slot >= max_slots) host_slot = 0;
        out->max_slots = max_slots;
        out->host_slot = host_slot;
        out->started = started != 0;
        out->session_id = sid ? (uint32_t)sid : 1u;
        for (int i = 0; i < kAeLanMaxSlots; ++i) {
            out->slot_name[i].clear();
            out->slot_id[i].clear();
        }
        for (int i = 0; i < max_slots; ++i) {
            char* nl = std::strchr(p, '\n');
            if (nl) {
                *nl = '\0';
                out->slot_name[i] = p;
                p = nl + 1;
            } else {
                out->slot_name[i] = p;
                p = p + std::strlen(p);
            }
        }
        ae_np_lan_sync_legacy_names(*out);
        return 0;
    }

    /* MOTK4 UPDATE: MOTK3 fields + per-slot bios_offer (valid/prefer/can_*). */
    static int ae_np_lan_parse_motk4_update(char* body, AeLanLobbyState* out) {
        if (!body || !out) return -1;
        char* lines[8] = {};
        char* p = body;
        for (int i = 0; i < 4; ++i) {
            lines[i] = p;
            char* nl = std::strchr(p, '\n');
            if (!nl) return -1;
            *nl = '\0';
            p = nl + 1;
        }
        int max_slots = std::atoi(lines[0]);
        int host_slot = std::atoi(lines[1]);
        int started = std::atoi(lines[2]);
        unsigned sid = (unsigned)std::strtoul(lines[3], nullptr, 10);
        if (max_slots < 2) max_slots = 2;
        if (max_slots > kAeLanMaxSlots) max_slots = kAeLanMaxSlots;
        if (host_slot < 0 || host_slot >= max_slots) host_slot = 0;
        out->max_slots = max_slots;
        out->host_slot = host_slot;
        out->started = started != 0;
        out->session_id = sid ? (uint32_t)sid : 1u;
        for (int i = 0; i < kAeLanMaxSlots; ++i) {
            out->slot_name[i].clear();
            out->slot_id[i].clear();
            g_lnch_lan_slot_bios[i] = {};
        }
        for (int i = 0; i < max_slots; ++i) {
            char* nl = std::strchr(p, '\n');
            if (!nl) return -1;
            *nl = '\0';
            out->slot_id[i] = p;
            p = nl + 1;
            nl = std::strchr(p, '\n');
            if (!nl) return -1;
            *nl = '\0';
            out->slot_name[i] = p;
            p = nl + 1;
            int bios_lines[4] = {};
            for (int b = 0; b < 4; ++b) {
                nl = std::strchr(p, '\n');
                if (!nl) return -1;
                *nl = '\0';
                bios_lines[b] = std::atoi(p);
                p = nl + 1;
            }
            if (bios_lines[0]) {
                ae_np_lan_store_slot_bios(i, bios_lines[1], bios_lines[2],
                                         bios_lines[3]);
            }
        }
        ae_np_lan_sync_legacy_names(*out);
        return 0;
    }

    /* MOTK3 UPDATE: header + (player_id, display_name) per slot. */
    static int ae_np_lan_parse_motk3_update(char* body, AeLanLobbyState* out) {
        if (!body || !out) return -1;
        char* lines[8] = {};
        char* p = body;
        for (int i = 0; i < 4; ++i) {
            lines[i] = p;
            char* nl = std::strchr(p, '\n');
            if (!nl) return -1;
            *nl = '\0';
            p = nl + 1;
        }
        int max_slots = std::atoi(lines[0]);
        int host_slot = std::atoi(lines[1]);
        int started = std::atoi(lines[2]);
        unsigned sid = (unsigned)std::strtoul(lines[3], nullptr, 10);
        if (max_slots < 2) max_slots = 2;
        if (max_slots > kAeLanMaxSlots) max_slots = kAeLanMaxSlots;
        if (host_slot < 0 || host_slot >= max_slots) host_slot = 0;
        out->max_slots = max_slots;
        out->host_slot = host_slot;
        out->started = started != 0;
        out->session_id = sid ? (uint32_t)sid : 1u;
        for (int i = 0; i < kAeLanMaxSlots; ++i) {
            out->slot_name[i].clear();
            out->slot_id[i].clear();
        }
        for (int i = 0; i < max_slots; ++i) {
            char* nl = std::strchr(p, '\n');
            if (!nl) return -1;
            *nl = '\0';
            out->slot_id[i] = p;
            p = nl + 1;
            nl = std::strchr(p, '\n');
            if (nl) {
                *nl = '\0';
                out->slot_name[i] = p;
                p = nl + 1;
            } else {
                out->slot_name[i] = p;
                p = p + std::strlen(p);
            }
        }
        ae_np_lan_sync_legacy_names(*out);
        return 0;
    }

    /* Send JOIN and wait for UPDATE / ERR. Returns 0, -1 full, -2 password, -3 timeout. */
    static int ae_np_lan_wait_join_ack(const std::string& endpoint, const char* password,
                                       AeLanLobbyState* out) {
        if (!out) return -1;
        char host[64];
        if (!ae_np_lan_endpoint_host(endpoint, host, sizeof(host))) return -3;
        if (!ae_np_lan_udp_ensure(false, 0)) return -3;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)ae_np_lan_endpoint_port(endpoint));
        if (inet_pton(AF_INET, host, &to.sin_addr) != 1) return -3;

        std::string me = psx_lobby_display_name();
        if (me.empty()) me = "Player";
        const char* my_id = ae_np_lan_local_player_id();
        char msg[448];
        int off = std::snprintf(msg, sizeof(msg), "MOTK3 JOIN\n%s\n%s\n%s\n", my_id,
                                me.c_str(), password ? password : "");
        ae_np_append_lan_bios_join(msg, sizeof(msg), &off);
        ae_np_lan_udp_sendto(to, msg);

        const uint32_t deadline = SDL_GetTicks() + 1000u;
        while ((int32_t)(deadline - SDL_GetTicks()) > 0) {
            char buf[1536];
            sockaddr_in from{};
#ifdef _WIN32
            int fromlen = (int)sizeof(from);
            const int n = recvfrom(g_lnch_lan_udp, buf, (int)sizeof(buf) - 1, 0,
                                   (sockaddr*)&from, &fromlen);
#else
            socklen_t fromlen = sizeof(from);
            const int n = (int)recvfrom(g_lnch_lan_udp, buf, sizeof(buf) - 1, 0,
                                        (sockaddr*)&from, &fromlen);
#endif
            if (n <= 0) {
                SDL_Delay(5);
                continue;
            }
            buf[n] = '\0';
            if (std::strncmp(buf, "MOTK1 ERR\n", 10) == 0) {
                const char* code = buf + 10;
                if (std::strncmp(code, "bad_password", 12) == 0) return -2;
                return -1;
            }
            if (std::strncmp(buf, "MOTK3 UPDATE\n", 13) == 0) {
                if (ae_np_lan_parse_motk3_update(buf + 13, out) != 0) continue;
                out->endpoint = endpoint;
                const int my_slot = ae_np_lan_find_slot_by_id(*out, my_id);
                if (my_slot < 0) return -1;
                g_lnch_lan_my_slot = my_slot;
                return 0;
            }
            if (std::strncmp(buf, "MOTK2 UPDATE\n", 13) == 0) {
                if (ae_np_lan_parse_motk2_update(buf + 13, out) != 0) continue;
                out->endpoint = endpoint;
                /* Legacy host: uniquified name may differ from requested. */
                int my_slot = ae_np_lan_find_guest_slot_by_name(*out, me.c_str());
                if (my_slot < 0) {
                    for (int i = 0; i < out->max_slots; ++i) {
                        if (i == out->host_slot) continue;
                        if (!out->slot_name[i].empty() &&
                            out->slot_name[i].rfind(me, 0) == 0) {
                            my_slot = i;
                            break;
                        }
                    }
                }
                if (my_slot < 0) return -1;
                g_lnch_lan_my_slot = my_slot;
                return 0;
            }
            if (std::strncmp(buf, "MOTK1 UPDATE\n", 13) == 0) {
                char* p = buf + 13;
                char* lines[4] = {};
                for (int i = 0; i < 4; ++i) {
                    lines[i] = p;
                    char* nl = std::strchr(p, '\n');
                    if (!nl) break;
                    *nl = '\0';
                    p = nl + 1;
                }
                if (!lines[0] || !lines[1] || !lines[2] || !lines[3]) continue;
                out->host_name = lines[0];
                out->joiner_name = lines[1];
                out->host_slot = (std::atoi(lines[2]) == 1) ? 1 : 0;
                out->started = std::atoi(lines[3]) != 0;
                out->max_slots = 2;
                out->slot_name[0].clear();
                out->slot_name[1].clear();
                out->slot_name[out->host_slot] = out->host_name;
                out->slot_name[1 - out->host_slot] = out->joiner_name;
                out->endpoint = endpoint;
                if (out->joiner_name != me) return -1;
                g_lnch_lan_my_slot = 1 - out->host_slot;
                return 0;
            }
            SDL_Delay(5);
        }
        return -3;
    }

    /* Drop orphan LAN registry files (host crashed / unreachable).
     * Never delete merely because started=1: host closes lobby UDP before
     * delay-sync bind, so PING fails mid-launch and a delete races the joiner
     * reading session_id/started from the file. Started lobbies are already
     * hidden by ae_np_lan_list_visible(). */
    static void ae_np_lan_rescan(void) {
        if (g_lnch_hosting_lan) {
            g_lnch_lan_latency_ms = 0;
            /* Still browse so the host list can show other LAN rooms. */
            ae_np_lan_browse_start(200u);
            AeLanLobbyState self{};
            if (ae_np_read_lan_file_state(&self) && !self.started) {
                AeLanDiscovered d{};
                std::snprintf(d.endpoint, sizeof(d.endpoint), "%s",
                              self.endpoint.c_str());
                std::snprintf(d.name, sizeof(d.name), "%s",
                              self.name.empty() ? "LAN Lobby" : self.name.c_str());
                std::snprintf(d.game, sizeof(d.game), "%s",
                              self.game.empty() ? "PSX" : self.game.c_str());
                d.player_count = ae_np_lan_occupied(self);
                d.max_slots = self.max_slots >= 2 ? self.max_slots : 2;
                d.has_password = self.password.empty() ? 0 : 1;
                d.latency_ms = 0;
                d.input_delay = g_lnch_lobby_input_delay;
                d.input_prediction = g_lnch_lobby_input_prediction;
                d.rollback = g_lnch_rollback ? 1 : 0;
                d.session_id = self.session_id ? self.session_id : 1u;
                d.last_seen_ms = SDL_GetTicks();
                ae_np_lan_discovered_upsert(d);
            }
            return;
        }
        if (g_lnch_joined_lan) return;

        /* Cross-machine discovery + legacy file row — non-blocking. */
        g_lan_browse_file_pending = 1;
        ae_np_lan_browse_start(300u);
        /* If the browse socket failed to open, promote the file row now. */
        ae_np_lan_browse_pump();
    }

    static bool ae_np_lan_list_visible(void) {
        if (g_lnch_hosting_lan) {
            AeLanLobbyState st;
            return ae_np_read_lan_file_state(&st) && !st.started;
        }
        AeLanLobbyState st;
        if (!ae_np_read_lan_file_state(&st) || st.started) return false;
        return true;
    }

    /* Extra LAN rows after the online server list. Prefer discovered table;
     * fall back to a single file-backed row when hosting / same-machine. */
    static int ae_np_lan_list_extra_count(void) {
        ae_np_lan_prune_discovered();
        if (g_lnch_lan_discovered_n > 0)
            return g_lnch_lan_discovered_n;
        return ae_np_lan_list_visible() ? 1 : 0;
    }

    /* Advertise local BIOS capability for lobby settle (OpenBIOS vs SCPH-1001). */
    static void ae_np_refresh_bios_offer(const char* launcher_bios_path) {
        PsxLobbyBiosOffer offer{};
        offer.valid = 1;
        offer.can_openbios =
            (s_openbios_allowed && psx_bios_bundled() != nullptr) ? 1 : 0;
        const char* argv0 = g_lnch_argv0 ? g_lnch_argv0 : "";
        auto path_is_retail = [&](const std::filesystem::path& p) -> bool {
            std::error_code ec;
            if (p.empty() || !std::filesystem::exists(p, ec)) return false;
            const PsxBiosBackend* b = bios_backend_for_file(p, nullptr, nullptr);
            return b && b->image && !b->image->image_bundled;
        };
        bool has_dump = false;
        if (launcher_bios_path && launcher_bios_path[0]) {
            const auto p = resolve_bios_path(launcher_bios_path, argv0);
            has_dump = path_is_retail(p);
        }
        if (!has_dump) has_dump = path_is_retail(read_cached_path(argv0, "bios.cfg"));
        if (!has_dump) has_dump = path_is_retail(discover_retail_bios_near(argv0));
        offer.can_scph1001 =
            (psx_bios_has_selectable() && has_dump) ? 1 : 0;

        /* Empty / bundled path = explicit OpenBIOS preference. */
        offer.prefer_openbios = 1;
        if (launcher_bios_path && launcher_bios_path[0]) {
            const auto p = resolve_bios_path(launcher_bios_path, argv0);
            if (path_is_retail(p))
                offer.prefer_openbios = 0;
        } else {
            /* No launcher path — bios.cfg empty ⇒ OpenBIOS; retail path ⇒ SCPH. */
            const auto cached = read_cached_path(argv0, "bios.cfg");
            if (path_is_retail(cached)) offer.prefer_openbios = 0;
        }
        if (!offer.can_openbios && offer.can_scph1001) offer.prefer_openbios = 0;
        psx_lobby_set_bios_offer(&offer);
    }

    static void ae_np_refresh_bios_offer_from_disk(void) {
        ae_np_refresh_bios_offer(nullptr);
    }

    static void ae_np_set_session_bios_token(const char* token) {
        g_lnch_session_bios[0] = '\0';
        if (!token || !token[0]) return;
        if (std::strcmp(token, "openbios") != 0 &&
            std::strcmp(token, "scph1001") != 0)
            return;
        std::snprintf(g_lnch_session_bios, sizeof(g_lnch_session_bios), "%s",
                      token);
    }

    static void ae_np_clear_session_bios_token(void) {
        g_lnch_session_bios[0] = '\0';
    }

    static void ae_np_lan_clear_slot_bios(int slot) {
        if (slot < 0 || slot >= kAeLanMaxSlots) return;
        g_lnch_lan_slot_bios[slot] = {};
    }

    static void ae_np_lan_store_slot_bios(int slot, int prefer_open, int can_open,
                                         int can_scph) {
        if (slot < 0 || slot >= kAeLanMaxSlots) return;
        AeLanSlotBios& b = g_lnch_lan_slot_bios[slot];
        b.valid = 1;
        b.prefer_openbios = prefer_open ? 1 : 0;
        b.can_openbios = can_open ? 1 : 0;
        b.can_scph1001 = can_scph ? 1 : 0;
        if (!b.can_openbios && !b.can_scph1001) b.can_openbios = 1;
    }

    static void ae_np_lan_sync_local_slot_bios(void) {
        ae_np_refresh_bios_offer_from_disk();
        const PsxLobbyBiosOffer* offer = psx_lobby_bios_offer();
        int slot = -1;
        if (g_lnch_hosting_lan) {
            AeLanLobbyState st;
            if (ae_np_read_lan_state(&st)) slot = st.host_slot;
        } else {
            slot = g_lnch_lan_my_slot;
        }
        if (slot < 0 || !offer || !offer->valid) return;
        ae_np_lan_store_slot_bios(slot, offer->prefer_openbios, offer->can_openbios,
                                  offer->can_scph1001);
    }

    /* Same settle rule as psx_lobby_settle_session_bios, over LAN seat offers. */
    static int ae_np_lan_settle_session_bios(char* out, size_t out_cap) {
        if (!out || out_cap < 9) return -1;
        out[0] = '\0';
        AeLanLobbyState st;
        if (!ae_np_read_lan_state(&st)) {
            std::strncpy(out, "openbios", out_cap - 1);
            out[out_cap - 1] = '\0';
            return 0;
        }
        ae_np_lan_sync_local_slot_bios();
        int any_prefer_open = 0;
        int any_cannot_scph = 0;
        int host_prefer_scph = 0;
        int saw_peer = 0;
        const int host_slot =
            (st.host_slot >= 0 && st.host_slot < kAeLanMaxSlots) ? st.host_slot : 0;
        for (int i = 0; i < st.max_slots && i < kAeLanMaxSlots; ++i) {
            if (st.slot_name[i].empty()) continue;
            saw_peer = 1;
            const AeLanSlotBios& b = g_lnch_lan_slot_bios[i];
            if (!b.valid) {
                any_cannot_scph = 1;
                continue;
            }
            if (b.prefer_openbios) any_prefer_open = 1;
            if (!b.can_scph1001) any_cannot_scph = 1;
            if (i == host_slot && !b.prefer_openbios && b.can_scph1001)
                host_prefer_scph = 1;
        }
        if (!saw_peer) any_cannot_scph = 1;
        if (any_cannot_scph)
            std::strncpy(out, "openbios", out_cap - 1);
        else if (host_prefer_scph)
            std::strncpy(out, "scph1001", out_cap - 1);
        else if (any_prefer_open)
            std::strncpy(out, "openbios", out_cap - 1);
        else
            std::strncpy(out, "scph1001", out_cap - 1);
        out[out_cap - 1] = '\0';
        return 0;
    }

    static void ae_np_append_lan_bios_join(char* msg, size_t msg_cap, int* io_off) {
        if (!msg || !io_off || *io_off < 0) return;
        ae_np_refresh_bios_offer_from_disk();
        const PsxLobbyBiosOffer* offer = psx_lobby_bios_offer();
        const char* prefer = "openbios";
        int can_open = 1;
        int can_scph = 0;
        if (offer && offer->valid) {
            prefer = offer->prefer_openbios ? "openbios" : "scph1001";
            can_open = offer->can_openbios ? 1 : 0;
            can_scph = offer->can_scph1001 ? 1 : 0;
        }
        const int n = std::snprintf(msg + *io_off, msg_cap - (size_t)*io_off,
                                    "%s\n%d\n%d\n", prefer, can_open, can_scph);
        if (n > 0) *io_off += n;
    }

    /* Parse optional JOIN bios tail: prefer\ncan_open\ncan_scph\n */
    static int ae_np_parse_lan_bios_tail(char* p, int* prefer_open, int* can_open,
                                        int* can_scph) {
        if (!p || !prefer_open || !can_open || !can_scph) return -1;
        char* lines[3] = {};
        for (int i = 0; i < 3; ++i) {
            if (!p || !*p) return -1;
            lines[i] = p;
            char* nl = std::strchr(p, '\n');
            if (nl) {
                *nl = '\0';
                p = nl + 1;
            } else {
                p = p + std::strlen(p);
            }
        }
        if (std::strcmp(lines[0], "openbios") == 0)
            *prefer_open = 1;
        else if (std::strcmp(lines[0], "scph1001") == 0)
            *prefer_open = 0;
        else
            return -1;
        *can_open = (std::atoi(lines[1]) != 0) ? 1 : 0;
        *can_scph = (std::atoi(lines[2]) != 0) ? 1 : 0;
        return 0;
    }

    static void ae_np_append_feat(PsxLobbyModPkg* pkg, const char* feat) {
        if (!pkg || !feat || !feat[0]) return;
        if (std::strstr(pkg->feats, feat)) return;
        const size_t used = std::strlen(pkg->feats);
        const size_t add = std::strlen(feat);
        if (used + add + 2 >= sizeof(pkg->feats)) return;
        if (used) {
            pkg->feats[used] = ',';
            std::memcpy(pkg->feats + used + 1, feat, add + 1);
        } else {
            std::memcpy(pkg->feats, feat, add + 1);
        }
    }

    static void ae_np_fill_required_mods(PsxLobbyMatchCaps* caps) {
        if (!caps) return;
        caps->mod_count = 0;
        std::memset(caps->mods, 0, sizeof(caps->mods));
#if defined(RECOMP_LAUNCHER)
        const RecompLauncherCModProvider* p =
            PSXRecompV4::mod_runtime_launcher_provider();
        if (!p || !p->feature_count || !p->feature_get) return;
        const int n = p->feature_count(p->ctx);
        for (int i = 0; i < n && caps->mod_count < PSX_LOBBY_MAX_MODS; ++i) {
            RecompLauncherCModFeature f{};
            if (!p->feature_get(p->ctx, i, &f) || !f.enabled || !f.package_id[0])
                continue;
            int found = -1;
            for (int m = 0; m < caps->mod_count; ++m) {
                if (std::strcmp(caps->mods[m].id, f.package_id) == 0) {
                    found = m;
                    break;
                }
            }
            if (found < 0) {
                found = caps->mod_count++;
                std::snprintf(caps->mods[found].id, sizeof(caps->mods[found].id),
                              "%s", f.package_id);
                std::snprintf(caps->mods[found].ver, sizeof(caps->mods[found].ver),
                              "%s", f.package_version);
                std::snprintf(caps->mods[found].name, sizeof(caps->mods[found].name),
                              "%s", f.package_name[0] ? f.package_name : f.package_id);
                caps->mods[found].builtin =
                    (std::strncmp(f.package_id, "psx.", 4) == 0) ? 1 : 0;
            }
            ae_np_append_feat(&caps->mods[found], f.id);
        }
#endif
    }

    static void ae_np_refresh_mod_offer() {
        PsxLobbyModOffer offer{};
        offer.valid = 1;
#if defined(RECOMP_LAUNCHER)
        const RecompLauncherCModProvider* p =
            PSXRecompV4::mod_runtime_launcher_provider();
        if (p && p->package_count && p->package_get) {
            const int pc = p->package_count(p->ctx);
            for (int i = 0; i < pc && offer.count < PSX_LOBBY_MAX_MODS; ++i) {
                RecompLauncherCModPackage pkg{};
                if (!p->package_get(p->ctx, i, &pkg) || !pkg.id[0]) continue;
                int added = 0;
                if (p->version_count && p->version_get) {
                    const int vc = p->version_count(p->ctx, pkg.id);
                    for (int v = 0; v < vc && offer.count < PSX_LOBBY_MAX_MODS; ++v) {
                        RecompLauncherCModVersion ver{};
                        if (!p->version_get(p->ctx, pkg.id, v, &ver) || !ver.version[0])
                            continue;
                        std::snprintf(offer.pkgs[offer.count].id,
                                      sizeof(offer.pkgs[offer.count].id), "%s", pkg.id);
                        std::snprintf(offer.pkgs[offer.count].ver,
                                      sizeof(offer.pkgs[offer.count].ver), "%s",
                                      ver.version);
                        offer.count++;
                        added = 1;
                    }
                }
                if (!added && pkg.version[0] && offer.count < PSX_LOBBY_MAX_MODS) {
                    std::snprintf(offer.pkgs[offer.count].id,
                                  sizeof(offer.pkgs[offer.count].id), "%s", pkg.id);
                    std::snprintf(offer.pkgs[offer.count].ver,
                                  sizeof(offer.pkgs[offer.count].ver), "%s", pkg.version);
                    offer.count++;
                }
            }
        }
#endif
        psx_lobby_set_mod_offer(&offer);
    }

    struct AeModXferHost {
        int active;
        char to[PSX_LOBBY_ID_LEN];
        int pkg_i;
        int pkg_n;
        PsxLobbyModPkg pkgs[PSX_LOBBY_MAX_MODS];
        std::vector<uint8_t> bytes;
        std::string sha;
        uint32_t off;
    };
    static AeModXferHost g_xfer_host{};

    static int ae_np_host_load_pkg(AeModXferHost* x) {
        while (x->pkg_i < x->pkg_n) {
            std::string err;
            x->bytes.clear();
            x->sha.clear();
            x->off = 0;
            if (PSXRecompV4::mod_runtime_export_package(
                    x->pkgs[x->pkg_i].id, x->pkgs[x->pkg_i].ver, x->bytes, &x->sha,
                    &err))
                return 1;
            psx_lobby_mod_xfer_send_fail(x->to, err.c_str());
            x->pkg_i++;
        }
        return 0;
    }

    static void ae_np_host_xfer_tick() {
        if (!g_xfer_host.active) {
            char from[PSX_LOBBY_ID_LEN];
            PsxLobbyModPkg mods[PSX_LOBBY_MAX_MODS];
            const int n = psx_lobby_mod_xfer_pull(from, sizeof(from), mods,
                                                  PSX_LOBBY_MAX_MODS);
            if (n <= 0) return;
            g_xfer_host = {};
            g_xfer_host.active = 1;
            std::snprintf(g_xfer_host.to, sizeof(g_xfer_host.to), "%s", from);
            g_xfer_host.pkg_n = n;
            for (int i = 0; i < n; ++i) g_xfer_host.pkgs[i] = mods[i];
            g_xfer_host.pkg_i = 0;
            if (!ae_np_host_load_pkg(&g_xfer_host)) {
                /* Every export already sent mod_xfer_fail. */
                g_xfer_host.active = 0;
                return;
            }
        }
        if (!psx_lobby_mod_xfer_connected() || !psx_lobby_mod_xfer_send_idle())
            return;
        if (!g_xfer_host.bytes.empty()) {
            const size_t n = g_xfer_host.bytes.size();
            if (n > 0xffffffffu) {
                psx_lobby_mod_xfer_send_fail(g_xfer_host.to, "package exceeds ZIP32");
                g_xfer_host.active = 0;
                return;
            }
            uint8_t* zip = static_cast<uint8_t*>(std::malloc(n));
            if (!zip) {
                psx_lobby_mod_xfer_send_fail(g_xfer_host.to, "out of memory");
                g_xfer_host.active = 0;
                return;
            }
            std::memcpy(zip, g_xfer_host.bytes.data(), n);
            g_xfer_host.bytes.clear();
            g_xfer_host.bytes.shrink_to_fit();
            if (psx_lobby_mod_xfer_queue_pkg(
                    g_xfer_host.pkgs[g_xfer_host.pkg_i].id,
                    g_xfer_host.pkgs[g_xfer_host.pkg_i].ver,
                    g_xfer_host.sha.c_str(), zip, (uint32_t)n) != 0) {
                psx_lobby_mod_xfer_send_fail(g_xfer_host.to, "send failed");
                g_xfer_host.active = 0;
                return;
            }
            g_xfer_host.pkg_i++;
            if (!ae_np_host_load_pkg(&g_xfer_host) && psx_lobby_mod_xfer_send_idle())
                (void)psx_lobby_mod_xfer_queue_done();
            return;
        }
        if (g_xfer_host.pkg_i >= g_xfer_host.pkg_n) {
            (void)psx_lobby_mod_xfer_queue_done();
            g_xfer_host.active = 0;
        }
    }

    static void ae_np_guest_xfer_tick() {
        int all_ok = 1;
        for (;;) {
            PsxLobbyModPkg meta{};
            uint8_t* data = nullptr;
            uint32_t len = 0;
            if (!psx_lobby_mod_package_take(&meta, &data, &len)) break;
            std::string err;
            const bool ok =
                PSXRecompV4::mod_runtime_install_bytes(data, len, &err);
            std::free(data);
            if (!ok) {
                all_ok = 0;
                psx_lobby_mod_xfer_note_fail(err.c_str());
            }
        }
        if (all_ok && psx_lobby_mod_xfer_host_done() && !psx_lobby_in_lobby() &&
            psx_lobby_need_mods_count() > 0) {
            ae_np_refresh_mod_offer();
            const char* lid = psx_lobby_need_mods_lobby_id();
            const char* pw = psx_lobby_pending_join_password();
            const char* bind = psx_lobby_pending_join_bind();
            if (lid && lid[0])
                (void)psx_lobby_join(lid, pw ? pw : "", bind);
        }
    }

    PsxLobbyMatchCaps ae_netplay_caps_from_settings(const RecompLauncherCSettings* s) {
        PsxLobbyMatchCaps caps{};
        caps.valid = 1;
        switch (s ? s->aspect_index : 0) {
            case 2:  caps.aspect_num = 21; caps.aspect_den = 9; break;
            case 1:  caps.aspect_num = 16; caps.aspect_den = 9; break;
            default: caps.aspect_num = 4;  caps.aspect_den = 3; break;
        }
        caps.turbo_loads   = s ? (s->turbo_loads != 0) : 0;
        caps.auto_skip_fmv = s ? (s->auto_skip_fmv != 0) : 0;
        caps.input_delay   = g_lnch_lobby_input_delay;
        if (caps.input_delay < 2) caps.input_delay = 2;
        if (caps.input_delay > 20) caps.input_delay = 20;
        caps.input_prediction = g_lnch_lobby_input_prediction;
        if (caps.input_prediction < 2) caps.input_prediction = 2;
        if (caps.input_prediction > 16) caps.input_prediction = 16;
        caps.force_input_relay = g_lnch_force_input_relay != 0;
        caps.force_turn = g_lnch_force_turn != 0;
        caps.rollback = g_lnch_rollback != 0;
        caps.multitap_analog = g_lnch_multitap_analog != 0;
        if (s) caps.multitap_analog = s->multitap_analog != 0;
        ae_np_fill_required_mods(&caps);
        return caps;
    }

    static void ae_np_push_match_caps(const RecompLauncherCSettings* settings) {
        if (!psx_lobby_in_lobby() || !psx_lobby_is_host()) return;
        const PsxLobbyMatchCaps* cur = psx_lobby_match_caps();
        PsxLobbyMatchCaps caps = (cur && cur->valid)
            ? *cur
            : ae_netplay_caps_from_settings(settings);
        caps.valid = 1;
        caps.input_delay = g_lnch_lobby_input_delay;
        if (caps.input_delay < 2) caps.input_delay = 2;
        if (caps.input_delay > 20) caps.input_delay = 20;
        caps.input_prediction = g_lnch_lobby_input_prediction;
        if (caps.input_prediction < 2) caps.input_prediction = 2;
        if (caps.input_prediction > 16) caps.input_prediction = 16;
        caps.force_input_relay = g_lnch_force_input_relay != 0;
        caps.force_turn = g_lnch_force_turn != 0;
        caps.rollback = g_lnch_rollback != 0;
        caps.multitap_analog = g_lnch_multitap_analog != 0;
        if (settings) caps.multitap_analog = settings->multitap_analog != 0;
        ae_np_fill_required_mods(&caps);
        (void)psx_lobby_set_match_caps(&caps);
    }

    int ae_np_input_delay_get(void*) {
        /* Online guests show host-authoritative match_caps. */
        if (!g_lnch_hosting_lan && !g_lnch_joined_lan) {
            const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
            if (caps && caps->valid) {
                int d = caps->input_delay;
                if (d < 2) d = 2;
                if (d > 20) d = 20;
                return d;
            }
        }
        return g_lnch_lobby_input_delay;
    }
    int ae_np_input_delay_set(void*, int delay_frames) {
        if (delay_frames < 2) delay_frames = 2;
        if (delay_frames > 20) delay_frames = 20;
        g_lnch_lobby_input_delay = delay_frames;
        ae_np_push_match_caps(nullptr);
        return 0;
    }
    int ae_np_force_input_relay_get(void*) {
        if (!g_lnch_hosting_lan && !g_lnch_joined_lan) {
            const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
            if (caps && caps->valid)
                return caps->force_input_relay ? 1 : 0;
        }
        return g_lnch_force_input_relay;
    }
    int ae_np_force_input_relay_set(void*, int force) {
        g_lnch_force_input_relay = force ? 1 : 0;
        ae_np_push_match_caps(nullptr);
        return 0;
    }
    int ae_np_force_turn_get(void*) {
        if (!g_lnch_hosting_lan && !g_lnch_joined_lan) {
            const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
            if (caps && caps->valid)
                return caps->force_turn ? 1 : 0;
        }
        return g_lnch_force_turn;
    }
    int ae_np_force_turn_set(void*, int force) {
        if (g_lnch_hosting_lan || g_lnch_joined_lan)
            return 0; /* LAN/Direct IP — no online delay-floor hint */
        /* Delay floor only (§108); does not change SFU vs ICE transport. */
        g_lnch_force_turn = force ? 1 : 0;
        ae_np_push_match_caps(nullptr);
        return 0;
    }
    int ae_np_rollback_get(void*) {
        if (!g_lnch_hosting_lan && !g_lnch_joined_lan) {
            const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
            if (caps && caps->valid)
                return caps->rollback ? 1 : 0;
        }
        return g_lnch_rollback;
    }
    int ae_np_rollback_set(void*, int enable) {
        g_lnch_rollback = enable ? 1 : 0;
        ae_np_push_match_caps(nullptr);
        return 0;
    }
    int ae_np_multitap_analog_get(void*) {
        if (g_force_digital_pads)
            return 0;
        if (!g_lnch_hosting_lan && !g_lnch_joined_lan) {
            const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
            if (caps && caps->valid)
                return caps->multitap_analog ? 1 : 0;
        }
        return g_lnch_multitap_analog;
    }
    int ae_np_multitap_analog_set(void*, int enable) {
        if (g_force_digital_pads)
            enable = 0;
        g_lnch_multitap_analog = enable ? 1 : 0;
        ae_np_push_match_caps(nullptr);
        return 0;
    }
    int ae_np_input_prediction_get(void*) {
        if (!g_lnch_hosting_lan && !g_lnch_joined_lan) {
            const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
            if (caps && caps->valid) {
                int p = caps->input_prediction;
                if (p < 2) p = 2;
                if (p > 16) p = 16;
                return p;
            }
        }
        return g_lnch_lobby_input_prediction;
    }
    int ae_np_input_prediction_set(void*, int prediction_frames) {
        if (prediction_frames < 2) prediction_frames = 2;
        if (prediction_frames > 16) prediction_frames = 16;
        g_lnch_lobby_input_prediction = prediction_frames;
        ae_np_push_match_caps(nullptr);
        return 0;
    }

    /* Seat ceiling for the active room (listing / LOBBY UI). 0 if unknown. */
    int ae_np_lobby_max_slots(void*) {
        if (g_lnch_hosting_lan || g_lnch_joined_lan) {
            AeLanLobbyState state;
            if (ae_np_read_lan_state(&state) && state.max_slots >= 2)
                return state.max_slots;
            return g_lnch_host_max_slots >= 2 ? g_lnch_host_max_slots : 0;
        }
        if (psx_lobby_in_lobby()) {
            const PsxLobbyJoinInfo* ji = psx_lobby_join_info();
            if (ji && ji->max_slots >= 2) return ji->max_slots;
            return g_lnch_host_max_slots >= 2 ? g_lnch_host_max_slots : 0;
        }
        return 0;
    }

    const char* ae_np_default_url(void*) {
        return g_lnch_lobby_url.empty() ? psx_lobby_default_url() : g_lnch_lobby_url.c_str();
    }

    void ae_np_save_identity(const char* player_name, const char* lobby_url) {
        if (g_lnch_settings_path.empty()) return;
        PSXRecompV4::UserSettings settings =
            PSXRecompV4::load_user_settings(g_lnch_settings_path);
        if (settings.parse_error) return;
        if (player_name && player_name[0]) {
            settings.netplay_player_name = player_name;
            settings.has_netplay_player_name = true;
        }
        if (lobby_url && lobby_url[0]) {
            settings.netplay_lobby_url = lobby_url;
            settings.has_netplay_lobby_url = true;
        }
        PSXRecompV4::save_user_settings(g_lnch_settings_path, settings);
    }

    void ae_np_set_lobby_url(void*, const char* url) {
        g_lnch_lobby_url = url && url[0] ? url : psx_lobby_default_url();
        ae_np_save_identity(nullptr, g_lnch_lobby_url.c_str());
    }

    int ae_np_connect(void*) {
        psx_lobby_set_game_identity(g_lnch_netplay_game_name.c_str(), psx_lobby_game_version());
        psx_lobby_set_disc_fp(g_session_disc_fp.c_str());
        psx_lobby_set_max_slots(g_lnch_game_players);
        const int rc = psx_lobby_connect(ae_np_default_url(nullptr));
        /* connect resets g_lc; re-apply so create/join never advertise "". */
        psx_lobby_set_game_identity(g_lnch_netplay_game_name.c_str(), psx_lobby_game_version());
        psx_lobby_set_disc_fp(g_session_disc_fp.c_str());
        psx_lobby_set_max_slots(g_lnch_game_players);
        return rc;
    }

    int ae_np_connected(void*) {
        return psx_lobby_connected();
    }

    static void ae_np_lan_udp_pump(void) {
        if (!g_lnch_hosting_lan && !(g_lnch_joined_lan && g_lnch_remote_lan))
            return;

        if (g_lnch_hosting_lan) {
            AeLanLobbyState st;
            if (!ae_np_read_lan_state(&st)) return;
            const int lobby_port = ae_np_lan_endpoint_port(st.endpoint);
            if (g_lnch_lan_udp == kAeLanSockInvalid)
                (void)ae_np_lan_udp_ensure(true, lobby_port);
            /* Periodic broadcast BEACON so Refresh can find us without a
             * unicast target (and for guests that listen between browses). */
            if (g_lnch_lan_udp != kAeLanSockInvalid && !st.started) {
                const uint32_t now = SDL_GetTicks();
                if (g_lnch_lan_beacon_announce_ms == 0u ||
                    now - g_lnch_lan_beacon_announce_ms >= 2000u) {
                    g_lnch_lan_beacon_announce_ms = now;
                    ae_np_lan_sock_set_broadcast(g_lnch_lan_udp);
                    char beacon[768];
                    if (ae_np_lan_format_beacon(beacon, sizeof(beacon), st) > 0)
                        ae_np_lan_broadcast_msg(g_lnch_lan_udp, lobby_port, beacon);
                }
            }
        } else if (g_lnch_remote_lan) {
            if (g_lnch_lan_udp == kAeLanSockInvalid)
                (void)ae_np_lan_udp_ensure(false, 0);
            const uint32_t now = SDL_GetTicks();
            if (g_lnch_lan_udp != kAeLanSockInvalid &&
                now - g_lnch_lan_join_pulse_ms >= 400u) {
                g_lnch_lan_join_pulse_ms = now;
                char host[64];
                if (ae_np_lan_endpoint_host(g_lnch_lan_endpoint, host, sizeof(host))) {
                    sockaddr_in to{};
                    to.sin_family = AF_INET;
                    to.sin_port = htons((uint16_t)ae_np_lan_endpoint_port(g_lnch_lan_endpoint));
                    if (inet_pton(AF_INET, host, &to.sin_addr) == 1) {
                        std::string me = psx_lobby_display_name();
                        if (me.empty()) me = "Player";
                        char msg[448];
                        int off = std::snprintf(
                            msg, sizeof(msg), "MOTK3 JOIN\n%s\n%s\n%s\n",
                            ae_np_lan_local_player_id(), me.c_str(),
                            g_lnch_remote_lan_state.password.c_str());
                        ae_np_append_lan_bios_join(msg, sizeof(msg), &off);
                        ae_np_lan_udp_sendto(to, msg);
                    }
                }
            }
        }

        if (g_lnch_lan_udp == kAeLanSockInvalid) return;

        for (;;) {
            char buf[1536];
            sockaddr_in from{};
#ifdef _WIN32
            int fromlen = (int)sizeof(from);
            const int n = recvfrom(g_lnch_lan_udp, buf, (int)sizeof(buf) - 1, 0,
                                   (sockaddr*)&from, &fromlen);
#else
            socklen_t fromlen = sizeof(from);
            const int n = (int)recvfrom(g_lnch_lan_udp, buf, sizeof(buf) - 1, 0,
                                        (sockaddr*)&from, &fromlen);
#endif
            if (n <= 0) break;
            buf[n] = '\0';

            if (std::strncmp(buf, "MOTK1 PING", 10) == 0 && g_lnch_hosting_lan) {
                ae_np_lan_udp_sendto(from, "MOTK1 PONG\n");
                continue;
            }

            /* LAN lobby discovery: answer browse with a BEACON (unicast). */
            if (std::strncmp(buf, "MOTK1 BROWSE", 12) == 0 && g_lnch_hosting_lan) {
                AeLanLobbyState st;
                if (ae_np_read_lan_state(&st) && !st.started) {
                    char beacon[768];
                    if (ae_np_lan_format_beacon(beacon, sizeof(beacon), st) > 0)
                        ae_np_lan_udp_sendto(from, beacon);
                }
                continue;
            }

            if (std::strncmp(buf, "MOTK3 JOIN\n", 11) == 0 && g_lnch_hosting_lan) {
                char* p = buf + 11;
                char* nl = std::strchr(p, '\n');
                if (!nl) continue;
                *nl = '\0';
                const char* player_id = p;
                p = nl + 1;
                nl = std::strchr(p, '\n');
                if (!nl) continue;
                *nl = '\0';
                const char* name = p;
                p = nl + 1;
                nl = std::strchr(p, '\n');
                const char* pass;
                char* bios_tail = nullptr;
                if (nl) {
                    *nl = '\0';
                    pass = p;
                    bios_tail = nl + 1;
                } else {
                    pass = p;
                }
                AeLanLobbyState st;
                if (!ae_np_read_lan_state(&st)) continue;
                if (st.password != pass) {
                    ae_np_lan_udp_sendto(from, "MOTK1 ERR\nbad_password\n");
                    continue;
                }
                const int slot = ae_np_lan_seat_guest(st, player_id, name);
                if (slot < 0) {
                    ae_np_lan_udp_sendto(from, "MOTK1 ERR\nfull\n");
                    continue;
                }
                int prefer_open = 1, can_open = 1, can_scph = 0;
                if (bios_tail &&
                    ae_np_parse_lan_bios_tail(bios_tail, &prefer_open, &can_open,
                                              &can_scph) == 0) {
                    ae_np_lan_store_slot_bios(slot, prefer_open, can_open, can_scph);
                } else {
                    /* Legacy JOIN without bios_offer — cannot assume SCPH. */
                    ae_np_lan_clear_slot_bios(slot);
                }
                st.started = false;
                ae_np_lan_sync_legacy_names(st);
                if (!ae_np_write_lan_state(st)) continue;
                ae_np_lan_set_peer_slot(slot, from);
                ae_np_lan_send_update_to_peers(st);
                continue;
            }

            if (std::strncmp(buf, "MOTK1 JOIN\n", 11) == 0 && g_lnch_hosting_lan) {
                char* p = buf + 11;
                char* nl = std::strchr(p, '\n');
                if (!nl) continue;
                *nl = '\0';
                const char* name = p;
                p = nl + 1;
                nl = std::strchr(p, '\n');
                if (nl) *nl = '\0';
                const char* pass = p;
                AeLanLobbyState st;
                if (!ae_np_read_lan_state(&st)) continue;
                if (st.password != pass) {
                    ae_np_lan_udp_sendto(from, "MOTK1 ERR\nbad_password\n");
                    continue;
                }
                /* Legacy JOIN: synthesize an id from peer addr so same-name
                 * clients still get distinct seats + (2)/(3) labels. */
                char synth_id[48];
                std::snprintf(synth_id, sizeof(synth_id), "motk1-%08x-%04x",
                              (unsigned)ntohl(from.sin_addr.s_addr),
                              (unsigned)ntohs(from.sin_port));
                const int slot = ae_np_lan_seat_guest(st, synth_id, name);
                if (slot < 0) {
                    ae_np_lan_udp_sendto(from, "MOTK1 ERR\nfull\n");
                    continue;
                }
                /* Legacy MOTK1 JOIN has no bios_offer. */
                ae_np_lan_clear_slot_bios(slot);
                st.started = false;
                ae_np_lan_sync_legacy_names(st);
                if (!ae_np_write_lan_state(st)) continue;
                ae_np_lan_set_peer_slot(slot, from);
                ae_np_lan_send_update_to_peers(st);
                continue;
            }

            if ((std::strncmp(buf, "MOTK3 LEAVE\n", 12) == 0 ||
                 std::strncmp(buf, "MOTK1 LEAVE\n", 12) == 0) &&
                g_lnch_hosting_lan) {
                const bool by_id = std::strncmp(buf, "MOTK3 LEAVE\n", 12) == 0;
                char* p = buf + 12;
                char* nl = std::strchr(p, '\n');
                if (nl) *nl = '\0';
                const char* leave_key = (p && p[0]) ? p : nullptr;
                AeLanLobbyState st;
                if (!ae_np_read_lan_state(&st)) continue;
                int cleared = -1;
                if (leave_key) {
                    if (by_id) {
                        cleared = ae_np_lan_find_slot_by_id(st, leave_key);
                        if (cleared == st.host_slot) cleared = -1;
                    } else {
                        for (int i = 0; i < st.max_slots; ++i) {
                            if (i == st.host_slot) continue;
                            if (st.slot_name[i] == leave_key) {
                                cleared = i;
                                break;
                            }
                        }
                    }
                    if (cleared >= 0) {
                        st.slot_name[cleared].clear();
                        st.slot_id[cleared].clear();
                        ae_np_lan_clear_slot_bios(cleared);
                    }
                } else {
                    /* Legacy leave without name: drop first guest. */
                    for (int i = 0; i < st.max_slots; ++i) {
                        if (i == st.host_slot) continue;
                        if (!st.slot_name[i].empty()) {
                            st.slot_name[i].clear();
                            st.slot_id[i].clear();
                            ae_np_lan_clear_slot_bios(i);
                            cleared = i;
                            break;
                        }
                    }
                }
                st.started = false;
                ae_np_lan_sync_legacy_names(st);
                ae_np_write_lan_state(st);
                if (cleared >= 0) ae_np_lan_clear_peer_slot(cleared);
                ae_np_lan_send_update_to_peers(st);
                continue;
            }

            if (!g_lnch_remote_lan) continue;

            if (std::strncmp(buf, "MOTK4 UPDATE\n", 13) == 0) {
                AeLanLobbyState st = g_lnch_remote_lan_state;
                if (ae_np_lan_parse_motk4_update(buf + 13, &st) != 0) continue;
                st.endpoint = g_lnch_lan_endpoint;
                g_lnch_remote_lan_state = st;
                const int my_slot =
                    ae_np_lan_find_slot_by_id(st, ae_np_lan_local_player_id());
                if (my_slot < 0) {
                    g_lnch_joined_lan = false;
                    g_lnch_remote_lan = false;
                    g_lnch_lan_endpoint.clear();
                    g_lnch_lan_my_slot = -1;
                    ae_np_lan_udp_close();
                } else {
                    g_lnch_lan_my_slot = my_slot;
                    ae_np_lan_sync_local_slot_bios();
                }
                continue;
            }

            if (std::strncmp(buf, "MOTK3 UPDATE\n", 13) == 0) {
                AeLanLobbyState st = g_lnch_remote_lan_state;
                if (ae_np_lan_parse_motk3_update(buf + 13, &st) != 0) continue;
                st.endpoint = g_lnch_lan_endpoint;
                g_lnch_remote_lan_state = st;
                const int my_slot =
                    ae_np_lan_find_slot_by_id(st, ae_np_lan_local_player_id());
                if (my_slot < 0) {
                    g_lnch_joined_lan = false;
                    g_lnch_remote_lan = false;
                    g_lnch_lan_endpoint.clear();
                    g_lnch_lan_my_slot = -1;
                    ae_np_lan_udp_close();
                } else {
                    g_lnch_lan_my_slot = my_slot;
                }
                continue;
            }

            if (std::strncmp(buf, "MOTK2 UPDATE\n", 13) == 0) {
                AeLanLobbyState st = g_lnch_remote_lan_state;
                if (ae_np_lan_parse_motk2_update(buf + 13, &st) != 0) continue;
                st.endpoint = g_lnch_lan_endpoint;
                g_lnch_remote_lan_state = st;
                if (g_lnch_lan_my_slot >= 0 &&
                    g_lnch_lan_my_slot < st.max_slots &&
                    !st.slot_name[g_lnch_lan_my_slot].empty()) {
                    /* keep assigned slot */
                } else {
                    std::string me = psx_lobby_display_name();
                    if (me.empty()) me = "Player";
                    const int my_slot =
                        ae_np_lan_find_guest_slot_by_name(st, me.c_str());
                    if (my_slot < 0) {
                        g_lnch_joined_lan = false;
                        g_lnch_remote_lan = false;
                        g_lnch_lan_endpoint.clear();
                        g_lnch_lan_my_slot = -1;
                        ae_np_lan_udp_close();
                    } else {
                        g_lnch_lan_my_slot = my_slot;
                    }
                }
                continue;
            }
            if (std::strncmp(buf, "MOTK1 UPDATE\n", 13) == 0) {
                char* p = buf + 13;
                char* lines[4] = {};
                for (int i = 0; i < 4; ++i) {
                    lines[i] = p;
                    char* nl = std::strchr(p, '\n');
                    if (!nl) break;
                    *nl = '\0';
                    p = nl + 1;
                }
                if (!lines[0] || !lines[1] || !lines[2] || !lines[3]) continue;
                g_lnch_remote_lan_state.host_name = lines[0];
                g_lnch_remote_lan_state.joiner_name = lines[1];
                g_lnch_remote_lan_state.host_slot = (std::atoi(lines[2]) == 1) ? 1 : 0;
                g_lnch_remote_lan_state.started = std::atoi(lines[3]) != 0;
                g_lnch_remote_lan_state.max_slots = 2;
                g_lnch_remote_lan_state.slot_name[0].clear();
                g_lnch_remote_lan_state.slot_name[1].clear();
                g_lnch_remote_lan_state.slot_name[g_lnch_remote_lan_state.host_slot] =
                    g_lnch_remote_lan_state.host_name;
                g_lnch_remote_lan_state.slot_name[1 - g_lnch_remote_lan_state.host_slot] =
                    g_lnch_remote_lan_state.joiner_name;
                std::string me = psx_lobby_display_name();
                if (me.empty()) me = "Player";
                if (g_lnch_remote_lan_state.joiner_name != me) {
                    g_lnch_joined_lan = false;
                    g_lnch_remote_lan = false;
                    g_lnch_lan_endpoint.clear();
                    g_lnch_lan_my_slot = -1;
                    ae_np_lan_udp_close();
                } else {
                    g_lnch_lan_my_slot = 1 - g_lnch_remote_lan_state.host_slot;
                }
                continue;
            }
            if (std::strncmp(buf, "MOTK1 START\n", 12) == 0) {
                g_lnch_remote_lan_state.started = true;
                /* MOTK1 START\n<session>\n[<delay>\n<prediction>\n<rollback>\n
                 * [<session_bios>\n]]
                 * Trailing caps are host-authoritative (incl. settled BIOS). */
                char* p = buf + 12;
                char* nl = std::strchr(p, '\n');
                if (nl) *nl = '\0';
                if (*p) {
                    const unsigned v = (unsigned)std::strtoul(p, nullptr, 10);
                    if (v) {
                        g_lnch_lan_session_id = (uint32_t)v;
                        g_lnch_remote_lan_state.session_id = (uint32_t)v;
                    }
                }
                if (nl) {
                    p = nl + 1;
                    char* lines[4] = {};
                    for (int i = 0; i < 4; ++i) {
                        if (!p || !*p) break;
                        lines[i] = p;
                        char* n2 = std::strchr(p, '\n');
                        if (n2) {
                            *n2 = '\0';
                            p = n2 + 1;
                        } else {
                            p = nullptr;
                        }
                    }
                    if (lines[0] && lines[0][0]) {
                        int d = std::atoi(lines[0]);
                        if (d < 2) d = 2;
                        if (d > 20) d = 20;
                        g_lnch_lobby_input_delay = d;
                    }
                    if (lines[1] && lines[1][0]) {
                        int pred = std::atoi(lines[1]);
                        if (pred < 2) pred = 2;
                        if (pred > 16) pred = 16;
                        g_lnch_lobby_input_prediction = pred;
                    }
                    if (lines[2] && lines[2][0])
                        g_lnch_rollback = (std::atoi(lines[2]) != 0) ? 1 : 0;
                    if (lines[3] && lines[3][0])
                        ae_np_set_session_bios_token(lines[3]);
                    else
                        /* Legacy host: force OpenBIOS so mixed local prefs
                         * cannot silently desync. */
                        ae_np_set_session_bios_token("openbios");
                }
                continue;
            }
            if (std::strncmp(buf, "MOTK1 KICK\n", 11) == 0 ||
                std::strncmp(buf, "MOTK1 ERR\n", 10) == 0) {
                g_lnch_joined_lan = false;
                g_lnch_remote_lan = false;
                g_lnch_lan_endpoint.clear();
                g_lnch_remote_lan_state = {};
                ae_np_lan_udp_close();
            }
        }
    }

    int ae_np_connecting(void*) {
        return psx_lobby_connecting();
    }

    int ae_np_need_mods_count(void*) {
        return psx_lobby_need_mods_count();
    }
    int ae_np_need_mods_get(void*, int index, RecompLauncherCNetplayNeedMod* out) {
        if (!out) return 0;
        PsxLobbyModPkg pkg{};
        if (!psx_lobby_need_mods_get(index, &pkg)) return 0;
        std::snprintf(out->id, sizeof(out->id), "%s", pkg.id);
        std::snprintf(out->version, sizeof(out->version), "%s", pkg.ver);
        std::snprintf(out->name, sizeof(out->name), "%s",
                      pkg.name[0] ? pkg.name : pkg.id);
        out->builtin = pkg.builtin;
        out->size = pkg.size;
        return 1;
    }
    int ae_np_need_mods_can_transfer(void*) {
        return psx_lobby_need_mods_can_transfer();
    }
    int ae_np_mod_xfer_start(void*) {
        return psx_lobby_mod_xfer_start();
    }
    void ae_np_mod_xfer_cancel(void*) {
        psx_lobby_mod_xfer_cancel();
    }
    int ae_np_mod_xfer_progress(void*) {
        return psx_lobby_mod_xfer_progress();
    }
    int ae_np_mod_xfer_failed(void*, char* err, size_t err_cap) {
        return psx_lobby_mod_xfer_failed(err, err_cap);
    }

    void ae_np_pump(void*) {
        psx_lobby_pump();
        ae_np_lan_browse_pump();
        ae_np_lan_udp_pump();
        ae_np_refresh_mod_offer();
        if (!g_lnch_hosting_lan && !g_lnch_joined_lan) {
            if (psx_lobby_is_host()) ae_np_host_xfer_tick();
            ae_np_guest_xfer_tick();
        }
        /* Lobby UI has no Ready toggle; production WS still requires every
         * seated player ready before start. Keep seats ready while in-room
         * (including after soft-return rematch clears ready). Re-advertise
         * when the local BIOS offer changes so settle stays current. */
        if (!g_lnch_hosting_lan && !g_lnch_joined_lan && psx_lobby_in_lobby()) {
            static PsxLobbyBiosOffer s_last_offer{};
            ae_np_refresh_bios_offer_from_disk();
            const PsxLobbyBiosOffer* cur = psx_lobby_bios_offer();
            const int offer_changed =
                !cur || !s_last_offer.valid ||
                cur->can_openbios != s_last_offer.can_openbios ||
                cur->can_scph1001 != s_last_offer.can_scph1001 ||
                cur->prefer_openbios != s_last_offer.prefer_openbios;
            if (!psx_lobby_local_ready() || offer_changed) {
                (void)psx_lobby_set_ready(1);
                if (cur) s_last_offer = *cur;
            }
        }
    }

    void ae_np_set_player_name(void*, const char* name) {
        psx_lobby_set_display_name(name && name[0] ? name : "Player");
        if (name && name[0]) ae_np_save_identity(name, nullptr);
    }

    const char* ae_np_player_name(void*) {
        return psx_lobby_display_name();
    }

    void ae_np_request_list(void*) {
        ae_np_lan_rescan();
        psx_lobby_request_list();
    }

    int ae_np_list_count(void*) {
        return psx_lobby_list_count() + ae_np_lan_list_extra_count();
    }

    int ae_np_list_get(void*, int index, RecompLauncherCNetplayLobby* out) {
        if (!out) return 0;
        const int remote_count = psx_lobby_list_count();
        if (index >= remote_count) {
            const int lan_i = index - remote_count;
            ae_np_lan_prune_discovered();
            if (g_lnch_lan_discovered_n > 0)
                return ae_np_lan_fill_lobby_from_discovered(lan_i, out);
            return (lan_i == 0 && ae_np_lan_list_visible())
                ? ae_np_read_lan_lobby(out) : 0;
        }
        PsxLobbyRow row{};
        if (!psx_lobby_list_get(index, &row)) return 0;
        std::snprintf(out->lobby_id, sizeof(out->lobby_id), "%s", row.lobby_id);
        std::snprintf(out->name, sizeof(out->name), "%s", row.name);
        std::snprintf(out->game_name, sizeof(out->game_name), "%s", row.game_name);
        std::snprintf(out->game_version, sizeof(out->game_version), "%s", row.game_version);
        out->player_count = row.player_count;
        out->max_slots = row.max_slots;
        out->has_password = row.has_password;
        out->latency_ms = row.latency_ms;
        return 1;
    }

    int ae_np_external_ip(void*, char* out, size_t out_len) {
        if (!out || out_len == 0) return 0;
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo("api.ipify.org", "80", &hints, &res) != 0 || !res)
            return 0;
#ifdef _WIN32
        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET) {
            freeaddrinfo(res);
            return 0;
        }
        DWORD timeout_ms = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms,
                   sizeof(timeout_ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms,
                   sizeof(timeout_ms));
#else
        int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s < 0) {
            freeaddrinfo(res);
            return 0;
        }
        timeval tv{};
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        int ok = 0;
#ifdef _WIN32
        const int connected =
            connect(s, res->ai_addr, (int)res->ai_addrlen) != SOCKET_ERROR;
#else
        const int connected =
            connect(s, res->ai_addr, (socklen_t)res->ai_addrlen) == 0;
#endif
        if (connected) {
            const char req[] =
                "GET / HTTP/1.1\r\n"
                "Host: api.ipify.org\r\n"
                "User-Agent: psxrecomp-netplay/1.0\r\n"
                "Connection: close\r\n\r\n";
#ifdef _WIN32
            (void)send(s, req, (int)strlen(req), 0);
            char resp[1024];
            int n = recv(s, resp, sizeof(resp) - 1, 0);
#else
            (void)send(s, req, strlen(req), 0);
            char resp[1024];
            ssize_t n = recv(s, resp, sizeof(resp) - 1, 0);
#endif
            if (n > 0) {
                resp[n] = '\0';
                char* body = strstr(resp, "\r\n\r\n");
                body = body ? body + 4 : resp;
                char ip[64] = {};
                int j = 0;
                for (int i = 0; body[i] && j < (int)sizeof(ip) - 1; ++i) {
                    if ((body[i] >= '0' && body[i] <= '9') || body[i] == '.')
                        ip[j++] = body[i];
                    else if (j > 0)
                        break;
                }
                if (j > 0) {
                    std::snprintf(out, out_len, "%s", ip);
                    ok = 1;
                }
            }
        }
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        freeaddrinfo(res);
        return ok;
    }

    /* Collect non-loopback IPv4 addresses for local_address_get. */
    static void ae_np_collect_local_addresses(
        std::vector<RecompLauncherCNetplayLocalAddress>* out) {
        if (!out) return;
        out->clear();
        auto push = [&](const char* ip, const char* label) {
            if (!ip || !ip[0]) return;
            if (std::strcmp(ip, "0.0.0.0") == 0 ||
                std::strcmp(ip, "127.0.0.1") == 0)
                return;
            for (const auto& existing : *out) {
                if (std::strcmp(existing.address, ip) == 0) return;
            }
            RecompLauncherCNetplayLocalAddress entry{};
            std::snprintf(entry.address, sizeof(entry.address), "%s", ip);
            if (label && label[0])
                std::snprintf(entry.label, sizeof(entry.label), "%s", label);
            out->push_back(entry);
        };
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                      GAA_FLAG_SKIP_DNS_SERVER;
        ULONG buf_len = 16 * 1024;
        std::vector<unsigned char> buf(buf_len);
        IP_ADAPTER_ADDRESSES* addrs =
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &buf_len);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            buf.resize(buf_len);
            addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
            rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &buf_len);
        }
        if (rc != NO_ERROR) return;
        for (IP_ADAPTER_ADDRESSES* a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            char label[64] = {};
            if (a->FriendlyName) {
                WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, label,
                                    (int)sizeof(label), nullptr, nullptr);
            }
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u;
                 u = u->Next) {
                if (!u->Address.lpSockaddr ||
                    u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                char ip[64] = {};
                if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
                push(ip, label);
            }
        }
#else
        struct ifaddrs* ifa = nullptr;
        if (getifaddrs(&ifa) != 0 || !ifa) return;
        for (struct ifaddrs* i = ifa; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
            char ip[64] = {};
            if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
            push(ip, i->ifa_name ? i->ifa_name : "");
        }
        freeifaddrs(ifa);
#endif
    }

    int ae_np_local_address_get(void*, int index,
                                RecompLauncherCNetplayLocalAddress* out) {
        if (!out || index < 0) return 0;
        std::memset(out, 0, sizeof(*out));
        std::vector<RecompLauncherCNetplayLocalAddress> addrs;
        ae_np_collect_local_addresses(&addrs);
        if (index >= (int)addrs.size()) return 0;
        *out = addrs[(size_t)index];
        return out->address[0] ? 1 : 0;
    }

    int ae_np_local_ip(void*, char* out, size_t out_len) {
        if (!out || out_len == 0) return 0;
        std::vector<RecompLauncherCNetplayLocalAddress> addrs;
        ae_np_collect_local_addresses(&addrs);
        if (addrs.empty()) {
            std::snprintf(out, out_len, "Unavailable");
            return 0;
        }
        std::snprintf(out, out_len, "%s", addrs[0].address);
        return 1;
    }

    /* LAN/Direct IP rooms own membership via the local file registry. Server
     * lobbies use WebSocket lobby_update. Never mix: LAN mode wins if set. */
    static bool ae_np_use_lan_members(void) {
        return g_lnch_hosting_lan || g_lnch_joined_lan;
    }

    static bool ae_np_use_ws_members(void) {
        return !ae_np_use_lan_members() && psx_lobby_in_lobby() != 0;
    }

    /* Joiner was cleared / file gone → treat as kicked or host left. */
    static void ae_np_poll_lan_joiner_still_seated(void) {
        if (!g_lnch_joined_lan) return;
        if (g_lnch_remote_lan) {
            /* Remote seat cleared by UDP KICK/ERR/UPDATE in pump. */
            return;
        }
        AeLanLobbyState state;
        if (!ae_np_read_lan_state(&state)) {
            g_lnch_joined_lan = false;
            g_lnch_lan_endpoint.clear();
            g_lnch_lan_my_slot = -1;
            return;
        }
        bool seated = false;
        const int by_id =
            ae_np_lan_find_slot_by_id(state, ae_np_lan_local_player_id());
        if (by_id >= 0 && by_id != state.host_slot) {
            seated = true;
            g_lnch_lan_my_slot = by_id;
        } else if (g_lnch_lan_my_slot >= 0 &&
                   g_lnch_lan_my_slot < state.max_slots &&
                   g_lnch_lan_my_slot != state.host_slot &&
                   !state.slot_name[g_lnch_lan_my_slot].empty()) {
            seated = true;
        } else {
            std::string me = psx_lobby_display_name();
            if (me.empty()) me = "Player";
            const int slot = ae_np_lan_find_guest_slot_by_name(state, me.c_str());
            if (slot >= 0) {
                seated = true;
                g_lnch_lan_my_slot = slot;
            }
        }
        if (!seated) {
            g_lnch_joined_lan = false;
            g_lnch_lan_endpoint.clear();
            g_lnch_lan_my_slot = -1;
        }
    }

    /* host_endpoint is in/out (capacity >= 64). Online may rewrite the UDP
     * port when the requested one is busy. Returns 0 ok, -4 port unavailable. */
    int ae_np_create(void*, const char* lobby_name, char* host_endpoint,
                     const char* password,
                     const RecompLauncherCSettings* settings,
                     int lan_only, int max_slots) {
        int game_max = g_lnch_game_players >= 2 ? g_lnch_game_players : 2;
        if (game_max > PSX_MAX_PLAYERS) game_max = PSX_MAX_PLAYERS;
        if (game_max > 8) game_max = 8;
        if (max_slots < 2) max_slots = 2;
        if (max_slots > game_max) max_slots = game_max;
        /* Lobby + delay-sync ceiling (party games up to 8). */
        if (max_slots > RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
            max_slots = RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
        g_lnch_host_max_slots = max_slots;
        PsxLobbyMatchCaps caps = ae_netplay_caps_from_settings(settings);
        char endpoint[96];
        if (host_endpoint && host_endpoint[0])
            std::snprintf(endpoint, sizeof(endpoint), "%s", host_endpoint);
        else
            std::snprintf(endpoint, sizeof(endpoint), "0.0.0.0:7777");
        const int want_port = ae_np_lan_endpoint_port(endpoint);

        /* LAN/Direct IP only: local registry + UDP membership (no lobby WS).
         * Online create always uses psx_lobby_create — even when the UI passes
         * a concrete LAN IPv4 for MotK-style host_bind rewrite. */
        if (lan_only) {
            /* LAN/Direct IP: exact port required — fail if busy. */
            if (psx_lobby_in_lobby())
                (void)psx_lobby_leave();
            if (!ae_np_write_lan_lobby(lobby_name, endpoint, password, max_slots))
                return -4;
            if (host_endpoint)
                std::snprintf(host_endpoint, 96, "%s", endpoint);
            return 0;
        }

        /* Online: auto-pick a free UDP port starting at the requested one. */
        const int free_port = ae_np_find_free_udp_port(want_port);
        if (free_port < 0) return -4;
        if (free_port != want_port &&
            !ae_np_endpoint_replace_port(endpoint, sizeof(endpoint), free_port)) {
            return -4;
        }
        if (host_endpoint)
            std::snprintf(host_endpoint, 96, "%s", endpoint);

        if (g_lnch_hosting_lan) {
            std::error_code ec;
            std::filesystem::remove(ae_np_lan_file(), ec);
        }
        ae_np_lan_udp_close();
        g_lnch_hosting_lan = false;
        g_lnch_joined_lan = false;
        g_lnch_remote_lan = false;
        g_lnch_remote_lan_state = {};
        g_lnch_lan_endpoint.clear();
        psx_lobby_set_max_slots(max_slots);
        /* Ensure TOC fp survives connect/reset before the lobby stores it. */
        psx_lobby_set_disc_fp(g_session_disc_fp.c_str());
        return psx_lobby_create(lobby_name && lobby_name[0] ? lobby_name : "Netplay Lobby",
                                g_lnch_netplay_game_name.c_str(), psx_lobby_game_version(),
                                password ? password : "", endpoint, &caps);
    }

    /* guest_bind is in/out (capacity >= 64). recomp-ui fills a real UDP port
     * (prefer 7778); never advertise :0 to the lobby. */
    int ae_np_join(void*, const char* lobby_id, const char* password,
                   char* guest_bind) {
        char bind_buf[64];
        const char* bind = guest_bind;
        const char* colon = (bind && bind[0]) ? std::strrchr(bind, ':') : nullptr;
        const unsigned port = (colon && colon[1])
            ? static_cast<unsigned>(std::strtoul(colon + 1, nullptr, 10)) : 0u;
        if (!bind || !bind[0] || port == 0u) {
            std::snprintf(bind_buf, sizeof(bind_buf), "0.0.0.0:7778");
            bind = bind_buf;
            if (guest_bind)
                std::snprintf(guest_bind, 64, "%s", bind_buf);
        }
        if (lobby_id && strncmp(lobby_id, "lan:", 4) == 0) {
            const char* endpoint = lobby_id + 4;
            if (!endpoint[0]) return -1;
            if (psx_lobby_in_lobby())
                (void)psx_lobby_leave();

            /* Adopt host D/P/rollback from the last BEACON (if any) before
             * seating — START still re-asserts host-authoritative caps. */
            ae_np_lan_apply_discovered_caps(endpoint);

            /* Must be a live LAN/Direct IP host (UDP PONG). Online-only hosts
             * never answer — refuse so we don't open a fake local room. */
            if (!ae_np_lan_probe_host_ms(endpoint, 750u))
                return -3;

            /* Peek the on-disk registry (ignore any prior remote seat). */
            const bool prior_remote = g_lnch_remote_lan;
            g_lnch_remote_lan = false;
            AeLanLobbyState file{};
            const bool have_file = ae_np_read_lan_file_state(&file);
            g_lnch_remote_lan = prior_remote;
            /* Same-machine / shared cwd: claim the local LAN file when the
             * endpoint matches. */
            if (have_file && !g_lnch_hosting_lan && file.endpoint == endpoint) {
                if (file.password != (password ? password : "")) return -2;
                std::string me = psx_lobby_display_name();
                if (me.empty()) me = "Player";
                const int slot =
                    ae_np_lan_seat_guest(file, ae_np_lan_local_player_id(), me.c_str());
                if (slot < 0) return -1;
                file.started = false;
                ae_np_lan_sync_legacy_names(file);
                g_lnch_remote_lan = false;
                g_lnch_remote_lan_state = {};
                if (!ae_np_write_lan_state(file)) return -1;
                g_lnch_hosting_lan = false;
                g_lnch_joined_lan = true;
                g_lnch_lan_my_slot = slot;
                g_lnch_lan_endpoint = file.endpoint;
                return 0;
            }

            /* Cross-machine Join Direct: JOIN must be acked before we seat. */
            ae_np_lan_udp_close();
            AeLanLobbyState seated{};
            seated.name = "Direct";
            seated.game =
                g_lnch_netplay_game_name.empty() ? "PSX" : g_lnch_netplay_game_name;
            seated.endpoint = endpoint;
            seated.password = password ? password : "";
            const int ack = ae_np_lan_wait_join_ack(endpoint, password, &seated);
            if (ack != 0) {
                ae_np_lan_udp_close();
                g_lnch_joined_lan = false;
                g_lnch_remote_lan = false;
                g_lnch_remote_lan_state = {};
                g_lnch_lan_endpoint.clear();
                return ack;
            }
            g_lnch_remote_lan = true;
            g_lnch_remote_lan_state = seated;
            ae_np_lan_sync_legacy_names(g_lnch_remote_lan_state);
            g_lnch_hosting_lan = false;
            g_lnch_joined_lan = true;
            g_lnch_lan_endpoint = endpoint;
            g_lnch_lan_join_pulse_ms = SDL_GetTicks();
            return 0;
        }
        /* Server join: leave any stale LAN-file room mode so membership follows WS. */
        ae_np_lan_udp_close();
        g_lnch_hosting_lan = false;
        g_lnch_joined_lan = false;
        g_lnch_remote_lan = false;
        g_lnch_remote_lan_state = {};
        g_lnch_lan_endpoint.clear();
        /* Match host create: send the verified mount fp, not a wiped "". */
        psx_lobby_set_disc_fp(g_session_disc_fp.c_str());
        ae_np_refresh_mod_offer();
        return psx_lobby_join(lobby_id, password ? password : "", bind);
    }

    const char* ae_np_last_error(void*) {
        const PsxLobbyJoinInfo* ji = psx_lobby_join_info();
        return (ji && ji->last_error[0]) ? ji->last_error : nullptr;
    }

    void ae_np_clear_last_error(void*) {
        PsxLobbyJoinInfo* ji = const_cast<PsxLobbyJoinInfo*>(psx_lobby_join_info());
        if (ji) ji->last_error[0] = '\0';
    }

    int ae_np_leave(void*) {
        if (g_lnch_hosting_lan) {
            for (int i = 0; i < kAeLanMaxSlots; ++i) {
                if (g_lnch_lan_peer_ok[i])
                    ae_np_lan_udp_sendto(g_lnch_lan_peers[i], "MOTK1 KICK\n");
            }
            std::error_code ec;
            std::filesystem::remove(ae_np_lan_file(), ec);
            g_lnch_hosting_lan = false;
        } else if (g_lnch_joined_lan) {
            std::string me = psx_lobby_display_name();
            if (me.empty()) me = "Player";
            if (g_lnch_remote_lan) {
                char host[64];
                if (ae_np_lan_endpoint_host(g_lnch_lan_endpoint, host, sizeof(host)) &&
                    g_lnch_lan_udp != kAeLanSockInvalid) {
                    sockaddr_in to{};
                    to.sin_family = AF_INET;
                    to.sin_port =
                        htons((uint16_t)ae_np_lan_endpoint_port(g_lnch_lan_endpoint));
                    if (inet_pton(AF_INET, host, &to.sin_addr) == 1) {
                        char leave_msg[160];
                        std::snprintf(leave_msg, sizeof(leave_msg),
                                      "MOTK3 LEAVE\n%s\n",
                                      ae_np_lan_local_player_id());
                        ae_np_lan_udp_sendto(to, leave_msg);
                    }
                }
            } else {
                AeLanLobbyState state;
                if (ae_np_read_lan_state(&state)) {
                    int slot = ae_np_lan_find_slot_by_id(state, ae_np_lan_local_player_id());
                    if (slot < 0 && g_lnch_lan_my_slot >= 0 &&
                        g_lnch_lan_my_slot < state.max_slots &&
                        g_lnch_lan_my_slot != state.host_slot) {
                        slot = g_lnch_lan_my_slot;
                    }
                    if (slot < 0)
                        slot = ae_np_lan_find_guest_slot_by_name(state, me.c_str());
                    if (slot >= 0) {
                        state.slot_name[slot].clear();
                        state.slot_id[slot].clear();
                    }
                    state.started = false;
                    ae_np_lan_sync_legacy_names(state);
                    ae_np_write_lan_state(state);
                }
            }
        }
        ae_np_lan_udp_close();
        g_lnch_joined_lan = false;
        g_lnch_remote_lan = false;
        g_lnch_remote_lan_state = {};
        g_lnch_lan_endpoint.clear();
        g_lnch_lan_my_slot = -1;
        g_lnch_pending_direct_launch = {};
        return psx_lobby_leave();
    }
    int ae_np_in_lobby(void*) {
        ae_np_poll_lan_joiner_still_seated();
        if (g_lnch_hosting_lan) {
            AeLanLobbyState state;
            return ae_np_read_lan_state(&state) ? 1 : 0;
        }
        if (g_lnch_joined_lan) return 1;
        return psx_lobby_in_lobby();
    }
    int ae_np_is_host(void*) {
        if (ae_np_use_lan_members()) return g_lnch_hosting_lan ? 1 : 0;
        if (ae_np_use_ws_members()) return psx_lobby_is_host();
        return psx_lobby_is_host();
    }
    int ae_np_member_count(void*) {
        if (ae_np_use_ws_members()) {
            const int n = psx_lobby_member_count();
            return n > 0 ? n : 1;
        }
        if (ae_np_use_lan_members()) {
            AeLanLobbyState state;
            if (!ae_np_read_lan_state(&state)) return 1;
            const int n = ae_np_lan_occupied(state);
            return n > 0 ? n : 1;
        }
        return psx_lobby_member_count();
    }

    int ae_np_member_get(void*, int index, RecompLauncherCNetplayMember* out) {
        if (!out) return 0;
        if (ae_np_use_ws_members()) {
            PsxLobbyMember mem{};
            if (!psx_lobby_member_get(index, &mem)) return 0;
            out->slot = mem.slot;
            std::snprintf(out->display_name, sizeof(out->display_name), "%s",
                          mem.display_name);
            out->ready = mem.ready;
            const char* host_id = psx_lobby_host_player_id();
            /* Prefer host_player_id; slot-0 fallback only when unknown (never
             * mark a guest as host after a seat swap). */
            if (host_id && host_id[0] && mem.player_id[0])
                out->is_host = (std::strcmp(host_id, mem.player_id) == 0) ? 1 : 0;
            else
                out->is_host = (mem.slot == 0) ? 1 : 0;
            {
                const char* self_id = psx_lobby_player_id();
                out->is_local = (self_id && self_id[0] && mem.player_id[0] &&
                                 std::strcmp(self_id, mem.player_id) == 0)
                                    ? 1
                                    : 0;
            }
            out->latency_ms = psx_lobby_member_latency_ms(mem.slot);
            out->bios_offer_valid = mem.bios_offer_valid;
            out->bios_can_scph1001 = mem.bios_can_scph1001;
            out->bios_prefer_openbios = mem.bios_prefer_openbios;
            return 1;
        }
        if (ae_np_use_lan_members()) {
            AeLanLobbyState state;
            if (!ae_np_read_lan_state(&state)) return 0;
            int seen = 0;
            for (int slot = 0; slot < state.max_slots; ++slot) {
                if (state.slot_name[slot].empty()) continue;
                if (seen == index) {
                    out->slot = slot;
                    std::snprintf(out->display_name, sizeof(out->display_name), "%s",
                                  state.slot_name[slot].c_str());
                    out->ready = 1;
                    out->is_host = (slot == state.host_slot) ? 1 : 0;
                    out->is_local = (slot == g_lnch_lan_my_slot) ? 1 : 0;
                    out->latency_ms = -1;
                    if (out->is_local) ae_np_lan_sync_local_slot_bios();
                    {
                        const AeLanSlotBios& b = g_lnch_lan_slot_bios[slot];
                        out->bios_offer_valid = b.valid;
                        out->bios_can_scph1001 = b.can_scph1001;
                        out->bios_prefer_openbios = b.prefer_openbios;
                    }
                    return 1;
                }
                ++seen;
            }
            return 0;
        }
        PsxLobbyMember mem{};
        if (!psx_lobby_member_get(index, &mem)) return 0;
        out->slot = mem.slot;
        std::snprintf(out->display_name, sizeof(out->display_name), "%s", mem.display_name);
        out->ready = mem.ready;
        const char* host_id = psx_lobby_host_player_id();
        if (host_id && host_id[0] && mem.player_id[0])
            out->is_host = (std::strcmp(host_id, mem.player_id) == 0) ? 1 : 0;
        else
            out->is_host = (mem.slot == 0) ? 1 : 0;
        {
            const char* self_id = psx_lobby_player_id();
            out->is_local = (self_id && self_id[0] && mem.player_id[0] &&
                             std::strcmp(self_id, mem.player_id) == 0)
                                ? 1
                                : 0;
        }
        out->latency_ms = psx_lobby_member_latency_ms(mem.slot);
        out->bios_offer_valid = mem.bios_offer_valid;
        out->bios_can_scph1001 = mem.bios_can_scph1001;
        out->bios_prefer_openbios = mem.bios_prefer_openbios;
        return 1;
    }

    int ae_np_move_member(void*, int from_slot, int to_slot) {
        if (from_slot < 0 || to_slot < 0 || from_slot == to_slot) return -1;
        /* Slot 0 = session host / sim authority; guests rearrange only. */
        if (from_slot == 0 || to_slot == 0) return -1;
        if (g_lnch_hosting_lan) {
            AeLanLobbyState state;
            if (!ae_np_read_lan_state(&state)) return -1;
            if (from_slot < 0 || to_slot < 0 ||
                from_slot >= state.max_slots || to_slot >= state.max_slots)
                return -1;
            std::swap(state.slot_name[from_slot], state.slot_name[to_slot]);
            std::swap(state.slot_id[from_slot], state.slot_id[to_slot]);
            if (state.host_slot == from_slot) state.host_slot = to_slot;
            else if (state.host_slot == to_slot) state.host_slot = from_slot;
            /* Swap peer bindings with seats. */
            if (from_slot < kAeLanMaxSlots && to_slot < kAeLanMaxSlots) {
                std::swap(g_lnch_lan_peers[from_slot], g_lnch_lan_peers[to_slot]);
                std::swap(g_lnch_lan_peer_ok[from_slot], g_lnch_lan_peer_ok[to_slot]);
            }
            state.started = false;
            ae_np_lan_sync_legacy_names(state);
            if (!ae_np_write_lan_state(state)) return -1;
            ae_np_lan_send_update_to_peers(state);
            return 0;
        }
        if (ae_np_use_ws_members() && psx_lobby_is_host())
            return psx_lobby_move_member(from_slot, to_slot);
        return -1;
    }

    int ae_np_kick_member(void*, int slot) {
        if (g_lnch_hosting_lan) {
            AeLanLobbyState state;
            if (!ae_np_read_lan_state(&state)) return -1;
            if (slot < 0 || slot >= state.max_slots || slot == state.host_slot)
                return -1;
            if (state.slot_name[slot].empty()) return -1;
            state.slot_name[slot].clear();
            state.slot_id[slot].clear();
            ae_np_lan_clear_slot_bios(slot);
            state.started = false;
            ae_np_lan_sync_legacy_names(state);
            if (!ae_np_write_lan_state(state)) return -1;
            if (slot < kAeLanMaxSlots && g_lnch_lan_peer_ok[slot]) {
                ae_np_lan_udp_sendto(g_lnch_lan_peers[slot], "MOTK1 KICK\n");
                ae_np_lan_clear_peer_slot(slot);
            }
            ae_np_lan_send_update_to_peers(state);
            return 0;
        }
        if (ae_np_use_ws_members() && psx_lobby_is_host())
            return psx_lobby_kick(slot);
        return -1;
    }

    int ae_np_local_ready(void*) { return psx_lobby_local_ready(); }
    int ae_np_all_ready(void*) { return psx_lobby_all_ready(); }
    int ae_np_set_ready(void*, int ready) {
        if (ready)
            ae_np_refresh_bios_offer_from_disk();
        return psx_lobby_set_ready(ready);
    }

    int ae_np_request_start(void*, const RecompLauncherCSettings* settings) {
        if (g_lnch_hosting_lan) {
            AeLanLobbyState state;
            if (!ae_np_read_lan_state(&state) || ae_np_lan_occupied(state) < 2)
                return -1;
            if (settings && settings->bios_path[0])
                ae_np_refresh_bios_offer(settings->bios_path);
            else
                ae_np_refresh_bios_offer_from_disk();
            char session_bios[16] = {};
            (void)ae_np_lan_settle_session_bios(session_bios, sizeof(session_bios));
            ae_np_set_session_bios_token(session_bios[0] ? session_bios
                                                        : "openbios");
            std::fprintf(stdout, "psxrecomp: LAN settled session BIOS = %s\n",
                         g_lnch_session_bios[0] ? g_lnch_session_bios
                                               : "openbios");
            state.started = true;
            state.session_id += 1u;
            if (state.session_id == 0) state.session_id = 1;
            g_lnch_lan_session_id = state.session_id;
            if (!ae_np_write_lan_state(state)) return -1;
            ae_np_lan_send_update_to_peers(state);
            char start_msg[128];
            int delay = g_lnch_lobby_input_delay;
            int pred = g_lnch_lobby_input_prediction;
            if (delay < 2) delay = 2;
            if (delay > 20) delay = 20;
            if (pred < 2) pred = 2;
            if (pred > 16) pred = 16;
            std::snprintf(start_msg, sizeof(start_msg),
                          "MOTK1 START\n%u\n%d\n%d\n%d\n%s\n",
                          (unsigned)state.session_id, delay, pred,
                          g_lnch_rollback ? 1 : 0,
                          g_lnch_session_bios[0] ? g_lnch_session_bios
                                                : "openbios");
            for (int i = 0; i < kAeLanMaxSlots; ++i) {
                if (g_lnch_lan_peer_ok[i])
                    ae_np_lan_udp_sendto(g_lnch_lan_peers[i], start_msg);
            }
            return 0;
        }
        if (!psx_lobby_is_host()) return -1;
        /* Publish current BIOS offer, then ensure host seat is ready. */
        if (settings && settings->bios_path[0])
            ae_np_refresh_bios_offer(settings->bios_path);
        else
            ae_np_refresh_bios_offer_from_disk();
        (void)psx_lobby_set_ready(1);
        PsxLobbyMatchCaps caps = ae_netplay_caps_from_settings(settings);
        (void)psx_lobby_settle_session_bios(caps.session_bios,
                                            sizeof(caps.session_bios));
        ae_np_set_session_bios_token(caps.session_bios[0] ? caps.session_bios
                                                         : "openbios");
        std::fprintf(stdout, "psxrecomp: lobby settled session BIOS = %s\n",
                     caps.session_bios[0] ? caps.session_bios : "openbios");
        return psx_lobby_request_start(&caps);
    }

    int ae_np_launch_pending(void*) {
        if ((g_lnch_hosting_lan || g_lnch_joined_lan) &&
            !g_lnch_pending_direct_launch.enabled) {
            AeLanLobbyState state;
            if (ae_np_read_lan_state(&state) && state.started) {
                /* Guests may see started=1 on UPDATE before MOTK1 START
                 * delivers session_bios — wait so both peers boot the same BIOS. */
                if (g_lnch_joined_lan && !g_lnch_session_bios[0])
                    return g_lnch_pending_direct_launch.enabled ||
                           psx_lobby_launch_pending();
                /* Free the lobby UDP port before delay-sync binds it. */
                ae_np_lan_udp_close();
                g_lnch_lan_session_id = state.session_id ? state.session_id : 1u;
                g_lnch_pending_direct_launch = {};
                g_lnch_pending_direct_launch.enabled = 1;
                {
                    int local_slot = g_lnch_hosting_lan ? state.host_slot : g_lnch_lan_my_slot;
                    if (local_slot < 0)
                        local_slot = g_lnch_hosting_lan ? state.host_slot : 1;
                    g_lnch_pending_direct_launch.local_slot = local_slot;
                }
                /* -1 => resolve at netplay start (prefer NETPLAY/P1 card). */
                g_lnch_pending_direct_launch.input_player = -1;
                g_lnch_pending_direct_launch.session_id = g_lnch_lan_session_id;
                g_lnch_pending_direct_launch.input_delay = g_lnch_lobby_input_delay;
                g_lnch_pending_direct_launch.input_prediction =
                    g_lnch_lobby_input_prediction;
                g_lnch_pending_direct_launch.max_slots =
                    state.max_slots >= 2 ? state.max_slots
                    : (g_lnch_host_max_slots >= 2 ? g_lnch_host_max_slots
                       : (g_lnch_game_players >= 2 ? g_lnch_game_players : 2));
                if (g_lnch_pending_direct_launch.max_slots > PSX_MAX_PLAYERS)
                    g_lnch_pending_direct_launch.max_slots = PSX_MAX_PLAYERS;
                if (g_lnch_pending_direct_launch.max_slots > kAeLanMaxSlots)
                    g_lnch_pending_direct_launch.max_slots = kAeLanMaxSlots;
                g_lnch_pending_direct_launch.force_input_relay = 0;
                g_lnch_pending_direct_launch.force_turn = 0;
                g_lnch_pending_direct_launch.rollback = g_lnch_rollback ? 1 : 0;
                g_lnch_pending_direct_launch.player_count = ae_np_lan_occupied(state);
                {
                    uint32_t occ = 0;
                    int high = g_lnch_pending_direct_launch.local_slot;
                    for (int i = 0; i < state.max_slots && i < kAeLanMaxSlots; ++i) {
                        if (state.slot_name[i].empty() && state.slot_id[i].empty())
                            continue;
                        occ |= (1u << (unsigned)i);
                        if (i > high) high = i;
                    }
                    if (high + 1 > g_lnch_pending_direct_launch.player_count)
                        g_lnch_pending_direct_launch.player_count = high + 1;
                    if (occ == 0u) {
                        const int n = g_lnch_pending_direct_launch.player_count;
                        occ = (n >= 32) ? 0xffffffffu : ((1u << n) - 1u);
                    }
                    g_lnch_pending_direct_launch.occupied_mask = occ;
                }
                if (g_lnch_hosting_lan) {
                    const size_t colon = state.endpoint.rfind(':');
                    const char* port = colon == std::string::npos
                        ? "7777" : state.endpoint.c_str() + colon + 1;
                    std::snprintf(g_lnch_pending_direct_launch.bind_hostport,
                                  sizeof(g_lnch_pending_direct_launch.bind_hostport),
                                  "0.0.0.0:%s", port);
                    /* 3+ host-as-relay: empty peer → lan_hub. 2P: accept-first. */
                    g_lnch_pending_direct_launch.peer_hostport[0] = '\0';
                } else {
                    std::snprintf(g_lnch_pending_direct_launch.bind_hostport,
                                  sizeof(g_lnch_pending_direct_launch.bind_hostport), "0.0.0.0:0");
                    std::snprintf(g_lnch_pending_direct_launch.peer_hostport,
                                  sizeof(g_lnch_pending_direct_launch.peer_hostport), "%s",
                                  state.endpoint.c_str());
                }
            }
        }
        return g_lnch_pending_direct_launch.enabled || psx_lobby_launch_pending();
    }
    void ae_np_clear_launch_pending(void*) {
        g_lnch_pending_direct_launch = {};
        psx_lobby_clear_launch_pending();
    }

    /* After a match soft-exit: keep seats, clear started/ready, rebind LAN UDP. */
    void ae_np_prepare_lobby_rematch(void) {
        g_lnch_pending_direct_launch = {};
        ae_np_clear_session_bios_token();
        psx_lobby_set_ready(0);
        psx_lobby_clear_launch_pending();
        psx_lobby_clear_signals();
        psx_lobby_set_ice_signal_accept(0);
        psx_lobby_resume_waiting_room_rtt();
        if (!(g_lnch_hosting_lan || g_lnch_joined_lan)) return;
        AeLanLobbyState st;
        if (ae_np_read_lan_state(&st)) {
            st.started = false;
            (void)ae_np_write_lan_state(st);
        }
        if (g_lnch_hosting_lan && !g_lnch_lan_endpoint.empty()) {
            ae_np_lan_udp_close();
            (void)ae_np_lan_udp_ensure(true, ae_np_lan_endpoint_port(g_lnch_lan_endpoint));
            if (ae_np_read_lan_state(&st))
                ae_np_lan_send_update_to_peers(st);
        } else if (g_lnch_remote_lan) {
            ae_np_lan_udp_close();
            (void)ae_np_lan_udp_ensure(false, 0);
            g_lnch_lan_join_pulse_ms = 0;
        }
    }

    const char* ae_np_lan_endpoint_cstr(void) {
        return g_lnch_lan_endpoint.empty() ? nullptr : g_lnch_lan_endpoint.c_str();
    }

    int ae_np_fill_launch(void*, RecompLauncherCNetplayLaunch* out) {
        if (!out) return 0;
        if (g_lnch_pending_direct_launch.enabled) {
            *out = g_lnch_pending_direct_launch;
            return 1;
        }
        const PsxLobbyJoinInfo* ji = psx_lobby_join_info();
        if (!ji || !ji->ok) return 0;
        const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
        /* Host match_caps are required online — do not silently default D/relay. */
        if (!caps || !caps->valid) return 0;
        out->enabled = 1;
        out->local_slot = ji->local_slot;
        /* -1 => resolve at netplay start (prefer NETPLAY/P1 card). */
        out->input_player = -1;
        std::snprintf(out->bind_hostport, sizeof(out->bind_hostport), "%s", ji->bind_hostport);
        std::snprintf(out->peer_hostport, sizeof(out->peer_hostport), "%s", ji->peer_hostport);
        out->session_id = ji->session_id;
        out->input_delay = caps->input_delay;
        out->input_prediction = caps->input_prediction;
        if (out->input_prediction < 2) out->input_prediction = 2;
        if (out->input_prediction > 16) out->input_prediction = 16;
        out->max_slots = ji->max_slots >= 2 ? ji->max_slots
                         : (g_lnch_game_players >= 2 ? g_lnch_game_players : 2);
        if (out->max_slots > PSX_MAX_PLAYERS) out->max_slots = PSX_MAX_PLAYERS;
        /* Seated width for delay-sync (not lobby ceiling). Bump for sparse
         * seat indices so a moved player at slot N still fits. */
        {
            int seated = ji->player_count > 0 ? ji->player_count : 0;
            int high = ji->local_slot;
            uint32_t occ = 0;
            const int mc = psx_lobby_member_count();
            for (int i = 0; i < mc; ++i) {
                PsxLobbyMember mem{};
                if (!psx_lobby_member_get(i, &mem)) continue;
                if (mem.slot > high) high = mem.slot;
                if (mem.slot >= 0 && mem.slot < 32)
                    occ |= (1u << (unsigned)mem.slot);
            }
            if (ji->local_slot >= 0 && ji->local_slot < 32)
                occ |= (1u << (unsigned)ji->local_slot);
            if (high + 1 > seated) seated = high + 1;
            if (seated < 2) seated = out->max_slots;
            if (seated > out->max_slots) seated = out->max_slots;
            out->player_count = seated;
            /* Sparse rooms leave holes in 0..seated-1; tell netplay which
             * seats are real so READY/admit do not wait on phantoms. */
            if (occ == 0u) {
                occ = (seated >= 32) ? 0xffffffffu : ((1u << seated) - 1u);
            } else if (seated < 32) {
                occ &= (1u << seated) - 1u;
            }
            out->occupied_mask = occ;
        }
        /* §108: online launch always SFU. Prefer caps.force_input_relay from
         * relay_endpoint rewrite; also infer when host==guest advertise. */
        out->force_input_relay =
            (g_lnch_hosting_lan || g_lnch_joined_lan)
                ? 0
                : (caps->force_input_relay ? 1 : 0);
        if (!out->force_input_relay && !g_lnch_hosting_lan &&
            !g_lnch_joined_lan && ji->host_endpoint[0] &&
            ji->guest_endpoint[0] &&
            std::strcmp(ji->host_endpoint, ji->guest_endpoint) == 0) {
            out->force_input_relay = 1;
            std::fprintf(stderr,
                         "psxrecomp: launch force_input_relay inferred "
                         "(host_endpoint==guest_endpoint=%s)\n",
                         ji->host_endpoint);
        }
        out->force_turn = caps->force_turn ? 1 : 0;
        out->rollback =
            (caps && caps->valid && caps->rollback) ? 1 : 0;
        if (caps->session_bios[0])
            ae_np_set_session_bios_token(caps->session_bios);
        return 1;
    }

    RecompLauncherCNetplayCallbacks g_lnch_netplay_callbacks = {
        nullptr,
        ae_np_default_url,
        ae_np_set_lobby_url,
        ae_np_connect,
        ae_np_connected,
        ae_np_pump,
        ae_np_set_player_name,
        ae_np_player_name,
        ae_np_request_list,
        ae_np_list_count,
        ae_np_list_get,
        ae_np_local_ip,
        ae_np_external_ip,
        ae_np_create,
        ae_np_join,
        ae_np_leave,
        ae_np_in_lobby,
        ae_np_is_host,
        ae_np_member_count,
        ae_np_member_get,
        ae_np_move_member,
        ae_np_local_ready,
        ae_np_all_ready,
        ae_np_set_ready,
        ae_np_request_start,
        ae_np_launch_pending,
        ae_np_clear_launch_pending,
        ae_np_fill_launch,
        ae_np_local_address_get,
        ae_np_kick_member,
        ae_np_last_error,
        ae_np_clear_last_error,
        ae_np_input_delay_get,
        ae_np_input_delay_set,
        ae_np_force_input_relay_get,
        ae_np_force_input_relay_set,
        ae_np_lobby_max_slots,
        ae_np_force_turn_get,
        ae_np_force_turn_set,
        ae_np_rollback_get,
        ae_np_rollback_set,
        ae_np_input_prediction_get,
        ae_np_input_prediction_set,
        ae_np_multitap_analog_get,
        ae_np_multitap_analog_set,
        ae_np_connecting,
        ae_np_need_mods_count,
        ae_np_need_mods_get,
        ae_np_need_mods_can_transfer,
        ae_np_mod_xfer_start,
        ae_np_mod_xfer_cancel,
        ae_np_mod_xfer_progress,
        ae_np_mod_xfer_failed,
    };

    /* Shared by first-boot launcher and soft-return rematch UI so capability
     * flags (View mode / perspective / Skip FMVs / renderer labels) cannot
     * drift apart — a bare launcher_profile_apply("psx") hides those rows. */
    std::string g_rui_keybinds_path;
    std::string g_rui_config_ini_path;
    static const char* const kPsxRendererLabels[] = {
        "Software",
        "OpenGL (Recommended)",
        "Vulkan",
    };
    static const char* const kPsxHostShortcutLabels[] = {
        "Rewind",
        "Save states",
    };

    void ae_rui_set_sidecar_paths(const char* argv0) {
        const auto exe = exe_dir_from_argv(argv0 ? argv0 : "");
        g_rui_keybinds_path = (exe / "keybinds.ini").string();
        g_rui_config_ini_path = (exe / "config.ini").string();
    }

    void ae_fill_psx_launcher_game_info(
        RecompLauncherCGameInfo* gi,
        const char* game_name_c,
        const char* region_c,
        int game_players_n,
        bool ws_offered_b,
        bool ws_ultrawide_offered_b,
        bool skip_fmv_offered_b,
        bool turbo_loads_offered_b,
        bool vulkan_offered_b,
        bool ctrl_lock_mode_b,
        bool ctrl_lock_device_b,
        int locked_pad_mode_i,
        const char* const* language_labels,
        int num_languages,
        int resume_netplay_room)
    {
        if (!gi) return;
        (void)ws_offered_b;
        (void)ws_ultrawide_offered_b;
        launcher_profile_apply("psx", gi);
        gi->name = game_name_c;
        gi->region = region_c;
        gi->keybinds_path =
            g_rui_keybinds_path.empty() ? nullptr : g_rui_keybinds_path.c_str();
        gi->config_path =
            g_rui_config_ini_path.empty() ? nullptr : g_rui_config_ini_path.c_str();
        gi->has_expected_crc = 0;
        gi->num_known_sha256 = 0;
        /* PSX display-view controls are owned by trusted mods. Do not expose
         * the generic Display -> View mode launcher row. */
        gi->widescreen_supported = 0;
        gi->num_players = game_players_n;
        gi->msu1_supported = 0;
        gi->sram_path = nullptr;
        gi->has_bios = psx_bios_has_selectable();
        gi->pad_mode_selectable = ctrl_lock_mode_b ? 0 : 1;
        gi->locked_pad_mode = locked_pad_mode_i;
        gi->lock_device = ctrl_lock_device_b ? 1 : 0;
        gi->aspect_mask = 0;
        gi->renderer_labels = kPsxRendererLabels;
        gi->num_renderers = vulkan_offered_b ? 3 : 2;
        gi->settings_bindings = 1;
        gi->assist_binding_labels = kPsxHostShortcutLabels;
        gi->assist_binding_count = PSX_ASSIST_BIND_COUNT;
        gi->has_skip_fmv = skip_fmv_offered_b ? 1 : 0;
        gi->has_turbo_loads = turbo_loads_offered_b ? 1 : 0;
        gi->has_geometry_precision = 1;
        gi->has_rewind_depth = 1;
        if (language_labels && num_languages > 0) {
            gi->language_labels = language_labels;
            gi->num_languages = num_languages;
        } else {
            gi->language_labels = nullptr;
            gi->num_languages = 0;
        }
        gi->disc_verify = ae_disc_verify;
        gi->memcard_inspect = ae_memcard_inspect;
        gi->mods = PSXRecompV4::mod_runtime_launcher_provider();
        gi->bios_verify = ae_bios_verify;
        /* HD texture pack. The in-exe UI is deliberately just an on/off switch
         * plus the active pack's folder — installing, listing and comparing
         * packs is retcomm-launcher's per-title job, and duplicating that
         * surface here would mean two implementations of the same thing.
         * Offered only once a pack is actually selected, so a title with no
         * packs shows no dead control. */
        gi->hdpack_supported = g_hd_texture_pack.empty() ? 0 : 1;
#if defined(PSX_HAS_RECOMP_NET) && defined(PSX_HAS_LOBBY_CLIENT)
        g_lnch_game_players = game_players_n;
        /* ae_disc_verify only fills netplay_ok/disc_fp when this is true. */
        g_lnch_netplay_available =
            game_players_n >= 2 && game_players_n <= PSX_MAX_PLAYERS;
        gi->netplay_supported = g_lnch_netplay_available ? 1 : 0;
        gi->netplay = g_lnch_netplay_available
            ? &g_lnch_netplay_callbacks : nullptr;
#else
        g_lnch_netplay_available = false;
        gi->netplay_supported = 0;
        gi->netplay = nullptr;
#endif
        gi->resume_netplay_room = resume_netplay_room ? 1 : 0;
#if defined(PSX_HAS_LOBBY_CLIENT)
        gi->resume_netplay_endpoint =
            resume_netplay_room ? ae_np_lan_endpoint_cstr() : nullptr;
#else
        gi->resume_netplay_endpoint = nullptr;
#endif
    }
}  // namespace
#endif

/* PSX_SELF_PROFILE=<seconds>: 1 kHz instruction-pointer sampler over the main
 * (emu) thread, histogrammed by exe-relative offset and dumped to
 * psx_self_profile.txt at the end of the window. Play-build-safe hot-spot
 * attribution without WPA/ETL symbolization: feed the offsets (plus the PE
 * link base 0x140000000) to llvm-addr2line -e <exe>. Env-gated diagnostic —
 * zero cost when unset. */
#ifdef _WIN32
struct SelfProfArgs { HANDLE thread; int seconds; };
static DWORD WINAPI self_profile_thread(LPVOID param) {
    SelfProfArgs *a = (SelfProfArgs *)param;
    const int n = a->seconds * 1000;
    static uintptr_t rips[600000];
    int count = 0;
    HMODULE exe = GetModuleHandleA(NULL);
    MODULEINFO mi; memset(&mi, 0, sizeof(mi));
    GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi));
    uintptr_t base = (uintptr_t)mi.lpBaseOfDll;
    uintptr_t size = (uintptr_t)mi.SizeOfImage;
    for (int i = 0; i < n && count < 600000; i++) {
        Sleep(1);
        if (SuspendThread(a->thread) == (DWORD)-1) continue;
        CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(a->thread, &ctx)) rips[count++] = (uintptr_t)ctx.Rip;
        ResumeThread(a->thread);
    }
    /* Bucket by 64-byte line inside the exe; everything else = external. */
    std::map<uintptr_t, int> hist;
    int external = 0;
    for (int i = 0; i < count; i++) {
        if (rips[i] >= base && rips[i] < base + size)
            hist[(rips[i] - base) & ~63ull]++;
        else external++;
    }
    std::vector<std::pair<int, uintptr_t>> top;
    for (auto &kv : hist) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    FILE *f = fopen("psx_self_profile.txt", "wb");
    if (f) {
        fprintf(f, "samples=%d external=%d (exe base offsets, 64B buckets; "
                   "addr2line VA = 0x140000000 + offset)\n", count, external);
        for (size_t i = 0; i < top.size() && i < 120; i++)
            fprintf(f, "%6d  0x%llx\n", top[i].first,
                    (unsigned long long)top[i].second);
        fclose(f);
    }
    fprintf(stdout, "psxrecomp: [self-profile] %d samples (%d external) -> psx_self_profile.txt\n",
            count, external);
    fflush(stdout);
    return 0;
}
#endif

int main(int argc, char** argv) {
    /* Force line-buffered output so messages appear even if killed. */
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::fprintf(stderr, "psxrecomp: main() entered\n");
    std::fflush(stderr);
#if defined(RECOMP_LAUNCHER)
    launcher_boot_timing_mark("host:main_enter");
#endif

    /* Setup-host zip-root exe: after Generate & rebuild, hand off to the
     * product binary under build-release/ (bios/, mods/, assets/, settings). */
#if defined(PSX_HAS_GAME_CODEGEN)
    psx_game_codegen_forward_if_built(argc, argv);
#endif

    /* Install crash handlers early so they catch issues during init too.
     * Writes psx_last_run_report.json on signal/SEH/atexit/fail-fast. */
    psx_crash_trace_install_handlers();
#if defined(RECOMP_LAUNCHER)
    launcher_boot_timing_mark("host:crash_handlers");
#endif

    const char* bios_path = PSX_DEFAULT_BIOS_PATH;
    const char* game_config_path = nullptr;
    const char* disc_override_path = nullptr;
    bool        bios_from_cli = false;  /* CLI --bios/positional wins over settings.toml */
    /* Did the PLAYER choose this BIOS (CLI or settings), as opposed to it
     * being the compile-time default? Only a real choice overrides the
     * bundled OpenBIOS — see docs/BIOS_SELECTION.md. */
    bool        bios_explicit = false;
    /* Launcher overrides (mirrors snesrecomp): --launcher forces the GUI back on
     * even when [launcher] skip_launcher = true is set; --no-launcher (and the
     * PSX_NO_LAUNCHER env) forces it off. --launcher wins if both are given. */
    bool        force_launcher    = false;
    bool        force_no_launcher = false;
    /* CLI overrides for running several instances side by side (soak fleet).
     * These win over any game-config value and, crucially, work for the BIOS
     * (which has no [game]-block config schema, so debug_port/renderer can't be
     * supplied via --game). -1 = "not set on the CLI". */
    int         cli_debug_port = -1;
    int         cli_renderer   = -1;   /* 0=software 1=opengl 2=vulkan */
    const char* cli_window_title = nullptr;  /* label windows in a fleet */
    const char* cli_memcard_dir = nullptr;   /* isolate writable state in a fleet */
    PsxNetplayConfig net_cfg;
    psx_netplay_config_defaults(&net_cfg);
    psx_netplay_apply_env(&net_cfg);  /* CLI flags below win over env */
    /* Parse args.
     *   --bios <path>       override the compile-time BIOS path
     *   --game <toml>       load a game config (single source of truth for
     *                       disc / memcard / window title / debug port)
     *   --disc <path>       override the game config disc path
     *   --debug-port <n>    override the TCP debug-server port (multi-instance)
     *   --memcard-dir <path> override card/save/options state (multi-instance)
     *   --renderer <name>   override the renderer: software|opengl|vulkan
     *   --launcher          force the GUI launcher (overrides skip_launcher)
     *   --no-launcher       skip the GUI launcher (boot straight in)
     *   --headless          skip SDL window/audio; use TCP screenshots/state
     *   --netplay           enable delay-sync LAN (also PSX_NETPLAY=1)
     *   --net-slot N        local player slot (0|1)
     *   --net-input-player N  host device to sample (0=P1, 1=P2; default auto)
     *   --net-bind H:P      local UDP bind (default 0.0.0.0:7777)
     *   --net-peer H:P      peer host:port
     *   --net-delay N       input delay in sim ticks (default 2)
     *   --net-session-id N  must match peer (default 1)
     *   <positional>        deprecated alias for --bios
     * No --game-root flag: that remains config-driven. */
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
            bios_from_cli = true;
            bios_explicit = true;
        } else if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            game_config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            disc_override_path = argv[++i];
        } else if (std::strcmp(argv[i], "--debug-port") == 0 && i + 1 < argc) {
            cli_debug_port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--memcard-dir") == 0 && i + 1 < argc) {
            cli_memcard_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
            const char* r = argv[++i];
            if      (std::strcmp(r, "software") == 0) cli_renderer = 0;
            else if (std::strcmp(r, "opengl")   == 0) cli_renderer = 1;
            else if (std::strcmp(r, "vulkan")   == 0) cli_renderer = 2;
        } else if (std::strcmp(argv[i], "--window-title") == 0 && i + 1 < argc) {
            cli_window_title = argv[++i];
        } else if (std::strcmp(argv[i], "--launcher") == 0) {
            force_launcher = true;
        } else if (std::strcmp(argv[i], "--no-launcher") == 0) {
            force_no_launcher = true;
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
            force_no_launcher = true;
        } else if (std::strcmp(argv[i], "--netplay") == 0) {
            net_cfg.enabled = 1;
        } else if (std::strcmp(argv[i], "--net-slot") == 0 && i + 1 < argc) {
            net_cfg.enabled = 1;
            net_cfg.local_slot = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--net-input-player") == 0 && i + 1 < argc) {
            net_cfg.enabled = 1;
            net_cfg.input_player = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--net-bind") == 0 && i + 1 < argc) {
            net_cfg.enabled = 1;
            std::snprintf(net_cfg.bind_hostport, sizeof(net_cfg.bind_hostport), "%s", argv[++i]);
        } else if (std::strcmp(argv[i], "--net-peer") == 0 && i + 1 < argc) {
            net_cfg.enabled = 1;
            std::snprintf(net_cfg.peer_hostport, sizeof(net_cfg.peer_hostport), "%s", argv[++i]);
        } else if (std::strcmp(argv[i], "--net-delay") == 0 && i + 1 < argc) {
            net_cfg.enabled = 1;
            net_cfg.input_delay = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--net-session-id") == 0 && i + 1 < argc) {
            net_cfg.enabled = 1;
            net_cfg.session_id = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        } else if (argv[i][0] != '-') {
            /* The positional BIOS alias predates the named CLI. Consume it at
             * most once. In particular, never let a stray split argument
             * replace an explicit --bios path (for example when a Windows
             * launcher fails to quote a multi-word --window-title value). */
            if (!bios_from_cli) {
                bios_path = argv[i];
                bios_from_cli = true;
                bios_explicit = true;
            } else {
                std::fprintf(stderr,
                    "psxrecomp: ignoring unexpected positional argument after BIOS selection: %s\n",
                    argv[i]);
            }
        }
    }
    if (const char *e = std::getenv("PSX_HEADLESS")) {
        if (e[0] && e[0] != '0') {
            g_headless = 1;
            force_no_launcher = true;
        }
    }

    std::string default_game_config_storage;
    if (!game_config_path) {
        std::filesystem::path default_game_config =
            resolve_existing_runtime_path(PSX_DEFAULT_GAME_CONFIG_PATH, argv[0]);
        if (!default_game_config.empty()) {
            default_game_config_storage = default_game_config.string();
            game_config_path = default_game_config_storage.c_str();
        }
    } else if (!std::filesystem::path(game_config_path).is_absolute()) {
        // An explicit --game with a relative path must ALSO anchor on the exe
        // dir, never cwd — otherwise the disc / memcard_dir / game_options.toml
        // that resolve against this file's parent silently point at cwd. Resolve
        // it the same way the default path is resolved.
        std::filesystem::path resolved =
            resolve_existing_runtime_path(game_config_path, argv[0]);
        if (!resolved.empty()) {
            default_game_config_storage = resolved.string();
            game_config_path = default_game_config_storage.c_str();
        }
    }
    /* Record the resolved config path for the TCP fmv_state report (observability:
     * proves which game.toml actually loaded — CLI --game vs the baked default). */
    g_active_config_path = game_config_path;

    std::filesystem::path memcard_dir;
    /* [savestate] dir — slot .pst root. Empty => use memcard_dir (historical
     * layout). Set independently so sibling builds of one game can share
     * memory cards while keeping their savestates apart. */
    std::filesystem::path savestate_dir;
    std::filesystem::path memcard1_path;   /* explicit slot-1 .mcd (empty => dir/card1.mcd) */
    std::filesystem::path memcard2_path;   /* explicit slot-2 .mcd (empty => dir/card2.mcd) */
    bool memcard1_enabled = true;
    bool memcard2_enabled = true;
    bool multitap_enabled = true; /* [controller] multitap; 3+ seats offline */
    bool multitap_analog = true;  /* DualShock-on-tap hack; game.toml/settings */
    /* [controller] device routing (defaults: P1 keyboard/digital, P2 none). */
    /* Dev builds default Player 1 to the first connected controller ("auto").
     * PSX_DEV_INPUT=1 additionally merges every other controller and the
     * keyboard for diagnostic convenience; normal routing stays exclusive.
     * Release keeps "keyboard" as its pre-launch default (the launcher assigns
     * the selected physical device). */
    std::string player_device[PSX_MAX_PLAYERS];
    int  player_mode[PSX_MAX_PLAYERS];
    int  player_deadzone[PSX_MAX_PLAYERS];
    int  ctrl_locked_mode[PSX_MAX_PLAYERS];
    for (int i = 0; i < PSX_MAX_PLAYERS; ++i) {
#if defined(PSX_DEBUG_TOOLS)
        player_device[i] = (i == 0) ? "auto" : "none";
#else
        player_device[i] = (i == 0) ? "keyboard" : "none";
#endif
        player_mode[i] = PSXRecompV4::PAD_MODE_ANALOG;
        player_deadzone[i] = kDefaultDeadzoneRaw;
        ctrl_locked_mode[i] = PSXRecompV4::PAD_MODE_ANALOG;
    }
    bool ctrl_lock_mode    = false; /* game.toml [controller] lock_mode; true hides the whole pad-mode selector */
    bool ctrl_lock_device  = false; /* game.toml [controller] lock_device; true hides the Player controller cards entirely */
    /* Widescreen remains product-owned. WipEout 3 explicitly opts into the
     * tested opening-sequence skip control exposed by recomp-ui. */
    constexpr bool ws_offered = false;
    constexpr bool ws_ultrawide_offered = false;
    constexpr bool frame_interpolation_offered = false;
    constexpr bool skip_fmv_offered = true;
    /* Load acceleration is likewise mod-owned (Fast Loading / CD Speed). The
     * former game.toml `offer_turbo_loads` opt-out is deprecated and ignored:
     * offering the generic switch is no longer possible for any title, so a
     * config that forgot to migrate can no longer force turbo on. */
    constexpr bool turbo_loads_offered = false;
    bool vulkan_offered = false; /* game.toml [video] offer_vulkan; developer opt-in for launcher visibility */
    /* Legacy single deadzone (<0 => keep per-slot / input.ini defaults). */
    int  resolved_deadzone = -1;
    /* Localization: the effective language (game.toml default -> settings.toml ->
     * launcher choice), applied to the translation layer AFTER the launcher runs.
     * lang_menu_options drives the launcher's "Localization" dropdown (empty =>
     * no dropdown; only games that declare [localization].languages get one). */
    std::string resolved_language = "en";
    std::vector<PSXRecompV4::RuntimeConfig::LanguageOption> lang_menu_options;
    std::filesystem::path resolved_disc;
    std::string window_title = PSX_WINDOW_TITLE;
    uint16_t   debug_port    = (uint16_t)DEFAULT_DEBUG_PORT;
    std::string game_name;
    std::string game_id;
    std::string game_region;          /* game.toml [game] region; empty => derive from game_id serial */
    int         game_players = 1;     /* game.toml [game] players; launcher hides the P2 row when 1 */
    bool        game_has_disc_crc = false;
    uint32_t    game_disc_crc     = 0;
    std::string disc_speed;   /* "1x" | "2x" | "4x" | "instant" */
    int        instant_rate  = 0;   /* 0 = cdrom.c built-in default */
    std::vector<PSXRecompV4::RuntimeConfig::WarmCdRoute> warm_cd_routes;
    uint32_t   game_entry_pc = 0;
    bool       fast_boot     = false;  /* DEPRECATED alias: HLE boot-skip only */
    bool       bios_hle      = false;  /* HLE kernel-service tier (bios_hle.c) */
    bool       bios_hle_keep_intro = false;
    /* Text-image guard source, captured at config load; armed after the disc
     * path is resolved (arm_text_image_guard). */
    std::string text_guard_exe_path;
    uint32_t    text_guard_load_addr = 0;

    /* Overlay cache init is deferred until after the launcher window so ABI
     * preflight / resident DLL loads do not delay first paint. */
    bool deferred_overlay_cache = false;
    std::filesystem::path deferred_overlay_project_root;
    std::vector<uint32_t> deferred_overlay_native_block;
    uint32_t deferred_overlay_config_hash = 0;
    std::string deferred_overlay_backend;
    bool deferred_has_overlay_ac = false;
    std::string deferred_overlay_ac;
    bool deferred_has_overlay_ac_tcc = false;
    std::string deferred_overlay_ac_tcc;
    bool deferred_overlay_capture_history = false;
    std::string deferred_overlay_capture_persist_dir;

    if (game_config_path) {
        try {
            const auto gc = PSXRecompV4::load_game_config(game_config_path);
            game_name = gc.name;
            game_id   = gc.id;
            game_region = gc.region;
            game_players = gc.players;
            /* CTR ND intro: an older workaround (PSX_ND_SIB_FLAP_LAST=1) skipped
             * wide additive 0x36 in OT ranks 1600..2099 to unmask sibling flaps.
             * After the AVSZ3 MAC0 fix (unshifted product → correct MAC0>>17 OT
             * indices), flaps/rain sort without that skip — and FLAP_LAST=1
             * shreds the crate glow fountain. Default is off; opt in via env
             * (dma.c) only for experiments. */
            apply_offline_pad_count(game_players, multitap_enabled);
            game_has_disc_crc = gc.has_disc_crc;
            game_disc_crc     = gc.disc_crc;
            g_netplay_disc_expect.require_cue = gc.netplay_require_cue;
            g_netplay_disc_expect.required_tracks = gc.netplay_required_tracks;
            g_netplay_disc_expect.has_required_leadout =
                gc.has_netplay_required_leadout;
            g_netplay_disc_expect.required_leadout_lba =
                gc.netplay_required_leadout_lba;
            g_netplay_disc_expect.required_disc_fp = gc.netplay_required_disc_fp;
            g_netplay_local_viewport =
                (gc.netplay_local_viewport == "vertical_split") ? 1 : 0;
            g_netplay_local_viewport_aspect =
                (gc.netplay_local_viewport_aspect == "16:9") ? 1 :
                (gc.netplay_local_viewport_aspect == "21:9") ? 2 :
                (gc.netplay_local_viewport_aspect == "adaptive") ? 3 : 0;
            if (!gc.discs.empty()) resolved_disc = gc.discs.front();
            if (gc.runtime.has_memcard_dir)  memcard_dir   = gc.runtime.memcard_dir;
            if (gc.runtime.has_window_title) window_title  = gc.runtime.window_title;
            if (gc.runtime.has_debug_port)   debug_port    = gc.runtime.debug_port;
            /* On-the-fly string translation / localization (framework feature —
             * text_xlate.cpp): load translations/ *.toml under the project root
             * and select the language. Capture inventory is always-on; APPLY is
             * gated by language + table presence. See docs/STRING_TRANSLATION.md. */
            text_xlate_init(gc.project_root.string().c_str(),
                            gc.runtime.language.c_str());
            resolved_language = gc.runtime.language;   /* launcher/settings may override */
            lang_menu_options = gc.runtime.languages;  /* launcher localization dropdown */
            if (gc.runtime.has_disc_speed)   disc_speed    = gc.runtime.disc_speed;
            if (gc.runtime.has_instant_max_per_frame)
                instant_rate = gc.runtime.instant_max_per_frame;
            warm_cd_routes = gc.runtime.warm_cd_routes;
            /* turbo_loads / offer_turbo_loads are deprecated and ignored. Load
             * acceleration is owned by the Mods catalog (Fast Loading / CD
             * Speed), which ships with every title and defaults off, so the
             * legacy config key no longer enables anything. Say so loudly:
             * MMX6 shipped `turbo_loads = true` in v1.0.4/v1.0.5 and players
             * had no UI to turn it back off. */
            if (gc.runtime.has_turbo_loads || gc.runtime.has_offer_turbo_loads) {
                std::fprintf(stdout,
                    "psxrecomp: game.toml [runtime] %s%s%s is DEPRECATED and "
                    "ignored; load acceleration is off unless the player "
                    "enables the \"Fast Loading (host pacing)\" mod. Remove "
                    "the key from game.toml.\n",
                    gc.runtime.has_turbo_loads ? "turbo_loads" : "",
                    (gc.runtime.has_turbo_loads &&
                     gc.runtime.has_offer_turbo_loads) ? " / " : "",
                    gc.runtime.has_offer_turbo_loads ? "offer_turbo_loads" : "");
            }
            if (gc.runtime.turbo_audio_sink) {
                g_turbo_audio_sink_enabled = 1;
                std::fprintf(stdout,
                    "psxrecomp: turbo_audio_sink enabled (opt-in)\n");
            }
            {
                extern int g_idle_skip_enabled;
                const char *idle_env = std::getenv("PSX_IDLE_SKIP");
                g_idle_skip_enabled = idle_env
                    ? (idle_env[0] == '1' ? 1 : 0)
                    : (gc.runtime.idle_skip ? 1 : 0);
                std::fprintf(stdout, "psxrecomp: idle_skip %s%s\n",
                             g_idle_skip_enabled ? "enabled" : "disabled",
                             idle_env ? " (environment override)" : "");
                psx_precise_slice_init_from_env();
            }
            for (uint32_t site : gc.vsync_event_horizon_sites)
                psx_vsync_query_hle_add_event_horizon_site(site);
            for (uint32_t site : gc.vsync_event_horizon_extra_sites)
                psx_vsync_query_hle_add_extra_event_horizon_site(site);
            g_video_scale      = gc.runtime.video_supersampling;
            /* Config is the baseline. Activation plugins run later and may
             * override it; config must not be re-applied after activation. */
            (void)psx_mod_set_crtc_refresh_multiplier(
                gc.runtime.video_crtc_refresh_multiplier);
            g_video_aa         = gc.runtime.video_antialiasing;
            g_video_texfilter  = gc.runtime.video_texture_filter;
            g_video_fmv_filter = gc.runtime.video_fmv_filter;
            g_video_geometry_correction   =
                gc.runtime.video_geometry_correction ? 1 : 0;
            g_video_perspective_texturing =
                gc.runtime.video_perspective_texturing ? 1 : 0;
            g_video_pgxp_depth = gc.runtime.video_pgxp_depth ? 1 : 0;
            g_video_pgxp_cpu_mode = gc.runtime.video_pgxp_cpu_mode ? 1 : 0;
            g_video_pgxp_tolerance = (float)gc.runtime.video_pgxp_tolerance;
            g_video_renderer   = gc.runtime.video_renderer;
            g_hd_textures      = gc.runtime.video_hd_textures ? 1 : 0;
            g_hd_texture_dump  = gc.runtime.video_hd_texture_dump ? 1 : 0;
            g_hd_texture_dir   = gc.runtime.video_hd_texture_dir;
            g_hd_texture_exclude = gc.runtime.video_hd_texture_exclude;
            g_bezel_path       = gc.runtime.video_bezel;
            if (gc.runtime.runtime_cpu_overclock != 100u) {
                psx_set_cpu_overclock(gc.runtime.runtime_cpu_overclock);
                std::fprintf(stdout, "psxrecomp: CPU overclock %u%%\n",
                             psx_get_cpu_overclock());
            }
            g_video_screen     = gc.runtime.video_screen_kind;
            g_video_aspect_num = gc.runtime.video_aspect_num;
            g_video_aspect_den = gc.runtime.video_aspect_den;
            g_low_latency_input = gc.runtime.video_low_latency_input ? 1 : 0;
            g_video_vsync       = gc.runtime.video_vsync;
            g_frame_interpolation = gc.runtime.video_frame_interpolation ? 1 : 0;
            g_frame_interpolation_fps = gc.runtime.video_frame_interpolation_fps;
            g_fmv_skip_total_table = gc.runtime.video_fmv_skip_total_table;
            g_fmv_skip_movie_id    = gc.runtime.video_fmv_skip_movie_id;
            if (gc.runtime.video_fmv_skip_end_total)
                g_fmv_skip_end_total = gc.runtime.video_fmv_skip_end_total;
            g_fmv_skip_no_xa       = gc.runtime.video_fmv_skip_no_xa ? 1 : 0;
            g_fmv_skip_no_xa_hold  = gc.runtime.video_fmv_skip_no_xa_hold;
            g_ws_anchor_addr   = gc.ws_sprite_anchor_addr;
            g_ws_hud_sprt      = gc.ws_hud_sprt_squash;
            gpu_ws_set_auto_ui_squash(gc.ws_auto_ui_squash ? 1 : 0);
            /* [widescreen] full_2d — opt a pure-2D sprite game (MMX6) into the
             * widescreen present path. Applied to the GPU layer up front so the
             * ws engage at game entry classifies every frame as gameplay. */
            gpu_ws_set_full_2d(gc.ws_full_2d ? 1 : 0);
            /* [widescreen.bg2d] engine tile-ring layout for the freshness
             * refill shared by the MMX5/MMX6 2D background hook. */
            gpu_ws_bg2d_configure(gc.ws_bg2d_layer_base,
                                  gc.ws_bg2d_ring_base,
                                  gc.ws_bg2d_map_size_addr,
                                  gc.ws_bg2d_layer_stride_addr,
                                  gc.ws_bg2d_ring_cols,
                                  gc.ws_bg2d_layer_count,
                                  gc.ws_bg2d_layer_struct_stride,
                                  gc.ws_bg2d_packet_cap);
            /* [widescreen] gte_game_mode — 3D-title gameplay detector (Ape). */
            gpu_ws_set_gte_game_mode(gc.ws_gte_game_mode ? 1 : 0);
            g_ws_fixed_outer_aspect = gc.ws_fixed_outer_aspect;
            gpu_ws_set_precise_nclip(gc.ws_precise_nclip ? 1 : 0);
            gpu_ws_set_gameplay_state_gate(
                gc.ws_gameplay_state_addr,
                gc.ws_gameplay_state_values.data(),
                (int)gc.ws_gameplay_state_values.size());
            /* Keep titles with known native-wide regressions on the original
             * projection-squash + stretched-present widescreen path. */
            g_ws_native_wide = gc.ws_native_wide ? 1 : 0;
            /* [widescreen] nw_hud_corners — push HUD to the true wide corners. */
            gpu_ws_set_nw_hud_corners(gc.ws_nw_hud_corners ? 1 : 0);
            /* Targeted left-HUD packet range — avoids shifting 2D scenery. */
            gpu_ws_set_nw_left_hud_packet_range(gc.ws_nw_left_hud_packet_lo,
                                                gc.ws_nw_left_hud_packet_hi);
            /* [widescreen] nw_backdrop — stretch full-frame 2D sky backdrop. */
            gpu_ws_set_nw_backdrop(gc.ws_nw_backdrop ? 1 : 0);
            /* [widescreen] nw_flat_backdrop — stretch flat sky/backdrop prims
             * in the native-wide mirror, preserving the canonical 4:3 image. */
            gpu_ws_set_nw_flat_backdrop(gc.ws_nw_flat_backdrop ? 1 : 0);
            /* [widescreen] nw_phase_backdrop — stretch only the textured
             * backdrop phase emitted before shaded 3D foreground geometry. */
            gpu_ws_set_nw_phase_backdrop(gc.ws_nw_phase_backdrop ? 1 : 0);
            gpu_ws_set_nw_textured_edges(gc.ws_nw_textured_edges ? 1 : 0,
                                         gc.ws_nw_textured_edge_scale);
            gl_renderer_set_wide_fast(gc.ws_nw_full_mirror ? 0 : 1);
            {
                std::vector<uint32_t> addresses, expected;
                addresses.reserve(gc.ws_signed_x_bound_sites.size());
                expected.reserve(gc.ws_signed_x_bound_sites.size());
                for (const auto& site : gc.ws_signed_x_bound_sites) {
                    addresses.push_back(site.address);
                    expected.push_back(site.expected);
                }
                gpu_ws_set_signed_x_bound_sites(addresses.data(), expected.data(),
                                                (int)addresses.size());
            }
            /* [widescreen] clear_reveal — enable opted-in scene/map-boundary
             * cleanup of synthetic native-wide margins. */
            gpu_ws_set_clear_reveal(gc.ws_clear_reveal ? 1 : 0);
            gpu_ws_set_cull_guard_pixels(gc.ws_cull_guard_pixels);
            gpu_ws_set_activation_guard_pixels(
                gc.ws_cull_activation_guard_pixels);
            gpu_ws_set_explicit_cull_sites(
                gc.ws_cull_bias_sites.data(), (int)gc.ws_cull_bias_sites.size(),
                gc.ws_cull_slti_sites.data(), (int)gc.ws_cull_slti_sites.size(),
                gc.ws_cull_range_sites.data(), (int)gc.ws_cull_range_sites.size());
            gpu_ws_set_slti_lower_cull_sites(
                gc.ws_cull_slti_lower_sites.data(),
                (int)gc.ws_cull_slti_lower_sites.size());
            gpu_ws_set_negsub_cull_sites(
                gc.ws_cull_negsub_sites.data(), (int)gc.ws_cull_negsub_sites.size());
            gpu_ws_set_vxrange_cull_sites(
                gc.ws_cull_vxrange_sites.data(), (int)gc.ws_cull_vxrange_sites.size());
            gpu_ws_set_depth_cull_sites(
                gc.ws_cull_depth_sites.data(), (int)gc.ws_cull_depth_sites.size());
            gpu_ws_set_plane_nx_sites(
                gc.ws_cull_plane_nx_sites.data(), (int)gc.ws_cull_plane_nx_sites.size());
            gpu_ws_set_xclip_load_sites(
                gc.ws_cull_xclip_load_sites.data(), (int)gc.ws_cull_xclip_load_sites.size());
            {
                std::vector<uint32_t> addresses, expected, results;
                addresses.reserve(gc.ws_cull_keep_sites.size());
                expected.reserve(gc.ws_cull_keep_sites.size());
                results.reserve(gc.ws_cull_keep_sites.size());
                for (const auto& site : gc.ws_cull_keep_sites) {
                    addresses.push_back(site.address);
                    expected.push_back(site.expected);
                    results.push_back(site.result);
                }
                gpu_ws_set_cull_keep_sites(
                    addresses.data(), expected.data(), results.data(),
                    (int)addresses.size());
            }
            {
                std::vector<uint32_t> addresses, expected, modes;
                addresses.reserve(gc.ws_cull_widen_sites.size());
                expected.reserve(gc.ws_cull_widen_sites.size());
                modes.reserve(gc.ws_cull_widen_sites.size());
                for (const auto& site : gc.ws_cull_widen_sites) {
                    addresses.push_back(site.address);
                    expected.push_back(site.expected);
                    modes.push_back((uint32_t)site.mode);
                }
                gpu_ws_set_cull_widen_sites(
                    addresses.data(), expected.data(), modes.data(),
                    (int)addresses.size());
            }
            {
                std::vector<uint32_t> addresses, expected;
                addresses.reserve(gc.ws_cull_angle_sites.size());
                expected.reserve(gc.ws_cull_angle_sites.size());
                for (const auto& site : gc.ws_cull_angle_sites) {
                    addresses.push_back(site.address);
                    expected.push_back(site.expected);
                }
                gpu_ws_set_angle_sites(
                    addresses.data(), expected.data(), (int)addresses.size());
            }
            {
                std::vector<uint32_t> addresses, expected, thresholds;
                std::vector<uint32_t> object_regs, x_regs, z_regs, y_regs;
                std::vector<uint32_t> queue_guards;
                addresses.reserve(gc.ws_aspect_cone.sites.size());
                expected.reserve(gc.ws_aspect_cone.sites.size());
                thresholds.reserve(gc.ws_aspect_cone.sites.size());
                object_regs.reserve(gc.ws_aspect_cone.sites.size());
                x_regs.reserve(gc.ws_aspect_cone.sites.size());
                z_regs.reserve(gc.ws_aspect_cone.sites.size());
                y_regs.reserve(gc.ws_aspect_cone.sites.size());
                queue_guards.reserve(gc.ws_aspect_cone.sites.size());
                const auto effective_reg = [](uint32_t site_reg,
                                              uint32_t default_reg) {
                    return site_reg == 0xFFFFFFFFu
                        ? default_reg : site_reg;
                };
                for (const auto& site : gc.ws_aspect_cone.sites) {
                    addresses.push_back(site.address);
                    expected.push_back(site.expected);
                    thresholds.push_back(site.cosine_threshold);
                    object_regs.push_back(effective_reg(
                        site.object_reg, gc.ws_aspect_cone.object_reg));
                    x_regs.push_back(effective_reg(
                        site.x_reg, gc.ws_aspect_cone.x_reg));
                    z_regs.push_back(effective_reg(
                        site.z_reg, gc.ws_aspect_cone.z_reg));
                    y_regs.push_back(effective_reg(
                        site.y_reg, gc.ws_aspect_cone.y_reg));
                    queue_guards.push_back(site.queue_guard ? 1u : 0u);
                }
                gpu_ws_set_aspect_cone(
                    addresses.data(), expected.data(), thresholds.data(),
                    object_regs.data(), x_regs.data(), z_regs.data(),
                    y_regs.data(), queue_guards.data(), (int)addresses.size(),
                    gc.ws_aspect_cone.forward_addr,
                    gc.ws_aspect_cone.object_type_offset,
                    gc.ws_aspect_cone.hysteresis_pixels,
                    gc.ws_aspect_cone.queue_reserve,
                    gc.ws_aspect_cone.queue_count_addrs.data(),
                    gc.ws_aspect_cone.queue_capacities.data(),
                    gc.ws_aspect_cone.queue_type_masks.data());
            }
            gte_ws_configure_dome_sites(
                gc.ws_dome_call_sites.data(), (int)gc.ws_dome_call_sites.size());
            /* [widescreen.cull] per-game gates + signature immediates for the
             * pattern-scanned interp widen hooks. A title that never opted in
             * must never have its live code scanned and rewritten. */
            gpu_ws_set_auto_hooks(gc.ws_auto_screen_x_cull ? 1 : 0,
                                  gc.ws_auto_backdrop_preload ? 1 : 0);
            if (!gc.ws_cull_w_imms.empty() || !gc.ws_cull_h_imms.empty())
                gpu_ws_set_cull_imms(gc.ws_cull_w_imms.data(), (int)gc.ws_cull_w_imms.size(),
                                     gc.ws_cull_h_imms.data(), (int)gc.ws_cull_h_imms.size());
            /* gc.runtime.offer_turbo_loads is deprecated and ignored — see
             * turbo_loads_offered above. Nothing to assign. */
            vulkan_offered = gc.vulkan_offered;
            /* Register the [widescreen.backdrop] store PCs so the dirty-RAM
             * interpreter applies the backdrop screenX squash on the interp
             * path (overlay backdrop handlers run interpreted when no cache
             * DLL is loaded — the recompiler emit only covers native). */
            if (!gc.ws_backdrop_x_sites.empty())
                psx_ws_set_backdrop_sites(gc.ws_backdrop_x_sites.data(),
                                          (int)gc.ws_backdrop_x_sites.size());
            g_audio_spu_hq     = gc.runtime.audio_spu_hq;
            g_auto_skip_fmv    = gc.runtime.video_auto_skip_fmv ? 1 : 0;
            /* [controller] game-declared input defaults (settings.toml/launcher
             * still override below). */
            if (gc.runtime.has_default_mode) {
                for (int i = 0; i < PSX_MAX_PLAYERS; ++i) {
                    player_mode[i] = (i == 0) ? gc.runtime.default_p1_mode
                                              : gc.runtime.default_p2_mode;
                    /* Beyond P2, reuse default_mode (same as P1 when set via default_mode). */
                    if (i >= 2) player_mode[i] = gc.runtime.default_p1_mode;
                }
            }
            if (gc.runtime.has_default_p1_device)
                player_device[0] = gc.runtime.default_p1_device;
            if (PSX_MAX_PLAYERS >= 2 && gc.runtime.has_default_p2_device)
                player_device[1] = gc.runtime.default_p2_device;
            for (int i = 0; i < PSX_MAX_PLAYERS; ++i)
                ctrl_locked_mode[i] = player_mode[i];
            ctrl_lock_mode    = gc.runtime.controller_lock_mode;
            ctrl_lock_device  = gc.runtime.controller_lock_device;
            if (gc.runtime.has_deadzone) {
                resolved_deadzone = gc.runtime.deadzone;
                for (int i = 0; i < PSX_MAX_PLAYERS; ++i)
                    player_deadzone[i] = gc.runtime.deadzone;
            }
            /* [controller] anti_deadzone — raises the minimum reported stick
             * magnitude to compensate for a game's own internal deadzone. */
            if (gc.runtime.has_anti_deadzone)
                controller_anti_deadzone = gc.runtime.anti_deadzone;
            /* Console port for SCPH-1070 when offline/netplay arms multitap.
             * Most titles use Port 1; Bomberman Party Edition needs Port 2. */
            if (gc.runtime.has_multitap_port) {
                const int phys = (gc.runtime.multitap_port == 2) ? 1 : 0;
                sio_set_multitap_port(phys);
            }
            if (gc.runtime.has_multitap_analog) {
                multitap_analog = gc.runtime.multitap_analog;
                sio_set_multitap_analog(multitap_analog ? 1 : 0);
#if defined(RECOMP_LAUNCHER) && defined(PSX_HAS_LOBBY_CLIENT)
                g_lnch_multitap_analog = multitap_analog ? 1 : 0;
#endif
            }
            /* LEGACY per-game pad-config opt-in (default modern). Only Tomba sets
             * it, so its launcher Hybrid mode's analog<->digital flip doesn't make
             * libpad manufacture a 1-frame "pad unplugged". sio_init() does not
             * touch this flag, so applying it here (config-load time) is stable.
             * Full history + removal plan: psxrecomp sio.c g_pad_legacy_cfg. */
            sio_set_legacy_cfg(gc.runtime.legacy_pad_config ? 1 : 0);
            { const char *e = std::getenv("PSX_GL_FORCE_CPU_PRESENT");
              if (e && e[0] && e[0] != '0') g_gl_fbo_present = 0; }
            game_entry_pc = gc.entry_pc;
            fast_boot     = gc.runtime.fast_boot;
            sio_set_ape_card_unstick(gc.runtime.ape_card_unstick ? 1 : 0);
            bios_hle      = gc.runtime.bios_hle;
            bios_hle_keep_intro = gc.runtime.bios_hle_keep_intro;
            /* Developer compatibility finding, applied before BIOS selection.
             * Not exposed to settings.toml on purpose — see BIOS_SELECTION.md. */
            s_openbios_allowed  = gc.runtime.openbios;
            /* Let the dispatch layer distinguish "dirty because text was
             * loaded" from "diverged because runtime wrote different code over
             * the original EXE image". Packed/self-modifying games can rewrite
             * their own text; those pages must execute from live RAM, not stale
             * static native code. Arming is DEFERRED until the disc path is
             * resolved (arm_text_image_guard below): release installs have no
             * local EXE file, so the guard extracts the boot EXE from the
             * user's disc image instead. */
            text_guard_exe_path  = gc.exe_path.string();
            text_guard_load_addr = gc.load_address;
            /* HLE-tier scheduler subsystem replacement default (env
             * PSX_HLE_SCHEDULER still wins; latched at first dispatch). */
            psx_hle_scheduler_set_default(gc.runtime.hle_scheduler ? 1 : 0);
            /* Pin the overlay-region floor to THIS game's main-EXE text end so
             * runtime-loaded overlays (which load just above it) are dispatched
             * via in-interpreter local-flow chaining, NOT the slow block-by-block
             * + bail-prone non-local-call path. Hardcoding the floor to Tomba 1's
             * text end (0x98000) wedged Tomba 2 (text ends 0x38800, overlays at
             * 0x85000+) at the Whoopee-Camp splash. See dirty_ram_interp.h. */
            {
                extern uint32_t g_overlay_region_floor;
                extern uint32_t g_text_image_lo;
                uint32_t text_end = (gc.load_address + gc.text_size) & 0x1FFFFFFFu;
                if (text_end > 0x00010000u /* DIRTY_RAM_KERNEL_WINDOW_END */)
                    g_overlay_region_floor = text_end;
                /* Pin the text BASE too. The floor alone assumes the boot EXE
                 * sits at the bottom of RAM; a high-loading EXE (Klonoa
                 * 0x180000, SFA3 0x113B00) streams its overlays into the RAM
                 * BELOW itself, which must be overlay region, not text. */
                uint32_t text_lo = gc.load_address & 0x1FFFFFFFu;
                if (text_lo > 0x00010000u && text_lo < g_overlay_region_floor)
                    g_text_image_lo = text_lo;
                std::fprintf(stdout,
                    "psxrecomp: overlay_region_floor = 0x%05X (game text end), "
                    "text_image_lo = 0x%05X\n",
                    g_overlay_region_floor, g_text_image_lo);
            }
            /* Overlay DLL cache (Layer A): stash config now; heavy init
             * (cache scan / ABI preflight / resident LoadLibrary) runs after
             * the launcher window so first UI paint is not blocked. */
            if (gc.runtime.overlay_cache) {
                deferred_overlay_cache = true;
                deferred_overlay_project_root = gc.project_root;
                deferred_overlay_native_block = gc.runtime.overlay_native_block;
                deferred_overlay_config_hash =
                    PSXRecompV4::overlay_codegen_config_hash(gc);
                deferred_overlay_backend = gc.runtime.overlay_backend;
                deferred_has_overlay_ac = gc.runtime.has_overlay_autocompile_cmd;
                deferred_overlay_ac = gc.runtime.overlay_autocompile_cmd;
                deferred_has_overlay_ac_tcc = gc.runtime.has_overlay_autocompile_cmd_tcc;
                deferred_overlay_ac_tcc = gc.runtime.overlay_autocompile_cmd_tcc;
                deferred_overlay_capture_history = gc.runtime.overlay_capture_history;
                deferred_overlay_capture_persist_dir = gc.runtime.overlay_capture_persist_dir;
            }
            std::fprintf(stdout, "psxrecomp: loaded game config %s (%s, %s)\n",
                         game_config_path, game_name.c_str(), game_id.c_str());
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "psxrecomp: failed to load --game %s: %s\n",
                         game_config_path, ex.what());
            return 1;
        }
    }
#if defined(RECOMP_LAUNCHER)
    launcher_boot_timing_mark("host:game_config_done");
#endif

    if (!game_name.empty()) s_picker_game_name = game_name;

    /* Layer the launcher-written settings.toml (next to the exe) over the
     * bundled game.toml. Any field present there overrides the config value;
     * the command line (--bios/--disc) still wins over the file. Absent =>
     * fall through to game.toml. The file is the launcher's persistence. */
    bool skip_launcher_setting = false;  /* [launcher] skip_launcher from settings.toml */
    std::string settings_bios_storage;  /* must outlive resolve_bios_for_runtime */
    std::string netplay_player_name;     /* [netplay] player_name from settings.toml */
    bool has_netplay_player_name = false;
    bool user_settings_has_renderer = false;
    {
        std::filesystem::path settings_path =
            exe_dir_from_argv(argv[0]) / "settings.toml";
#if defined(RECOMP_LAUNCHER)
        g_lnch_settings_path = settings_path;
#endif
        const PSXRecompV4::UserSettings us =
            PSXRecompV4::load_user_settings(settings_path);
        user_settings_has_renderer = us.has_renderer;
        if (us.parse_error) {
            /* The file exists but is not valid TOML: every setting in it (the
             * user's renderer choice, BIOS/disc paths, ...) is being ignored,
             * and any later launcher save would overwrite it with defaults.
             * Preserve their file and say so loudly instead of both failing
             * silently (GH Tomba2Recomp#1 triage: a stray character disabled a
             * user's whole settings file with no indication anywhere). */
            std::error_code rn_ec;
            std::filesystem::path bad = settings_path;
            bad += ".bad";
            std::filesystem::remove(bad, rn_ec);
            rn_ec.clear();
            std::filesystem::rename(settings_path, bad, rn_ec);
            launcher_warning("Settings file ignored",
                "settings.toml has a TOML syntax error, so ALL settings in it were "
                "ignored this run (renderer, BIOS/disc paths, everything).\n\n" +
                (rn_ec ? "The broken file was left at:\n" + settings_path.string()
                       : "The broken file was preserved as:\n" + bad.string()) +
                "\n\nFix the syntax error and restore the file, or set your options "
                "again from the launcher (a fresh settings.toml will be written).");
        }
        if (us.has_skip_launcher)  skip_launcher_setting = us.skip_launcher;
        if (us.has_renderer) {
            if (us.renderer == 2 && !vulkan_offered) {
                g_video_renderer = 1;
                std::fprintf(stdout,
                    "psxrecomp: settings requested Vulkan, but this game does not "
                    "offer Vulkan in the launcher; using OpenGL.\n");
            } else {
                g_video_renderer = us.renderer;
            }
        }
        if (us.has_netplay_player_name && !us.netplay_player_name.empty()) {
            netplay_player_name = us.netplay_player_name;
            has_netplay_player_name = true;
        }
#if defined(RECOMP_LAUNCHER)
        if (us.has_netplay_lobby_url && !us.netplay_lobby_url.empty())
            g_lnch_lobby_url = us.netplay_lobby_url;
#endif
        if (us.has_supersampling)  g_video_scale     = us.supersampling;
        if (us.has_window_width)   g_video_win_w     = us.window_width;
        if (us.has_window_width && us.window_width > 0) g_video_win_w_explicit = true;
        if (us.has_antialiasing)   g_video_aa        = us.antialiasing;
        if (us.has_texture_filter) g_video_texfilter = us.texture_filter;
        if (us.has_fmv_filter)     g_video_fmv_filter = us.fmv_filter;
        if (us.has_hd_textures)     g_hd_textures     = us.hd_textures ? 1 : 0;
        if (us.has_hd_texture_pack) g_hd_texture_pack = us.hd_texture_pack.string();
        if (us.has_geometry_correction)
            g_video_geometry_correction = us.geometry_correction ? 1 : 0;
        if (us.has_perspective_texturing)
            g_video_perspective_texturing = us.perspective_texturing ? 1 : 0;
        if (us.has_screen_kind)    g_video_screen    = us.screen_kind;
        if (us.has_auto_skip_fmv)  g_auto_skip_fmv   = us.auto_skip_fmv ? 1 : 0;
        /* turbo_loads is deliberately NOT restored from settings.toml. It is a
         * write-only latch: the launcher stopped drawing a Turbo loads row when
         * load acceleration moved to the Mods catalog, so a persisted `true`
         * was simultaneously authoritative and unreachable — one run of a build
         * whose game.toml said true latched it forever and no later config
         * change could undo it (MegaManX6Recomp#14). Report it and move on;
         * write_user_settings no longer emits the key, so the stale row is
         * dropped on the next save. */
        if (us.has_turbo_loads && us.turbo_loads)
            std::fprintf(stdout,
                "psxrecomp: settings.toml [video] turbo_loads = true is "
                "DEPRECATED and ignored; enable the \"Fast Loading (host "
                "pacing)\" mod instead. The stale key is dropped on the next "
                "settings save.\n");
        if (us.has_fast_boot)      fast_boot = us.fast_boot;
        if (us.has_bios_hle)       bios_hle  = us.bios_hle;
        if (us.has_fullscreen)     g_fullscreen      = us.fullscreen;
        if (us.has_aspect_ratio) {
            g_video_aspect_num = us.aspect_num;
            g_video_aspect_den = us.aspect_den;
        }
        if (us.has_audio_freq)     g_audio_freq      = us.audio_freq;
        if (us.has_spu_hq)         g_audio_spu_hq    = us.spu_hq;
        if (us.has_rewind_depth)  g_rewind_depth   = us.rewind_depth;
        if (us.has_rewind_interval) g_rewind_interval = us.rewind_interval;
        if (us.has_hotkey_pad_rewind)
            g_hotkey_pad_rewind = normalize_hotkey_pad_binding(
                us.hotkey_pad_rewind,
                PSX_HOTKEY_PAD_SELECT_R3);
        if (us.has_hotkey_pad_save_state_menu)
            g_hotkey_pad_save_state_menu = normalize_hotkey_pad_binding(
                us.hotkey_pad_save_state_menu,
                PSX_HOTKEY_PAD_SELECT_R1);
        if (us.has_bios_path && !bios_from_cli && !us.bios_path.empty()) {
            settings_bios_storage = us.bios_path.string();
            bios_path = settings_bios_storage.c_str();
            bios_explicit = true;
        }
        if (us.has_disc_path && !disc_override_path)
            resolved_disc = normalize_disc_path_for_launch(us.disc_path);
        if (us.has_memcard_dir)                      memcard_dir   = us.memcard_dir;
        if (us.has_memcard1_path)    memcard1_path    = us.memcard1_path;
        if (us.has_memcard2_path)    memcard2_path    = us.memcard2_path;
        if (us.has_memcard1_enabled) memcard1_enabled = us.memcard1_enabled;
        if (us.has_memcard2_enabled) memcard2_enabled = us.memcard2_enabled;
        if (us.has_savestate_dir)    savestate_dir    = us.savestate_dir;
        if (us.has_multitap_enabled) multitap_enabled = us.multitap_enabled;
        if (us.has_multitap_analog) {
            multitap_analog = us.multitap_analog;
            sio_set_multitap_analog(multitap_analog ? 1 : 0);
#if defined(RECOMP_LAUNCHER) && defined(PSX_HAS_LOBBY_CLIENT)
            g_lnch_multitap_analog = multitap_analog ? 1 : 0;
#endif
        }
        if (us.has_language) resolved_language = us.language;
        {
            const int n = std::min(PSX_MAX_PLAYERS,
                                   PSXRecompV4::UserSettings::kMaxControllerPlayers);
            for (int i = 0; i < n; ++i) {
                if (us.has_p_device[i]) player_device[i] = us.p_device[i];
                if (us.has_p_mode[i])   player_mode[i]   = us.p_mode[i];
                if (us.has_p_deadzone[i]) player_deadzone[i] = us.p_deadzone[i];
            }
            if (us.has_deadzone) resolved_deadzone = us.deadzone;
        }
        apply_offline_pad_count(game_players, multitap_enabled);
        if (us.has_low_latency_input) g_low_latency_input = us.low_latency_input ? 1 : 0;
        if (us.has_vsync)             g_video_vsync       = us.vsync;
        if (us.has_frame_interpolation)
            g_frame_interpolation = us.frame_interpolation ? 1 : 0;
        if (us.has_frame_interpolation_fps)
            g_frame_interpolation_fps = us.frame_interpolation_fps;
    }

    /* lock_mode: the game supports exactly ONE pad type (e.g. X4 / Tomba 2 are
     * digital-only — X4's pre-DualShock libpad silently discards input from a
     * pad answering id 0x73). The launcher hides its selector for such games,
     * but that alone left two holes: (a) launcher-less builds still honoured a
     * settings.toml p1_mode/p2_mode, and (b) a settings.toml persisted BEFORE
     * the game declared lock_mode fed the stale mode back as the launcher's
     * locked_mode. Clamp to the game-declared modes here, after every
     * config/settings source has been applied, so a locked game can never boot
     * a pad type it doesn't support. */
    g_force_digital_pads = 0;
    if (ctrl_lock_mode) {
        int all_digital = 1;
        for (int i = 0; i < PSX_MAX_PLAYERS; ++i) {
            player_mode[i] = ctrl_locked_mode[i];
            if (ctrl_locked_mode[i] != PSXRecompV4::PAD_MODE_DIGITAL)
                all_digital = 0;
        }
        /* Digital-only titles (e.g. BPE): DualShock-on-tap cannot be armed from
         * settings.toml or Lobby Settings either. */
        if (all_digital) {
            g_force_digital_pads = 1;
            multitap_analog = false;
            sio_set_multitap_analog(0);
#if defined(RECOMP_LAUNCHER) && defined(PSX_HAS_LOBBY_CLIENT)
            g_lnch_multitap_analog = 0;
#endif
        }
    }
    /* Skip FMVs is mod-owned on PSX. Clamp stale generic settings before
     * seeding recomp-ui; an enabled activation plugin applies the feature
     * after the final mod-plan commit. */
    if (!skip_fmv_offered && g_auto_skip_fmv) {
        std::fprintf(stdout,
            "psxrecomp: Skip FMVs is mod-owned on PSX; "
            "ignoring the legacy Settings value\n");
        g_auto_skip_fmv = 0;
    }
    /* Load acceleration is mod-owned on PSX, unconditionally. Nothing upstream
     * of this point is allowed to have enabled it (the game.toml and
     * settings.toml keys are both deprecated and ignored), so this is a
     * belt-and-braces clamp rather than the migration path it used to be — an
     * enabled Fast Loading / CD Speed plugin turns it on further down, after
     * the final mod-plan commit. */
    if (g_turbo_loads_enabled) {
        std::fprintf(stdout,
            "psxrecomp: Turbo loads is mod-owned on PSX; "
            "ignoring the legacy value\n");
        g_turbo_loads_enabled = 0;
    }
    /* Treat presentation interpolation the same way when a title moves it to
     * Mods. Both the boolean and target rate are cleared so launcher-less
     * starts cannot revive an old generic Settings selection. */
    if (!frame_interpolation_offered &&
        (g_frame_interpolation || g_frame_interpolation_fps)) {
        std::fprintf(stdout,
            "psxrecomp: Frame interpolation is mod-owned for this title; "
            "ignoring the legacy Settings value\n");
        g_frame_interpolation = 0;
        g_frame_interpolation_fps = 0;
    }

    /* Widescreen/View mode is mod-owned on PSX. Clamp the generic display
     * aspect to native 4:3 so neither a legacy game.toml offer/default nor a
     * stale settings.toml value can engage it before trusted mod activation. */
    if (!ws_offered && (g_video_aspect_num != 4 || g_video_aspect_den != 3)) {
        std::fprintf(stdout, "psxrecomp: widescreen is mod-owned on PSX; "
                     "clamping display aspect %d:%d -> 4:3\n",
                     g_video_aspect_num, g_video_aspect_den);
        g_video_aspect_num = 4;
        g_video_aspect_den = 3;
    }
    if (!ws_ultrawide_offered && g_video_aspect_num * 9 == g_video_aspect_den * 21) {
        std::fprintf(stdout, "psxrecomp: 21:9 is not offered for this title; clamping to %s\n",
                     ws_offered ? "16:9" : "4:3");
        g_video_aspect_num = ws_offered ? 16 : 4;
        g_video_aspect_den = ws_offered ? 9 : 3;
    }

    /* Latency knobs: env overrides win over config (for A/B measurement).
     * PSX_LOW_LATENCY_INPUT=0/1 ; PSX_VSYNC=1(vsync)/0(immediate)/-1(adaptive)
     * (vsync XOR wall-clock pacer — vsync clocks only ~60 Hz panels;
     * on Wayland driver vsync is disabled for cadence unless
     * PSX_WAYLAND_ALLOW_VSYNC=1);
     * PSX_FRAME_INTERPOLATION=0/1; PSX_FRAME_INTERPOLATION_FPS=0|90+. */
    if (const char *e = std::getenv("PSX_LOW_LATENCY_INPUT")) g_low_latency_input = atoi(e) ? 1 : 0;
    if (const char *e = std::getenv("PSX_VSYNC"))             g_video_vsync       = atoi(e);
#ifdef _WIN32
    if (const char *e = std::getenv("PSX_SELF_PROFILE")) {
        int secs = atoi(e);
        if (secs > 0) {
            static SelfProfArgs spa;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                            GetCurrentProcess(), &spa.thread,
                            0, FALSE, DUPLICATE_SAME_ACCESS);
            spa.seconds = secs;
            CreateThread(NULL, 0, self_profile_thread, &spa, 0, NULL);
            std::fprintf(stdout, "psxrecomp: [self-profile] armed for %d s\n", secs);
        }
    }
#endif
    if (const char *e = std::getenv("PSX_WAYLAND_ALLOW_VSYNC"))
        g_wayland_allow_vsync = atoi(e) ? 1 : 0;
    if (const char *e = std::getenv("PSX_SMOOTH_60FPS"))
        psx_smooth_60fps_set(atoi(e) ? 1 : 0);
    if (const char *e = std::getenv("PSX_FRAME_INTERPOLATION"))
        g_frame_interpolation = atoi(e) ? 1 : 0;
    if (const char *e = std::getenv("PSX_FRAME_INTERPOLATION_FPS")) {
        int fps = atoi(e);
        if (fps == 0 || fps >= 90) g_frame_interpolation_fps = fps;
    }

    /* Apply writable-state isolation before game-options and launcher setup,
     * not with the later renderer/port overrides. Explicit slot paths from a
     * shared settings.toml must not escape the isolated directory. */
    if (cli_memcard_dir) {
        memcard_dir = std::filesystem::path(cli_memcard_dir);
        if (memcard_dir.is_relative())
            memcard_dir = exe_dir_from_argv(argv[0]) / memcard_dir;
        memcard_dir = memcard_dir.lexically_normal();
        memcard1_path.clear();
        memcard2_path.clear();
        /* Same rationale as the card paths: this flag exists to isolate ALL
         * writable state, so a settings.toml [savestate] dir pointing outside
         * the isolated directory must not survive it. */
        savestate_dir.clear();
        std::error_code memcard_ec;
        std::filesystem::create_directories(memcard_dir, memcard_ec);
        if (memcard_ec) {
            std::fprintf(stderr,
                "psxrecomp: cannot create --memcard-dir %s: %s\n",
                memcard_dir.string().c_str(), memcard_ec.message().c_str());
            return 1;
        }
        std::fprintf(stdout, "psxrecomp: CLI writable-state directory = %s\n",
                     memcard_dir.string().c_str());
    }

    /* Resolve the effective memory-card directory now (before the launcher) so
     * the launcher can introspect the real card files. The same default is used
     * by the runtime below. */
    if (memcard_dir.empty()) memcard_dir = default_memcard_dir(argv[0]);

    /* Savestate root. Unset => the memcard dir, i.e. exactly the historical
     * <memcard_dir>/<bios_token>/state_*.pst layout. A relative setting is
     * resolved against the executable directory, matching --memcard-dir. */
    if (!savestate_dir.empty()) {
        if (savestate_dir.is_relative())
            savestate_dir = exe_dir_from_argv(argv[0]) / savestate_dir;
        savestate_dir = savestate_dir.lexically_normal();
        std::error_code ss_ec;
        std::filesystem::create_directories(savestate_dir, ss_ec);
        if (ss_ec) {
            std::fprintf(stderr,
                "psxrecomp: cannot create [savestate] dir %s: %s — "
                "falling back to the memory-card directory\n",
                savestate_dir.string().c_str(), ss_ec.message().c_str());
            savestate_dir.clear();
        }
    }
    if (savestate_dir.empty()) savestate_dir = memcard_dir;

    /* The game's OWN native OPTION settings (game_options.toml, next to
     * game.toml) — persisted across launches, kept separate from game.toml
     * (recomp config) and settings.toml (launcher). Values are saved to
     * <memcard_dir>/<game_id>.options. Best-effort: a malformed file disables
     * the feature, it never blocks boot. */
    if (game_config_path) {
        try {
            std::filesystem::path go_path =
                std::filesystem::path(game_config_path).parent_path() / "game_options.toml";
            const PSXRecompV4::GameOptions go = PSXRecompV4::load_game_options(go_path);
            if (!go.options.empty()) {
                std::vector<uint32_t>    go_addrs;
                std::vector<uint8_t>     go_sizes;
                std::vector<const char*> go_names;
                std::vector<int32_t>     go_vmins;
                std::vector<int32_t>     go_vmaxs;
                for (const auto& o : go.options) {
                    go_addrs.push_back(o.addr);
                    go_sizes.push_back((uint8_t)o.size);
                    go_names.push_back(o.name.c_str());
                    /* Declared range validates the persisted value at restore; no
                     * range => full int32 span (accept anything that fits). */
                    go_vmins.push_back(o.has_range ? (int32_t)o.vmin : (int32_t)0x80000000);
                    go_vmaxs.push_back(o.has_range ? (int32_t)o.vmax : (int32_t)0x7FFFFFFF);
                }
                std::string go_state = (memcard_dir /
                    ((game_id.empty() ? std::string("game") : game_id) + ".options")).string();
                game_options_configure(go_state.c_str(), go_addrs.data(), go_sizes.data(),
                                       go_names.data(), go_vmins.data(), go_vmaxs.data(),
                                       (int)go_addrs.size());
                std::fprintf(stdout,
                    "psxrecomp: game options persistence armed (%d field(s)) -> %s\n",
                    (int)go_addrs.size(), go_state.c_str());
            }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "psxrecomp: game_options.toml ignored: %s\n", ex.what());
        }
    }

    /* Scan <exe_dir>/mods and load persisted enable/option state. Everything
     * downstream — the launcher Mods tab (gi.mods), disc patching, the
     * psx_mod_set_* callbacks — is inert until this runs. */
    {
        std::string mod_error;
        if (!PSXRecompV4::mod_runtime_initialize(
                exe_dir_from_argv(argv[0]) / "mods", game_id,
                game_entry_pc, text_guard_exe_path, &mod_error)) {
            std::fprintf(stderr, "psxrecomp: mods unavailable: %s\n",
                         mod_error.c_str());
        }
    }

#if defined(RECOMP_LAUNCHER)
    launcher_boot_timing_mark("host:pre_overlay_worker");
#endif
    /* Overlay cache: run ABI preflight / resident DLL loads on a worker so the
     * launcher can open immediately and init overlaps with UI time. Join before
     * guest boot. When the launcher is skipped, the join still runs below. */
    std::thread overlay_init_thread;
    std::exception_ptr overlay_init_exc;
    auto run_deferred_overlay_init = [&]() {
        std::filesystem::path exe_dir = exe_dir_from_argv(argv[0]);
        std::string cache_dir = (exe_dir / "cache").string();
        std::filesystem::path captures_path =
            resolve_overlay_capture_path(deferred_overlay_project_root, exe_dir, game_id);
        if (!overlay_capture_set_path(captures_path.string().c_str())) {
            captures_path = exe_dir / "overlay_captures.json";
            if (!overlay_capture_set_path(captures_path.string().c_str())) {
                throw std::runtime_error(
                    "overlay capture path exceeds runtime limit");
            }
        }
        overlay_capture_set_enabled(1);
        std::fprintf(stdout,
            "psxrecomp: additive overlay capture store = %s (+ .d history)\n",
            captures_path.string().c_str());
        std::string capture_persist_dir;
        if (deferred_overlay_capture_history &&
            !deferred_overlay_capture_persist_dir.empty()) {
            std::filesystem::path persist =
                deferred_overlay_project_root / deferred_overlay_capture_persist_dir;
            std::error_code persist_ec;
            std::filesystem::create_directories(persist, persist_ec);
            if (persist_ec) {
                std::fprintf(stderr,
                    "psxrecomp: cannot create overlay capture history %s: %s\n",
                    persist.string().c_str(), persist_ec.message().c_str());
            } else {
                capture_persist_dir = persist.string();
            }
        }
        overlay_capture_configure_history(
            deferred_overlay_capture_history ? 1 : 0,
            capture_persist_dir.empty() ? nullptr :
                capture_persist_dir.c_str(),
            game_id.c_str());
        overlay_loader_init(cache_dir.c_str(), game_id.c_str(),
                            deferred_overlay_config_hash);
        for (uint32_t addr : deferred_overlay_native_block) {
            overlay_loader_native_block_add(addr);
        }
        if (!deferred_overlay_native_block.empty()) {
            std::fprintf(stdout,
                "psxrecomp: overlay native blocklist seeded with %zu entr%s\n",
                deferred_overlay_native_block.size(),
                deferred_overlay_native_block.size() == 1 ? "y" : "ies");
        }
        overlay_autocapture_set_enabled(0);
        const char *cfg_backend = deferred_overlay_backend.empty()
                ? nullptr : deferred_overlay_backend.c_str();
        int gcc_avail = deferred_has_overlay_ac
                        && autocompile_toolchain_available();
        OverlayBackend eff = overlay_backend_resolve(cfg_backend, gcc_avail);
        std::string built_tcc_cmd;
        std::string env_ac_cmd;
        const std::string *ac_cmd = nullptr;
        if (eff == OVERLAY_BACKEND_TCC) {
            if (deferred_has_overlay_ac_tcc) {
                ac_cmd = &deferred_overlay_ac_tcc;
            } else {
                extern int g_psx_cps_mode;
                std::filesystem::path xd = exe_dir_from_argv(argv[0]);
                std::filesystem::path tk = xd / "overlay_toolchain";
                std::filesystem::path py = tk / "python" / "python.exe";
                if (std::filesystem::exists(py)) {
                    auto cmd_quote = [](const std::string& s) {
                        return std::string("\"") + s + "\"";
                    };
                    built_tcc_cmd =
                        cmd_quote(py.string()) + " " +
                        cmd_quote((tk / "compile_overlays.py").string()) +
                        " --captures " + cmd_quote(captures_path.string()) +
                        " --game-toml " + cmd_quote(std::string(
                            game_config_path ? game_config_path : "game.toml")) +
                        " --recompiler " + cmd_quote((tk / "psxrecomp-game.exe").string()) +
                        " --runtime-include " + cmd_quote((tk / "include").string()) +
                        " --out-dir " + cmd_quote((xd / "cache").string()) +
                        (g_psx_cps_mode ? " --cps" : "") +
                        " --compiler tcc --tcc " +
                        cmd_quote((tk / "tcc" / "tcc.exe").string());
                    ac_cmd = &built_tcc_cmd;
                    std::fprintf(stdout,
                        "psxrecomp: tcc tier using bundled toolchain (%s)\n",
                        tk.string().c_str());
                } else {
                    std::fprintf(stdout,
                        "psxrecomp: tcc tier active but no bundled toolchain at %s "
                        "(overlay gaps -> interpreter)\n", tk.string().c_str());
                }
            }
        } else {
            if (deferred_has_overlay_ac)
                ac_cmd = &deferred_overlay_ac;
        }
        if (const char *e = std::getenv("PSX_OVERLAY_AUTOCOMPILE_CMD")) {
            if (e[0]) {
                env_ac_cmd = e;
                ac_cmd = &env_ac_cmd;
                std::fprintf(stdout,
                    "psxrecomp: overlay autocompile command overridden by environment\n");
            }
        }
        if (const char *e = std::getenv("PSX_OVERLAY_AUTOCOMPILE_OFF")) {
            if (e[0] && e[0] != '0') {
                ac_cmd = nullptr;
                std::fprintf(stdout,
                    "psxrecomp: overlay autocompile disabled by environment\n");
            }
        }
        if (ac_cmd) {
            autocompile_set_cache_paths(cache_dir.c_str(),
                                        captures_path.string().c_str());
            std::string ac_cwd = deferred_overlay_project_root.string();
            if (const char *e = std::getenv("PSX_OVERLAY_AUTOCOMPILE_CWD")) {
                if (e[0]) ac_cwd = e;
            }
            autocompile_configure(ac_cmd->c_str(), ac_cwd.c_str());
            overlay_autocapture_set_enabled(1);
            std::fprintf(stdout,
                "psxrecomp: overlay autocompile enabled (%s); cache=%s; captures=%s\n",
                overlay_backend_name(eff), cache_dir.c_str(),
                captures_path.string().c_str());
        }
        code_provider_init(cfg_backend, gcc_avail);
    };

    if (deferred_overlay_cache) {
        overlay_init_thread = std::thread([&]() {
            try {
                run_deferred_overlay_init();
            } catch (...) {
                overlay_init_exc = std::current_exception();
            }
        });
    }

#if defined(RECOMP_LAUNCHER)
    /* Integrated recomp-ui launcher: shown in its own GL window before the emulator
     * boots. Seeded with the effective settings (game.toml ∪ settings.toml);
     * on LAUNCH the user's choices are persisted to settings.toml and applied.
     * The launcher window/GL context is destroyed afterward, but SDL subsystems
     * stay initialized so the game window can open without a second SDL_Init.
     *
     * Skip the GUI (boot straight in) when ANY of: PSX_NO_LAUNCHER=1 env,
     * --no-launcher, or the persisted [launcher] skip_launcher setting — unless
     * --launcher forces it back on (mirrors snesrecomp's SkipLauncher / --launcher).
     * This removes the dismiss-the-launcher round-trip for scripted/debug runs. */
    const bool want_launcher =
        force_launcher ||
        (!std::getenv("PSX_NO_LAUNCHER") && !force_no_launcher && !skip_launcher_setting);
    if (want_launcher) {
        launcher_boot_timing_mark("host:before_sdl_init");
    /* Per-monitor DPI awareness, BEFORE any SDL_Init.
     *
     * Without it Windows virtualises everything this process sees: on a
     * 7680x4320 panel at 400% scaling SDL_GetDisplayUsableBounds reports
     * 1920x1032, so the window is clamped to roughly 1376 LOGICAL pixels and
     * opens as a small box, while the desktop compositor then upscales it.
     * The internal render resolution is unaffected -- which is the trap: the
     * game renders at supersampling 16 and the result is thrown away scaling
     * a 1376-wide window up to an 8K display.
     *
     * permonitorv2 makes SDL report physical pixels, so the window sizes
     * against the real panel and the drawable matches it 1:1. */
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
#ifdef SDL_HINT_WINDOWS_DPI_SCALING
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");
#endif
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0) {
            launcher_boot_timing_mark("host:after_sdl_init");
            recomp_launcher_set_preserve_sdl(1);
            int lr = 2; /* 0 = launch, 1 = quit, 2 = unavailable */
            const bool bios_choice_supported =
                psx_bios_has_selectable() != 0 || psx_bios_registry_count == 0;
            PSXRecompV4::UserSettings seed;
            /* Netplay session BIOS is match-only; never overwrite seed/bios.cfg. */
            std::filesystem::path match_session_bios_path;
            bool match_session_bios_set = false;
            seed.renderer = g_video_renderer;             seed.has_renderer = true;
            seed.supersampling = g_video_scale;           seed.has_supersampling = true;
            seed.antialiasing = g_video_aa;               seed.has_antialiasing = true;
            seed.texture_filter = g_video_texfilter;      seed.has_texture_filter = true;
            seed.fmv_filter = g_video_fmv_filter;         seed.has_fmv_filter = true;
            /* Seeded (and marked present) so a launcher save round-trips the
             * player's hand-edited value instead of dropping the key. */
            seed.geometry_correction = (g_video_geometry_correction != 0);
            seed.has_geometry_correction = true;
            seed.perspective_texturing = (g_video_perspective_texturing != 0);
            seed.has_perspective_texturing = true;
            seed.screen_kind = g_video_screen;            seed.has_screen_kind = true;
            seed.auto_skip_fmv = (g_auto_skip_fmv != 0);
            seed.has_auto_skip_fmv = skip_fmv_offered;
            seed.turbo_loads = (g_turbo_loads_enabled != 0);
            seed.has_turbo_loads = turbo_loads_offered;
            seed.fast_boot = fast_boot;                   seed.has_fast_boot = true;
            seed.bios_hle  = bios_hle;                    seed.has_bios_hle  = true;
            seed.fullscreen = g_fullscreen;                seed.has_fullscreen = true;
            seed.frame_interpolation = (g_frame_interpolation != 0);
            seed.has_frame_interpolation = true;
            seed.frame_interpolation_fps = g_frame_interpolation_fps;
            seed.has_frame_interpolation_fps = true;
            seed.aspect_num = g_video_aspect_num;
            seed.aspect_den = g_video_aspect_den;         seed.has_aspect_ratio = true;
            seed.audio_freq = g_audio_freq;               seed.has_audio_freq = true;
            seed.spu_hq = g_audio_spu_hq;                 seed.has_spu_hq = true;
            seed.rewind_depth = g_rewind_depth;           seed.has_rewind_depth = true;
            seed.rewind_interval = g_rewind_interval;     seed.has_rewind_interval = true;
            seed.hotkey_pad_rewind = g_hotkey_pad_rewind;
            seed.has_hotkey_pad_rewind = true;
            seed.hotkey_pad_save_state_menu = g_hotkey_pad_save_state_menu;
            seed.has_hotkey_pad_save_state_menu = true;
            seed.skip_launcher = skip_launcher_setting;   seed.has_skip_launcher = true;
            if (has_netplay_player_name) {
                seed.netplay_player_name = netplay_player_name;
                seed.has_netplay_player_name = true;
            }
            if (!g_lnch_lobby_url.empty()) {
                seed.netplay_lobby_url = g_lnch_lobby_url;
                seed.has_netplay_lobby_url = true;
            }
            if (bios_choice_supported && bios_explicit && bios_path && bios_path[0]) {
                seed.bios_path = bios_path;
                seed.has_bios_path = true;
            }
            /* Hydrate launcher BIOS from bios.cfg when settings.toml has none,
             * and rewrite relative paths to absolute so reopen after generate
             * does not depend on cwd. */
            if (bios_choice_supported && !seed.has_bios_path) {
                std::filesystem::path cached =
                    read_cached_path(argv[0], "bios.cfg");
                if (!cached.empty()) {
                    seed.bios_path = cached;
                    seed.has_bios_path = true;
                }
            }
            if (seed.has_bios_path) {
                std::filesystem::path resolved =
                    resolve_bios_path(seed.bios_path.string().c_str(), argv[0]);
                std::error_code ec;
                if (!resolved.empty() && std::filesystem::exists(resolved, ec)) {
                    seed.bios_path = std::filesystem::weakly_canonical(resolved, ec);
                    if (ec) seed.bios_path = std::filesystem::absolute(resolved, ec);
                    seed.has_bios_path = true;
                } else {
                    seed.bios_path.clear();
                    seed.has_bios_path = false;
                }
            }
            /* First-run setup: if nothing remembered, adopt a retail dump next
             * to the install (SCPH1001…). Missing → leave empty (OpenBIOS).
             * Never override an existing bios.cfg (including cleared OpenBIOS). */
            if (bios_choice_supported && !seed.has_bios_path) {
                std::error_code ec;
                const auto cfg = sidecar_cfg_path(argv[0], "bios.cfg");
                if (!std::filesystem::exists(cfg, ec)) {
                    std::filesystem::path found = discover_retail_bios_near(argv[0]);
                    if (!found.empty()) {
                        seed.bios_path = found;
                        seed.has_bios_path = true;
                        write_cached_path(argv[0], "bios.cfg", found);
                        std::fprintf(stderr,
                                     "psxrecomp: setup adopted retail BIOS %s\n",
                                     found.string().c_str());
                    }
                }
            }
            if (!resolved_disc.empty())    { seed.disc_path = resolved_disc; seed.has_disc_path = true; }
            seed.memcard_dir = memcard_dir;          seed.has_memcard_dir = true;
            seed.memcard1_enabled = memcard1_enabled; seed.has_memcard1_enabled = true;
            seed.memcard2_enabled = memcard2_enabled; seed.has_memcard2_enabled = true;
            seed.multitap_enabled = multitap_enabled; seed.has_multitap_enabled = true;
            seed.multitap_analog = multitap_analog; seed.has_multitap_analog = true;
            if (!memcard1_path.empty()) { seed.memcard1_path = memcard1_path; seed.has_memcard1_path = true; }
            if (!memcard2_path.empty()) { seed.memcard2_path = memcard2_path; seed.has_memcard2_path = true; }
            seed.language = resolved_language; seed.has_language = true;
            {
                const int n = std::min(PSX_MAX_PLAYERS,
                                       PSXRecompV4::UserSettings::kMaxControllerPlayers);
                for (int i = 0; i < n; ++i) {
                    seed.p_device[i] = player_device[i];
                    seed.has_p_device[i] = true;
                    seed.p_mode[i] = player_mode[i];
                    seed.has_p_mode[i] = true;
                    seed.p_deadzone[i] = player_deadzone[i];
                    seed.has_p_deadzone[i] = true;
                }
                seed.deadzone = player_deadzone[0];
                seed.has_deadzone = true;
            }
            seed.window_width = g_video_win_w; seed.has_window_width = true;

            /* recomp-ui creates + owns its SDL2/GL window internally, so there
             * is no launcher window/context to manage here. */
            std::string assets_dir_str = exe_dir_from_argv(argv[0]).string();
            /* Same keybinds.ini / config.ini the runtime reads — never cwd. */
            ae_rui_set_sidecar_paths(argv[0]);
            std::string rui_initial_disc = resolved_disc.string();
            std::string rui_title = (game_name.empty() ? std::string("PSX") : game_name)
                                     + " - Launcher";

            RecompLauncherCSettings ls{};
            ls.output_method  = 2;  /* OpenGL */
            ls.window_scale   = std::max(1, std::min(4, g_video_win_w / 320));
            ls.fullscreen     = seed.fullscreen;
            ls.ignore_aspect  = 0;
            ls.linear_filter  = (seed.texture_filter != 0) ? 1 : 0;
            ls.widescreen     = (seed.aspect_num == 16 && seed.aspect_den == 9) ? 1 : 0;
            ls.widescreen_hud = ls.widescreen;
            ls.enable_audio   = 1;
            ls.audio_freq     = seed.audio_freq;
            ls.volume         = host_volume_get();
            {
                const int n = std::min(PSX_MAX_PLAYERS, RECOMP_LAUNCHER_MAX_PLAYERS);
                for (int i = 0; i < n; ++i) {
                    const std::string& d = player_device[i];
                    ls.player_src[i] =
                        PSXRecompV4::launcher_source_from_device(d);
                    /* Round to the nearest launcher percent. Truncation turned a
                     * saved 20% value (6553/32767) into 19%, which the launcher's
                     * 5% normalization then silently reduced to 15%. */
                    ls.deadzone[i] =
                        (player_deadzone[i] * 100 + 32767 / 2) / 32767;
                    ls.pad_mode[i] = (ls.player_src[i] == 1)
                                        ? PSXRecompV4::PAD_MODE_DIGITAL
                                        : player_mode[i];
                    ls.player_gamepad_guid[i][0] = '\0';
                    if (ls.player_src[i] == 2 && !d.empty() && d != "auto" &&
                        d != "gamepad" && d != "controller") {
                        std::snprintf(ls.player_gamepad_guid[i],
                                      sizeof(ls.player_gamepad_guid[i]), "%s",
                                      d.c_str());
                    }
                }
            }
            ls.skip_launcher  = seed.skip_launcher ? 1 : 0;
            ls.msu1_enabled   = 0;
            ls.msu1_dir[0]    = '\0';
            std::snprintf(ls.netplay_player_name, sizeof(ls.netplay_player_name), "%s",
                          has_netplay_player_name ? netplay_player_name.c_str() : "");
            /* aspect_index: 0 = 4:3, 1 = 16:9, 2 = 21:9 (see RecompLauncherCSettings). */
            ls.aspect_index   = (seed.aspect_num * 9 == seed.aspect_den * 21) ? 2 :
                                 (seed.aspect_num == 16 && seed.aspect_den == 9) ? 1 : 0;

            /* ---- deeper PSX-style settings (capability-gated via launcher_profile
             * below). Sourced 1:1 from PSXRecompV4::UserSettings (config_loader.h). */
            ls.window_width      = seed.window_width;
            ls.renderer           = seed.renderer;
            /* Fresh / invalid seed → OpenGL (DEFAULT_VIDEO_RENDERER), never
             * Software — unless the user explicitly saved software. */
            if (ls.renderer < 0 || ls.renderer > (vulkan_offered ? 2 : 1))
                ls.renderer = PSXRecompV4::DEFAULT_VIDEO_RENDERER;
            if (ls.renderer == 0 && !user_settings_has_renderer &&
                PSXRecompV4::DEFAULT_VIDEO_RENDERER != 0)
                ls.renderer = PSXRecompV4::DEFAULT_VIDEO_RENDERER;
            ls.supersampling      = seed.supersampling;
            ls.antialiasing       = seed.antialiasing ? 1 : 0;
            ls.texture_filter     = seed.texture_filter;
            /* HD textures: on/off plus the active pack folder, which the UI
             * shows read-only. Selection itself belongs to the launcher. */
            ls.hdpack_enabled     = g_hd_textures ? 1 : 0;
            std::snprintf(ls.hdpack_dir, sizeof(ls.hdpack_dir), "%s",
                          g_hd_texture_pack.c_str());
            ls.fmv_filter         = cfg_fmv_filter_to_launcher(seed.fmv_filter);
            ls.geometry_correction   = seed.geometry_correction ? 1 : 0;
            ls.perspective_texturing = seed.perspective_texturing ? 1 : 0;
            ls.screen_kind        = seed.screen_kind;
            ls.frame_interp       = seed.frame_interpolation ? 1 : 0;
            ls.frame_interp_fps   = seed.frame_interpolation_fps;
            ls.spu_hq             = seed.spu_hq ? 1 : 0;
            ls.rewind_depth      = seed.rewind_depth > 0 ? seed.rewind_depth : 50;
            ls.rewind_interval   = seed.rewind_interval > 0 ? seed.rewind_interval : 15;
            ls.assist_pad_bind[PSX_ASSIST_BIND_REWIND] =
                normalize_hotkey_pad_binding(seed.hotkey_pad_rewind,
                    PSX_HOTKEY_PAD_SELECT_R3);
            ls.assist_pad_bind[PSX_ASSIST_BIND_SAVE_STATE_MENU] =
                normalize_hotkey_pad_binding(seed.hotkey_pad_save_state_menu,
                    PSX_HOTKEY_PAD_SELECT_R1);
            ls.auto_skip_fmv      = seed.auto_skip_fmv ? 1 : 0;
            ls.turbo_loads        = seed.turbo_loads ? 1 : 0;
            /* Localization: index of resolved_language within lang_menu_options
             * (match by code; games with no [runtime].languages list leave
             * lang_menu_options empty and this stays 0 / unused). */
            ls.language_index = 0;
            for (size_t li = 0; li < lang_menu_options.size(); li++) {
                if (lang_menu_options[li].code == resolved_language) {
                    ls.language_index = (int)li;
                    break;
                }
            }
            {
                std::string bios_str = seed.bios_path.string();
                std::snprintf(ls.bios_path, sizeof(ls.bios_path), "%s", bios_str.c_str());
            }

            /* Memory-card slots: point each slot at the REAL card the runtime
             * will use — an explicit settings.toml path if set, else the default
             * <memcard_dir>/cardN.mcd the runtime derives — so the launcher's
             * block grids reflect the actual on-disk saves (memcard_inspect). */
            {
                std::string mc1 = seed.has_memcard1_path ? seed.memcard1_path.string()
                                                         : (memcard_dir / "card1.mcd").string();
                std::string mc2 = seed.has_memcard2_path ? seed.memcard2_path.string()
                                                         : (memcard_dir / "card2.mcd").string();
                std::snprintf(ls.memcard_path[0], sizeof(ls.memcard_path[0]), "%s", mc1.c_str());
                std::snprintf(ls.memcard_path[1], sizeof(ls.memcard_path[1]), "%s", mc2.c_str());
            }
            ls.memcard_enabled[0] = seed.memcard1_enabled ? 1 : 0;
            ls.memcard_enabled[1] = seed.memcard2_enabled ? 1 : 0;
#if defined(RECOMP_LAUNCHER_HAS_MULTITAP_ENABLED)
            ls.multitap_enabled = seed.multitap_enabled ? 1 : 0;
#endif
#if defined(RECOMP_LAUNCHER_HAS_MULTITAP_ANALOG)
            ls.multitap_analog = seed.multitap_analog ? 1 : 0;
            g_lnch_multitap_analog = seed.multitap_analog ? 1 : 0;
#endif

            /* Region badge: game.toml [game] region wins verbatim; otherwise derive
             * from the game_id serial prefix (SCUS/SLUS/LSP -> USA, SCES/SLES ->
             * Europe, SCPS/SLPS/SLPM -> Japan). Empty/unknown -> no badge (gi.region
             * left null; the launcher model treats that as "" and hides the row). */
            const std::string rui_region =
                !game_region.empty() ? game_region : region_label_from_serial(game_id);

            /* Localization: one C-string array pointing at lang_menu_options'
             * own storage (that vector outlives this call, it's a main()-scope
             * local), so no separate copy is needed. Empty when the game declares
             * no [runtime].languages -> Localization stays hidden (num_languages=0). */
            std::vector<const char*> rui_lang_labels;
            rui_lang_labels.reserve(lang_menu_options.size());
            for (const auto& lo : lang_menu_options) rui_lang_labels.push_back(lo.label.c_str());

            /* Real disc-verify + memcard-inspect expect serial/CRC on file-scope
             * statics (C-ABI callback can't capture these locals). */
            g_lnch_expected_serial = game_id;
            g_lnch_expected_crc    = game_disc_crc;
            g_lnch_has_crc         = game_has_disc_crc;
            g_lnch_argv0           = argv[0];

            RecompLauncherCGameInfo gi{};
            ae_fill_psx_launcher_game_info(
                &gi,
                game_name.empty() ? nullptr : game_name.c_str(),
                rui_region.empty() ? nullptr : rui_region.c_str(),
                game_players,
                ws_offered,
                ws_ultrawide_offered,
                skip_fmv_offered,
                turbo_loads_offered,
                vulkan_offered,
                ctrl_lock_mode,
                ctrl_lock_device,
                ctrl_locked_mode[0],
                rui_lang_labels.empty() ? nullptr : rui_lang_labels.data(),
                (int)rui_lang_labels.size(),
                /*resume_netplay_room=*/0);
#if defined(PSX_HAS_SETUP_WIZARD)
            /* MotK ships tools/prepare_disc.py (2448→2352). Offer it in the
             * first-run wizard so players need not run the script by hand. */
            {
                const auto root = find_upward(exe_dir_from_argv(argv[0]),
                                              "tools/prepare_disc.py");
                if (!root.empty()) {
                    gi.prepare_disc = ae_prepare_disc;
                    gi.prepare_disc_label = "Convert 2448-byte dump…";
                    gi.prepare_disc_note =
                        "If you have a 2448-byte/sector MotK ISO dump, convert it "
                        "to a MODE2/2352 .bin/.cue under motk/ (same as "
                        "tools/prepare_disc.py).";
                }
            }
            /* Quiet first-run detection: missing/unreadable BIOS or disc opens
             * the setup wizard inside recomp-ui (cross-platform file pickers). */
            {
                bool bios_ok = false;
                RecompLauncherCBiosVerify bv{};
                if (ls.bios_path[0]) {
                    if (ae_bios_verify(ls.bios_path, &bv) && bv.ok) {
                        bios_ok = true;
                        /* Keep an absolute path in the model / settings writeback. */
                        std::filesystem::path resolved =
                            resolve_bios_path(ls.bios_path, argv[0]);
                        std::error_code ec;
                        if (!resolved.empty() &&
                            std::filesystem::exists(resolved, ec)) {
                            auto abs = std::filesystem::weakly_canonical(resolved, ec);
                            if (ec) abs = std::filesystem::absolute(resolved, ec);
                            std::snprintf(ls.bios_path, sizeof(ls.bios_path), "%s",
                                          abs.string().c_str());
                        }
                    } else {
                        ls.bios_path[0] = '\0';
                    }
                }
                if (!bios_ok) {
                    if (ae_bios_verify("", &bv) && bv.ok) bios_ok = true;
                }
                bool disc_ok = false;
                if (!rui_initial_disc.empty()) {
                    std::error_code ec;
                    if (std::filesystem::exists(rui_initial_disc, ec)) {
                        const DiscValidation dv =
                            validate_disc_image(rui_initial_disc, game_id);
                        disc_ok = dv.opened && dv.has_header;
                    }
                    if (!disc_ok) rui_initial_disc.clear();
                }
                gi.needs_setup = (!bios_ok || !disc_ok) ? 1 : 0;
            }
#if defined(PSX_HAS_GAME_CODEGEN)
            /* Local codegen: missing generated/ or MOTK_FORCE_SETUP opens the
             * generate & rebuild wizard (may also set prepare_required). */
            psx_game_codegen_setup_apply(&gi);
            /* host_apply forces has_bios for OpenBIOS-only setup packages. */
            if (gi.setup_wizard_supported)
                gi.has_bios = 1;
#endif
#endif /* PSX_HAS_SETUP_WIZARD */
            launcher_boot_timing_mark("host:setup_checks_done");
#if defined(PSX_HAS_RECOMP_NET) && defined(PSX_HAS_LOBBY_CLIENT)
            g_lnch_netplay_game_name = game_name.empty() ? "PSX" : game_name;
            apply_offline_pad_count(game_players, multitap_enabled);
            psx_lobby_set_max_slots(game_players);
#endif

            char rui_out_disc[1024] = {0};
            launcher_boot_timing_mark("host:before_run_window");
            int rui_rc = recomp_launcher_run_window(
                rui_title.c_str(), &ls, &gi, assets_dir_str.c_str(),
                rui_initial_disc.c_str(), rui_out_disc, sizeof(rui_out_disc));
            launcher_boot_timing_mark("host:after_run_window");

            lr = rui_rc;

            if (lr == 0) {
                seed.netplay_player_name = ls.netplay_player_name;
                seed.has_netplay_player_name = true;
                if (rui_out_disc[0]) {
                    seed.disc_path = rui_out_disc;
                    seed.has_disc_path = true;
                }
                seed.fullscreen    = ls.fullscreen;            seed.has_fullscreen = true;
                seed.skip_launcher = ls.skip_launcher != 0;   seed.has_skip_launcher = true;
                /* aspect_index round-trips 0/1/2 -> 4:3 / 16:9 / 21:9, superseding the
                 * legacy ls.widescreen bool (still set above for older callers). */
                switch (ls.aspect_index) {
                    case 2:  seed.aspect_num = 21; seed.aspect_den = 9; break;
                    case 1:  seed.aspect_num = 16; seed.aspect_den = 9; break;
                    default: seed.aspect_num = 4;  seed.aspect_den = 3; break;
                }
                seed.has_aspect_ratio = true;
                /* has_texture_filter is on (PSX profile) so the launcher edits
                 * ls.texture_filter directly (0=nearest,1=bilinear); ls.linear_filter
                 * is the legacy fallback field for consoles without the cap and is
                 * left unused here. */
                seed.texture_filter = ls.texture_filter ? 1 : 0; seed.has_texture_filter = true;
                seed.fmv_filter = launcher_fmv_filter_to_cfg(ls.fmv_filter);
                seed.has_fmv_filter = true;
                {
                    const int n = std::min(PSX_MAX_PLAYERS, RECOMP_LAUNCHER_MAX_PLAYERS);
                    const int un = std::min(n, PSXRecompV4::UserSettings::kMaxControllerPlayers);
                    for (int i = 0; i < n; ++i) {
                        if (ls.player_src[i] == 1) {
                            player_device[i] = "keyboard";
                            /* Keyboard is always a digital pad at runtime. */
                            player_mode[i] = PSXRecompV4::PAD_MODE_DIGITAL;
                        } else if (ls.player_src[i] == 0) {
                            player_device[i] = "none";
                            player_mode[i] = ls.pad_mode[i];
                        } else if (ls.player_gamepad_guid[i][0]) {
                            player_device[i] = ls.player_gamepad_guid[i];
                            player_mode[i] = ls.pad_mode[i];
                        } else if (PSXRecompV4::launcher_source_from_device(
                                       player_device[i]) <= 1) {
                            player_device[i] = "gamepad";
                            player_mode[i] = ls.pad_mode[i];
                        } else {
                            player_mode[i] = ls.pad_mode[i];
                        }
                        player_deadzone[i] = ls.deadzone[i] * 32767 / 100;
                        if (i < un) {
                            seed.p_device[i] = player_device[i];
                            seed.has_p_device[i] = true;
                            seed.p_mode[i] = player_mode[i];
                            seed.has_p_mode[i] = true;
                            seed.p_deadzone[i] = player_deadzone[i];
                            seed.has_p_deadzone[i] = true;
                        }
                    }
                    seed.deadzone = player_deadzone[0];
                    seed.has_deadzone = true;
                }

                /* ---- deeper PSX-style settings write-back (mirrors the seed
                 * fields above), all gated on by the "psx" launcher_profile caps. */
                seed.window_width          = ls.window_width;          seed.has_window_width          = true;
                seed.renderer              = ls.renderer;              seed.has_renderer              = true;
                seed.supersampling         = ls.supersampling;         seed.has_supersampling         = true;
                seed.antialiasing          = ls.antialiasing != 0;     seed.has_antialiasing          = true;
                seed.geometry_correction   = ls.geometry_correction != 0;
                seed.has_geometry_correction = true;
                seed.perspective_texturing = ls.perspective_texturing != 0;
                seed.has_perspective_texturing = true;
                seed.fmv_filter            = launcher_fmv_filter_to_cfg(ls.fmv_filter);
                seed.has_fmv_filter        = true;
                seed.screen_kind           = ls.screen_kind;           seed.has_screen_kind           = true;
                seed.frame_interpolation   = ls.frame_interp != 0;     seed.has_frame_interpolation   = true;
                seed.frame_interpolation_fps = ls.frame_interp_fps;    seed.has_frame_interpolation_fps = true;
                seed.audio_freq            = ls.audio_freq;            seed.has_audio_freq            = true;
                seed.spu_hq                = ls.spu_hq != 0;           seed.has_spu_hq                = true;
                seed.rewind_depth          = ls.rewind_depth > 0 ? ls.rewind_depth : 50;
                seed.has_rewind_depth      = true;
                seed.rewind_interval       = ls.rewind_interval > 0 ? ls.rewind_interval : 15;
                seed.has_rewind_interval   = true;
                seed.hotkey_pad_rewind = normalize_hotkey_pad_binding(
                    ls.assist_pad_bind[PSX_ASSIST_BIND_REWIND],
                    PSX_HOTKEY_PAD_SELECT_R3);
                seed.has_hotkey_pad_rewind = true;
                seed.hotkey_pad_save_state_menu = normalize_hotkey_pad_binding(
                    ls.assist_pad_bind[PSX_ASSIST_BIND_SAVE_STATE_MENU],
                    PSX_HOTKEY_PAD_SELECT_R1);
                seed.has_hotkey_pad_save_state_menu = true;
                seed.auto_skip_fmv = ls.auto_skip_fmv != 0;
                seed.has_auto_skip_fmv = skip_fmv_offered;
                seed.turbo_loads = ls.turbo_loads != 0;
                seed.has_turbo_loads = turbo_loads_offered;
                host_volume_set(ls.volume);
                /* Bundled-BIOS builds ignore any launcher-supplied path (the
                 * picker is hidden, but a stale settings file could still
                 * carry one) — resolve_bios_for_runtime would reject it and
                 * the identity gate makes it meaningless. */
                if (bios_choice_supported && ls.bios_path[0]) {
                    std::filesystem::path resolved =
                        resolve_bios_path(ls.bios_path, argv[0]);
                    std::error_code ec;
                    if (!resolved.empty() && std::filesystem::exists(resolved, ec)) {
                        seed.bios_path =
                            std::filesystem::weakly_canonical(resolved, ec);
                        if (ec)
                            seed.bios_path = std::filesystem::absolute(resolved, ec);
                    } else {
                        seed.bios_path = ls.bios_path;
                    }
                    seed.has_bios_path = true;
                } else {
                    seed.bios_path.clear();
                    seed.has_bios_path = false;
                }
                /* Memory-card slots: enable flags + any Browse/New paths. */
                seed.memcard1_enabled = ls.memcard_enabled[0] != 0; seed.has_memcard1_enabled = true;
                seed.memcard2_enabled = ls.memcard_enabled[1] != 0; seed.has_memcard2_enabled = true;
#if defined(RECOMP_LAUNCHER_HAS_MULTITAP_ENABLED)
                seed.multitap_enabled = ls.multitap_enabled != 0;
                seed.has_multitap_enabled = true;
                multitap_enabled = seed.multitap_enabled;
                apply_offline_pad_count(game_players, multitap_enabled);
#endif
#if defined(RECOMP_LAUNCHER_HAS_MULTITAP_ANALOG)
                seed.multitap_analog = (!g_force_digital_pads && ls.multitap_analog != 0);
                seed.has_multitap_analog = true;
                multitap_analog = seed.multitap_analog;
                sio_set_multitap_analog(multitap_analog ? 1 : 0);
                if (game_config_path && game_config_path[0] && !g_force_digital_pads) {
                    (void)PSXRecompV4::upsert_game_toml_controller_bool(
                        game_config_path, "multitap_analog", multitap_analog);
                }
#endif
                if (ls.memcard_path[0][0]) { seed.memcard1_path = ls.memcard_path[0]; seed.has_memcard1_path = true; }
                if (ls.memcard_path[1][0]) { seed.memcard2_path = ls.memcard_path[1]; seed.has_memcard2_path = true; }

                /* Localization: persist the chosen language code (only meaningful
                 * when the game declared a menu; otherwise leave seed.language
                 * untouched. */
                if (!lang_menu_options.empty() && ls.language_index >= 0 &&
                    ls.language_index < (int)lang_menu_options.size()) {
                    seed.language = lang_menu_options[ls.language_index].code;
                    seed.has_language = true;
                }
#if defined(PSX_HAS_LOBBY_CLIENT)
                if (ls.netplay_launch.enabled) {
                    const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
                    if (caps && caps->valid) {
                        /* Host-authoritative DualShock-on-tap for the match. */
                        multitap_analog = (!g_force_digital_pads &&
                                           caps->multitap_analog != 0);
                        seed.multitap_analog = multitap_analog;
                        seed.has_multitap_analog = true;
                        sio_set_multitap_analog(multitap_analog ? 1 : 0);
                        seed.aspect_num = caps->aspect_num ? caps->aspect_num : seed.aspect_num;
                        seed.aspect_den = caps->aspect_den ? caps->aspect_den : seed.aspect_den;
                        seed.has_aspect_ratio = true;
                        if (turbo_loads_offered) {
                            seed.turbo_loads = caps->turbo_loads != 0;
                            seed.has_turbo_loads = true;
                        }
                        if (skip_fmv_offered) {
                            seed.auto_skip_fmv = caps->auto_skip_fmv != 0;
                            seed.has_auto_skip_fmv = true;
                        }
                        if (caps->language[0]) {
                            seed.language = caps->language;
                            seed.has_language = true;
                        }
                        /* Match BIOS settle (lobby bios_offer → session_bios).
                         * Ephemeral for this boot only — seed / bios.cfg keep
                         * the player's offline preference. */
                        if (caps->session_bios[0])
                            ae_np_set_session_bios_token(caps->session_bios);
                    }
                }
                /* LAN START / lobby token — apply even when match_caps absent. */
                if (ls.netplay_launch.enabled && g_lnch_session_bios[0] &&
                    resolve_match_session_bios_path(
                        g_lnch_session_bios,
                        seed.has_bios_path ? seed.bios_path
                                           : std::filesystem::path{},
                        ls.bios_path, argv[0],
                        &match_session_bios_path)) {
                    match_session_bios_set = true;
                    if (std::strcmp(g_lnch_session_bios, "scph1001") == 0 &&
                        match_session_bios_path.empty()) {
                        std::fprintf(stderr,
                            "psxrecomp: session BIOS scph1001 required but no "
                            "validated dump found — aborting launch (mixed "
                            "OpenBIOS/SCPH would desync)\n");
                        match_session_bios_set = false;
                        ls.netplay_launch.enabled = 0;
                        g_lnch_pending_direct_launch = {};
                        ae_np_clear_session_bios_token();
                        if (overlay_init_thread.joinable())
                            overlay_init_thread.join();
                        SDL_Quit();
                        return 1;
                    } else if (match_session_bios_path.empty()) {
                        std::fprintf(stdout,
                            "psxrecomp: netplay session BIOS = OpenBIOS "
                            "(match only; preference unchanged)\n");
                    } else {
                        std::fprintf(stdout,
                            "psxrecomp: netplay session BIOS = SCPH-1001 "
                            "(%s; match only; preference unchanged)\n",
                            match_session_bios_path.string().c_str());
                    }
                }
#endif
            }

            if (lr == 1) {
                std::fprintf(stdout, "psxrecomp: launcher closed; exiting.\n");
                if (overlay_init_thread.joinable())
                    overlay_init_thread.join();
                SDL_Quit();
                return 0;
            }
#if defined(PSX_HAS_GAME_CODEGEN)
            if (lr == RECOMP_LAUNCHER_RESULT_RELAUNCH) {
                const char* disc_for_relaunch =
                    rui_out_disc[0] ? rui_out_disc
                                    : (rui_initial_disc.empty()
                                           ? ""
                                           : rui_initial_disc.c_str());
                std::fprintf(stdout,
                             "psxrecomp: relaunch after generate/rebuild\n");
                if (overlay_init_thread.joinable())
                    overlay_init_thread.join();
                psx_game_codegen_relaunch_or_exit(disc_for_relaunch);
                /* Does not return on success. */
                SDL_Quit();
                return 1;
            }
#endif
            if (lr == 0) {
                if (ls.netplay_launch.enabled) {
                    net_cfg.enabled = 1;
                    net_cfg.local_slot = ls.netplay_launch.local_slot;
                    net_cfg.input_player = ls.netplay_launch.input_player;
                    net_cfg.session_id = ls.netplay_launch.session_id;
                    net_cfg.input_delay = ls.netplay_launch.input_delay;
                    net_cfg.input_prediction = ls.netplay_launch.input_prediction;
                    net_cfg.force_input_relay = ls.netplay_launch.force_input_relay ? 1 : 0;
                    net_cfg.force_turn = ls.netplay_launch.force_turn ? 1 : 0;
                    net_cfg.rollback = ls.netplay_launch.rollback ? 1 : 0;
                    net_cfg.player_count = ls.netplay_launch.player_count;
                    net_cfg.slot_count = ae_np_session_slot_count(
                        ls.netplay_launch.player_count, ls.netplay_launch.max_slots,
                        ls.netplay_launch.local_slot, game_players);
                    if (net_cfg.player_count <= 0)
                        net_cfg.player_count = net_cfg.slot_count;
                    net_cfg.occupied_mask = ls.netplay_launch.occupied_mask;
                    std::snprintf(net_cfg.bind_hostport, sizeof(net_cfg.bind_hostport), "%s",
                                  ls.netplay_launch.bind_hostport);
                    std::snprintf(net_cfg.peer_hostport, sizeof(net_cfg.peer_hostport), "%s",
                                  ls.netplay_launch.peer_hostport);
                    g_netplay_from_lobby = 1;
                    std::fprintf(stdout,
                        "psxrecomp: launcher netplay slot=%d slots=%d mask=0x%x bind=%s peer=%s session=%u\n",
                        net_cfg.local_slot, net_cfg.slot_count,
                        (unsigned)net_cfg.occupied_mask, net_cfg.bind_hostport,
                        net_cfg.peer_hostport, (unsigned)net_cfg.session_id);
                    std::fflush(stdout);
                } else {
                    g_netplay_from_lobby = 0;
                }
                g_video_renderer  = seed.renderer;
                g_video_scale     = seed.supersampling;
                g_video_aa        = seed.antialiasing;
                g_video_texfilter = seed.texture_filter;
                g_video_fmv_filter = seed.fmv_filter;
                g_video_geometry_correction   = seed.geometry_correction ? 1 : 0;
                g_video_perspective_texturing = seed.perspective_texturing ? 1 : 0;
                g_video_screen    = seed.screen_kind;
                g_auto_skip_fmv = skip_fmv_offered && seed.auto_skip_fmv ? 1 : 0;
                g_turbo_loads_enabled =
                    turbo_loads_offered && seed.turbo_loads ? 1 : 0;
                fast_boot = seed.fast_boot;
                bios_hle  = seed.bios_hle;
                g_fullscreen      = seed.fullscreen;
                g_frame_interpolation = seed.frame_interpolation ? 1 : 0;
                g_frame_interpolation_fps = seed.frame_interpolation_fps;
                g_video_aspect_num = seed.aspect_num;
                g_video_aspect_den = seed.aspect_den;
                g_audio_freq      = seed.audio_freq;
                g_audio_spu_hq    = seed.spu_hq;
                g_rewind_depth   = seed.has_rewind_depth && seed.rewind_depth > 0
                    ? seed.rewind_depth : 50;
                g_rewind_interval = seed.has_rewind_interval && seed.rewind_interval > 0
                    ? seed.rewind_interval : 15;
                g_hotkey_pad_rewind = seed.has_hotkey_pad_rewind
                    ? seed.hotkey_pad_rewind
                    : PSX_HOTKEY_PAD_SELECT_R3;
                g_hotkey_pad_save_state_menu = seed.has_hotkey_pad_save_state_menu
                    ? seed.hotkey_pad_save_state_menu
                    : PSX_HOTKEY_PAD_SELECT_R1;
                skip_launcher_setting = seed.skip_launcher;
                if (seed.has_bios_path) {
                    settings_bios_storage = seed.bios_path.string();
                    bios_path = settings_bios_storage.c_str();
                    bios_explicit = true;
                    bios_from_cli = false; /* launcher pick supersedes argv --bios */
                    write_cached_path(argv[0], "bios.cfg", seed.bios_path);
                } else {
                    /* OpenBIOS: always clear, even when launched with --bios.
                     * Write an empty bios.cfg (do not delete — missing triggers
                     * first-run retail rediscovery). */
                    bios_from_cli = false;
                    bios_explicit = false;
                    settings_bios_storage.clear();
                    bios_path = PSX_BUNDLED_BIOS_PATH;
                    write_cached_path(argv[0], "bios.cfg", {});
                }
                /* Session settle overrides runtime BIOS only — preference already
                 * written above. Use bios_explicit so resolve_bios_for_runtime
                 * does not re-read bios.cfg and undo an OpenBIOS match. */
                if (match_session_bios_set) {
                    bios_from_cli = false;
                    bios_explicit = true;
                    if (match_session_bios_path.empty()) {
                        settings_bios_storage = PSX_BUNDLED_BIOS_PATH;
                        bios_path = settings_bios_storage.c_str();
                    } else {
                        settings_bios_storage = match_session_bios_path.string();
                        bios_path = settings_bios_storage.c_str();
                    }
                }
                if (seed.has_disc_path) {
                    seed.disc_path = normalize_disc_path_for_launch(seed.disc_path);
                    resolved_disc = seed.disc_path;
                    write_cached_path(argv[0], "disc.cfg", resolved_disc);
                }
                memcard1_enabled = seed.memcard1_enabled;
                memcard2_enabled = seed.memcard2_enabled;
                if (seed.has_memcard1_path) memcard1_path = seed.memcard1_path;
                if (seed.has_memcard2_path) memcard2_path = seed.memcard2_path;
                if (seed.has_language) resolved_language = seed.language;
                {
                    const int n = std::min(PSX_MAX_PLAYERS,
                                           PSXRecompV4::UserSettings::kMaxControllerPlayers);
                    for (int i = 0; i < n; ++i) {
                        if (seed.has_p_device[i]) player_device[i] = seed.p_device[i];
                        if (seed.has_p_mode[i]) player_mode[i] = seed.p_mode[i];
                        if (seed.has_p_deadzone[i]) player_deadzone[i] = seed.p_deadzone[i];
                    }
                    if (seed.has_deadzone) resolved_deadzone = seed.deadzone;
                }
                g_video_win_w = seed.window_width;
                /* Persist the user's choices next to the exe. */
                PSXRecompV4::save_user_settings(
                    exe_dir_from_argv(argv[0]) / "settings.toml", seed);
            }
        }
    }
#endif

    if (overlay_init_thread.joinable()) {
        overlay_init_thread.join();
        if (overlay_init_exc) {
            try {
                std::rethrow_exception(overlay_init_exc);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "psxrecomp: overlay cache init failed: %s\n",
                             ex.what());
                return 1;
            }
        }
    }

    {
        /* Netplay: apply the host lobby mod plan (or clear when vanilla).
         * Do not re-resolve the local offline selection from disk. */
        std::string mod_error;
        if (net_cfg.enabled) {
            if (!PSXRecompV4::mod_runtime_commit_for_netplay(resolved_disc,
                                                            &mod_error)) {
                std::fprintf(stderr,
                             "psxrecomp: cannot apply netplay mods: %s\n",
                             mod_error.c_str());
                return 1;
            }
        } else if (!PSXRecompV4::mod_runtime_commit(resolved_disc, &mod_error)) {
            std::fprintf(stderr, "psxrecomp: cannot launch with selected mods: %s\n",
                         mod_error.c_str());
            return 1;
        }
    }
    /* Activation callbacks are re-run after every launcher session. Clear
     * game-owned controller overrides first so disabling a package cannot
     * leave its prior mode latched across a soft return to the launcher. */
    g_mod_controller_mode_override[0] = -1;
    g_mod_controller_mode_override[1] = -1;
    g_mod_load_wall_multiplier = -1;
    g_mod_load_release_frames = -1;
    g_mod_disc_speed_divisor = -1;
    g_mod_disc_instant_rate = -1;
    g_turbo_audio_sink_enabled = 0;
    psx_ram_reset_size_request();
    g_turbo_load_wall_multiplier = 0;
    g_turbo_load_release_frames = TURBO_LOADS_RELEASE_FRAMES;
    if (!turbo_loads_offered)
        g_turbo_loads_enabled = 0;
    g_frame_interpolation_blend = g_frame_interpolation_blend_default;
    mod_runtime_activate_plugins();
    apply_netplay_local_viewport_aspect(net_cfg.enabled);
    if (g_mod_controller_mode_override[0] >= 0)
        player_mode[0] = g_mod_controller_mode_override[0];
    if (g_mod_controller_mode_override[1] >= 0)
        player_mode[1] = g_mod_controller_mode_override[1];
    if (g_mod_load_wall_multiplier >= 0) {
        g_turbo_loads_enabled = 1;
        g_turbo_load_wall_multiplier = g_mod_load_wall_multiplier;
        g_turbo_load_release_frames = g_mod_load_release_frames;
        /* Fast Loading advances the guest at a host rate greater than real
         * time. Keep the canonical SPU/CD stream running, but discard the
         * accelerated presentation-side audio until pacing resumes; otherwise
         * the SDL bridge overflows and the load becomes observably unstable. */
        g_turbo_audio_sink_enabled = g_turbo_load_wall_multiplier > 1;
        if (g_turbo_load_wall_multiplier) {
            std::fprintf(stdout,
                "psxrecomp: mod selected %dx load acceleration "
                "(%d release frames)\n",
                g_turbo_load_wall_multiplier, g_turbo_load_release_frames);
        } else {
            std::fprintf(stdout,
                "psxrecomp: mod selected uncapped load acceleration "
                "(%d release frames)\n",
                g_turbo_load_release_frames);
        }
    }

    /* Re-apply the resolved language to the translation layer. text_xlate_init
     * (at config load) only saw the game.toml default; this folds in the
     * settings.toml override and the launcher's choice. No-op when unchanged. */
    text_xlate_set_language(resolved_language.c_str());

    /* CLI overrides win over config — applied last, before backend/port init.
     * Enables a soak fleet: several instances on distinct ports + renderers,
     * including BIOS instances that have no [game]-block config. */
    if (cli_debug_port >= 0) debug_port      = (uint16_t)cli_debug_port;
    if (cli_renderer   >= 0) g_video_renderer = cli_renderer;
    if (cli_window_title)    window_title     = cli_window_title;
    if (cli_memcard_dir) {
        memcard_dir = std::filesystem::path(cli_memcard_dir);
        if (memcard_dir.is_relative())
            memcard_dir = exe_dir_from_argv(argv[0]) / memcard_dir;
        memcard_dir = memcard_dir.lexically_normal();
        memcard1_path.clear();
        memcard2_path.clear();
    }

    std::filesystem::path resolved_bios =
        resolve_bios_for_runtime(bios_path, argv[0], bios_explicit);
    if (resolved_bios.empty()) {
        std::fprintf(stderr, "psxrecomp: no BIOS selected; exiting.\n");
        return 1;
    }
    if (game_config_path || disc_override_path || !resolved_disc.empty()) {
        resolved_disc = resolve_disc_for_runtime(resolved_disc, disc_override_path, game_id, argv[0]);
        if (game_config_path && resolved_disc.empty()) {
            std::fprintf(stderr, "psxrecomp: no disc image selected; exiting.\n");
            return 1;
        }
    }

    /* memcard_dir was resolved to its default before the launcher (above). */

    std::string bios_path_str    = resolved_bios.string();
    std::string memcard_dir_str  = memcard_dir.string();
    /* A disc-patching mod builds a private patched image; mount that instead of
     * the stock disc, leaving the user's original untouched (master behaviour). */
    const std::filesystem::path& mod_disc =
        PSXRecompV4::mod_runtime_effective_disc_path();
    std::string disc_path_str =
        (mod_disc.empty() ? resolved_disc : mod_disc).string();
    if (!mod_disc.empty()) {
        std::fprintf(stdout,
            "psxrecomp: stock disc remains %s; mounting private mod cache %s\n",
            resolved_disc.string().c_str(), mod_disc.string().c_str());
    }

    /* HD texture pack. Keyed off the STOCK disc, not mod_disc: the pack folder
     * belongs next to the user's own image (where a RetroArch-authored pack
     * already sits) and must not move when a disc-patching mod is toggled. */
    /* Precedence: game.toml < settings.toml (applied above with the other
     * [video] keys) < environment. settings.toml is the layer both the
     * launcher's pack manager and recomp-ui's toggle write, so a pack selected
     * in either survives the other; env stays a developer override. */
    {
        const char *hd_env = std::getenv("PSX_HD_TEXTURE_DUMP");
        if (hd_env && hd_env[0] && std::strcmp(hd_env, "0") != 0) g_hd_texture_dump = 1;
        hd_env = std::getenv("PSX_HD_TEXTURES");
        if (hd_env && hd_env[0]) g_hd_textures = (std::strcmp(hd_env, "0") != 0) ? 1 : 0;
        hd_env = std::getenv("PSX_HD_TEXTURE_DIR");
        if (hd_env && hd_env[0]) g_hd_texture_dir = hd_env;
        hd_env = std::getenv("PSX_HD_TEXTURE_PACK");
        if (hd_env && hd_env[0]) g_hd_texture_pack = hd_env;
    }
    /* Title-side authoring packs remain external user content. If the user
     * placed a permitted replacement folder beside the disc, discover the
     * exact Beetle convention first, then an explicit scale/coverage variant.
     * Never search outside the disc directory and never copy the pack into
     * the executable's release tree. */
    if (g_hd_texture_pack.empty() && !resolved_disc.empty()) {
        const std::filesystem::path parent = resolved_disc.parent_path();
        const std::string stem = resolved_disc.stem().string();
        const std::filesystem::path exact =
            parent / (stem + "-texture-replacements");
        std::error_code ec;
        if (std::filesystem::is_directory(exact, ec)) {
            g_hd_texture_pack = exact.string();
        } else {
            std::filesystem::path best;
            const std::string prefix = stem + "-texture-replacements-";
            for (const auto &entry : std::filesystem::directory_iterator(parent, ec)) {
                if (ec) break;
                if (!entry.is_directory(ec)) continue;
                const std::string name = entry.path().filename().string();
                if (name.rfind(prefix, 0) == 0 && name.size() > prefix.size()) {
                    if (best.empty() || name < best.filename().string()) best = entry.path();
                }
            }
            if (!best.empty()) g_hd_texture_pack = best.string();
        }
    }
    tex_pack_init(resolved_disc.string().c_str(), g_hd_textures, g_hd_texture_dump,
                  g_hd_texture_dir.c_str(), g_hd_texture_pack.c_str());
    /* Config-owned exclusions survive pack-directory regeneration (the pack
     * dir is gitignored game content; losing its exclude.txt silently
     * re-enabled font-atlas substitution - the doubled-glyph defect). */
    for (const std::string &hx : g_hd_texture_exclude) {
        unsigned h = 0;
        if (std::sscanf(hx.c_str(), "%x", &h) == 1)
            tex_pack_add_excluded((uint32_t)h);
    }
    /* Coverage report for the launcher's per-pack stats. atexit alongside the
     * other end-of-session flushes; a no-op when no pack is active. */
    std::atexit(tex_pack_write_coverage);

session_reboot:
    /* Rematch after lobby soft-return re-enters here with updated net_cfg. */
    static int s_emu_session = 0;
    const bool rematch_session = (++s_emu_session > 1);
    std::fprintf(stdout, "psxrecomp runtime: loading BIOS from %s%s\n",
                 bios_path_str.c_str(),
                 rematch_session ? " (rematch)" : "");
    /* Soft-exit longjmps out of device service with a leftover guest clock;
     * rematch must start from cycle 0 or vblanks never fire again. */
    psx_cycles_reset_for_boot();
    starvation_ring_reset();
    present_session_reset();
    /* Rematch ≈ cold start for netplay sim residue a cold peer lacks
     * (pad edges, dig0 latch, tip densify, FMV flags, IRQ resume).
     * Idempotent with BYE teardown; device *_init still runs below. */
    psx_netplay_cold_reset();
    {
        extern uint64_t s_frame_count;
        extern uint32_t g_debug_current_func_addr;
        extern uint32_t g_debug_last_store_pc;
        extern int g_call_unit_depth;
        extern int g_psx_dispatch_depth;
        s_frame_count = 0;
        g_debug_current_func_addr = 0;
        g_debug_last_store_pc = 0;
        /* Soft-exit mid-call can skip the depth restore if the longjmp path
         * was not taken; sticky depth suppresses overlay IRQ delivery and
         * freezes rematch after lockstep armed. */
        g_call_unit_depth = 0;
        g_psx_dispatch_depth = 0;
    }
    /* Cold start activates via resolve_bios_for_runtime before this label.
     * Rematch only rewrites bios_path_str — without re-activate, memory_init
     * loads new ROM bytes against the prior match's linked backend (e.g.
     * SCPH-1001 dump + sticky OPENBIOS) and dig0 never publishes cleanly. */
    if (!validate_bios_for_launch(std::filesystem::path(bios_path_str))) {
        std::fprintf(stderr, "psxrecomp: BIOS activate failed for %s%s\n",
                     bios_path_str.c_str(),
                     rematch_session ? " (rematch)" : "");
        return 1;
    }
    if (rematch_session) {
        std::fprintf(stdout,
            "psxrecomp: rematch BIOS activated id=%s bundled=%d "
            "deliver_event_ret=0x%x shell_entry=0x%x\n",
            psx_bios_image.image_id ? psx_bios_image.image_id : "?",
            psx_bios_image.image_bundled ? 1 : 0,
            (unsigned)psx_bios_image.deliver_event_ret,
            (unsigned)psx_bios_image.shell_entry_phys);
        std::fflush(stdout);
    }
    memory_init(bios_path_str.c_str());
#ifndef PSX_HAVE_VULKAN
    /* Vulkan was not compiled in (PSX_ENABLE_VULKAN=OFF or no SDK found).
     * Refuse a vulkan request from ANY source (config / CLI / launcher seed)
     * and fall back to OpenGL, so the window is never created with
     * SDL_WINDOW_VULKAN against an inert backend stub. */
    if (g_video_renderer == 2) {
        std::fprintf(stdout, "psxrecomp: Vulkan renderer is not available in this build "
                             "(PSX_ENABLE_VULKAN=OFF); using OpenGL instead.\n");
        g_video_renderer = 1;
    }
#endif
    /* Netplay: CPU VRAM is digest/snap authority. Prefer dual-raster OpenGL
     * (SW@1× + GL@settings SSAA FBO present, never glReadPixels). Vulkan
     * present not yet cpu-auth — fall back to a software window. */
    s_netplay_gl_present = 0;
    s_netplay_sim_native_scale = 0;
    gl_renderer_set_cpu_auth_dual(0);
    if (net_cfg.enabled) {
        s_netplay_sim_native_scale = 1;
        if (g_video_renderer == 1) {
            s_netplay_gl_present = 1;
            g_gl_fbo_present = 1;
            gl_renderer_set_cpu_auth_dual(1);
            gr_set_backend(GR_BACKEND_OPENGL);
            std::fprintf(stdout,
                         "psxrecomp: netplay — dual-raster "
                         "(SW@1x CPU-auth + OpenGL present quality; no FBO readback)\n");
            std::fprintf(stdout,
                         "psxrecomp: renderer backend requested: opengl "
                         "(netplay dual-raster)\n");
        } else if (g_video_renderer == 2) {
            std::fprintf(stdout,
                         "psxrecomp: netplay — Vulkan present not yet CPU-auth; "
                         "using software window (was vulkan)\n");
            g_video_renderer = 0;
            gr_set_backend(GR_BACKEND_SOFTWARE);
            std::fprintf(stdout,
                         "psxrecomp: renderer backend requested: software "
                         "(netplay sim)\n");
        } else {
            gr_set_backend(GR_BACKEND_SOFTWARE);
            std::fprintf(stdout,
                         "psxrecomp: renderer backend requested: software "
                         "(netplay sim)\n");
        }
    } else {
        /* Select the renderer backend BEFORE gpu_init() (which runs gr_init ->
         * the backend's init on the VRAM buffer). Software is the default and
         * the fallback; an unavailable OpenGL backend reverts to software. */
        gr_set_backend(g_video_renderer == 2 ? GR_BACKEND_VULKAN :
                       g_video_renderer == 1 ? GR_BACKEND_OPENGL :
                                              GR_BACKEND_SOFTWARE);
        std::fprintf(stdout, "psxrecomp: renderer backend requested: %s\n",
                     g_video_renderer == 2 ? "vulkan" :
                     g_video_renderer == 1 ? "opengl" : "software");
    }
    gpu_init();
    /* Internal-resolution supersampling (SSAA). Must follow gpu_init.
     * Dual-raster: gr_set_scale(N) arms GL hr FBO @ N× while glb_set_scale
     * keeps SW at 1×. SW-only netplay: force scale 1. Offline: full SSAA. */
    if (g_video_scale < 1) g_video_scale = 1;
    /* The ceiling is per-backend, not global. SW_MAX_INTERNAL_SCALE exists
     * because the software path allocates a VRAM-sized hi-res MIRROR that costs
     * 1 MB * scale^2; under GL that mirror stays at 1x (glb_set_scale) and the
     * cost is an FBO instead, so GL can go considerably higher. Applying the
     * software limit to every backend capped GL at 4x -- about 2048x960
     * internal -- which is well short of 1440p/4K/8K on hardware that can
     * trivially do it. The GL backend clamps again to the driver's real texture
     * limit once the context exists. */
    {
        const int max_scale = (g_video_renderer == 1) ? GL_MAX_INTERNAL_SCALE
                                                      : SW_MAX_INTERNAL_SCALE;
        if (g_video_scale > max_scale) g_video_scale = max_scale;
    }
    if (net_cfg.enabled && s_netplay_gl_present && gl_renderer_cpu_auth_dual()) {
        gr_set_scale(g_video_scale);
        if (g_video_scale > 1) {
            std::fprintf(stdout,
                         "psxrecomp: netplay GL present supersampling %dx "
                         "(SW authority stays 1x)\n",
                         g_video_scale);
        }
    } else if (net_cfg.enabled || s_netplay_sim_native_scale) {
        gr_set_scale(1);
        if (g_video_scale > 1) {
            std::fprintf(stdout,
                         "psxrecomp: netplay sim supersampling clamped to 1x "
                         "(settings %dx kept for offline)\n",
                         g_video_scale);
        }
    } else {
        gr_set_scale(g_video_scale);
    }
    /* The scale we asked the backend for. Read it before the reflection below:
     * the GL backend's gr_scale() reports its REAL internal scale, which is
     * still 0/1 until the GL context comes up later, so g_video_scale does not
     * hold the effective factor at this point. */
    const int requested_scale = g_video_scale;
    g_video_scale = gr_scale(); /* reflect any clamp / alloc fallback */
    gr_set_texture_filter(g_video_texfilter);
    /* Sub-pixel vertex precision + perspective-correct UVs. Both default off;
     * with both off every setter below leaves the tracking caches disabled and
     * the draw path is the faithful integer one, unchanged. */
    /* Env overrides (debug/validation path, like PSX_BIOS_HLE): arm the
     * corrections from process start so free-running (headless) boots can be
     * measured from the first projected vertex — a TCP toggle always arrives
     * after the interesting window. '0' = off, anything else = on. */
    if (const char* e = std::getenv("PSX_GEOMETRY_CORRECTION"))
        g_video_geometry_correction = (*e && *e != '0') ? 1 : 0;
    if (const char* e = std::getenv("PSX_PERSPECTIVE_TEXTURING"))
        g_video_perspective_texturing = (*e && *e != '0') ? 1 : 0;
    if (const char* e = std::getenv("PSX_PGXP_CPU_MODE"))
        g_video_pgxp_cpu_mode = (*e && *e != '0') ? 1 : 0;
    gte_geometry_correction_set(g_video_geometry_correction);
    gpu_texture_correction_set(g_video_perspective_texturing);
    pgxp_set_cpu_mode(g_video_pgxp_cpu_mode);
    pgxp_set_tolerance(g_video_pgxp_tolerance);
    if (g_video_geometry_correction || g_video_perspective_texturing) {
        std::fprintf(stdout,
                     "psxrecomp: geometry correction %s, perspective texturing %s%s\n",
                     g_video_geometry_correction ? "on" : "off",
                     g_video_perspective_texturing ? "on" : "off",
                     (g_video_geometry_correction && requested_scale < 2)
                         ? " (needs [video] supersampling >= 2 to be visible)" : "");
    }
    /* Display aspect. Identity at the default 4:3. The present letterbox uses
     * this aspect; native-wide fills it with a genuinely wider frame (no
     * stretch), squash mode stretches the 4:3 frame into it. */
    gl_renderer_set_display_aspect(g_video_aspect_num, g_video_aspect_den);
    gl_renderer_set_fixed_outer_aspect(g_ws_fixed_outer_aspect ? 1 : 0);
    vk_renderer_set_display_aspect(g_video_aspect_num, g_video_aspect_den);
    vk_renderer_set_fixed_outer_aspect(g_ws_fixed_outer_aspect ? 1 : 0);
    gl_renderer_set_pgxp_depth(g_video_pgxp_depth);
    if (g_video_pgxp_depth) {
        /* Unarmed prims (no recovered depth: ship model, HUD, CPU-built
         * effects) must ALWAYS PASS, or the depth test culls them against
         * world Z they never took part in — measured live: the player ship
         * and trackside monitors vanish without this. */
        gl_renderer_set_depth_always(1);
    }
    if (g_video_pgxp_depth)
        std::fprintf(stdout, "psxrecomp: PGXP depth buffer on (ordering-table "
                             "sort augmented by recovered per-vertex W)\n");
    if (g_video_aspect_num * 3 != g_video_aspect_den * 4) {
        /* Hold widescreen off through the BIOS boot (authentic 4:3 logos);
         * the per-frame present path engages it at game entry. */
        g_ws_engaged = false;
        std::fprintf(stdout,
                     "psxrecomp: widescreen %d:%d (%s%s%s; engages at game entry)\n",
                     g_video_aspect_num, g_video_aspect_den,
                     g_ws_native_wide ? "native-wide, present 1:1"
                                      : "GTE X-squash + stretched present",
                     g_ws_anchor_addr ? " + sprite tags" : "",
                     g_ws_hud_sprt ? " + HUD squash" : "");
    }
    /* Present-time screen-colour model (verified-enhancement LUT). Default raw
     * is byte-identical; PSX_SCREEN env overrides this at scanout. */
    gpu_set_screen_kind(g_video_screen);
    if (g_video_scale > 1 || g_video_texfilter)
        std::fprintf(stdout,
                     "psxrecomp: supersampling %dx (antialiasing %s, texture filter %s)\n",
                     g_video_scale, g_video_aa ? "on" : "off",
                     g_video_texfilter ? "bilinear" : "nearest");
    if (g_video_screen != 0)
        std::fprintf(stdout, "psxrecomp: screen-colour model %s\n",
                     g_video_screen == 1 ? "crt" : g_video_screen == 2 ? "composite"
                                                 : "trinitron");
    dma_init();
    mdec_init();
    timers_init();
    interrupts_init();
    sio_init();
    psx_event_step_conservative_env_init();
    /* Seed per-player device routing from the resolved [controller] config.
     * SDL controller handles are opened later (after SDL_Init); here we only
     * set the PSX-visible connection + pad type so the BIOS sees the right
     * ports during early boot. */
    for (int s = 0; s < PSX_MAX_PLAYERS; ++s) {
        set_player_device(g_players[s], player_device[s], player_mode[s]);
        g_players[s].deadzone = player_deadzone[s];
    }
    /* Multitap stays OFF through BIOS boot: SCPH-1070 on port 1 breaks shell /
     * LoadExe pad bring-up for titles that expect a lone digital pad. Offline
     * 3+ player builds arm it after game entry (see vblank path); netplay arms
     * it from psx_netplay when slot_count >= 3. */
    if (net_cfg.enabled) {
        /* Netplay BIOS boot must not mirror per-peer DualShock vs keyboard —
         * that forked dig0 snap sio/pads (baseline ext) on rematch. Session
         * start also re-canonicalizes; seed here so early boot is identical. */
        int np_slots = net_cfg.slot_count;
        if (np_slots < 2) np_slots = 2;
        if (np_slots > PSX_MAX_PLAYERS) np_slots = PSX_MAX_PLAYERS;
        sio_netplay_canonicalize_session_pads(np_slots);
    } else {
        for (int s = 0; s < PSX_MAX_PLAYERS; s++) {
            /* Dev-any-input keeps P1 connected even with no assigned controller so the
             * keyboard / any plugged-in controller can drive port 1 standalone. */
            const bool dev_p1 = (dev_any_input_enabled() && s == 0);
            const int mode = effective_player_mode_for_sio(g_players[s], s);
            sio_set_pad_connected(s, (g_players[s].kind != 0 || dev_p1) ? 1 : 0);
            sio_set_pad_analog(s, pad_mode_boot_analog(mode), 0x80, 0x80, 0x80, 0x80);
            sio_set_pad_config_capable(s, mode != PSXRecompV4::PAD_MODE_DIGITAL);
        }
    }
    /* SPU float-shadow gate must be set before spu_init() (which runs
     * spu_shadow_reset()). Default OFF; PSX_AUDIO_SHADOW env overrides. */
    spu_shadow_set_enabled(g_audio_spu_hq ? 1 : 0);
    if (g_audio_spu_hq)
        std::fprintf(stdout, "psxrecomp: SPU float-shadow enabled (verified-enhancement)\n");
    spu_init();
    cdrom_init(disc_path_str.empty() ? NULL : disc_path_str.c_str());

    /* A disc was requested but nothing mounted. cdrom_init() is non-fatal here
     * (BIOS-only targets run with an empty drive on purpose), so without this
     * check the game would boot into an empty drive and render NOTHING -- the
     * "black screen on a .cue that works as a .bin" symptom. The earlier
     * validate_disc_for_launch() pass cannot catch it: identify_disc() reads
     * the data track FILE directly, so a cue whose sheet the mounting reader
     * rejects still shows a green "Disc verified" badge. Report the actual
     * failure instead of leaving the player staring at black. */
    if (!disc_path_str.empty() && !cdrom_has_disc()) {
        const PSXRecompV4::DiscPathResolution r =
            PSXRecompV4::resolve_disc_path(disc_path_str);
        std::string detail =
            "The disc image was found and verified, but the CD-ROM drive could "
            "not mount it, so the game would boot with an empty drive.\n\n"
            "Selected:\n" + disc_path_str;
        if (r.mount != r.picked) detail += "\nMounted as:\n" + r.mount.string();
        if (!r.note.empty())     detail += "\n\n" + r.note;
        detail += "\n\nIf this is a .cue, check that every FILE line it names "
                  "exists next to it; selecting the .bin directly also works.";
        launcher_warning("Disc Could Not Be Mounted", detail);
        return 1;
    }
    for (const auto& route : warm_cd_routes) {
        cdrom_register_warm_route(route.arm_lba, route.lbas.data(),
                                  (int)route.lbas.size(),
                                  route.instant_max_per_frame);
        std::fprintf(stdout,
                     "psxrecomp: warm CD route armed at LBA %d (%zu entries, "
                     "%d sectors/frame)\n",
                     route.arm_lba, route.lbas.size(),
                     route.instant_max_per_frame);
    }
    if (!disc_path_str.empty()) {
        /* GetID must report the inserted disc's license region (the BIOS CD
         * driver revalidates it mid-game). Derive it from the disc's boot
         * serial via the same disc_identity module the launch check uses. */
        const auto ident = PSXRecompV4::identify_disc(
            disc_path_str, /*expected_serial*/"", /*expected_crc*/0,
            /*has_expected_crc*/false, /*compute_crc*/false);
        if (ident.region == "PAL")         cdrom_set_disc_scex("SCEE");
        else if (ident.region == "NTSC-J") cdrom_set_disc_scex("SCEI");
        else if (ident.region == "NTSC-U") cdrom_set_disc_scex("SCEA");
        if (!ident.region.empty())
            std::fprintf(stdout, "psxrecomp: disc region %s (serial %s)\n",
                         ident.region.c_str(), ident.detected_serial.c_str());
    }
    /* Arm the text-image guard now that both possible sources are resolved:
     * the local EXE file (dev checkouts) and the disc image (every install). */
    if (game_config_path)
        arm_text_image_guard(text_guard_exe_path, text_guard_load_addr,
                             disc_path_str);
    /* Executable/overlay patches from enabled mods, applied once the guard is
     * armed so a patched image is never mistaken for a divergent one. */
    mod_runtime_enable_disc_patches();
    {
        int divisor = 1; /* default: authentic 1x timing */
        if (disc_speed == "instant") divisor = 0;
        else if (disc_speed == "4x") divisor = 4;
        else if (disc_speed == "2x") divisor = 2;
        if (g_mod_disc_speed_divisor >= 0)
            divisor = g_mod_disc_speed_divisor;
        /* Store for post-BIOS application; boot always runs at 1x so the
         * BIOS disc-init sequence sees correct timing. */
        cdrom_set_game_speed(divisor);
        if (g_mod_disc_instant_rate > 0)
            cdrom_set_instant_rate(g_mod_disc_instant_rate);
        else if (instant_rate > 0)
            cdrom_set_instant_rate(instant_rate);
        if (divisor != 1)
            std::fprintf(stdout, "psxrecomp: disc speed divisor=%d (applied post-BIOS, "
                         "instant budget %d/frame)\n",
                         divisor, cdrom_get_instant_rate());
    }
    {
        std::string mc1 = memcard1_path.string();
        std::string mc2 = memcard2_path.string();
        const MemcardSlotConfig slots[2] = {
            { mc1.empty() ? nullptr : mc1.c_str(), memcard1_enabled ? 1 : 0 },
            { mc2.empty() ? nullptr : mc2.c_str(), memcard2_enabled ? 1 : 0 },
        };
        memcard_init_slots(memcard_dir_str.c_str(), slots);
    }
    if (!rematch_session) {
        std::atexit(memcard_flush_all);
        /* Persist the game's native OPTION settings on any exit path (belt-and-
         * suspenders; save-on-change already persists them live). No-op if not yet
         * armed, so it can't overwrite the saved file with boot defaults. */
        std::atexit(game_options_save_now);
#ifndef PSX_NO_DEBUG_TOOLS
        debug_server_init(debug_port);
#else
        (void)debug_port;
#endif
#ifdef PSX_COSIM
        cosim_init();  /* first-divergence oracle server */
#endif
        /* Heartbeat always on — see freeze_heartbeat.c rationale. */
        freeze_heartbeat_start("psx-runtime");
    } else {
#ifndef PSX_NO_DEBUG_TOOLS
        (void)debug_port;
#endif
    }
    /* Register game entry_pc for post-BIOS disc speed switch. Fires once when
     * the BIOS hands control to the game EXE — not on the BIOS shell. */
    if (game_entry_pc != 0)
        fntrace_set_game_range(game_entry_pc, 0);

  if (g_headless) {
    std::fprintf(stdout, "psxrecomp: headless frontend enabled\n");
  } else {
    /* ---- SDL init ---- */
    /* Scale quality governs SDL's logical-size -> window scaling. Linear when
     * antialiasing is on so the (super)sampled frame stays smooth when the
     * window is resized; nearest preserves crisp pixels otherwise. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_video_aa ? "1" : "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Prefer SDL's own HIDAPI driver over platform-native so Steam's virtual
     * Xbox controller (injected by Steam Input / Remote Play) is enumerated
     * as a game controller rather than a raw HID device. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    /* SDL3 aliases the SDL2-era PS5 rumble hint to enhanced reports. Enabling
     * it also preserves DualSense rumble on the explicit SDL2 fallback. */
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1",
                            SDL_HINT_OVERRIDE);
    /* ...but HIDAPI's Xbox sub-driver is OFF by default on Windows (Xbox pads are
     * normally RAWINPUT/XInput there). With RAWINPUT disabled above, a PHYSICAL
     * Xbox One/Series controller would be claimed by nobody -> not a GameController
     * -> zero input (PS5 DualSense works regardless: its HIDAPI driver is on by
     * default). Enable the HIDAPI Xbox driver so HIDAPI handles Xbox pads too. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    if (!SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) {
    /* Per-monitor DPI awareness, BEFORE any SDL_Init.
     *
     * Without it Windows virtualises everything this process sees: on a
     * 7680x4320 panel at 400% scaling SDL_GetDisplayUsableBounds reports
     * 1920x1032, so the window is clamped to roughly 1376 LOGICAL pixels and
     * opens as a small box, while the desktop compositor then upscales it.
     * The internal render resolution is unaffected -- which is the trap: the
     * game renders at supersampling 16 and the result is thrown away scaling
     * a 1376-wide window up to an 8K display.
     *
     * permonitorv2 makes SDL report physical pixels, so the window sizes
     * against the real panel and the drawable matches it 1:1. */
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
#ifdef SDL_HINT_WINDOWS_DPI_SCALING
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");
#endif
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
    }
    load_input_config(argv[0]);
    /* The launcher / settings.toml / game.toml deadzone (when set) is the
     * user-facing authority; apply it over the input.ini value here, after
     * load_input_config has read input.ini. */
    if (resolved_deadzone >= 0)
        controller_deadzone = std::max(0, std::min(32767, resolved_deadzone));
    else
        controller_deadzone = kDefaultDeadzoneRaw;
    for (int s = 0; s < PSX_MAX_PLAYERS; ++s) {
        if (player_deadzone[s] >= 0)
            g_players[s].deadzone = std::max(0, std::min(32767, player_deadzone[s]));
        else
            g_players[s].deadzone = controller_deadzone;
    }
    controller_deadzone = g_players[0].deadzone;
    refresh_player_devices();  /* open SDL handles to match the player config */
#ifndef PSX_SDL_NO_AUDIO
    audio_trace_init();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        PsxSdlAudioSpec want = {};
        PsxSdlAudioSpec have = {};
        want.freq = g_audio_freq;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        const bool legacy = audio_legacy_mode();
        want.allow_frequency_change = legacy ? 0 : 1;
        if (!legacy)
            want.callback = sdl_drc_callback;  /* pull model: bridge resamples + DRC */
        sdl_audio_device = psx_sdl_audio_open(&want, &have);
        if (sdl_audio_device) {
            if (!legacy) {
                rab_config cfg; rab_config_defaults(&cfg);
                cfg.channels    = 2;
                cfg.source_rate = 44100.0;            /* SPU render rate */
                cfg.host_rate   = (double)have.freq;  /* actual device rate */
                if (rab_init(&s_drc, &cfg) == 0) s_drc_ready = true;
            }
            g_audio_host_rate = have.freq;
            audio_trace_set_tap_rate(AUDIO_TAP_HOST, (uint32_t)have.freq);
            (void)psx_sdl_audio_resume(sdl_audio_device);
        }
    }
    /* Always register: SPU advance is guest-cycle budgeted and must not
     * depend on a successful host open (Win↔Linux aux/spu fork). Routed
     * through the gated wrapper so turbo mute/sink still apply. */
    psx_set_midframe_audio_pump(sdl_audio_pump_midframe);
#endif

    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (g_video_renderer == 1) {
        configure_core_gl_context_attributes();
        win_flags |= SDL_WINDOW_OPENGL;
    }
    if (g_video_renderer == 2) win_flags |= SDL_WINDOW_VULKAN;
    /* Fullscreen on launch (launcher's tri-state Fullscreen control): 1 =
     * borderless desktop fullscreen (keeps the desktop resolution, letterboxes
     * the image), 2 = exclusive fullscreen (real display-mode change), 0 =
     * windowed. Matches the in-game Alt+Enter / Cmd+Ctrl+F hotkey behaviour. */
    win_flags |= psx_fullscreen_flag_for_mode(g_fullscreen);
    /* Open at the user-chosen window size (default 1280 wide) instead of the
     * old hardcoded 640x480, so the game doesn't boot into a tiny window. The
     * height follows the configured display aspect (4:3 native, wider for the
     * widescreen hack); the present path letterboxes to the same aspect, so
     * the image scales to fill the larger window with no further distortion. */
    int game_w = g_video_win_w, game_h = 0;
    clamp_window_aspect(&game_w, &game_h, g_video_aspect_num, g_video_aspect_den);
    sdl_window = SDL_CreateWindow(
        window_title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        game_w, game_h,
        win_flags
    );
    if (!sdl_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    psx_apply_window_icon(sdl_window, argv[0]);

    /* Maximise instead of computing the frame size ourselves.
     *
     * clamp_window_aspect fits the CLIENT area to the usable bounds, but a
     * window is client plus title bar and borders, so fitting the client to a
     * full-height display produced a window taller than the screen that hung
     * off the top. Deriving the decoration size first does not work either:
     * SDL_GetWindowBordersSize reports nothing useful before the window is
     * shown, so the correction silently did not apply.
     *
     * The window manager already solves this exactly. Maximise and let it fit
     * the work area, decorations and taskbar included. Only when the request
     * was "fit the display" (window_width unset) -- an explicit width is a
     * deliberate choice and is left alone. */
    if (!g_fullscreen && !g_video_win_w_explicit)
        SDL_MaximizeWindow(sdl_window);

    /* Host refresh: if the panel is within ~2% of the live CRTC rate (NTSC
     * 59.94 / PAL 50), record it so driver vsync can own cadence. Otherwise
     * keep the PSX CRTC period and force swap interval 0. Mods that own
     * native VBlank keep their cadence.
     *
     * Sanitize aggressively: Wayland/SDL has reported pixel width (3840) as
     * refresh_rate, which selects wall-pacer+swap0 while the compositor still
     * blocks → 30 Hz / 0.50x. Prefer SDL3 numerator/denominator when present. */
    {
        SDL_DisplayMode dm{};
        int disp_idx = SDL_GetWindowDisplayIndex(sdl_window);
        const char *reject_why = nullptr;
        double raw_hz = 0.0;
        if (disp_idx >= 0 && SDL_GetCurrentDisplayMode(disp_idx, &dm) == 0) {
            const double float_hz = (double)dm.refresh_rate;
            raw_hz = float_hz;
#if defined(PSX_SDL3)
            if (dm.refresh_rate_numerator > 0 && dm.refresh_rate_denominator > 0) {
                raw_hz = (double)dm.refresh_rate_numerator /
                         (double)dm.refresh_rate_denominator;
            }
#endif
            g_host_refresh_hz = display_mode_refresh_hz(dm, &reject_why);
            if (reject_why) {
                std::printf("psxrecomp: ignoring bogus host refresh %.0f Hz "
                            "(%s; mode %dx%d, sdl float %.0f); treating as unknown\n",
                            raw_hz, reject_why, dm.w, dm.h, float_hz);
            }
        }
        refresh_frame_pacer_period();
        s_present_crtc_period_cycles = gpu_vblank_period_cycles();
        s_present_crtc_is_pal = gpu_display_is_pal() ? 1 : 0;
        if (g_host_refresh_hz > 0.0)
            std::printf("psxrecomp: host panel %.0f Hz; cadence pending guest "
                        "GP1(08h) display mode\n", g_host_refresh_hz);
        else
            std::printf("psxrecomp: host refresh unknown; cadence pending guest "
                        "GP1(08h) display mode\n");
    }

    /* OpenGL backend: create the GL context now. On failure, relabel the
     * facade back to software (rasterization already runs through software in
     * this phase) and fall through to the SDL_Renderer present path below. */
    if (g_video_renderer == 1) {
        gl_renderer_set_swap_interval(present_effective_swap_interval()); /* applied at context init */
        g_gl_active = (gl_renderer_init_context(sdl_window) != 0);

    /* Bezel artwork ([video] bezel): a still image behind the frame that fills
     * the letterbox/pillarbox margins. Loaded here because the GL context now
     * exists and stb_image is already linked for the HD texture pack. A
     * relative path resolves against the disc directory, so a pack of team
     * wallpapers can sit beside the disc like the texture pack does. */
    if (!g_bezel_path.empty() && g_bezel_path != "off" && g_gl_active) {
        /* Resolve the setting to a file.
         *
         *   "random"      one of the bundled team logos, chosen ONCE per
         *                 launch and kept for the whole session -- a mark that
         *                 changed mid-run would read as a glitch;
         *   "qirex" etc.  that team;
         *   anything else a path, relative to the disc directory.
         *
         * Bundled art lives next to the executable so a stock install has it
         * without touching the disc folder. */
        std::filesystem::path bp;
        const std::filesystem::path bez_dir =
            exe_dir_from_argv(argv[0]) / "bezels";
        if (g_bezel_path == "random") {
            std::vector<std::filesystem::path> pool;
            std::error_code bec;
            for (const auto &e : std::filesystem::directory_iterator(bez_dir, bec)) {
                if (bec) break;
                if (e.is_regular_file(bec) && e.path().extension() == ".png")
                    pool.push_back(e.path());
            }
            std::sort(pool.begin(), pool.end());   /* directory order is not defined */
            if (!pool.empty()) {
                std::random_device rd;
                bp = pool[rd() % pool.size()];
            }
        } else {
            bp = std::filesystem::path(g_bezel_path);
            if (!bp.has_extension()) {             /* a team name */
                std::filesystem::path named = bez_dir / (g_bezel_path + ".png");
                std::error_code nec;
                if (std::filesystem::exists(named, nec)) bp = named;
            }
            if (bp.is_relative()) bp = resolved_disc.parent_path() / bp;
        }
        std::vector<unsigned char> file;
        if (FILE *bf = std::fopen(bp.string().c_str(), "rb")) {
            std::fseek(bf, 0, SEEK_END);
            const long len = std::ftell(bf);
            std::fseek(bf, 0, SEEK_SET);
            if (len > 0) {
                file.resize((size_t)len);
                if (std::fread(file.data(), 1, file.size(), bf) != file.size())
                    file.clear();
            }
            std::fclose(bf);
        }
        int bw = 0, bh = 0, bc = 0;
        unsigned char *px = file.empty() ? nullptr
            : stbi_load_from_memory(file.data(), (int)file.size(), &bw, &bh, &bc, 4);
        if (px) {
            gl_renderer_set_bezel(px, bw, bh);
            stbi_image_free(px);
            std::fprintf(stdout, "psxrecomp: bezel artwork %dx%d from %s\n",
                         bw, bh, bp.string().c_str());
        } else {
            std::fprintf(stdout, "psxrecomp: bezel artwork not loaded: %s\n",
                         bp.string().c_str());
        }
    }
        if (!g_gl_active) {
            gr_set_backend(GR_BACKEND_SOFTWARE);
            gl_renderer_set_cpu_auth_dual(0);
            g_gl_fbo_present = 0;
            s_netplay_gl_present = 0;
            if (net_cfg.enabled) {
                gr_set_scale(1);
                s_netplay_sim_native_scale = 1;
            }
        } else if (net_cfg.enabled || s_netplay_gl_present) {
            /* Dual-raster: FBO present at settings SSAA; SW@1× authority.
             * Scale was applied via s_req_scale before init_gpu_raster. */
            gl_renderer_set_cpu_auth_dual(1);
            g_gl_fbo_present = 1;
            s_netplay_gl_present = 1;
            s_netplay_sim_native_scale = 1;
        }
        /* The GL backend establishes its real internal scale HERE (raster init),
         * which is AFTER the earlier offline `g_video_scale = gr_scale()` sync.
         * Re-sync offline so staging matches. Netplay keeps g_video_scale as
         * the settings preference (equals gr_scale() under dual-raster). */
        if (!netplay_cpu_auth_gpu())
            g_video_scale = gr_scale();
        gl_renderer_set_interpolation(g_frame_interpolation, g_host_refresh_hz,
                                      (double)g_frame_interpolation_fps,
                                      /*blend_mode*/ 0);
    }
    /* Vulkan backend: create the instance/device/swapchain on the
     * SDL_WINDOW_VULKAN window. On failure, fall back to software (vkb_init
     * already initialized the software renderer on the shared VRAM array). */
    if (g_video_renderer == 2) {
        vk_renderer_set_present_mode(present_effective_swap_interval());
        g_vk_active = (vk_renderer_init_context(sdl_window) != 0);
        if (!g_vk_active) gr_set_backend(GR_BACKEND_SOFTWARE);
        if (!netplay_cpu_auth_gpu())
            g_video_scale = gr_scale();
    }
    latency_ring_set_backend(g_vk_active ? "vulkan" : g_gl_active ? "opengl" : "software");
    latency_ring_set_present_mode(present_effective_swap_interval());
    /* Title bar shows the clean game title (set at window creation); the active
     * renderer is reported via the debug server / config, not appended here. */

    /* Force OpenGL renderer.
     *
     * History (TombaRecomp/ISSUES.md #6):
     *   1. Originally SDL_RENDERER_ACCELERATED with fallback to software.
     *      Froze "Not Responding" after extended uptime — was thought to
     *      be GPU-driver-side hangs.
     *   2. Switched to SDL_RENDERER_SOFTWARE only. Still froze. Software
     *      renderer goes through Windows GDI; the GDI path hangs the SDL
     *      main thread under heavy emulation load.
     *   3. Bisection: NO_AUDIO+NO_RENDER ran indefinitely (~7+ min, 40k+
     *      frames) but the game never progressed past BIOS boot because
     *      it depends on the renderer being present. NO_AUDIO alone with
     *      software renderer froze at frame 3084 — same as full debug
     *      build. So audio is innocent; software renderer (GDI path) is
     *      the culprit.
     *   4. SDL_HINT_RENDER_DRIVER=opengl + SDL_RENDERER_ACCELERATED.
     *      Ran indefinitely past every prior freeze point. OpenGL driver
     *      uses a different presentation path that doesn't hit the GDI
     *      hang. This is now the default.
     *
     * Note: the freeze became prevalent only after the FMV-speed fix
     * (commit b486c13) raised cycle throughput. Before that, the slower
     * MDEC/DMA workload was below whatever GDI threshold trips the bug. */
    /* The OpenGL force above is a Windows-only workaround for the GDI
     * presentation hang; macOS/Linux have no GDI path, so let SDL choose its
     * native backend (Metal on Apple Silicon). PRESENTVSYNC removes tearing;
     * fall back progressively if a driver can't provide vsync/accel. */
  if (!g_gl_active && !g_vk_active) {
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
    /* PRESENTVSYNC only when driver vsync owns cadence; otherwise the
     * wall-clock pacer holds 59.94Hz (may tear). */
    Uint32 rflags = SDL_RENDERER_ACCELERATED |
                    (present_effective_swap_interval() != 0
                         ? SDL_RENDERER_PRESENTVSYNC : 0u);
    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, rflags);
    if (!sdl_renderer)
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
    if (!sdl_renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Present in a logical space of the configured aspect (640x480 at native
     * 4:3, wider at widescreen aspects; scaled by the supersampling factor so
     * the full internal resolution reaches a large/fullscreen window; SDL
     * still scales and letterboxes to the real output). Identity in the
     * default window when supersampling is off, so native rendering is
     * unchanged. Netplay CPU-auth: always 1× (sim has no hi-res mirror). */
    {
        const int tex_scale = netplay_cpu_auth_gpu() ? 1 : g_video_scale;
        g_logical_w = 480 * g_video_aspect_num * tex_scale / g_video_aspect_den;
        SDL_RenderSetLogicalSize(sdl_renderer, g_logical_w, 480 * tex_scale);
    }
  }

    /* Staging buffer + backing texture are sized for the internal resolution
     * (640x512 native, times the supersampling factor). Netplay: 1×. */
    {
        const int tex_scale = netplay_cpu_auth_gpu() ? 1 : g_video_scale;
        sdl_pixel_buf = (uint32_t*)std::malloc(
            (size_t)640 * tex_scale * 512 * tex_scale * sizeof(uint32_t));
        if (!sdl_pixel_buf) {
            std::fprintf(stderr, "failed to allocate %dx staging buffer\n", tex_scale);
            return 1;
        }
    }

  if (!g_gl_active && !g_vk_active) {
    const int tex_scale = netplay_cpu_auth_gpu() ? 1 : g_video_scale;
    sdl_texture = SDL_CreateTexture(
        sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        640 * tex_scale, 512 * tex_scale
    );
    if (!sdl_texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureScaleMode(sdl_texture,
                            g_video_aa ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
  }
  }

    /* Register vblank presentation callback. */
    gpu_set_vblank_callback(sdl_vblank_present);

    /* Delay-sync LAN (recomp-net). Menu/lobby UI is later work — CLI/env only.
     * Must start after SDL so local pad capture has devices; before the guest
     * fiber so the first vblanks already pump HELLO/START. */
    if (net_cfg.enabled) {
        /* Refuse mismatched / incomplete mounts before opening transport.
         * Launcher already gates create/join; this covers CLI --netplay and
         * any path that skipped the UI verify. */
        {
            const std::filesystem::path disc_check = resolved_disc;
            if (!disc_check.empty()) {
#if defined(RECOMP_LAUNCHER)
                const std::string& expect_serial = g_lnch_expected_serial;
                const uint32_t expect_crc = g_lnch_expected_crc;
                const bool has_crc = g_lnch_has_crc;
#else
                const std::string expect_serial = game_id;
                const uint32_t expect_crc = game_disc_crc;
                const bool has_crc = game_has_disc_crc;
#endif
                PSXRecompV4::DiscIdentity nid = PSXRecompV4::identify_disc(
                    disc_check, expect_serial, expect_crc, has_crc,
                    /*compute_crc*/ false, &g_netplay_disc_expect);
                g_session_disc_fp = nid.disc_fp;
                g_session_netplay_disc_ok = nid.netplay_ok && !nid.disc_fp.empty();
                psx_lobby_set_disc_fp(nid.disc_fp.c_str());
                if (!g_session_netplay_disc_ok) {
                    std::fprintf(stderr,
                        "psxrecomp: netplay refused — disc mount not valid for "
                        "online (%s)\n",
                        nid.netplay_detail.empty()
                            ? "TOC fingerprint missing or policy failed"
                            : nid.netplay_detail.c_str());
                    return 1;
                }
            } else if (!g_session_netplay_disc_ok || g_session_disc_fp.empty()) {
                std::fprintf(stderr,
                    "psxrecomp: netplay refused — no verified disc TOC "
                    "fingerprint (mount the supported .cue dump)\n");
                return 1;
            }
        }
        /* Transport role and gameplay slot are independent. An empty peer
         * means this process listens for the first peer, even when the lobby
         * owner was reordered into gameplay slot 1 (Player 2). */
        if (!net_cfg.bind_hostport[0]) {
            std::fprintf(stderr,
                "psxrecomp: netplay refused — empty bind "
                "(bind='%s' peer='%s' session=%u)\n",
                net_cfg.bind_hostport, net_cfg.peer_hostport,
                (unsigned)net_cfg.session_id);
            return 1;
        }
        /* Resolve which host PlayerInput feeds this peer's net sample.
         * Auto (-1): always prefer dashboard P1 ("PLAYER N / NETPLAY") — that
         * pad is published as lobby local_slot. Seat-card P2/P3… are only used
         * when P1 is empty (legacy same-PC layout). Else sole assigned / P1. */
        if (net_cfg.input_player < 0 || net_cfg.input_player >= PSX_MAX_PLAYERS) {
            const int seat = net_cfg.local_slot;
            int sole = -1, n_assigned = 0;
            for (int i = 0; i < PSX_MAX_PLAYERS; ++i) {
                if (g_players[i].kind == 0) continue;
                ++n_assigned;
                sole = i;
            }
            if (g_players[0].kind != 0)
                net_cfg.input_player = 0;
            else if (seat >= 0 && seat < PSX_MAX_PLAYERS && g_players[seat].kind != 0)
                net_cfg.input_player = seat;
            else if (n_assigned == 1)
                net_cfg.input_player = sole;
            else
                net_cfg.input_player = 0;
        }
        if (net_cfg.slot_count < 2)
            net_cfg.slot_count = game_players >= 2 ? game_players : 2;
        if (net_cfg.slot_count > PSX_MAX_PLAYERS)
            net_cfg.slot_count = PSX_MAX_PLAYERS;
        s_netplay_present_sim_watermark = 0; /* §74: sim restarts per session */
        const int nrc = psx_netplay_start(&net_cfg);
        if (nrc != 0) {
            std::fprintf(stderr,
                "psxrecomp: netplay start failed (%d) — built without recomp-net, "
                "or bind/peer invalid (slot=%d bind=%s peer=%s)\n",
                nrc, net_cfg.local_slot, net_cfg.bind_hostport, net_cfg.peer_hostport);
            return 1;
        }
        apply_netplay_local_viewport_aspect(net_cfg.enabled);
        std::printf("psxrecomp: netplay transport=%s slot=%d input_player=%d delay=%d "
                    "force_turn=%d bind=%s peer=%s session=%u\n",
                    psx_netplay_transport_name(),
                    net_cfg.local_slot, net_cfg.input_player, net_cfg.input_delay,
                    net_cfg.force_turn ? 1 : 0,
                    net_cfg.bind_hostport,
                    (std::strcmp(psx_netplay_transport_name(), "ice") == 0)
                        ? "(ice)" : net_cfg.peer_hostport,
                    (unsigned)net_cfg.session_id);
    }

    /* Initialize CPU state. */
    CPUState cpu;
    std::memset(&cpu, 0, sizeof(cpu));
    /* R3000A load-delay interlock init (Beetle: BACKED_LDWhich=0x20 = no pending
     * load; ReadFudge=0 so the first load gets no fudge). Rest is correctly 0. */
    cpu.ld_which_t = 0x20;
    psx_icache_reset();   /* all I-cache lines cold at reset */

    /* Wire memory function pointers. Guest data loads now route their CPU cycle
     * cost through the faithful load-delay interlock (psx_cyc_load_* in memory.c),
     * so these VALUE accessors are the UNCHARGED psx_read_* — the data-access
     * wait-state is charged once, inside the interlock, never here. */
    cpu.read_word  = psx_read_word;
    cpu.write_word = psx_write_word;
    cpu.read_half  = psx_read_half;
    cpu.write_half = psx_write_half;
    cpu.read_byte  = psx_read_byte;
    cpu.write_byte = psx_write_byte;
    /* (sljit removed 2026-07-15: overlay_sljit_init_helpers wired the JIT-shard
     * host-helper table into cpu here; no shards consume it now.) */

    /* BIOS backend select (CLAUDE.md §0 amendment 2026-07-02): LLE (the
     * recompiled BIOS — default, reference implementation, oracle) vs the
     * opt-in HLE tier (implemented kernel services computed in-runtime, LLE
     * fallback for everything else). Boot always starts at the real reset
     * vector; with the HLE boot-skip on, the tier's dispatch hook intercepts
     * the shell entry one-shot (the boot animation is skipped; kernel init +
     * game EXE load still run in the real recompiled BIOS, unpaced) — this
     * REPLACES the old fast_boot snapshot restore, and fast_boot=true now
     * aliases the boot-skip alone. Env overrides: PSX_BIOS_HLE /
     * PSX_BIOS_HLE_KEEP_INTRO ('0' = off, anything else = on). */
    {
        if (const char* e = std::getenv("PSX_BIOS_HLE"))
            bios_hle = (e[0] && e[0] != '0');
        if (const char* e = std::getenv("PSX_BIOS_HLE_KEEP_INTRO"))
            bios_hle_keep_intro = (e[0] && e[0] != '0');
        /* The two axes (kernel-call HLE, boot-skip) have DIFFERENT per-image
         * requirements, so they are decided in ONE pure place —
         * psx_bios_hle_plan(), runtime/src/bios_hle_plan.c. Conflating them
         * broke boot-skip on OpenBIOS: call-HLE needs deliver_event_ret, the
         * boot-skip needs only shell_entry_phys, so refusing the former must
         * not silently cancel the latter. */
        PsxBiosHleRequest req;
        req.bios_hle               = bios_hle ? 1 : 0;
        req.keep_intro             = bios_hle_keep_intro ? 1 : 0;
        req.fast_boot              = fast_boot ? 1 : 0;
        req.have_deliver_event_ret = (psx_bios_image.deliver_event_ret != 0);
        req.have_shell_entry       = (psx_bios_image.shell_entry_phys != 0);
        req.have_game_entry        = (game_entry_pc != 0);
        const PsxBiosHlePlan plan = psx_bios_hle_plan(req);

        /* Call-HLE is a per-image capability, not just a preference: an image
         * exporting no DeliverEvent anchor (OpenBIOS until validated) declares
         * that axis STRUCTURALLY UNAVAILABLE — servicing B0 events with
         * mismatched semantics wedges the guest in event waits. */
        if (plan.call_hle_denied) {
            std::fprintf(stdout,
                "psxrecomp: bios_hle kernel-call tier unavailable on %s (no "
                "DeliverEvent anchor); kernel calls stay LLE\n",
                psx_bios_image.image_id);
        }
        if (plan.boot_skip_denied) {
            std::fprintf(stdout,
                "psxrecomp: BIOS boot-skip unavailable on %s (no shell entry "
                "anchor); playing the real intro\n",
                psx_bios_image.image_id);
        }
        psx_bios_hle_configure(plan.call_hle, plan.boot_skip);
        std::fprintf(stdout,
                     "psxrecomp: bios_backend=%s  bios_boot=%s  image=%s\n",
                     psx_bios_hle_backend_name(),
                     psx_bios_hle_boot_skip_enabled()
                         ? "HLE (shell skipped)" : "LLE (real intro)",
                     psx_bios_image.image_id ? psx_bios_image.image_id : "?");
    }

    /* R3000A reset state. */
    cpu.pc = 0xBFC00000u;
    cpu.cop0[12] = 0x00400000u; /* SR: BEV=1 (boot exception vectors) */

    /* Host hotkeys: config.ini [KeyMap] next to the exe (VolumeUp/Down).
     * Prefer exe-dir file; if missing, fall back to cwd config.ini so a
     * launcher edit made before config_path was wired still applies. */
    {
        namespace fs = std::filesystem;
        const fs::path exe_cfg = exe_dir_from_argv(argv[0]) / "config.ini";
        std::error_code ec;
        if (fs::is_regular_file(exe_cfg, ec)) {
            host_keymap_load(exe_cfg.string().c_str());
        } else if (fs::is_regular_file(fs::path("config.ini"), ec)) {
            host_keymap_load("config.ini");
        } else {
            host_keymap_load(nullptr);
        }
    }

    /* User save states: slots live under
     * <memcard>/<openbios|scph1001>/, keyed by entry_pc + boot_state integrity
     * (codegen hash/abi/ver). Memory cards stay in the memcard root. */
    if (game_entry_pc != 0) {
        const char* bios_token =
            psx_bios_image.image_bundled ? "openbios" : "scph1001";
        uint32_t openbios_ws = 0;
        if (const PsxBiosBackend* bundled = psx_bios_bundled()) {
            if (bundled->image)
                openbios_ws = bundled->image->image_wordsum;
        }
        /* savestate_dir, not memcard_dir: sibling builds of one game may share
         * memory cards while keeping slot .pst files apart (see [savestate]
         * dir). Defaults to memcard_dir when unset. */
        savestate_configure(savestate_dir.string().c_str(),
                            memory_get_bios_checksum(), game_entry_pc,
                            bios_token, openbios_ws);
        psx_rewind_set_depth((uint32_t)g_rewind_depth);
        psx_rewind_set_interval((uint32_t)g_rewind_interval);
        psx_rewind_configure(memory_get_bios_checksum(), game_entry_pc);
        /* Headless / agent load: PSX_LOAD_SLOT=N stages slot load (0..11)
         * at the next safe block boundary after boot. */
        if (const char *ls = std::getenv("PSX_LOAD_SLOT")) {
            int slot = atoi(ls);
            if (slot >= 0 && slot < 12) {
                if (psx_netplay_active()) {
                    std::fprintf(stdout,
                        "psxrecomp: PSX_LOAD_SLOT=%d ignored (use the save-state menu after netplay starts)\n",
                        slot);
                } else if (savestate_request_load(slot)) {
                    std::fprintf(stdout, "psxrecomp: PSX_LOAD_SLOT=%d staged\n", slot);
                }
            }
        }
    }
    /* Netplay: refresh RB bios/entry after savestate_configure (start() ran
     * earlier with zeros); guest also sandboxes .pst/.mcd under saves/netplay/. */
    psx_netplay_bind_guest_saves();

    /* Let memory subsystem see SR for cache-isolation checks. */
    memory_set_sr_ptr(&cpu.cop0[12]);

    /* Wire the CAUSE.IP2 mirror. IP2 is combinational on real hardware and has
     * to track (I_STAT & I_MASK) through raises, acks and mask writes; this
     * hands interrupts.c the one location it is allowed to maintain. Also
     * performs the power-on recompute. */
    psx_irq_set_cause_ptr(&cpu.cop0[13]);

    /* Wire debug server to CPU state for register queries. */
    debug_server_set_cpu(&cpu);
    /* Master digests / FRAME_COMMIT for netplay hash_confirm watermark. */
    psx_netplay_bind_cpu(&cpu);
    /* Solo rollback resim self-check (PSX_RB_SELFCHECK=1, offline only). */
    psx_selfcheck_init(&cpu, memory_get_bios_checksum(), game_entry_pc);

    /* Execute. */
    std::fprintf(stdout, "psxrecomp runtime: executing from PC=0x%08X\n", cpu.pc);

#if defined(PSX_ORACLE_BUILD)
    std::fprintf(stdout, "psxrecomp ORACLE: interpreter mode (port %d)\n", DEFAULT_DEBUG_PORT);
    interp_init(&cpu);
    interp_trace_enable(1);

    /* Breakpoints focused on VSync diagnostics.
     * Only break on the VSync incrementer — count how many times it's hit. */
    interp_break_add(0x8005A5BCu);  /* VSync counter incrementer — KSEG0 */

    std::fprintf(stderr, "ORACLE: running with BP at 0x8005A5BC (VSync incrementer)...\n");
    std::fflush(stderr);

    uint64_t total_executed = 0;
    const uint64_t max_total = 100000000ULL; /* 100M instructions */
    uint32_t vsync_hits = 0;
    for (;;) {
        uint32_t ran = interp_step(&cpu, 1000000);
        total_executed += ran;
        if (interp_hit_breakpoint()) {
            vsync_hits++;
            if (vsync_hits <= 3 || (vsync_hits % 100 == 0)) {
                std::fprintf(stderr, "ORACLE: VSync hit #%u at %llu instructions, ra=0x%08X, gte_exec=%llu\n",
                             vsync_hits, (unsigned long long)total_executed, cpu.gpr[31],
                             (unsigned long long)gte_get_exec_count());
                /* Dump last 20 trace entries for first 3 hits. */
                if (vsync_hits <= 3) {
                    uint64_t tseq = interp_trace_count();
                    uint32_t tavail = (tseq < 1048576ULL) ? (uint32_t)tseq : 1048576u;
                    uint32_t tstart = (tavail > 20) ? tavail - 20 : 0;
                    for (uint32_t ti = tstart; ti < tavail; ti++) {
                        const InterpTraceEntry* e = interp_trace_get(ti);
                        if (e) std::fprintf(stderr, "  [%llu] PC=0x%08X insn=0x%08X ra=0x%08X v0=0x%08X\n",
                                           (unsigned long long)e->seq, e->pc, e->insn,
                                           e->gpr[31], e->gpr[2]);
                    }
                }
                std::fflush(stderr);
            }
            /* Step past the breakpoint: temporarily remove, step 1, re-add. */
            uint32_t bp_pc = cpu.pc;
            interp_break_remove(bp_pc);
            interp_step(&cpu, 1);
            total_executed++;
            interp_break_add(bp_pc);
        }
        if (ran == 0) {
            std::fprintf(stderr, "ORACLE: halted at PC=0x%08X after %llu total instructions\n",
                         cpu.pc, (unsigned long long)total_executed);
            break;
        }
        if (total_executed >= max_total) {
            std::fprintf(stderr, "ORACLE: reached %llu instructions, stopping. PC=0x%08X\n",
                         (unsigned long long)total_executed, cpu.pc);
            /* Dump last 30 trace entries (ring-relative). */
            uint64_t tseq2 = interp_trace_count();
            uint32_t tavail2 = (tseq2 < 1048576ULL) ? (uint32_t)tseq2 : 1048576u;
            uint32_t tstart2 = (tavail2 > 30) ? tavail2 - 30 : 0;
            for (uint32_t ti = tstart2; ti < tavail2; ti++) {
                const InterpTraceEntry* e = interp_trace_get(ti);
                if (e) std::fprintf(stderr, "  [%llu] PC=0x%08X insn=0x%08X ra=0x%08X\n",
                                   (unsigned long long)e->seq, e->pc, e->insn, e->gpr[31]);
            }
            std::fflush(stderr);
            break;
        }
        /* Poll debug server. */
        debug_server_poll();
    }
    /* Read VSync counter from RAM. */
    uint32_t vsync_counter = cpu.read_word(0x80079D9Cu);
    uint32_t init_flag_48 = cpu.read_word(0x80079D48u);
    uint32_t init_flag_4C = cpu.read_word(0x80079D4Cu);
    std::fprintf(stderr, "ORACLE: VSync counter = 0x%08X (%u), init_flag@48 = 0x%08X, init_flag@4C = 0x%08X\n",
                 vsync_counter, vsync_counter, init_flag_48, init_flag_4C);
    std::fprintf(stderr, "ORACLE: VSync incrementer hit count = %u\n", vsync_hits);
    std::fflush(stderr);
#else
    /* Deterministic TCB-scheduler trampoline (carve-out): wraps the top-level
     * dispatch so a cooperative thread switch unwinds here and re-dispatches the
     * target thread, instead of the old per-frame host-fiber recreate. Returns
     * only on the abnormal top-level pc==0 exit; the diagnostic dump below runs
     * exactly as it did for the bare psx_dispatch. (HLE_SCHEDULER_CARVEOUT_PLAN.md)
     *
     * Hidden toggle: PSX_HLE_SCHEDULER=0 reverts to the legacy LLE host-fiber
     * bridge (default 1 = HLE). The trampoline is transparent in LLE mode (the
     * fiber path never longjmps to it). */
    std::fprintf(stdout, "psxrecomp: thread scheduler = %s (PSX_HLE_SCHEDULER)\n",
                 psx_hle_scheduler_enabled() ? "HLE (deterministic TCB)"
                                             : "LLE (host fibers)");
    std::fflush(stdout);

    /* General control-flow parity trace (parity_trace.h). Armed from boot via
     * PSX_PARITY_TRACE=1 so the pre-divergence window is always covered (never
     * arm-then-time). Watched TCB / freeze trigger / watch words are env-tunable;
     * defaults target the MMX6 cutscene→gameplay wedge (thread1 0xA000E35C,
     * trigger dispatch 0x800CD3F8, watch the handshake flag + state struct). */
    if (const char* pt = std::getenv("PSX_PARITY_TRACE"); pt && pt[0] && pt[0] != '0') {
        auto envhex = [](const char* k, uint32_t dflt) -> uint32_t {
            const char* v = std::getenv(k);
            return (v && v[0]) ? (uint32_t)std::strtoul(v, nullptr, 0) : dflt;
        };
        uint32_t tcb     = envhex("PSX_PARITY_TCB",     0xA000E35Cu);
        uint32_t trigger = envhex("PSX_PARITY_TRIGGER", 0x800CD3F8u);
        uint32_t watch[PARITY_WATCH_MAX] = {
            0x801FEB78u, /* thread1 stack saved-ra slot */
            0x8006D9ACu, /* handshake flag (setter target) */
            0x800CD3F8u, /* cutscene state struct: outer/sub */
            0x800CD3FCu, /* countdown */
            0x800CD404u, /* done flag region */
            0x800200F4u, /* func_8002000C resume point */
        };
        int wc = PARITY_WATCH_MAX;
        if (const char* wl = std::getenv("PSX_PARITY_WATCH"); wl && wl[0]) {
            wc = 0; std::string s(wl); size_t p = 0;
            while (p < s.size() && wc < PARITY_WATCH_MAX) {
                size_t c = s.find(',', p);
                std::string tok = s.substr(p, c == std::string::npos ? c : c - p);
                if (!tok.empty()) watch[wc++] = (uint32_t)std::strtoul(tok.c_str(), nullptr, 0);
                if (c == std::string::npos) break; p = c + 1;
            }
        }
        parity_trace_config(tcb, trigger, 0x88u, 0x00u, watch, wc);
        parity_trace_arm(1);
        std::fprintf(stdout, "psxrecomp: parity trace ARMED tcb=0x%08X trigger=0x%08X (%d watch)\n",
                     tcb, trigger, wc);
        std::fflush(stdout);
    }

    /* Device-event cycle ring (device_trace.h): armed from boot under the same
     * PSX_PARITY_TRACE gate, or PSX_DEVTRACE=1 standalone. Captures every
     * CD/DMA/timer/VBlank/SIO/SPU IRQ raise with its guest cycle for the
     * cross-process device-timing diff (tools/devtrace_diff.py). */
    {
        const char* pt = std::getenv("PSX_PARITY_TRACE");
        const char* dt = std::getenv("PSX_DEVTRACE");
        if ((pt && pt[0] && pt[0] != '0') || (dt && dt[0] && dt[0] != '0')) {
            device_trace_arm(1);
            std::fprintf(stdout, "psxrecomp: device-event trace ARMED\n");
            std::fflush(stdout);
        }
    }

    /* Delay-sync: do not free-run boot while HELLO/START is in flight.
     * Park until tick 0 pads are published, then enter the guest. */
    if (psx_netplay_active()) {
        SDL_PumpEvents();
        SDL_FlushEvent(SDL_QUIT);
        std::printf("psxrecomp: netplay waiting for peer START + tick-0 admit…\n");
        std::fflush(stdout);
        netplay_barrier_admit(-1);
        if (psx_return_to_lobby_requested())
            goto soft_return_lobby;
        netplay_host_present_uncap();
        /* Idle-skip advances guest cycles without CD/device aging in lockstep —
         * force off so both peers share the same CD FSM (dig_c folds CD). */
        {
            extern int g_idle_skip_enabled;
            if (g_idle_skip_enabled != 0) {
                g_idle_skip_enabled = 0;
                std::printf("psxrecomp: netplay — idle_skip forced off\n");
                std::fflush(stdout);
            }
        }
        g_auto_skip_fmv = 0;
        std::printf("psxrecomp: netplay lockstep armed (sim_tick=%u, vsync off)\n",
                    (unsigned)psx_netplay_sim_tick());
        std::fflush(stdout);
        /* Rematch: dump sticky guards once before guest entry (soft-exit can
         * leave device_service / call_unit elevated if a path skipped the
         * longjmp fixup). */
        if (rematch_session) {
            extern int psx_in_device_service;
            extern int g_call_unit_depth;
            char stall[96];
            uint32_t sim = 0;
            int lead = 0;
            stall[0] = '\0';
            psx_netplay_admit_wait_info(stall, sizeof(stall), &sim, &lead);
            std::fprintf(stderr,
                "psxrecomp: rematch enter-guest sim=%u stall=%s lead=%d "
                "device_svc=%d call_unit=%d cyc=%llu\n",
                (unsigned)sim, stall[0] ? stall : "-", lead,
                psx_in_device_service, g_call_unit_depth,
                (unsigned long long)psx_get_cycle_count());
            std::fflush(stderr);
        }
    }

    psx_scheduler_run(&cpu);
    if (psx_return_to_lobby_requested() && g_netplay_from_lobby)
        goto soft_return_lobby;
#endif

    /* If we reach here, all execution completed without MMIO abort.
     * During normal operation the guest runs an infinite main loop and the
     * top-level psx_dispatch NEVER returns. Reaching this point means the
     * outermost trampoline loop saw cpu->pc == 0 (some jr/tail-transfer
     * published a null PC) — an abnormal boot exit. Dump the always-on
     * fntrace ring tail (last dispatch chain) to a JSON artifact so we can
     * see exactly which targets led to the null PC. (CLAUDE.md ring-buffer
     * model: consume the always-on ring after the fact, not arm-and-time.) */
    {
        FILE* tf = std::fopen("psx_cps_exit_trace.json", "wb");
        if (tf) {
            std::fprintf(tf, "{\n  \"final_pc\": \"0x%08X\",\n  \"final_ra\": \"0x%08X\",\n"
                            "  \"final_sp\": \"0x%08X\",\n  \"fntrace_seq\": %llu,\n  \"tail\": [\n",
                         cpu.pc, cpu.gpr[31], cpu.gpr[29],
                         (unsigned long long)g_fntrace_seq);
            uint64_t seq = g_fntrace_seq;
            uint32_t n = seq < 128u ? (uint32_t)seq : 128u;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t idx = seq - n + i;
                const FntraceEntry* e = &g_fntrace_ring[idx % FNTRACE_RING_CAP];
                std::fprintf(tf,
                    "    {\"seq\":%llu,\"frame\":%u,\"target\":\"0x%08X\",\"ra\":\"0x%08X\","
                    "\"sp\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\"}%s\n",
                    (unsigned long long)idx, e->frame, e->target, e->ra, e->sp,
                    e->a0, e->a1, (i + 1 < n) ? "," : "");
            }
            std::fprintf(tf, "  ],\n  \"stack\": [\n");
            uint32_t sbase = (cpu.gpr[29] - 0x40u) & ~3u;
            for (int w = 0; w < 40; w++) {
                uint32_t a = sbase + (uint32_t)w * 4u;
                uint32_t v = cpu.read_word(a);
                std::fprintf(tf, "    {\"addr\":\"0x%08X\",\"val\":\"0x%08X\"}%s\n",
                             a, v, (w < 39) ? "," : "");
            }
            std::fprintf(tf, "  ]\n}\n");
            std::fclose(tf);
        }
    }

    /* Diagnostic: the guest published a null PC at the top level (abnormal). With
     * PSX_EXIT_HALT set, halt-and-serve here instead of shutting down so the
     * still-loaded overlays + full guest state are live-inspectable over TCP. */
    { const char *e = std::getenv("PSX_EXIT_HALT");
      if (e && e[0] && e[0] != '0') {
          extern void psx_fatal_halt(const char *reason);
          psx_fatal_halt("top-level dispatch returned PC=0 (abnormal boot exit — inspect live)");
      }
    }

    std::fprintf(stdout, "psxrecomp runtime: execution completed, PC=0x%08X\n", cpu.pc);
    /* The PC=0 top-level exit is an abnormal-boot class (see interrupts.c:
     * approximate exception-resume PCs severing the live native chain). The
     * registers at exit name the culprit without needing a debug-tools build:
     * ra = who returned/jumped here, epc = the last exception context. */
    {
        extern uint32_t g_debug_current_func_addr, g_debug_last_store_pc;
        std::fprintf(stdout,
                     "psxrecomp runtime: [exit regs] ra=0x%08X sp=0x%08X "
                     "epc=0x%08X sr=0x%08X func=0x%08X last_store_pc=0x%08X "
                     "v0=0x%08X a0=0x%08X t9=0x%08X\n",
                     cpu.gpr[31], cpu.gpr[29], cpu.cop0[14], cpu.cop0[12],
                     g_debug_current_func_addr, g_debug_last_store_pc,
                     cpu.gpr[2], cpu.gpr[4], cpu.gpr[25]);
    }
    { extern uint64_t g_slice_fired, g_slice_irq_taken, g_dirty_ram_insns_run;
      extern uint32_t g_slice_exit_pc, g_slice_exit_reason, g_slice_exit_iter;
      extern uint32_t g_slice_exit_dispatchable, g_slice_exit_dirty, g_slice_exit_in_text, g_slice_exit_want;
      std::fprintf(stdout, "psxrecomp runtime: [slice diag] slice_fired=%llu slice_irq_taken=%llu dirty_insns=%llu exit_pc=0x%08X reason=%u iter=%u dispatchable=%u dirty=%u in_text=%u want=%u\n",
                   (unsigned long long)g_slice_fired, (unsigned long long)g_slice_irq_taken,
                   (unsigned long long)g_dirty_ram_insns_run, g_slice_exit_pc, g_slice_exit_reason,
                   g_slice_exit_iter, g_slice_exit_dispatchable, g_slice_exit_dirty,
                   g_slice_exit_in_text, g_slice_exit_want); }
    { extern int g_pczero_latched; extern uint32_t g_pczero_addr, g_pczero_ra, g_pczero_in_exc;
      extern uint64_t g_pczero_count;
      std::fprintf(stdout, "psxrecomp runtime: [pczero tripwire] latched=%u count=%llu addr=0x%08X ra=0x%08X in_exc=%u\n",
                   g_pczero_latched, (unsigned long long)g_pczero_count,
                   g_pczero_addr, g_pczero_ra, g_pczero_in_exc); }
    /* pc=0 escape journal tail: names the exact runtime site that published
     * (or rescued) each recent null PC, with frame/depth/ra/epc context —
     * the play-build replacement for the debug-only fntrace ring, which is
     * compiled out here (the exit trace JSON was blind: fntrace_seq=0). */
    {
        uint64_t n = g_pc0j_seq < PSX_PC0J_CAP ? g_pc0j_seq : PSX_PC0J_CAP;
        std::fprintf(stdout, "psxrecomp runtime: [pc0 journal] %llu total, last %llu:\n",
                     (unsigned long long)g_pc0j_seq, (unsigned long long)n);
        for (uint64_t i = 0; i < n; i++) {
            const Pc0JournalEntry *e =
                &g_pc0j_ring[(g_pc0j_seq - n + i) % PSX_PC0J_CAP];
            std::fprintf(stdout,
                         "  seq=%llu site=%u frame=%u depth=%d a=0x%08X "
                         "ra=0x%08X epc=0x%08X d=0x%08X\n",
                         (unsigned long long)e->seq, e->site, e->frame,
                         e->depth, e->a, e->b, e->c, e->d);
        }
    }

    shutdown_runtime();
    if (g_gl_active) gl_renderer_shutdown();
    if (g_vk_active) vk_renderer_shutdown();
    SDL_DestroyTexture(sdl_texture);   /* NULL-safe in GL mode */
    SDL_DestroyRenderer(sdl_renderer); /* NULL-safe in GL mode */
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();

    return 0;

soft_return_lobby:
    /* Netplay soft-exit: tear down the match, keep the lobby seat, and reopen
     * the launcher on the LOBBY room so every peer can rematch. */
    teardown_game_session_keep_lobby();
#if defined(RECOMP_LAUNCHER) && defined(PSX_HAS_LOBBY_CLIENT)
    ae_np_prepare_lobby_rematch();
    {
        std::string assets_dir_str = exe_dir_from_argv(argv[0]).string();
        std::string rui_title = (game_name.empty() ? std::string("PSX") : game_name)
                                 + " - Launcher";
        std::string rui_initial_disc = disc_path_str;

        ae_rui_set_sidecar_paths(argv[0]);
        g_lnch_expected_serial = game_id;
        g_lnch_expected_crc = game_disc_crc;
        g_lnch_has_crc = game_has_disc_crc;
        g_lnch_argv0 = argv[0];
        g_lnch_netplay_game_name = game_name.empty() ? "PSX" : game_name;
        psx_lobby_set_max_slots(game_players);

        RecompLauncherCSettings ls{};
        ls.output_method = 2;
        ls.window_scale = std::max(1, std::min(4, g_video_win_w / 320));
        ls.fullscreen = g_fullscreen ? 1 : 0;
        ls.ignore_aspect = 0;
        ls.linear_filter = (g_video_texfilter != 0) ? 1 : 0;
        ls.widescreen =
            (g_video_aspect_num == 16 && g_video_aspect_den == 9) ? 1 : 0;
        ls.widescreen_hud = ls.widescreen;
        ls.enable_audio = 1;
        ls.audio_freq = g_audio_freq;
        ls.volume = host_volume_get();
        ls.window_width = g_video_win_w;
        ls.renderer = g_video_renderer;
        if (ls.renderer < 0 || ls.renderer > (vulkan_offered ? 2 : 1))
            ls.renderer = PSXRecompV4::DEFAULT_VIDEO_RENDERER;
        ls.supersampling = g_video_scale;
        ls.antialiasing = g_video_aa ? 1 : 0;
        ls.texture_filter = g_video_texfilter;
        ls.hdpack_enabled = g_hd_textures ? 1 : 0;
        std::snprintf(ls.hdpack_dir, sizeof(ls.hdpack_dir), "%s",
                      g_hd_texture_pack.c_str());
        ls.fmv_filter = cfg_fmv_filter_to_launcher(g_video_fmv_filter);
        ls.geometry_correction = g_video_geometry_correction ? 1 : 0;
        ls.perspective_texturing = g_video_perspective_texturing ? 1 : 0;
        ls.screen_kind = g_video_screen;
        ls.frame_interp = g_frame_interpolation ? 1 : 0;
        ls.frame_interp_fps = g_frame_interpolation_fps;
        ls.spu_hq = g_audio_spu_hq ? 1 : 0;
        ls.auto_skip_fmv = (skip_fmv_offered && g_auto_skip_fmv) ? 1 : 0;
        ls.turbo_loads = (turbo_loads_offered && g_turbo_loads_enabled) ? 1 : 0;
        ls.rewind_depth = g_rewind_depth;
        ls.rewind_interval = g_rewind_interval;
        ls.assist_pad_bind[PSX_ASSIST_BIND_REWIND] =
            normalize_hotkey_pad_binding(
                g_hotkey_pad_rewind,
                PSX_HOTKEY_PAD_SELECT_R3);
        ls.assist_pad_bind[PSX_ASSIST_BIND_SAVE_STATE_MENU] =
            normalize_hotkey_pad_binding(
                g_hotkey_pad_save_state_menu,
                PSX_HOTKEY_PAD_SELECT_R1);
        ls.aspect_index = (g_video_aspect_num * 9 == g_video_aspect_den * 21) ? 2
            : (g_video_aspect_num == 16 && g_video_aspect_den == 9) ? 1 : 0;
        ls.language_index = 0;
        for (size_t li = 0; li < lang_menu_options.size(); li++) {
            if (lang_menu_options[li].code == resolved_language) {
                ls.language_index = (int)li;
                break;
            }
        }
        std::snprintf(ls.netplay_player_name, sizeof(ls.netplay_player_name), "%s",
                      psx_lobby_display_name());
        /* Soft-return shows the durable preference (bios.cfg), not the match
         * session BIOS that may have temporarily overridden boot. */
        {
            std::filesystem::path preferred = read_cached_path(argv[0], "bios.cfg");
            ls.bios_path[0] = '\0';
            if (!preferred.empty()) {
                std::filesystem::path resolved =
                    resolve_bios_path(preferred.string().c_str(), argv[0]);
                std::error_code ec;
                if (!resolved.empty() && std::filesystem::exists(resolved, ec)) {
                    auto abs = std::filesystem::weakly_canonical(resolved, ec);
                    if (ec) abs = std::filesystem::absolute(resolved, ec);
                    std::snprintf(ls.bios_path, sizeof(ls.bios_path), "%s",
                                  abs.string().c_str());
                } else {
                    std::snprintf(ls.bios_path, sizeof(ls.bios_path), "%s",
                                  preferred.string().c_str());
                }
            }
        }
#if defined(RECOMP_LAUNCHER_HAS_MULTITAP_ENABLED)
        ls.multitap_enabled = multitap_enabled ? 1 : 0;
#endif
#if defined(RECOMP_LAUNCHER_HAS_MULTITAP_ANALOG)
        ls.multitap_analog = multitap_analog ? 1 : 0;
        g_lnch_multitap_analog = multitap_analog ? 1 : 0;
#endif
        /* Preserve input devices across soft-return. A zeroed ls left every
         * player_src at None, so the rematch UI (and a subsequent writeback)
         * looked like netplay had wiped the pad assignment. */
        {
            const int n = std::min(PSX_MAX_PLAYERS, RECOMP_LAUNCHER_MAX_PLAYERS);
            for (int i = 0; i < n; ++i) {
                const std::string& d = player_device[i];
                ls.player_src[i] =
                    PSXRecompV4::launcher_source_from_device(d);
                ls.deadzone[i] =
                    (player_deadzone[i] * 100 + 32767 / 2) / 32767;
                ls.pad_mode[i] = (ls.player_src[i] == 1)
                                    ? PSXRecompV4::PAD_MODE_DIGITAL
                                    : player_mode[i];
                ls.player_gamepad_guid[i][0] = '\0';
                if (ls.player_src[i] == 2 && !d.empty() && d != "auto" &&
                    d != "gamepad" && d != "controller") {
                    std::snprintf(ls.player_gamepad_guid[i],
                                  sizeof(ls.player_gamepad_guid[i]), "%s",
                                  d.c_str());
                }
            }
        }

        const std::string rui_region =
            !game_region.empty() ? game_region : region_label_from_serial(game_id);
        std::vector<const char*> rui_lang_labels;
        rui_lang_labels.reserve(lang_menu_options.size());
        for (const auto& lo : lang_menu_options)
            rui_lang_labels.push_back(lo.label.c_str());

        RecompLauncherCGameInfo gi{};
        ae_fill_psx_launcher_game_info(
            &gi,
            game_name.empty() ? nullptr : game_name.c_str(),
            rui_region.empty() ? nullptr : rui_region.c_str(),
            game_players,
            ws_offered,
            ws_ultrawide_offered,
            skip_fmv_offered,
            turbo_loads_offered,
            vulkan_offered,
            ctrl_lock_mode,
            ctrl_lock_device,
            ctrl_locked_mode[0],
            rui_lang_labels.empty() ? nullptr : rui_lang_labels.data(),
            (int)rui_lang_labels.size(),
            /*resume_netplay_room=*/1);
#if defined(PSX_HAS_SETUP_WIZARD) && defined(PSX_HAS_GAME_CODEGEN)
        psx_game_codegen_setup_apply(&gi);
#endif

        char rui_out_disc[1024] = {0};
        recomp_launcher_set_preserve_sdl(1);
        const int rui_rc = recomp_launcher_run_window(
            rui_title.c_str(), &ls, &gi, assets_dir_str.c_str(),
            rui_initial_disc.c_str(), rui_out_disc, sizeof(rui_out_disc));

        if (rui_rc == 1) {
            /* User closed the launcher — leave the lobby and exit. */
            if (g_lnch_netplay_callbacks.leave)
                (void)g_lnch_netplay_callbacks.leave(g_lnch_netplay_callbacks.ctx);
            else
                (void)psx_lobby_leave();
            psx_lobby_disconnect();
            SDL_Quit();
            return 0;
        }
#if defined(PSX_HAS_GAME_CODEGEN)
        if (rui_rc == RECOMP_LAUNCHER_RESULT_RELAUNCH) {
            const char* disc_for_relaunch =
                rui_out_disc[0] ? rui_out_disc
                                : (rui_initial_disc.empty()
                                       ? ""
                                       : rui_initial_disc.c_str());
            std::fprintf(stdout,
                         "psxrecomp: relaunch after generate/rebuild\n");
            psx_game_codegen_relaunch_or_exit(disc_for_relaunch);
            SDL_Quit();
            return 1;
        }
#endif

        if (rui_rc == 0) {
            host_volume_set(ls.volume);
            if (rui_out_disc[0]) {
                resolved_disc = normalize_disc_path_for_launch(rui_out_disc);
                disc_path_str = resolved_disc.string();
            }
            if (ls.netplay_launch.enabled) {
                {
                    const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
                    if (caps && caps->valid) {
                        multitap_analog = caps->multitap_analog != 0;
                        g_lnch_multitap_analog = multitap_analog ? 1 : 0;
                        sio_set_multitap_analog(multitap_analog ? 1 : 0);
                    } else {
                        multitap_analog = ls.multitap_analog != 0;
                        g_lnch_multitap_analog = multitap_analog ? 1 : 0;
                        sio_set_multitap_analog(multitap_analog ? 1 : 0);
                    }
                }
                net_cfg = {};
                net_cfg.enabled = 1;
                net_cfg.local_slot = ls.netplay_launch.local_slot;
                net_cfg.input_player = ls.netplay_launch.input_player;
                net_cfg.session_id = ls.netplay_launch.session_id;
                net_cfg.input_delay = ls.netplay_launch.input_delay;
                net_cfg.input_prediction = ls.netplay_launch.input_prediction;
                net_cfg.force_input_relay = ls.netplay_launch.force_input_relay ? 1 : 0;
                net_cfg.force_turn = ls.netplay_launch.force_turn ? 1 : 0;
                net_cfg.rollback = ls.netplay_launch.rollback ? 1 : 0;
                net_cfg.player_count = ls.netplay_launch.player_count;
                net_cfg.slot_count = ae_np_session_slot_count(
                    ls.netplay_launch.player_count, ls.netplay_launch.max_slots,
                    ls.netplay_launch.local_slot, game_players);
                if (net_cfg.player_count <= 0)
                    net_cfg.player_count = net_cfg.slot_count;
                net_cfg.occupied_mask = ls.netplay_launch.occupied_mask;
                std::snprintf(net_cfg.bind_hostport, sizeof(net_cfg.bind_hostport), "%s",
                              ls.netplay_launch.bind_hostport);
                std::snprintf(net_cfg.peer_hostport, sizeof(net_cfg.peer_hostport), "%s",
                              ls.netplay_launch.peer_hostport);
                g_netplay_from_lobby = 1;
            } else {
                net_cfg = {};
                g_netplay_from_lobby = 0;
            }
            /* Same player_src → player_device writeback as the first launcher
             * exit path, so rematch honors (or keeps) the card selections. */
            {
                const int n = std::min(PSX_MAX_PLAYERS, RECOMP_LAUNCHER_MAX_PLAYERS);
                for (int i = 0; i < n; ++i) {
                    if (ls.player_src[i] == 1) {
                        player_device[i] = "keyboard";
                        player_mode[i] = PSXRecompV4::PAD_MODE_DIGITAL;
                    } else if (ls.player_src[i] == 0) {
                        player_device[i] = "none";
                        player_mode[i] = ls.pad_mode[i];
                    } else if (ls.player_gamepad_guid[i][0]) {
                        player_device[i] = ls.player_gamepad_guid[i];
                        player_mode[i] = ls.pad_mode[i];
                    } else if (PSXRecompV4::launcher_source_from_device(
                                   player_device[i]) <= 1) {
                        player_device[i] = "gamepad";
                        player_mode[i] = ls.pad_mode[i];
                    } else {
                        player_mode[i] = ls.pad_mode[i];
                    }
                    player_deadzone[i] = ls.deadzone[i] * 32767 / 100;
                }
            }
            /* Persist controller (and rematch video) choices without wiping
             * the rest of settings.toml — merge into the on-disk file. */
            {
                const auto settings_path =
                    exe_dir_from_argv(argv[0]) / "settings.toml";
                PSXRecompV4::UserSettings us =
                    PSXRecompV4::load_user_settings(settings_path);
                const int un = std::min(PSX_MAX_PLAYERS,
                                        PSXRecompV4::UserSettings::kMaxControllerPlayers);
                for (int i = 0; i < un; ++i) {
                    us.p_device[i] = player_device[i];
                    us.has_p_device[i] = true;
                    us.p_mode[i] = player_mode[i];
                    us.has_p_mode[i] = true;
                    us.p_deadzone[i] = player_deadzone[i];
                    us.has_p_deadzone[i] = true;
                }
                us.deadzone = player_deadzone[0];
                us.has_deadzone = true;
#if defined(RECOMP_LAUNCHER_HAS_MULTITAP_ENABLED)
                multitap_enabled = ls.multitap_enabled != 0;
                us.multitap_enabled = multitap_enabled;
                us.has_multitap_enabled = true;
                apply_offline_pad_count(game_players, multitap_enabled);
#endif
#if defined(RECOMP_LAUNCHER_HAS_MULTITAP_ANALOG)
                multitap_analog = ls.multitap_analog != 0;
                us.multitap_analog = multitap_analog;
                us.has_multitap_analog = true;
                g_lnch_multitap_analog = multitap_analog ? 1 : 0;
                sio_set_multitap_analog(multitap_analog ? 1 : 0);
                if (game_config_path && game_config_path[0]) {
                    (void)PSXRecompV4::upsert_game_toml_controller_bool(
                        game_config_path, "multitap_analog", multitap_analog);
                }
#endif
                us.renderer = ls.renderer;
                us.has_renderer = true;
                us.supersampling = ls.supersampling;
                us.has_supersampling = true;
                us.antialiasing = ls.antialiasing != 0;
                us.has_antialiasing = true;
                us.texture_filter = ls.texture_filter;
                us.has_texture_filter = true;
                /* Only the toggle is user-editable here; hdpack_dir is shown
                 * read-only, so the launcher's pack choice is never clobbered. */
                g_hd_textures = ls.hdpack_enabled ? 1 : 0;
                us.hd_textures = ls.hdpack_enabled != 0;
                us.has_hd_textures = true;
                us.fmv_filter = launcher_fmv_filter_to_cfg(ls.fmv_filter);
                us.has_fmv_filter = true;
                us.geometry_correction = ls.geometry_correction != 0;
                us.has_geometry_correction = true;
                us.perspective_texturing = ls.perspective_texturing != 0;
                us.has_perspective_texturing = true;
                us.screen_kind = ls.screen_kind;
                us.has_screen_kind = true;
                us.frame_interpolation = ls.frame_interp != 0;
                us.has_frame_interpolation = true;
                us.frame_interpolation_fps = ls.frame_interp_fps;
                us.has_frame_interpolation_fps = true;
                us.audio_freq = ls.audio_freq;
                us.has_audio_freq = true;
                us.spu_hq = ls.spu_hq != 0;
                us.has_spu_hq = true;
                us.rewind_depth = ls.rewind_depth > 0 ? ls.rewind_depth : 50;
                us.has_rewind_depth = true;
                us.rewind_interval = ls.rewind_interval > 0 ? ls.rewind_interval : 15;
                us.has_rewind_interval = true;
                us.hotkey_pad_rewind = normalize_hotkey_pad_binding(
                    ls.assist_pad_bind[PSX_ASSIST_BIND_REWIND],
                    PSX_HOTKEY_PAD_SELECT_R3);
                us.has_hotkey_pad_rewind = true;
                us.hotkey_pad_save_state_menu = normalize_hotkey_pad_binding(
                    ls.assist_pad_bind[PSX_ASSIST_BIND_SAVE_STATE_MENU],
                    PSX_HOTKEY_PAD_SELECT_R1);
                us.has_hotkey_pad_save_state_menu = true;
                us.auto_skip_fmv = ls.auto_skip_fmv != 0;
                us.has_auto_skip_fmv = skip_fmv_offered;
                us.turbo_loads = ls.turbo_loads != 0;
                us.has_turbo_loads = turbo_loads_offered;
                us.fullscreen = ls.fullscreen != 0;
                us.has_fullscreen = true;
                us.window_width = ls.window_width > 0 ? ls.window_width : g_video_win_w;
                us.has_window_width = true;
                switch (ls.aspect_index) {
                    case 2:  us.aspect_num = 21; us.aspect_den = 9; break;
                    case 1:  us.aspect_num = 16; us.aspect_den = 9; break;
                    default: us.aspect_num = 4;  us.aspect_den = 3; break;
                }
                us.has_aspect_ratio = true;
                if (ls.bios_path[0]) {
                    us.bios_path = ls.bios_path;
                    us.has_bios_path = true;
                    write_cached_path(argv[0], "bios.cfg", us.bios_path);
                } else {
                    us.bios_path.clear();
                    us.has_bios_path = false;
                    write_cached_path(argv[0], "bios.cfg", {});
                }
                (void)PSXRecompV4::save_user_settings(settings_path, us);
            }
            g_video_renderer = ls.renderer;
            g_video_scale = ls.supersampling;
            g_video_aa = ls.antialiasing;
            g_video_texfilter = ls.texture_filter;
            g_video_fmv_filter = launcher_fmv_filter_to_cfg(ls.fmv_filter);
            g_video_geometry_correction = ls.geometry_correction ? 1 : 0;
            g_video_perspective_texturing = ls.perspective_texturing ? 1 : 0;
            g_video_screen = ls.screen_kind;
            /* Load acceleration and FMV skipping are mod-owned on PSX, and the
             * launcher struct these come from was snapshotted BEFORE
             * mod_runtime_activate_plugins() ran. Applying them here would
             * clobber whatever the Fast Loading / CD Speed / Tweaks plugins
             * decided with a stale pre-activation value — turning a player's
             * enabled mod silently back off on the first in-game Apply. The
             * offered flags are false for both, so leave both globals alone. */
            if (skip_fmv_offered)     g_auto_skip_fmv = ls.auto_skip_fmv ? 1 : 0;
            if (turbo_loads_offered)  g_turbo_loads_enabled = ls.turbo_loads ? 1 : 0;
            g_fullscreen = ls.fullscreen != 0;
            g_frame_interpolation = ls.frame_interp ? 1 : 0;
            g_frame_interpolation_fps = ls.frame_interp_fps;
            g_audio_freq = ls.audio_freq;
            g_audio_spu_hq = ls.spu_hq != 0;
            if (ls.rewind_depth > 0) {
                g_rewind_depth = ls.rewind_depth;
                psx_rewind_set_depth((uint32_t)g_rewind_depth);
            }
            if (ls.rewind_interval > 0) {
                g_rewind_interval = ls.rewind_interval;
                psx_rewind_set_interval((uint32_t)g_rewind_interval);
            }
            g_hotkey_pad_rewind = normalize_hotkey_pad_binding(
                ls.assist_pad_bind[PSX_ASSIST_BIND_REWIND],
                PSX_HOTKEY_PAD_SELECT_R3);
            g_hotkey_pad_save_state_menu = normalize_hotkey_pad_binding(
                ls.assist_pad_bind[PSX_ASSIST_BIND_SAVE_STATE_MENU],
                PSX_HOTKEY_PAD_SELECT_R1);
            switch (ls.aspect_index) {
                case 2:  g_video_aspect_num = 21; g_video_aspect_den = 9; break;
                case 1:  g_video_aspect_num = 16; g_video_aspect_den = 9; break;
                default: g_video_aspect_num = 4;  g_video_aspect_den = 3; break;
            }
            g_video_win_w = ls.window_width > 0 ? ls.window_width : g_video_win_w;
            /* Preference for persistence; session settle may override boot path. */
            if (ls.bios_path[0])
                bios_path_str = ls.bios_path;
            else {
                std::filesystem::path ob =
                    resolve_bios_path(PSX_BUNDLED_BIOS_PATH, argv[0]);
                bios_path_str = ob.empty() ? PSX_BUNDLED_BIOS_PATH : ob.string();
            }
            if (net_cfg.enabled) {
                const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
                if (caps && caps->valid && caps->session_bios[0])
                    ae_np_set_session_bios_token(caps->session_bios);
                std::filesystem::path session_path;
                if (g_lnch_session_bios[0] &&
                    resolve_match_session_bios_path(
                        g_lnch_session_bios,
                        ls.bios_path[0] ? std::filesystem::path(ls.bios_path)
                                        : std::filesystem::path{},
                        ls.bios_path, argv[0], &session_path)) {
                    if (std::strcmp(g_lnch_session_bios, "scph1001") == 0 &&
                        session_path.empty()) {
                        std::fprintf(stderr,
                            "psxrecomp: rematch session BIOS scph1001 required "
                            "but no validated dump — aborting\n");
                        SDL_Quit();
                        return 1;
                    }
                    if (session_path.empty()) {
                        std::filesystem::path ob =
                            resolve_bios_path(PSX_BUNDLED_BIOS_PATH, argv[0]);
                        bios_path_str =
                            ob.empty() ? PSX_BUNDLED_BIOS_PATH : ob.string();
                        std::fprintf(stdout,
                            "psxrecomp: rematch session BIOS = OpenBIOS "
                            "(preference unchanged)\n");
                    } else {
                        bios_path_str = session_path.string();
                        std::fprintf(stdout,
                            "psxrecomp: rematch session BIOS = SCPH-1001 (%s; "
                            "preference unchanged)\n",
                            bios_path_str.c_str());
                    }
                }
            }
            {
                std::string mod_error;
                if (net_cfg.enabled) {
                    if (!PSXRecompV4::mod_runtime_commit_for_netplay(resolved_disc,
                                                                    &mod_error)) {
                        std::fprintf(stderr,
                                     "psxrecomp: cannot apply netplay mods for "
                                     "rematch: %s\n",
                                     mod_error.c_str());
                        SDL_Quit();
                        return 1;
                    }
                } else if (!PSXRecompV4::mod_runtime_commit(resolved_disc,
                                                            &mod_error)) {
                    std::fprintf(stderr,
                                 "psxrecomp: cannot relaunch with selected "
                                 "mods: %s\n",
                                 mod_error.c_str());
                    SDL_Quit();
                    return 1;
                }
            }
            apply_netplay_local_viewport_aspect(net_cfg.enabled);
            std::printf("psxrecomp: rematch from lobby (netplay=%d)\n",
                        net_cfg.enabled ? 1 : 0);
            std::fflush(stdout);
            goto session_reboot;
        }
    }
#endif
    psx_lobby_disconnect();
    SDL_Quit();
    return 0;
}
