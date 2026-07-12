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
#include "boot_state.h"
#include "bios_hle.h"
#include "savestate.h"
#include "overlay_capture.h"
#include "overlay_loader.h"
#include "autocompile.h"
#include "code_provider.h"
#include "overlay_sljit.h"
#include "overlay_backend.h"
#include "gpu.h"
#include "present_ring.h"
#include "gpu_sw_renderer.h"
#include "gpu_render.h"
#include "gpu_gl_renderer.h"
#include "gpu_vk_renderer.h"
#include "frame_pacing.h"
#include "latency_ring.h"
#include "sio.h"
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
#include "game_options.h"
#include "crc32.h"
#include "disc_identity.h"
#include "iso_reader.h"      /* text-image guard: extract the boot EXE from the disc */
#include "psx_keybinds.h"    /* configurable keyboard->DualShock keybinds (keybinds.ini) */
#if defined(PSX_LAUNCHER)
#include "launcher.h"
#endif
#include <SDL.h>
#include <algorithm>
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
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#ifndef PSX_DEFAULT_BIOS_PATH
#define PSX_DEFAULT_BIOS_PATH "bios/SCPH1001.BIN"
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
    extern int      g_psx_cps_mode;
    extern uint64_t g_slice_fired, g_slice_irq_taken, g_dirty_ram_insns_run;
    extern uint32_t g_slice_exit_pc, g_slice_exit_reason, g_slice_exit_iter;
    extern uint32_t g_slice_exit_dispatchable, g_slice_exit_dirty, g_slice_exit_in_text, g_slice_exit_want;
}

/* memory.c */
extern "C" void     memory_init(const char* bios_path);
extern "C" void     memory_set_sr_ptr(const uint32_t *p);
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
static SDL_Window*   sdl_window;
static SDL_Renderer* sdl_renderer;
static SDL_Texture*  sdl_texture;
/* Per-player input device routing (PSX ports 1 & 2). Seeded from the
 * [controller] settings the launcher writes; the runtime opens the matching
 * SDL controller (or uses the keyboard) and feeds each PSX pad slot. */
struct PlayerInput {
    int   kind = 0;            /* 0=none, 1=keyboard, 2=controller */
    char  guid[40] = {0};      /* SDL joystick GUID string when kind==controller */
    /* Pad input mode (PSXRecompV4::PadMode): 0=hybrid (default), 1=analog,
     * 2=digital. hybrid_analog is the per-frame auto-switch latch used only in
     * hybrid mode: true => currently presenting DualShock (stick was the last
     * input), false => currently presenting a digital pad (D-pad was last). */
    int   mode = PSXRecompV4::PAD_MODE_HYBRID;
    bool  hybrid_analog = false;
    SDL_GameController* handle = nullptr;
    SDL_JoystickID      instance = -1;
};
static PlayerInput g_players[2];
/* ARGB8888 staging buffer. Sized for the active internal resolution:
 * 640*scale x 512*scale. Allocated once the supersampling scale is known
 * (sized for the native 640x512 when supersampling is off). */
static uint32_t*     sdl_pixel_buf = nullptr;

/* [video] options, resolved from the game config (defaults: native + AA). */
static int           g_video_scale = 1;     /* internal-resolution SSAA factor */
static bool          g_video_aa    = true;  /* linear present filtering */
static int           g_video_texfilter = 0; /* 0=nearest, 1=bilinear */
static int           g_video_renderer = 0;  /* 0=software, 1=opengl (requested) */
static int           g_fullscreen     = 0;  /* launch the game window in desktop fullscreen */
static int           g_video_screen   = 0;  /* 0=raw,1=crt,2=composite,3=trinitron */
static int           g_video_win_w    = 1280; /* window width (height follows aspect) */
static bool          g_audio_spu_hq   = false; /* SPU float-shadow (env overrides) */
static int           g_auto_skip_fmv  = 0;   /* skip FMVs the instant they're detected */
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
 * it trims the display-side scanout latency the CPU-side ring can't see. */
static int           g_low_latency_input = 1;
static int           g_video_vsync        = 1;
static int           g_frame_interpolation = 0;
static int           g_frame_interpolation_fps = 0;
static double        g_host_refresh_hz = 0.0;

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
/* [widescreen] per-game hooks (see config_loader.h): anchor scratch addr for
 * tagged sprite prims + HUD SPRT center-squash. Inert at 0/false. */
static uint32_t      g_ws_anchor_addr = 0;
static bool          g_ws_hud_sprt = false;
/* Runtime-only transition cleanup; kept out of gpu.h because generated game
 * units include that ABI header and do not need this frontend-only setter. */
extern "C" void gpu_ws_set_clear_reveal(int on);
/* Widescreen engages at game entry (fntrace_is_game_started): the BIOS boot
 * — Sony logo, PS logo, shell — presents authentic 4:3 with no GTE squash.
 * Starts true when the configured aspect is already 4:3 (nothing to engage). */
static bool          g_ws_engaged = true;
/* Wide-aspect strategy: native-wide (render the wider FOV into a wider frame,
 * present 1:1 — the GTE is NOT squashed) vs. the legacy squash hack. Default
 * native-wide; toggle live via the ws_nw TCP command for A/B comparison. */
static int           g_ws_native_wide = 1;
/* Logical present width for the SDL_Renderer (software) path; 640*scale at
 * 4:3, wider for wide aspects. Height is always 480*scale. Set at window
 * creation alongside SDL_RenderSetLogicalSize. */
static int           g_logical_w = 640;

/* Clamp a requested window width to the primary display's usable area so an
 * oversized choice (e.g. 1920 on a 1080p panel) still fits on screen. Keeps
 * the given aspect: height = width*den/num. */
static void clamp_window_aspect(int* w, int* h, int num, int den) {
    int width = *w;
    if (width < 640) width = 640;
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(0, &bounds) == 0 && bounds.w > 0 && bounds.h > 0) {
        if (width > bounds.w)             width = bounds.w;
        if (width * den / num > bounds.h) width = bounds.h * num / den;
    }
    *w = width;
    *h = width * den / num;
}

/* Live native-wide vs squash toggle (ws_nw TCP command) for A/B comparison.
 * Re-engages with the chosen mode in place if widescreen is already running. */
extern "C" void psx_ws_set_native_wide(int on) {
    g_ws_native_wide = on ? 1 : 0;
    if (g_ws_engaged && g_video_aspect_num * 3 != g_video_aspect_den * 4) {
        int mode = g_ws_native_wide ? 2 : 1;
        gte_set_display_aspect(mode == 1 ? g_video_aspect_num : 4,
                               mode == 1 ? g_video_aspect_den : 3);
        gpu_ws_configure(g_video_aspect_num, g_video_aspect_den,
                         g_ws_anchor_addr, g_ws_hud_sprt ? 1 : 0, mode);
    }
}
extern "C" int psx_ws_get_native_wide(void) { return g_ws_native_wide; }

static bool          g_gl_active = false;    /* GL context live -> GL present path */
static bool          g_vk_active = false;    /* Vulkan context live -> VK present path */
/* Present straight from the FBO (fast, no readback). Set PSX_GL_FORCE_CPU_PRESENT=1
 * to force the software readout path instead — a diagnostic/fallback that also
 * keeps CPU VRAM current every frame (so screenshots reflect the screen). */
static int           g_gl_fbo_present = 1;

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
}
/* Engage turbo only after a load has been continuously in progress for this many
 * frames. Filters brief incidental reads (e.g. a boss/stage-select screen that
 * streams XA music and reads a few preview sectors as you hover): each 1-2 frame
 * blip would otherwise flip turbo on, muting the music for the audio hangover and
 * stuttering the frame rate. Real loads hold for hundreds of frames, so they
 * lose only this brief authentic-paced prefix. */
#define TURBO_LOADS_ENGAGE_FRAMES 20
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
int g_audio_host_rate = 44100;

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
                              std::filesystem::path& out) {
    // Headless: never open an interactive file dialog (it blocks). Fail the
    // resolve so boot aborts cleanly with the stderr message the caller printed.
    if (g_headless) {
        std::fprintf(stderr, "psxrecomp: headless — cannot prompt for '%s'; "
                             "supply it via game.toml / --disc / --bios.\n", title);
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
    std::fprintf(stderr, "psxrecomp: %s requires a command-line path on this platform.\n", title);
    return false;
#endif
}

static std::string uppercase_ascii(std::string s) {
    for (char& c : s) {
        c = (char)std::toupper((unsigned char)c);
    }
    return s;
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

static bool validate_bios_for_launch(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamoff size = f.tellg();
    if (size != 512 * 1024) {
        launcher_warning("BIOS Warning",
            "The selected BIOS is not 512 KiB. Please select SCPH1001.BIN.");
        return false;
    }
    std::vector<uint8_t> data((size_t)size);
    if (!read_at(f, 0, data.data(), data.size())) return false;
    const uint32_t crc = crc32_compute(data.data(), data.size());
    if (crc != 0x37157331u) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "The selected BIOS CRC32 is %08X, but this build was validated with SCPH1001.BIN CRC32 37157331.\n\n"
            "The runtime will try it anyway, but boot may fail.", crc);
        launcher_warning("BIOS Warning", buf);
    }
    return true;
}

static std::filesystem::path resolve_bios_path(const char* requested, const char* argv0);

static std::filesystem::path resolve_bios_for_runtime(const char* requested,
                                                      const char* argv0) {
    std::filesystem::path resolved = resolve_bios_path(requested, argv0);
    if (!resolved.empty() && std::filesystem::exists(resolved) &&
        validate_bios_for_launch(resolved)) {
        return resolved;
    }

    std::filesystem::path cached = read_cached_path(argv0, "bios.cfg");
    if (!cached.empty() && std::filesystem::exists(cached) &&
        validate_bios_for_launch(cached)) {
        return cached;
    }

    /* Interactive pick. Be explicit about WHICH of the two user-supplied
     * files is being requested — first-time users see two pickers in a
     * row and the difference between "BIOS" and "disc image" is not
     * obvious to non-technical players. */
    launcher_info((s_picker_game_name + " — PlayStation BIOS needed").c_str(),
        s_picker_game_name + " does not include any Sony or game files.\n\n"
        "Step 1 of 2 — PlayStation BIOS\n\n"
        "In the next window, select your PlayStation BIOS dump. The file is "
        "usually named SCPH1001.BIN and is exactly 512 KB. You must dump it "
        "from your own console or otherwise legally obtain it.\n\n"
        "(This is NOT the game disc — that is asked for next.)");
    std::string bios_title =
        s_picker_game_name + " — Step 1 of 2: select PlayStation BIOS (SCPH1001.BIN)";
    for (;;) {
        std::filesystem::path picked;
        if (!pick_runtime_file(
                bios_title.c_str(),
                "PlayStation BIOS (*.bin)\0*.bin\0All Files (*.*)\0*.*\0",
                picked)) {
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
        std::filesystem::path p = std::filesystem::absolute(disc_override);
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
        if (std::filesystem::exists(cd) && validate_disc_for_launch(cd, game_id)) {
            return cd;
        }
    }

    std::filesystem::path cached = read_cached_path(argv0, "disc.cfg");
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
        ".bin, or .iso.\n\n"
        "(This is NOT the BIOS — the BIOS was already chosen.)");
    std::string disc_title =
        s_picker_game_name + " — Step 2 of 2: select " + s_picker_game_name +
        " disc image (.cue / .bin / .iso)";
    for (;;) {
        std::filesystem::path picked;
        if (!pick_runtime_file(
                disc_title.c_str(),
                "PS1 Disc Images (*.cue;*.bin;*.iso)\0*.cue;*.bin;*.iso\0All Files (*.*)\0*.*\0",
                picked)) {
            return {};
        }
        if (validate_disc_for_launch(picked, game_id)) {
            write_cached_path(argv0, "disc.cfg", picked);
            return picked;
        }
    }
}

static std::filesystem::path resolve_bios_path(const char* requested, const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(requested);
    if (fs::exists(p, ec)) {
        fs::path abs = fs::absolute(p, ec);
        return ec ? p : abs;
    }
    if (p.is_absolute()) return p;

    // Anchor on the exe directory — never cwd (see exe_dir_from_argv).
    fs::path found = find_upward(exe_dir_from_argv(argv0), p);
    if (!found.empty()) return found / p;
    return p;
}

// Fallback memcard directory used when no game config (or its [runtime]
// block) specifies one: the executable's directory (authoritative, never cwd —
// see exe_dir_from_argv), so saves always live next to the binary.
static std::filesystem::path default_memcard_dir(const char* argv0) {
    return exe_dir_from_argv(argv0);
}

static void close_controller(void);

extern "C" void overlay_compile_worker_stop(void);
static void shutdown_runtime(void) {
    overlay_compile_worker_stop();   /* signal + join the off-thread sljit worker */
    memcard_flush_all();
    overlay_capture_write_json();
    if (sdl_audio_device) {
        SDL_ClearQueuedAudio(sdl_audio_device);
        SDL_CloseAudioDevice(sdl_audio_device);   /* stops the pull callback */
        sdl_audio_device = 0;
    }
    if (s_drc_ready) { rab_free(&s_drc); s_drc_ready = false; }
    close_controller();
    debug_server_shutdown();
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
 * frames in one spu_render call. 1764 frames = 40 ms. */
static const int sdl_audio_fade_samples = 44100 * 40 / 1000;  /* 40 ms */
static int       sdl_audio_fadein_left  = 0;

static void sdl_audio_pump(void) {
    if (!sdl_audio_device) return;

    const uint32_t bytes_per_frame = sizeof(int16_t) * 2u;
    const bool legacy = audio_legacy_mode();
    static int had_audio = 0;
    uint32_t queued = 0;   /* RENDER event b: bytes (legacy) / fill ms (bridge) */
    if (legacy) {
        /* Historical push model + baseline measurement: a drained (==0) queue
         * means the device was silence-filling since the last pump = a gap.
         * Only meaningful after the first audio has been queued. */
        const uint32_t max_queue_bytes = 44100u * bytes_per_frame / 5u;
        queued = SDL_GetQueuedAudioSize(sdl_audio_device);
        if (queued == 0 && had_audio) {
            g_legacy_underruns++;
            audio_trace_event(AUDIO_EV_UNDERRUN, 0, 0);
        }
        if (queued > max_queue_bytes) {
            audio_trace_event(AUDIO_EV_PUMP_SKIP, queued, 0);
            return;
        }
    } else {
        /* No host-queue backpressure check: the bridge's ring + DRC hold the
         * fill near target, so we always render this frame and push it. */
        if (!s_drc_ready) return;
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
    if (last_cycles == 0) last_cycles = now_cycles;
    uint64_t delta = (now_cycles - last_cycles) + cycle_carry;
    last_cycles = now_cycles;
    int frames = (int)(delta / 768u);
    cycle_carry = delta % 768u;
    if (frames <= 0) return;
    if (frames > 2048) {
        /* A burst beyond one buffer (e.g. right after an unmute or a long
         * stall): render one full buffer and DROP the remainder of the debt —
         * same semantic as the mute model (voice positions freeze across the
         * gap) rather than time-compressing a backlog into garble. */
        frames = 2048;
        cycle_carry = 0;
    }

    audio_trace_event(AUDIO_EV_RENDER, (uint32_t)frames, queued);
    spu_render(sdl_audio_buf, frames);
    if (sdl_audio_fadein_left > 0) {
        const float g0 = 1.0f - (float)sdl_audio_fadein_left
                                / (float)sdl_audio_fade_samples;
        int ramp = sdl_audio_fadein_left < frames ? sdl_audio_fadein_left : frames;
        const float g1 = 1.0f - (float)(sdl_audio_fadein_left - ramp)
                                / (float)sdl_audio_fade_samples;
        sdl_audio_gain_ramp(sdl_audio_buf, ramp, g0, g1);
        sdl_audio_fadein_left -= ramp;
    }
    if (legacy) {
        /* T3 tap: the exact post-fade bytes handed to the host audio queue. */
        audio_trace_pcm(AUDIO_TAP_HOST, sdl_audio_buf, frames);
        SDL_QueueAudio(sdl_audio_device, sdl_audio_buf,
                       (uint32_t)frames * bytes_per_frame);
        had_audio = 1;
    } else {
        /* Hand to the bridge (band-limited resample + DRC) instead of
         * SDL_QueueAudio. Lock guards the SPSC ring against the pull callback.
         * The T3 tap moves to sdl_drc_callback: what the device actually
         * receives is the bridge's device-rate output, not this buffer. */
        SDL_LockAudioDevice(sdl_audio_device);
        rab_push(&s_drc, sdl_audio_buf, frames);
        SDL_UnlockAudioDevice(sdl_audio_device);
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
extern "C" int psx_audio_out_stats(double *fill_ms, uint64_t *underruns,
                                   uint64_t *overflow_drops, double *correction,
                                   int *legacy, int *host_rate)
{
    extern int g_audio_host_rate;   /* set at device open */
    *legacy = audio_legacy_mode() ? 1 : 0;
    *host_rate = g_audio_host_rate;
    if (*legacy || !s_drc_ready) {
        *fill_ms = sdl_audio_device
                   ? (double)SDL_GetQueuedAudioSize(sdl_audio_device)
                     / (44100.0 * 4.0) * 1000.0
                   : 0.0;
        *underruns = g_legacy_underruns;
        *overflow_drops = 0;
        *correction = 0.0;
        return sdl_audio_device != 0;
    }
    rab_stats st;
    rab_get_stats(&s_drc, &st);
    *fill_ms = st.last_fill_ms;
    *underruns = st.underrun_events;
    *overflow_drops = st.overflow_drops;
    *correction = st.last_correction;
    return 1;
}

/* Lightweight production-safe cadence probe. Unlike the TCP debug build this
 * adds no per-block recording; it is intended for long, representative attract
 * runs where instrumentation overhead would itself cause audio starvation. */
static void runtime_perf_diag_tick() {
    static int enabled = -1;
    static uint64_t last_counter = 0, last_frame = 0, last_spu = 0;
    static uint64_t last_underruns = 0, last_overflows = 0;
    static uint64_t last_up[6] = {0};
    static uint32_t last_overlay_loads = 0;
    static uint64_t last_overlay_load_us = 0;
    if (enabled < 0) {
        const char *e = std::getenv("PSX_RUNTIME_PERF_DIAG");
        enabled = e && e[0] && e[0] != '0';
    }
    if (!enabled) return;
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    extern uint64_t s_frame_count;
    AudioTraceStats audio;
    audio_trace_get_stats(&audio);
    double fill_ms = 0.0, correction = 0.0;
    uint64_t underruns = 0, overflows = 0;
    int legacy = 0, host_rate = 0;
    psx_audio_out_stats(&fill_ms, &underruns, &overflows, &correction,
                        &legacy, &host_rate);
    uint64_t up[6] = {0};
    gl_renderer_runtime_diag(up);
    uint32_t overlay_loads = 0;
    uint64_t overlay_load_us = 0, overlay_load_max_us = 0, overlay_load_last_us = 0;
    overlay_loader_get_counters(&overlay_loads, nullptr, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    overlay_loader_get_load_timing(&overlay_load_us, &overlay_load_max_us,
                                   &overlay_load_last_us);
    if (!last_counter) {
        last_counter = now; last_frame = s_frame_count;
        last_spu = audio.tap_frames[AUDIO_TAP_SPU_OUT];
        last_underruns = underruns; last_overflows = overflows;
        last_overlay_loads = overlay_loads;
        last_overlay_load_us = overlay_load_us;
        for (int i = 0; i < 6; i++) last_up[i] = up[i];
        return;
    }
    if (now - last_counter < freq * 5u) return;
    double dt = (double)(now - last_counter) / (double)freq;
    fprintf(stdout, "psxrecomp: runtime cadence: guest=%.2f Hz, spu=%.1f Hz, "
            "audio_fill=%.1f ms, underruns=+%llu, overflows=+%llu, corr=%+.5f; "
            "GL upload=%.1f calls/s %.1f rect/s %.2f Mpix/s, "
            "cpu=%.1f tex=%.1f draw=%.1f ms/s; overlay loads=+%u "
            "wall=%.1f ms max=%.1f last=%.1f ms\n",
            (double)(s_frame_count - last_frame) / dt,
            (double)(audio.tap_frames[AUDIO_TAP_SPU_OUT] - last_spu) / dt,
            fill_ms, (unsigned long long)(underruns - last_underruns),
            (unsigned long long)(overflows - last_overflows), correction,
            (double)(up[0] - last_up[0]) / dt,
            (double)(up[1] - last_up[1]) / dt,
            (double)(up[2] - last_up[2]) / dt / 1.0e6,
            (double)(up[3] - last_up[3]) * 1000.0 / (double)freq / dt,
            (double)(up[4] - last_up[4]) * 1000.0 / (double)freq / dt,
            (double)(up[5] - last_up[5]) * 1000.0 / (double)freq / dt,
            overlay_loads - last_overlay_loads,
            (double)(overlay_load_us - last_overlay_load_us) / 1000.0,
            (double)overlay_load_max_us / 1000.0,
            (double)overlay_load_last_us / 1000.0);
    fflush(stdout);
    last_counter = now; last_frame = s_frame_count;
    last_spu = audio.tap_frames[AUDIO_TAP_SPU_OUT];
    last_underruns = underruns; last_overflows = overflows;
    last_overlay_loads = overlay_loads;
    last_overlay_load_us = overlay_load_us;
    for (int i = 0; i < 6; i++) last_up[i] = up[i];
}

static void sdl_audio_update(int turbo_active) {
    if (!sdl_audio_device) return;
    {   /* Tag audio events with the vblank frame counter. */
        extern uint64_t s_frame_count;
        audio_trace_note_frame((uint32_t)s_frame_count);
    }
    const int HANGOVER_FRAMES = 8;  /* ~133 ms at 60 fps */
    static int muted = 0;
    static int hangover = 0;

    if (turbo_active) {
        if (!muted) {
            int tail = sdl_audio_fade_samples;
            const int buf_cap = (int)(sizeof(sdl_audio_buf) / (2 * sizeof(int16_t)));
            if (tail > buf_cap) tail = buf_cap;
            /* An unmute ramp may still be in progress; start the down-ramp
             * from its current gain so the edge stays continuous. */
            const float g0 = 1.0f - (float)sdl_audio_fadein_left
                                    / (float)sdl_audio_fade_samples;
            sdl_audio_fadein_left = 0;
            spu_render(sdl_audio_buf, tail);
            sdl_audio_gain_ramp(sdl_audio_buf, tail, g0, 0.0f);
            audio_trace_event(AUDIO_EV_MUTE, (uint32_t)tail, 0);
            if (audio_legacy_mode()) {
                audio_trace_pcm(AUDIO_TAP_HOST, sdl_audio_buf, tail);
                SDL_QueueAudio(sdl_audio_device, sdl_audio_buf,
                               (uint32_t)tail * sizeof(int16_t) * 2u);
            } else if (s_drc_ready) {
                SDL_LockAudioDevice(sdl_audio_device);
                rab_push(&s_drc, sdl_audio_buf, tail);
                SDL_UnlockAudioDevice(sdl_audio_device);
            }
            muted = 1;
        }
        hangover = HANGOVER_FRAMES;
        return;
    }
    if (muted) {
        if (hangover > 0) { hangover--; return; }
        muted = 0;
        sdl_audio_fadein_left = sdl_audio_fade_samples;
        audio_trace_event(AUDIO_EV_UNMUTE, (uint32_t)sdl_audio_fadein_left, 0);
        extern int g_audio_unmute_resync;
        g_audio_unmute_resync = 1;
    }
    sdl_audio_pump();
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
    uint16_t bit;
    const char* ini_name;
    std::vector<ControllerSource> sources;
};

static int controller_device_index = 0;
static int controller_deadzone = 12000;
static std::array<PsxButtonMap, 16> controller_map = {{
    { PAD_UP,       "up",       {} },
    { PAD_DOWN,     "down",     {} },
    { PAD_LEFT,     "left",     {} },
    { PAD_RIGHT,    "right",    {} },
    { PAD_CROSS,    "cross",    {} },
    { PAD_CIRCLE,   "circle",   {} },
    { PAD_SQUARE,   "square",   {} },
    { PAD_TRIANGLE, "triangle", {} },
    { PAD_L1,       "l1",       {} },
    { PAD_R1,       "r1",       {} },
    { PAD_L2,       "l2",       {} },
    { PAD_R2,       "r2",       {} },
    { PAD_L3,       "l3",       {} },
    { PAD_R3,       "r3",       {} },
    { PAD_START,    "start",    {} },
    { PAD_SELECT,   "select",   {} },
}};

static std::string trim_copy(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace((unsigned char)s[first])) first++;
    size_t last = s.size();
    while (last > first && std::isspace((unsigned char)s[last - 1])) last--;
    return s.substr(first, last - first);
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

    if (s == "a")              { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_A; return out; }
    if (s == "b")              { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_B; return out; }
    if (s == "x")              { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_X; return out; }
    if (s == "y")              { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_Y; return out; }
    if (s == "back" || s == "view" || s == "select") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_BACK; return out;
    }
    if (s == "start" || s == "menu") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_START; return out;
    }
    if (s == "guide")          { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_GUIDE; return out; }
    if (s == "leftstick")      { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_LEFTSTICK; return out; }
    if (s == "rightstick")     { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_RIGHTSTICK; return out; }
    if (s == "leftshoulder" || s == "lb" || s == "l1") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; return out;
    }
    if (s == "rightshoulder" || s == "rb" || s == "r1") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; return out;
    }
    if (s == "dpup" || s == "dpadup") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_DPAD_UP; return out;
    }
    if (s == "dpdown" || s == "dpaddown") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_DPAD_DOWN; return out;
    }
    if (s == "dpleft" || s == "dpadleft") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_DPAD_LEFT; return out;
    }
    if (s == "dpright" || s == "dpadright") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; return out;
    }
    if (s == "lefttrigger" || s == "lt" || s == "l2") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_TRIGGERLEFT; return out;
    }
    if (s == "righttrigger" || s == "rt" || s == "r2") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_TRIGGERRIGHT; return out;
    }
    if (s == "leftx+" || s == "lsright") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_LEFTX; return out;
    }
    if (s == "leftx-" || s == "lsleft") {
        out.kind = ControllerSource::Kind::AxisNegative; out.id = SDL_CONTROLLER_AXIS_LEFTX; return out;
    }
    if (s == "lefty+" || s == "lsdown") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_LEFTY; return out;
    }
    if (s == "lefty-" || s == "lsup") {
        out.kind = ControllerSource::Kind::AxisNegative; out.id = SDL_CONTROLLER_AXIS_LEFTY; return out;
    }
    if (s == "rightx+" || s == "rsright") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_RIGHTX; return out;
    }
    if (s == "rightx-" || s == "rsleft") {
        out.kind = ControllerSource::Kind::AxisNegative; out.id = SDL_CONTROLLER_AXIS_RIGHTX; return out;
    }
    if (s == "righty+" || s == "rsdown") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_RIGHTY; return out;
    }
    if (s == "righty-" || s == "rsup") {
        out.kind = ControllerSource::Kind::AxisNegative; out.id = SDL_CONTROLLER_AXIS_RIGHTY; return out;
    }

    SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(s.c_str());
    if (button != SDL_CONTROLLER_BUTTON_INVALID) {
        out.kind = ControllerSource::Kind::Button;
        out.id = button;
        return out;
    }
    SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(s.c_str());
    if (axis != SDL_CONTROLLER_AXIS_INVALID) {
        out.kind = ControllerSource::Kind::AxisPositive;
        out.id = axis;
        return out;
    }
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

static void set_default_controller_mapping(void) {
    for (auto& entry : controller_map) entry.sources.clear();

    auto set_sources = [](const char* name, const char* sources) {
        for (auto& entry : controller_map) {
            if (std::strcmp(entry.ini_name, name) == 0) {
                entry.sources = parse_source_list(sources);
                return;
            }
        }
    };

    set_sources("up",       "dpup,lefty-");
    set_sources("down",     "dpdown,lefty+");
    set_sources("left",     "dpleft,leftx-");
    set_sources("right",    "dpright,leftx+");
    set_sources("cross",    "a");
    set_sources("circle",   "b");
    set_sources("square",   "x");
    set_sources("triangle", "y");
    set_sources("l1",       "leftshoulder");
    set_sources("r1",       "rightshoulder");
    set_sources("l2",       "lefttrigger");
    set_sources("r2",       "righttrigger");
    set_sources("l3",       "leftstick");
    set_sources("r3",       "rightstick");
    set_sources("start",    "start");
    set_sources("select",   "back");
}

static std::string default_input_ini_text(void) {
    return
        "; PSXRecomp input mapping. PSX buttons are active when any listed source is pressed.\n"
        "; Sources use SDL/Xbox names: a,b,x,y,back,start,leftshoulder,rightshoulder,\n"
        "; lefttrigger,righttrigger,leftstick,rightstick (stick clicks -> L3/R3),\n"
        "; dpup,dpdown,dpleft,dpright,leftx-/leftx+/lefty-/lefty+.\n"
        "\n"
        "[controller]\n"
        "enabled = true\n"
        "device = 0\n"
        "deadzone = 12000\n"
        "\n"
        "[mapping]\n"
        "up = dpup,lefty-\n"
        "down = dpdown,lefty+\n"
        "left = dpleft,leftx-\n"
        "right = dpright,leftx+\n"
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
        "select = back\n";
}

static void load_input_config(const char* argv0) {
    set_default_controller_mapping();

    namespace fs = std::filesystem;
    fs::path config_path = exe_dir_from_argv(argv0) / "input.ini";
    std::error_code ec;
    if (!fs::exists(config_path, ec)) {
        std::ofstream out(config_path, std::ios::binary);
        if (out) out << default_input_ini_text();
        return;
    }

    std::ifstream in(config_path);
    if (!in) return;

    bool controller_enabled = true;
    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.resize(comment);
        line = trim_copy(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower_copy(trim_copy(line.substr(1, line.size() - 2)));
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
                    entry.sources = parse_source_list(value);
                    break;
                }
            }
        }
    }

    if (!controller_enabled) {
        for (auto& entry : controller_map) entry.sources.clear();
    }

    /* Configurable KEYBOARD keybinds (keybinds.ini, next to the exe) — separate
     * from input.ini's gamepad map. Loads the user's map (or writes defaults on
     * first run). The launcher may have just edited+saved this file; we re-read
     * it here so the runtime always reflects the current bindings. */
    psx_keybinds_init(argv0);
}

static void close_player(PlayerInput& p) {
    if (p.handle) {
        SDL_GameControllerClose(p.handle);
        p.handle = nullptr;
        p.instance = -1;
    }
}

static void close_controller(void) {
    close_player(g_players[0]);
    close_player(g_players[1]);
}

/* Open the SDL controller whose GUID matches p.guid. If no exact GUID match
 * exists (e.g. a different physical unit of the same model, or Steam's virtual
 * pad at an unpredictable slot), fall back to the first controller not already
 * claimed by the other player. */
static void open_player(PlayerInput& p, const PlayerInput& other) {
    if (p.kind != 2 || p.handle) return;

    int chosen = -1, fallback = -1;
    const int joysticks = SDL_NumJoysticks();
    for (int i = 0; i < joysticks; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
        char buf[40] = {0};
        SDL_JoystickGetGUIDString(g, buf, sizeof(buf));
        /* Skip a device already opened by the other player. */
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        if (other.handle && other.instance == inst) continue;
        if (p.guid[0] && std::strcmp(buf, p.guid) == 0) { chosen = i; break; }
        if (fallback < 0) fallback = i;
    }
    if (chosen < 0) chosen = fallback;
    if (chosen < 0) return;

    p.handle = SDL_GameControllerOpen(chosen);
    if (p.handle) {
        SDL_Joystick* joy = SDL_GameControllerGetJoystick(p.handle);
        p.instance = joy ? SDL_JoystickInstanceID(joy) : -1;
        const char* name = SDL_GameControllerName(p.handle);
        std::fprintf(stdout, "psxrecomp runtime: opened controller for slot: %s\n",
                     name ? name : "(unnamed)");
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

/* Open/close SDL handles so they match g_players, and (re)assert each slot's
 * PSX connection + pad type. Safe to call repeatedly (hotplug, boot). */
static void refresh_player_devices(void) {
    for (int s = 0; s < 2; s++) {
        PlayerInput& p = g_players[s];
        if (p.kind != 2) close_player(p);           /* keyboard/none: no handle */
        else open_player(p, g_players[s ^ 1]);
        sio_set_pad_connected(s, p.kind != 0 ? 1 : 0);
        sio_set_pad_analog(s, pad_mode_boot_analog(p.mode), 0x80, 0x80, 0x80, 0x80);
        /* DIGITAL mode == a plain digital controller that ignores the DualShock
         * config-mode commands (real SCPH-1080 behaviour); ANALOG/HYBRID == a
         * config-capable DualShock. A digital pad that wrongly answered 0x43
         * sent Tomba 2's pad driver down the config path -> phantom 0x00 reads. */
        sio_set_pad_config_capable(s, p.mode != PSXRecompV4::PAD_MODE_DIGITAL);
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

static bool controller_source_pressed_h(SDL_GameController* h, const ControllerSource& source) {
    if (!h) return false;

    switch (source.kind) {
    case ControllerSource::Kind::Button:
        return SDL_GameControllerGetButton(
            h, (SDL_GameControllerButton)source.id) != 0;
    case ControllerSource::Kind::AxisPositive:
        return SDL_GameControllerGetAxis(
            h, (SDL_GameControllerAxis)source.id) > controller_deadzone;
    case ControllerSource::Kind::AxisNegative:
        return SDL_GameControllerGetAxis(
            h, (SDL_GameControllerAxis)source.id) < -controller_deadzone;
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

static uint16_t controller_pad_buttons(SDL_GameController* h) {
    uint16_t buttons = 0xFFFF;  /* all released */
    if (!h) return buttons;
    for (const auto& entry : controller_map) {
        for (const auto& source : entry.sources) {
            if (controller_source_pressed_h(h, source)) {
                buttons &= (uint16_t)~entry.bit;
                break;
            }
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
static void axes_to_pad_pair(int16_t vx, int16_t vy, uint8_t* obx, uint8_t* oby) {
    const double dz = controller_deadzone;
    double x = vx, y = vy;
    double mag = std::sqrt(x * x + y * y);            /* 0 .. ~46341 */
    if (mag <= dz || mag <= 0.0) { *obx = 0x80; *oby = 0x80; return; }
    double capped = mag > 32767.0 ? 32767.0 : mag;
    double range = 32767.0 - dz;
    if (range < 1.0) range = 1.0;
    double newmag = (capped - dz) * 32767.0 / range;  /* 0 .. 32767 */
    double scale = newmag / mag;                       /* along true direction */
    int sx = (int)std::lround(x * scale);
    int sy = (int)std::lround(y * scale);
    if (sx > 32767) sx = 32767; else if (sx < -32768) sx = -32768;
    if (sy > 32767) sy = 32767; else if (sy < -32768) sy = -32768;
    int bx = (sx + 32768) >> 8;
    int by = (sy + 32768) >> 8;
    *obx = (uint8_t)(bx < 0 ? 0 : (bx > 255 ? 255 : bx));
    *oby = (uint8_t)(by < 0 ? 0 : (by > 255 ? 255 : by));
}

/* Buttons for a player's selected device (0xFFFF = none pressed). `player` is
 * 1 or 2 — selects which keybinds.ini section drives a keyboard port. */
static uint16_t pad_buttons_for(const PlayerInput& p, int player) {
    if (p.kind == 1) return pad_from_keyboard(player);
    if (p.kind == 2) return controller_pad_buttons(p.handle);
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
        axes_to_pad_pair(SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTX),
                         SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTY),
                         &out[0], &out[1]);
        axes_to_pad_pair(SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_RIGHTX),
                         SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_RIGHTY),
                         &out[2], &out[3]);
        if (fold_dpad) {
            if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  out[0] = 0x00;
            if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) out[0] = 0xFF;
            if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_UP))    out[1] = 0x00;
            if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  out[1] = 0xFF;
        }
    }
}

/* HYBRID-mode auto-switch detectors (mirror the DualShock analog LED toggling).
 * hybrid_stick_active: the LEFT stick is deflected past the deadzone — the
 * player has reached for analog. hybrid_dpad_active: any D-pad direction (or,
 * for the keyboard, an arrow key) is held — the player wants classic digital.
 * The keyboard has no analog stick, so a keyboard player stays digital. */
static bool hybrid_stick_active(const PlayerInput& p) {
    if (p.kind != 2 || !p.handle) return false;
    const int dz = controller_deadzone;
    int lx = SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTY);
    return lx > dz || lx < -dz || ly > dz || ly < -dz;
}
static bool hybrid_dpad_active(const PlayerInput& p, int player, bool kb_always) {
    if (p.kind == 2 && p.handle) {
        if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  ||
            SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
            SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_UP)    ||
            SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
            return true;
    }
    if (p.kind == 1 || kb_always) {
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
 * downgrade. Controlled by PSX_DEV_INPUT (default ON for the dev workflow); set
 * PSX_DEV_INPUT=0 to restore strict single-device-per-port routing. */
static bool dev_any_input_enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* e = std::getenv("PSX_DEV_INPUT");
        cached = (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F')) ? 0 : 1;
    }
    return cached != 0;
}

/* Merge (active-low AND) the button words of every connected game controller.
 * Handles are resolved from SDL's already-open set when possible and opened once
 * on first sight otherwise (kept open for the process lifetime; if a slot later
 * closes a shared device this self-heals by reopening next frame). This does NOT
 * disturb per-slot routing — SDL returns the same handle for an already-open
 * device, so reads are shared and harmless. */
static uint16_t dev_all_controllers_buttons() {
    uint16_t btn = 0xFFFF;
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        SDL_GameController* h = SDL_GameControllerFromInstanceID(inst);
        if (!h) h = SDL_GameControllerOpen(i);   /* open once; SDL keeps it */
        if (h) btn &= controller_pad_buttons(h);
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
                         SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_LEFTY), &lx, &ly);
        axes_to_pad_pair(SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_RIGHTX),
                         SDL_GameControllerGetAxis(h, SDL_CONTROLLER_AXIS_RIGHTY), &rx, &ry);
        if (lx != 0x80 || ly != 0x80) { st[0] = lx; st[1] = ly; }
        if (rx != 0x80 || ry != 0x80) { st[2] = rx; st[3] = ry; }
    }
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

    int mode;
    if (p.kind != 0)                  mode = p.mode;
    else if (dev_any_input_enabled()) mode = (int)PSXRecompV4::PAD_MODE_HYBRID;
    else                              mode = (int)PSXRecompV4::PAD_MODE_DIGITAL;

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
    if (!eff_analog) { st[0] = st[1] = st[2] = st[3] = 0x80; }
    sio_set_pad_sticks(0, st[0], st[1], st[2], st[3]);
    sio_request_pad_type(0, eff_analog);
}

static void sample_pad_into_sio(int override) {
    if (override >= 0) {
        apply_input_override_to_sio(override);
        return;
    }
    for (int s = 0; s < 2; s++) {
        PlayerInput& p = g_players[s];
        const int  player  = s + 1;             /* keybinds.ini section (1|2) */
        /* Dev input: P1 is driven by the keyboard AND every connected controller,
         * so a tester can navigate from whatever is plugged in (P2 keeps strict
         * per-port routing). */
        const bool dev_here = (dev_any_input_enabled() && s == 0);
        if (p.kind == 0 && !dev_here) continue;  /* no device in this port */

        /* Buttons: merge the assigned device with the keyboard (PSX pad word is
         * active-low, so AND combines "pressed on either source"). In dev mode P1
         * also folds in the keyboard binds and EVERY connected controller. */
        uint16_t btn = (p.kind != 0) ? pad_buttons_for(p, player) : (uint16_t)0xFFFF;
        if (dev_here) {
            btn &= pad_from_keyboard(1);           /* keyboard drives P1 binds     */
            btn &= dev_all_controllers_buttons();  /* any plugged-in controller too */
        }
        sio_set_pad_state_slot(s, btn);

        /* Resolve the pad type this frame. An assigned device keeps its configured
         * mode (a launcher-selected analog DualShock stays analog, so its input
         * path / SIO handshake cadence is preserved exactly). A P1 with no assigned
         * device but dev-any-input on presents as HYBRID — boots analog like a
         * DualShock and auto-drops to digital on the d-pad — so any plugged
         * controller and the keyboard both navigate. */
        int mode;
        if (p.kind != 0)      mode = p.mode;
        else if (dev_here)    mode = (int)PSXRecompV4::PAD_MODE_HYBRID;
        else                  mode = (int)PSXRecompV4::PAD_MODE_DIGITAL;
        int eff_analog;
        uint8_t st[4] = { 0x80, 0x80, 0x80, 0x80 };
        if (mode == PSXRecompV4::PAD_MODE_DIGITAL) {
            eff_analog = 0;
        } else if (mode == PSXRecompV4::PAD_MODE_ANALOG) {
            eff_analog = 1;
            pad_sticks_for(p, player, st, /*fold_dpad=*/true);
        } else { /* HYBRID */
            if (hybrid_stick_active(p))                       p.hybrid_analog = true;
            else if (hybrid_dpad_active(p, player, dev_here)) p.hybrid_analog = false;
            eff_analog = p.hybrid_analog ? 1 : 0;
            if (eff_analog) pad_sticks_for(p, player, st, /*fold_dpad=*/false);
        }
        /* Dev mode: fold the keyboard's stick binds AND any connected controller's
         * sticks onto the analog stick, so an analog-mode P1 steers from whatever
         * is plugged in (P1 binds). */
        if (dev_here && eff_analog) {
            const Uint8* keys = SDL_GetKeyboardState(NULL);
            psx_keybinds_sticks(keys, 1, st);
            dev_any_controller_sticks(st);
        }
        /* Push sticks every frame; request the pad type (digital/analog) through
         * the coherent channel so a hybrid stick<->d-pad flip is applied only at
         * an idle, non-config bus boundary (never mid-poll / mid-handshake). This
         * is the fix for the v0.5.0 phantom-input regression: slamming the type
         * each frame raced Tomba's DualShock config handshake -> garbage button
         * reads. eff_analog still reflects this frame's mode (digital / analog /
         * hybrid auto-switch). */
        sio_set_pad_sticks(s, st[0], st[1], st[2], st[3]);
        sio_request_pad_type(s, eff_analog);
    }
}

static void sample_headless_pad_into_sio(int override) {
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
 * window-creation time (sync-to-host-refresh) so the pacer and SDL PRESENTVSYNC
 * do not fight — a fixed 59.94 pacer against a 60.00 Hz panel makes rendered
 * frames land on an uneven vblank count (a 2/3/1 beat) that reads as
 * moving-object judder/flicker. See g_frame_period_ms. */
static constexpr double PSX_FRAME_PERIOD_MS = 1000.0 / 59.94;
/* Live pacer period (ms). Defaults to the PSX rate; set to the host refresh
 * period when the panel is within ~2% of 60 Hz so 30fps content pads evenly to
 * two host refreshes. Left at the PSX rate on non-~60Hz panels to avoid running
 * the sim at the wrong speed. */
static double g_frame_period_ms = PSX_FRAME_PERIOD_MS;

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

/* Called from gpu_vblank_tick() at each simulated vblank. */
static void sdl_vblank_present(void) {
#ifndef PSX_NO_DEBUG_TOOLS
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

    runtime_perf_diag_tick();

    /* Host-stack-usage profile sample — frame counter is now current, and we are
     * on the guest fiber (see §17 block above). BEFORE the turbo/fast-boot early
     * returns so the curve is captured even when presents are skipped. */
    stack_profile_sample();

    /* Step 2.8 automation ticks — BEFORE the turbo/fast-boot early returns
     * below, so capture detection and compile pickup keep running while the
     * frontend is skipping presents. Both are cheap and emu-thread-only. */
    overlay_autocapture_tick();
    {   /* Apply a finished batch compile via the active code provider (gcc:
         * cache rescan on done; sljit: no-op — it produces synchronously). */
        const CodeProvider *cp = code_provider_active();
        if (cp->poll_main) cp->poll_main();
    }

    if (!g_headless) {
        /* Pump SDL events to prevent window freeze. */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                psx_crash_trace_set_exit_origin("sdl_window_close");
                shutdown_runtime();
                std::exit(0);
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                refresh_player_devices();
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (ev.cdevice.which == g_players[0].instance ||
                    ev.cdevice.which == g_players[1].instance) {
                    close_controller();
                    refresh_player_devices();
                }
            } else if (ev.type == SDL_KEYDOWN) {
                const Uint16 mod = ev.key.keysym.mod;
                /* Save states: Shift+F1-F12 = save slot 0-11, F1-F12 = load.
                 * (F11 is a save slot per the user's spec, so fullscreen is
                 * Alt+Enter / Cmd+Ctrl+F only — no F11.) */
                if (ev.key.keysym.sym >= SDLK_F1 && ev.key.keysym.sym <= SDLK_F12) {
                    int slot = (int)(ev.key.keysym.sym - SDLK_F1);   /* 0..11 */
                    if (mod & KMOD_SHIFT) savestate_request_save(slot);
                    else                  savestate_request_load(slot);
                }
                /* Fullscreen toggle: Alt+Enter or Cmd/Ctrl+F. FULLSCREEN_DESKTOP
                 * keeps the desktop resolution; the renderer's logical size
                 * letterboxes the 640x480 image. */
                else if ((ev.key.keysym.sym == SDLK_RETURN && (mod & KMOD_ALT)) ||
                         (ev.key.keysym.sym == SDLK_f && (mod & (KMOD_GUI | KMOD_CTRL)))) {
                    Uint32 is_fs = SDL_GetWindowFlags(sdl_window) &
                                   SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(sdl_window,
                        is_fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
            }
        }
    }

    /* Sample each player's device and feed the matching SIO pad slot.
     * Debug server input override (when active) drives port 1 only. With
     * g_low_latency_input this early sample is re-done after the pacer wait
     * (below) for the interactive present path; it still covers the turbo /
     * FMV-skip paths that early-return before pacing. */
    if (g_headless) sample_headless_pad_into_sio(override);
    else            sample_pad_into_sio(override);

    /* Latency ring: open this present cycle's slot, stamping when input was
     * sampled into SIO.  Always-on; queried via the debug server "latency". */
    latency_ring_frame_begin();

    /* Turbo-active test, shared by the audio gate here and the pacing/
     * present gate below. sdl_audio_update owns the mute + fade-in/out +
     * debounce across turbo transitions (see its comment). */
    int turbo_loads_active = 0;
    if (g_turbo_loads_enabled) {
        extern int fntrace_is_game_started(void);
        /* Hysteresis: count consecutive frames the load has held, and only
         * engage turbo once it is SUSTAINED (TURBO_LOADS_ENGAGE_FRAMES). This
         * stops brief incidental reads on music screens (boss/stage select) from
         * flickering turbo on and chopping the audio. cdrom_load_in_progress()
         * already bridges short intra-load gaps (CD_BURST_GAP_FRAMES), so a real
         * load's counter does not reset mid-load. */
        static int load_run = 0;
        if (fntrace_is_game_started() && cdrom_load_in_progress()) {
            if (load_run < (1 << 20)) load_run++;
        } else {
            load_run = 0;
        }
        if (load_run >= TURBO_LOADS_ENGAGE_FRAMES)
            turbo_loads_active = 1;
    }
    /* HLE boot-skip window: from reset until the game entry PC first
     * dispatches, run unpaced so the (shell-skipped) BIOS kernel init +
     * SYSTEM.CNF + game EXE load compress to host speed. This replaces the
     * old fast_boot snapshot restore; all guest timing is authentic. */
    if (psx_bios_hle_boot_turbo_active())
        turbo_loads_active = 1;

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
    if (g_auto_skip_fmv) {
        static uint32_t s_last_mdec = 0;
        static int      s_fmv_hold  = 0;
        uint32_t mc = mdec_get_decode_count();
        int mdec_decoding = (mc != s_last_mdec);
        s_last_mdec = mc;
        int xa = cdrom_xa_stream_active();
        /* fmv_skip_no_xa: silent RAM-preloaded movies (no XA stream) never
         * satisfy the MDEC+XA detector; with the per-game opt-in, MDEC
         * activity alone qualifies. */
        int detected = mdec_decoding && (xa || g_fmv_skip_no_xa);
        if (detected) s_fmv_hold = (!xa && g_fmv_skip_no_xa)
                                     ? g_fmv_skip_no_xa_hold : 4;
        else if (s_fmv_hold > 0) s_fmv_hold--;
        fmv_skip_active = (s_fmv_hold > 0) && (xa || g_fmv_skip_no_xa);
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
    /* Turbo-loads no longer mutes audio. Loads at 1x+turbo are sub-second, and
     * the mute/fade/hangover model produced more audible disruption (cuts and
     * "restarts") than just letting the stream play through; the audio queue is
     * capped (sdl_audio_pump max_queue_bytes) so a brief turbo burst can't build
     * unbounded latency. FMV-skip still mutes: it fast-forwards an entire movie,
     * where rendering the time-compressed audio would be pure noise. */
    sdl_audio_update(fmv_skip_active);
#endif

    if (g_headless) return;

    /* TCP turbo is for automated validation and trace capture. It keeps the
     * simulation advancing and the debug server polling, but removes frontend
     * presentation and wall-clock pacing. */
#ifndef PSX_NO_DEBUG_TOOLS
    if (debug_server_turbo_enabled()) return;
#endif

    /* Turbo mode: while TAB is held, skip both VRAM->ARGB conversion and
     * SDL_RenderPresent. The recompiled BIOS still advances simulated
     * cycles every vblank, so the BIOS proceeds at whatever rate the host
     * CPU sustains without graphics-driver vsync overhead. Present once
     * every TURBO_PRESENT_EVERY frames so the user sees visual progress. */
    {
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        static int turbo_skip = 0;
        const int TURBO_PRESENT_EVERY = 30;
        if (keys[SDL_SCANCODE_TAB]) {
            turbo_skip = (turbo_skip + 1) % TURBO_PRESENT_EVERY;
            if (turbo_skip != 0) return;  /* skip render this frame */
        } else {
            turbo_skip = 0;
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
        static int tl_skip = 0;
        const int TL_PRESENT_EVERY = 30;
        tl_skip = (tl_skip + 1) % TL_PRESENT_EVERY;
        if (tl_skip != 0) return;
    }

    /* FMV auto-skip: run uncapped (no wall-clock pacing) and suppress nearly all
     * presents so the fast-forwarded movie is invisible. Present 1-in-30 only so
     * the window keeps pumping and never looks hung during the brief skip. */
    if (fmv_skip_active) {
        static int fs_skip = 0;
        const int FMV_PRESENT_EVERY = 30;
        fs_skip = (fs_skip + 1) % FMV_PRESENT_EVERY;
        if (fs_skip != 0) return;
    }

    /* Wall-clock pacing: always runs once fast_boot has ended, even when the
     * display is still disabled (e.g. game crt0 setup). Skipped only by the
     * turbo and fast_boot early-returns above. frame_pacer_wait is the
     * race-free replacement for the old open-coded loop whose double
     * counter read could underflow into a ~24.7-day SDL_Delay (Bug B
     * hard freeze). */
    {
        static FramePacer pacer = { 0 };
        frame_pacer_wait(&pacer, g_frame_period_ms);
    }
    latency_ring_mark(LAT_PACED);

    /* Low-latency input: the early sample above is now ~one pacer-wait old.
     * Refresh the device state and re-sample right before present so the next
     * CPU frame reads near-fresh input (the dominant input->photon cost on a
     * vsync-light box). Re-stamp the ring's input mark to measure from here. */
    if (g_low_latency_input) {
        SDL_GameControllerUpdate();  /* refresh pad state after the wait */
        SDL_PumpEvents();            /* refresh keyboard state */
        sample_pad_into_sio(override);
        latency_ring_restamp_input();
    }

    /* Engage widescreen at game entry: BIOS boot stays authentic 4:3. */
    if (!g_ws_engaged) {
        extern int fntrace_is_game_started(void);
        if (fntrace_is_game_started()) {
            g_ws_engaged = true;
            int mode = g_ws_native_wide ? 2 : 1;
            /* Native-wide: GTE drawn un-squashed — feed it the 4:3 ratio
             * (identity squash). Squash mode: feed the real wide aspect. */
            gte_set_display_aspect(mode == 1 ? g_video_aspect_num : 4,
                                   mode == 1 ? g_video_aspect_den : 3);
            gpu_ws_configure(g_video_aspect_num, g_video_aspect_den,
                             g_ws_anchor_addr, g_ws_hud_sprt ? 1 : 0, mode);
        }
    }

    /* ---- Display from our VRAM ---- */
    uint32_t w = 0, h = 0;
    uint32_t present_w = 0;  /* display width actually presented (w + native-wide EXTRA) */
    int active_scale = 1;   /* hi-res mirror used only for 15-bit display */
    bool fmv_frame = false;  /* FMV/boot — present pillarboxed 4:3 in widescreen */
    bool pin_43    = false;  /* pillarbox this present (FMV, or a native-wide
                                game frame that could not present wide) */
    {
        static bool disabled_frame_presented = false;
        GpuDisplayInfo di;
        gpu_get_display_info(&di);
        if (di.disabled || di.width == 0 || di.height == 0) {
            present_ring_commit(PRES_PATH_BLANK, (uint16_t)di.width,
                                (uint16_t)di.height, 0);
#ifndef PSX_SDL_NO_RENDER
            if (!disabled_frame_presented) {
                disabled_frame_presented = true;
                if (g_gl_active) {
                    gl_renderer_present_blank();
                } else if (g_vk_active) {
                    vk_renderer_present_blank();
                } else {
                    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
                    SDL_RenderClear(sdl_renderer);
                    SDL_RenderPresent(sdl_renderer);
                }
            }
#endif
            return;
        }
        disabled_frame_presented = false;
        w = di.width; h = di.height;
        /* 4:3-pinned frames: the pre-game BIOS boot, plus (once engaged) every
         * frame the widescreen layer presents native — FMV video and full-2D
         * menu/title screens. gpu_ws_present_native_43() is the single source
         * of truth, shared with the GTE/GPU squash so content and present stay
         * locked: we squash IFF we stretch. */
        fmv_frame = !g_ws_engaged || gpu_ws_present_native_43() != 0;
        /* MDEC movies are already decoded at their authored cadence and are
         * CPU/upload heavy. High-refresh crossfades only contend with decoding
         * and can starve audio, so present native-4:3/MDEC phases directly.
         * The classification catches the transition frame before the first
         * decode; the activity stamp also covers authentic 4:3 configurations. */
        if (g_gl_active)
            gl_renderer_set_interpolation_suspended(
                fmv_frame || mdec_recently_active(2));

        /* Canonical present width. Native-wide does NOT widen the canonical read
         * (that bled across adjacent framebuffers); it composites into a separate
         * wide surface and presents from there instead (see gpu wide compositor). */
        present_w = w;

        /* Native-wide present: on a game frame, if the active backend has the
         * wide compositor, present the wider surface (canonical width + EXTRA)
         * from the displayed buffer's surface. FMV/menu frames stay 4:3. */
        bool wide_present = (!fmv_frame && g_ws_engaged &&
                             ws_native_wide_active() && gr_wide_supported());
        if (wide_present) present_w = w + (uint32_t)ws_nw_extra();

        /* Native-wide invariant: canonical (320-wide) content is NEVER
         * stretched across the wide window — a game frame that cannot present
         * wide (compositor unsupported, or the surface fallback below)
         * pillarboxes 4:3 like FMV/menus instead. Only squash mode (1) may
         * stretch: its canonical content is pre-squashed FOR the stretch. */
        const bool nw_pin = g_ws_engaged && g_ws_native_wide;

        /* Ring the classification now that it's final (only the software/CPU
         * wide path below can still fall back — it amends this entry). A
         * CANONICAL game frame on a widescreen window IS the stretch. */
        PresRingEntry* pres_entry = present_ring_commit(
            fmv_frame ? PRES_PATH_NATIVE_43
                      : (wide_present ? PRES_PATH_WIDE : PRES_PATH_CANONICAL),
            (uint16_t)w, (uint16_t)h, (uint16_t)present_w);

        /* OpenGL: 15-bit frames ALWAYS present straight from the authoritative
         * VRAM FBO — one deterministic path (the old per-frame FBO-vs-CPU
         * alternation caused the menu seam/jitter). 24-bit (FMV) frames and
         * the PSX_GL_FORCE_CPU_PRESENT diagnostic sync the FBO down and use
         * the CPU readout below. */
#ifndef PSX_SDL_NO_RENDER
        if (g_gl_active && g_gl_fbo_present && !di.depth24) {
            if (wide_present) {
                /* GPU-direct native-wide present: blit the displayed buffer's
                 * wide FBO straight to the window (GPU-side, like the canonical
                 * present_vram). This avoids the glFinish + glReadPixels CPU
                 * round-trip the old path did EVERY frame — that readback was the
                 * GL-only slowdown (SW's wide path is pure CPU, no GPU sync, so
                 * SW stayed smooth). Falls through to the CPU readout path only if
                 * the wide surface for this buffer doesn't exist yet. */
                if (gl_renderer_present_wide_fbo((int)di.display_x, (int)di.display_y,
                                                 (int)h, g_video_aa ? 1 : 0))
                    return;
            } else {
                gl_renderer_present_vram((int)di.display_x, (int)di.display_y,
                                         (int)present_w, (int)h, g_video_aa ? 1 : 0,
                                         (fmv_frame || nw_pin) ? 1 : 0);
                return;
            }
        }
        if (g_gl_active) gl_renderer_sync_cpu();
        /* Vulkan owns every frame: 15-bit frames present straight from the GPU
         * VRAM image (deterministic blit, no readback), mirroring the GL path;
         * 24-bit (FMV) frames go through the CPU present (Phase 3). The Vulkan
         * window has no SDL_Renderer, so we must never fall through below. */
        if (g_vk_active) {
            if (di.depth24) {
                /* 24-bit (FMV) frames: the packed RGB bytes live in the CPU
                 * VRAM mirror (transfer path keeps it current; sync_cpu folds
                 * down any GPU-drawn content). Compose the ARGB frame exactly
                 * like the software present, then hand it to the Vulkan CPU
                 * present (staging image -> swapchain blit). */
                vk_renderer_sync_cpu();
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < present_w; x++)
                        sdl_pixel_buf[y * present_w + x] =
                            gpu_display_pixel_argb(&di, x, y);
                vk_renderer_present_cpu(sdl_pixel_buf, (int)present_w, (int)h,
                                        g_video_aa ? 1 : 0, fmv_frame ? 1 : 0);
            } else if (wide_present &&
                       vk_renderer_present_wide((int)di.display_x, (int)di.display_y,
                                                (int)h, g_video_aa ? 1 : 0)) {
                /* presented wide */
            } else {
                vk_renderer_present_vram((int)di.display_x, (int)di.display_y,
                                         (int)present_w, (int)h, g_video_aa ? 1 : 0,
                                         (fmv_frame || nw_pin) ? 1 : 0);
            }
            return;
        }
#endif

        /* The hi-res mirror is a 15-bit copy of VRAM; 24-bit display (FMV)
         * reads packed bytes the mirror can't represent, so fall back to the
         * native path for those frames (the present filter still upscales). */
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
            int n = gr_render_wide_display(sdl_pixel_buf, (int)(sw * sizeof(uint32_t)),
                                           (int)di.display_x, (int)di.display_y, (int)h);
            if (n > 0) {
                active_scale = s;
            } else {
                wide_present = false;
                present_w = w;
                pres_entry->path          = PRES_PATH_CANONICAL;
                pres_entry->wide_fellback = 1;
                pres_entry->present_w     = (uint16_t)present_w;
            }
        }
        pin_43 = fmv_frame || (nw_pin && !wide_present);
        if (!wide_present) {
            if (active_scale > 1) {
                int sw = (int)present_w * active_scale;
                gr_render_display_hires(sdl_pixel_buf, (int)(sw * sizeof(uint32_t)),
                                        (int)di.display_x, (int)di.display_y,
                                        (int)present_w, (int)h);
            } else {
                for (uint32_t y = 0; y < h; y++) {
                    for (uint32_t x = 0; x < present_w; x++) {
                        sdl_pixel_buf[y * present_w + x] = gpu_display_pixel_argb(&di, x, y);
                    }
                }
            }
        }

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
            if (blend_cfg && simple_sw) {
                static uint32_t prev_buf[640 * 512];
                static uint32_t prev_px = 0;
                const uint32_t npx = present_w * h;
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
    if (g_gl_active) {
        /* OpenGL present: upload the active display rect and draw a full-screen
         * quad. SDL_GL_SwapWindow handles vsync; the wall-clock pacer above
         * still owns timing. 24-bit (FMV) frames pin to native 4:3. */
        gl_renderer_present(sdl_pixel_buf, src_w, src_h, g_video_aa ? 1 : 0,
                            pin_43 ? 1 : 0);
    } else {
    SDL_Rect src = { 0, 0, src_w, src_h };
    SDL_UpdateTexture(sdl_texture, &src, sdl_pixel_buf,
                      (int)(src_w * sizeof(uint32_t)));

    /* FMV (24-bit) frames are authored 4:3 with no GTE squash to compensate
     * the widescreen stretch — pillarbox them at native 4:3 instead. Same for
     * native-wide game frames that could not present wide (pin_43): canonical
     * content is never stretched across the wide window. */
    int dst_w = pin_43 ? 640 * g_video_scale : g_logical_w;
    SDL_Rect dst = { (g_logical_w - dst_w) / 2, 0, dst_w, 480 * g_video_scale };
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, &src, &dst);

    /* Vsync self-heal. The renderer is created with PRESENTVSYNC for
     * tear-free output, but the wall-clock pacer above already holds
     * 59.94 Hz, so driver vsync is redundant for timing. Under some
     * driver states (observed: NVIDIA GL with the swap queue wedged)
     * SwapBuffers blocks ~1.5 s per present, dragging the whole
     * emulation to ~0.7 fps for minutes (freeze dump 1781045865:
     * 8/8 main-thread samples inside wglSwapBuffers). If presents
     * block pathologically several times in a row, drop driver vsync
     * for the rest of the session; our own pacing keeps the rate. */
    {
        latency_ring_mark(LAT_SWAP_BEGIN);
        const Uint64 t0 = SDL_GetPerformanceCounter();
        SDL_RenderPresent(sdl_renderer);
        const Uint64 t1 = SDL_GetPerformanceCounter();
        latency_ring_mark(LAT_SWAP_END);
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
}

int main(int argc, char** argv) {
    /* Force line-buffered output so messages appear even if killed. */
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::fprintf(stderr, "psxrecomp: main() entered\n");
    std::fflush(stderr);

    /* Install crash handlers early so they catch issues during init too.
     * Writes psx_last_run_report.json on signal/SEH/atexit/fail-fast. */
    psx_crash_trace_install_handlers();

    const char* bios_path = PSX_DEFAULT_BIOS_PATH;
    const char* game_config_path = nullptr;
    const char* disc_override_path = nullptr;
    bool        bios_from_cli = false;  /* CLI --bios/positional wins over settings.toml */
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
    /* Parse args.
     *   --bios <path>       override the compile-time BIOS path
     *   --game <toml>       load a game config (single source of truth for
     *                       disc / memcard / window title / debug port)
     *   --disc <path>       override the game config disc path
     *   --debug-port <n>    override the TCP debug-server port (multi-instance)
     *   --renderer <name>   override the renderer: software|opengl|vulkan
     *   --launcher          force the GUI launcher (overrides skip_launcher)
     *   --no-launcher       skip the GUI launcher (boot straight in)
     *   --headless          skip SDL window/audio; use TCP screenshots/state
     *   <positional>        deprecated alias for --bios
     * No --memcard-dir / --game-root flags: those are config-driven. */
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
            bios_from_cli = true;
        } else if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            game_config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            disc_override_path = argv[++i];
        } else if (std::strcmp(argv[i], "--debug-port") == 0 && i + 1 < argc) {
            cli_debug_port = std::atoi(argv[++i]);
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
        } else if (argv[i][0] != '-') {
            bios_path = argv[i];
            bios_from_cli = true;
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
    std::filesystem::path memcard1_path;   /* explicit slot-1 .mcd (empty => dir/card1.mcd) */
    std::filesystem::path memcard2_path;   /* explicit slot-2 .mcd (empty => dir/card2.mcd) */
    bool memcard1_enabled = true;
    bool memcard2_enabled = true;
    /* [controller] device routing (defaults: P1 keyboard/digital, P2 none). */
    /* Dev builds default Player 1 to the first connected controller ("auto"):
     * combined with dev-any-input (dev_any_input_enabled(), default ON) the
     * selected controller, EVERY other plugged-in controller, AND the keyboard
     * all drive P1 with no launcher setup. If no controller is present, "auto"
     * opens nothing and the keyboard/any-controller merge still drives P1.
     * Release keeps "keyboard" (the launcher assigns devices). */
#if defined(PSX_DEBUG_TOOLS)
    std::string p1_device = "auto";
#else
    std::string p1_device = "keyboard";
#endif
    std::string p2_device = "none";
    int  p1_mode = PSXRecompV4::PAD_MODE_HYBRID;
    int  p2_mode = PSXRecompV4::PAD_MODE_HYBRID;
    bool ctrl_allow_hybrid = true;  /* game.toml [controller] allow_hybrid; false hides Hybrid in the launcher */
    bool ctrl_lock_mode    = false; /* game.toml [controller] lock_mode; true hides the whole pad-mode selector */
    bool ctrl_lock_device  = false; /* game.toml [controller] lock_device; true hides the Player controller cards entirely */
    /* The game-DECLARED port modes, captured at game.toml load and immune to the
     * settings.toml overrides below. Under lock_mode these are the only valid
     * modes (the game supports exactly one pad type), so they are what the
     * runtime clamps to and what the launcher locks its selector to. */
    int  ctrl_locked_p1_mode = PSXRecompV4::PAD_MODE_HYBRID;
    int  ctrl_locked_p2_mode = PSXRecompV4::PAD_MODE_HYBRID;
    bool ws_offered = true; /* game.toml [widescreen] offer; false hides the launcher toggle + clamps 4:3 */
    bool ws_ultrawide_offered = false;
    int  resolved_deadzone = -1;  /* <0 => keep input.ini/runtime default (12000) */
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
    bool        game_has_disc_crc = false;
    uint32_t    game_disc_crc     = 0;
    std::string disc_speed;   /* "1x" | "2x" | "4x" | "instant" */
    int        instant_rate  = 0;   /* 0 = cdrom.c built-in default */
    uint32_t   game_entry_pc = 0;
    bool       fast_boot     = false;  /* DEPRECATED alias: HLE boot-skip only */
    bool       bios_hle      = false;  /* HLE kernel-service tier (bios_hle.c) */
    bool       bios_hle_keep_intro = false;
    /* Text-image guard source, captured at config load; armed after the disc
     * path is resolved (arm_text_image_guard). */
    std::string text_guard_exe_path;
    uint32_t    text_guard_load_addr = 0;

    if (game_config_path) {
        try {
            const auto gc = PSXRecompV4::load_game_config(game_config_path);
            game_name = gc.name;
            game_id   = gc.id;
            game_has_disc_crc = gc.has_disc_crc;
            game_disc_crc     = gc.disc_crc;
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
            if (gc.runtime.turbo_loads) {
                g_turbo_loads_enabled = 1;
                std::fprintf(stdout, "psxrecomp: turbo_loads enabled (opt-in)\n");
            }
            g_video_scale      = gc.runtime.video_supersampling;
            g_video_aa         = gc.runtime.video_antialiasing;
            g_video_texfilter  = gc.runtime.video_texture_filter;
            g_video_renderer   = gc.runtime.video_renderer;
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
            /* [widescreen] clear_reveal — enable opted-in scene/map-boundary
             * cleanup of synthetic native-wide margins. */
            gpu_ws_set_clear_reveal(gc.ws_clear_reveal ? 1 : 0);
            gpu_ws_set_cull_guard_pixels(gc.ws_cull_guard_pixels);
            gpu_ws_set_explicit_cull_sites(
                gc.ws_cull_bias_sites.data(), (int)gc.ws_cull_bias_sites.size(),
                gc.ws_cull_slti_sites.data(), (int)gc.ws_cull_slti_sites.size());
            gte_ws_configure_dome_sites(
                gc.ws_dome_call_sites.data(), (int)gc.ws_dome_call_sites.size());
            /* [widescreen.cull] per-game gates + signature immediates for the
             * pattern-scanned interp/sljit widen hooks. A title that never
             * opted in must never have its live code scanned and rewritten. */
            gpu_ws_set_auto_hooks(gc.ws_auto_screen_x_cull ? 1 : 0,
                                  gc.ws_auto_backdrop_preload ? 1 : 0);
            if (!gc.ws_cull_w_imms.empty() || !gc.ws_cull_h_imms.empty())
                gpu_ws_set_cull_imms(gc.ws_cull_w_imms.data(), (int)gc.ws_cull_w_imms.size(),
                                     gc.ws_cull_h_imms.data(), (int)gc.ws_cull_h_imms.size());
            ws_offered = gc.ws_offered;
            ws_ultrawide_offered = gc.ws_ultrawide_offered;
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
                p1_mode = gc.runtime.default_p1_mode;
                p2_mode = gc.runtime.default_p2_mode;
            }
            ctrl_locked_p1_mode = gc.runtime.default_p1_mode;
            ctrl_locked_p2_mode = gc.runtime.default_p2_mode;
            ctrl_allow_hybrid = gc.runtime.controller_allow_hybrid;
            ctrl_lock_mode    = gc.runtime.controller_lock_mode;
            ctrl_lock_device  = gc.runtime.controller_lock_device;
            if (gc.runtime.has_deadzone) resolved_deadzone = gc.runtime.deadzone;
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
            bios_hle      = gc.runtime.bios_hle;
            bios_hle_keep_intro = gc.runtime.bios_hle_keep_intro;
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
                uint32_t text_end = (gc.load_address + gc.text_size) & 0x1FFFFFFFu;
                if (text_end > 0x00010000u /* DIRTY_RAM_KERNEL_WINDOW_END */)
                    g_overlay_region_floor = text_end;
                std::fprintf(stdout,
                    "psxrecomp: overlay_region_floor = 0x%05X (game text end)\n",
                    g_overlay_region_floor);
            }
            /* Overlay DLL cache (Layer A). Off unless enabled in [runtime];
             * when on, capture overlay bytes and scan cache/<game_id>/ for
             * precompiled overlay DLLs. */
            if (gc.runtime.overlay_cache) {
                std::filesystem::path exe_dir = exe_dir_from_argv(argv[0]);
                std::string cache_dir = (exe_dir / "cache").string();
                overlay_capture_set_out_dir(exe_dir.string().c_str());
                overlay_capture_set_enabled(1);
                overlay_loader_init(cache_dir.c_str(), game_id.c_str());
                for (uint32_t addr : gc.runtime.overlay_native_block) {
                    overlay_loader_native_block_add(addr);
                }
                if (!gc.runtime.overlay_native_block.empty()) {
                    std::fprintf(stdout,
                        "psxrecomp: overlay native blocklist seeded with %zu entr%s\n",
                        gc.runtime.overlay_native_block.size(),
                        gc.runtime.overlay_native_block.size() == 1 ? "y" : "ies");
                }
                /* Step 2.8 variant-capture automation. Autocapture is ON
                 * whenever the cache is on: it writes the player-shareable
                 * overlay_captures.json (the contribution file the README
                 * promises) even on machines with no compile toolchain.
                 * The background compile additionally needs a configured
                 * command; autocompile_request() no-ops without one. */
                overlay_autocapture_set_enabled(1);
                /* Resolve the overlay tier first so we wire the RIGHT compiler's
                 * autocompile command. gcc is "available" only when a gcc cmd is
                 * configured AND a gcc toolchain is actually reachable (a real
                 * dev/production box). auto => gcc if so, else tcc; auto-no-gcc =>
                 * tcc even with gcc present (simulate a toolchain-less user box).
                 * env PSX_OVERLAY_BACKEND overrides. Tiers: static > gcc > tcc >
                 * (sljit, deprecated/off) > interp. */
                const char *cfg_backend = gc.runtime.overlay_backend.empty()
                        ? nullptr : gc.runtime.overlay_backend.c_str();
                int gcc_avail = gc.runtime.has_overlay_autocompile_cmd
                                && autocompile_toolchain_available();
                OverlayBackend eff = overlay_backend_resolve(cfg_backend, gcc_avail);
                /* gcc and tcc run the IDENTICAL recompiler->C->DLL->load pipeline;
                 * only the compiler binary differs. Wire the autocompile spawn with
                 * the command for the resolved tier (tcc cmd for the tcc tier, gcc
                 * cmd otherwise). gcc shards already on disk still LOAD either way
                 * (the loader is compiler-blind), so a tcc box uses shipped gcc
                 * shards first and fills the rest with tcc. */
                std::string built_tcc_cmd;  /* runtime-constructed bundled tcc cmd */
                const std::string *ac_cmd = nullptr;
                if (eff == OVERLAY_BACKEND_TCC) {
                    if (gc.runtime.has_overlay_autocompile_cmd_tcc) {
                        ac_cmd = &gc.runtime.overlay_autocompile_cmd_tcc;  /* explicit override (dev) */
                    } else {
                        /* PRODUCTION: construct the tcc autocompile cmd from the
                         * self-contained toolchain bundled beside the exe
                         * (<exe>/overlay_toolchain/ = embedded python + tcc +
                         * recompiler + compile_overlays.py + runtime headers). No
                         * system python or gcc required. */
                        extern int g_psx_cps_mode;
                        std::filesystem::path xd = exe_dir_from_argv(argv[0]);
                        std::filesystem::path tk = xd / "overlay_toolchain";
                        std::filesystem::path py = tk / "python" / "python.exe";
                        if (std::filesystem::exists(py)) {
                            built_tcc_cmd =
                                py.string() + " " + (tk / "compile_overlays.py").string() +
                                " --captures " + (xd / "overlay_captures.json").string() +
                                " --game-toml " + std::string(game_config_path ? game_config_path : "game.toml") +
                                " --recompiler " + (tk / "psxrecomp-game.exe").string() +
                                " --runtime-include " + (tk / "include").string() +
                                " --out-dir " + (xd / "cache").string() +
                                (g_psx_cps_mode ? " --cps" : "") +
                                " --compiler tcc --tcc " + (tk / "tcc" / "tcc.exe").string();
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
                    if (gc.runtime.has_overlay_autocompile_cmd)
                        ac_cmd = &gc.runtime.overlay_autocompile_cmd;
                }
                if (ac_cmd) {
                    /* Pin the compile's WRITE cache + READ captures to the SAME
                     * canonical locations the loader uses (cache_dir = <exe>/cache,
                     * <exe>/overlay_captures.json — set above). The framework owns
                     * the cache location; no game.toml --out-dir/--captures can make
                     * the write drift from the read. Single source of truth, all
                     * games, dev or prod. */
                    std::string captures_path =
                        (exe_dir / "overlay_captures.json").string();
                    autocompile_set_cache_paths(cache_dir.c_str(),
                                                captures_path.c_str());
                    autocompile_configure(ac_cmd->c_str(),
                                          gc.project_root.string().c_str());
                    std::fprintf(stdout,
                        "psxrecomp: overlay autocompile enabled (%s); cache=%s\n",
                        overlay_backend_name(eff), cache_dir.c_str());
                }
                code_provider_init(cfg_backend, gcc_avail);
                /* Now that the backend is resolved, apply the sljit live policy:
                 * a toolchain-less (sljit) machine runs validated shards live so
                 * it self-improves on the normal play path; PSX_OVERLAY_SLJIT_LIVE
                 * overrides. (Validated-live, not blind — see the dispatch gate.) */
                overlay_loader_apply_live_policy();
            }
            std::fprintf(stdout, "psxrecomp: loaded game config %s (%s, %s)\n",
                         game_config_path, game_name.c_str(), game_id.c_str());
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "psxrecomp: failed to load --game %s: %s\n",
                         game_config_path, ex.what());
            return 1;
        }
    }

    if (!game_name.empty()) s_picker_game_name = game_name;

    /* Layer the launcher-written settings.toml (next to the exe) over the
     * bundled game.toml. Any field present there overrides the config value;
     * the command line (--bios/--disc) still wins over the file. Absent =>
     * fall through to game.toml. The file is the launcher's persistence. */
    bool skip_launcher_setting = false;  /* [launcher] skip_launcher from settings.toml */
    std::string settings_bios_storage;  /* must outlive resolve_bios_for_runtime */
    {
        std::filesystem::path settings_path =
            exe_dir_from_argv(argv[0]) / "settings.toml";
        const PSXRecompV4::UserSettings us =
            PSXRecompV4::load_user_settings(settings_path);
        if (us.has_skip_launcher)  skip_launcher_setting = us.skip_launcher;
        if (us.has_renderer)       g_video_renderer  = us.renderer;
        if (us.has_supersampling)  g_video_scale     = us.supersampling;
        if (us.has_window_width)   g_video_win_w     = us.window_width;
        if (us.has_antialiasing)   g_video_aa        = us.antialiasing;
        if (us.has_texture_filter) g_video_texfilter = us.texture_filter;
        if (us.has_screen_kind)    g_video_screen    = us.screen_kind;
        if (us.has_auto_skip_fmv)  g_auto_skip_fmv   = us.auto_skip_fmv ? 1 : 0;
        if (us.has_turbo_loads)    g_turbo_loads_enabled = us.turbo_loads ? 1 : 0;
        if (us.has_fast_boot)      fast_boot = us.fast_boot;
        if (us.has_bios_hle)       bios_hle  = us.bios_hle;
        if (us.has_fullscreen)     g_fullscreen      = us.fullscreen ? 1 : 0;
        if (us.has_aspect_ratio) {
            g_video_aspect_num = us.aspect_num;
            g_video_aspect_den = us.aspect_den;
        }
        if (us.has_spu_hq)         g_audio_spu_hq    = us.spu_hq;
        if (us.has_bios_path && !bios_from_cli) {
            settings_bios_storage = us.bios_path.string();
            bios_path = settings_bios_storage.c_str();
        }
        if (us.has_disc_path && !disc_override_path) resolved_disc = us.disc_path;
        if (us.has_memcard_dir)                      memcard_dir   = us.memcard_dir;
        if (us.has_memcard1_path)    memcard1_path    = us.memcard1_path;
        if (us.has_memcard2_path)    memcard2_path    = us.memcard2_path;
        if (us.has_memcard1_enabled) memcard1_enabled = us.memcard1_enabled;
        if (us.has_memcard2_enabled) memcard2_enabled = us.memcard2_enabled;
        if (us.has_language) resolved_language = us.language;
        if (us.has_p1_device) p1_device = us.p1_device;
        if (us.has_p2_device) p2_device = us.p2_device;
        if (us.has_p1_mode) p1_mode = us.p1_mode;
        if (us.has_p2_mode) p2_mode = us.p2_mode;
        if (us.has_deadzone)  resolved_deadzone = us.deadzone;
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
    if (ctrl_lock_mode) {
        p1_mode = ctrl_locked_p1_mode;
        p2_mode = ctrl_locked_p2_mode;
    }

    /* [widescreen] offer=false: this title's widescreen is unported/unvalidated,
     * so the launcher hides its toggle — and, same completeness treatment as
     * lock_mode above, the runtime clamps the display aspect to native 4:3 here
     * so a stale persisted 16:9 in settings.toml can't engage the hack in
     * launcher-less builds either. */
    if (!ws_offered && (g_video_aspect_num != 4 || g_video_aspect_den != 3)) {
        std::fprintf(stdout, "psxrecomp: widescreen not offered for this title; "
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
     * PSX_LOW_LATENCY_INPUT=0/1 ; PSX_VSYNC=1(vsync)/0(immediate)/-1(adaptive);
     * PSX_FRAME_INTERPOLATION=0/1; PSX_FRAME_INTERPOLATION_FPS=0|90+. */
    if (const char *e = std::getenv("PSX_LOW_LATENCY_INPUT")) g_low_latency_input = atoi(e) ? 1 : 0;
    if (const char *e = std::getenv("PSX_VSYNC"))             g_video_vsync       = atoi(e);
    if (const char *e = std::getenv("PSX_FRAME_INTERPOLATION"))
        g_frame_interpolation = atoi(e) ? 1 : 0;
    if (const char *e = std::getenv("PSX_FRAME_INTERPOLATION_FPS")) {
        int fps = atoi(e);
        if (fps == 0 || fps >= 90) g_frame_interpolation_fps = fps;
    }

    /* Resolve the effective memory-card directory now (before the launcher) so
     * the launcher can introspect the real card files. The same default is used
     * by the runtime below. */
    if (memcard_dir.empty()) memcard_dir = default_memcard_dir(argv[0]);

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

#if defined(PSX_LAUNCHER)
    /* Integrated launcher: shown in its own GL window before the emulator
     * boots. Seeded with the effective settings (game.toml ∪ settings.toml);
     * on LAUNCH the user's choices are persisted to settings.toml and applied.
     * The launcher window/context is fully torn down before the emulator's own
     * window is created, so the emulator boot path below is untouched.
     *
     * Skip the GUI (boot straight in) when ANY of: PSX_NO_LAUNCHER=1 env,
     * --no-launcher, or the persisted [launcher] skip_launcher setting — unless
     * --launcher forces it back on (mirrors snesrecomp's SkipLauncher / --launcher).
     * This removes the dismiss-the-launcher round-trip for scripted/debug runs. */
    const bool want_launcher =
        force_launcher ||
        (!std::getenv("PSX_NO_LAUNCHER") && !force_no_launcher && !skip_launcher_setting);
    if (want_launcher) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0) {
            PSXRecompV4::UserSettings seed;
            seed.renderer = g_video_renderer;             seed.has_renderer = true;
            seed.supersampling = g_video_scale;           seed.has_supersampling = true;
            seed.antialiasing = g_video_aa;               seed.has_antialiasing = true;
            seed.texture_filter = g_video_texfilter;      seed.has_texture_filter = true;
            seed.screen_kind = g_video_screen;            seed.has_screen_kind = true;
            seed.auto_skip_fmv = (g_auto_skip_fmv != 0);  seed.has_auto_skip_fmv = true;
            seed.turbo_loads = (g_turbo_loads_enabled != 0); seed.has_turbo_loads = true;
            seed.fast_boot = fast_boot;                   seed.has_fast_boot = true;
            seed.bios_hle  = bios_hle;                    seed.has_bios_hle  = true;
            seed.fullscreen = (g_fullscreen != 0);        seed.has_fullscreen = true;
            seed.frame_interpolation = (g_frame_interpolation != 0);
            seed.has_frame_interpolation = true;
            seed.frame_interpolation_fps = g_frame_interpolation_fps;
            seed.has_frame_interpolation_fps = true;
            seed.aspect_num = g_video_aspect_num;
            seed.aspect_den = g_video_aspect_den;         seed.has_aspect_ratio = true;
            seed.spu_hq = g_audio_spu_hq;                 seed.has_spu_hq = true;
            seed.skip_launcher = skip_launcher_setting;   seed.has_skip_launcher = true;
            if (bios_path && bios_path[0]) { seed.bios_path = bios_path; seed.has_bios_path = true; }
            if (!resolved_disc.empty())    { seed.disc_path = resolved_disc; seed.has_disc_path = true; }
            seed.memcard_dir = memcard_dir;          seed.has_memcard_dir = true;
            seed.memcard1_enabled = memcard1_enabled; seed.has_memcard1_enabled = true;
            seed.memcard2_enabled = memcard2_enabled; seed.has_memcard2_enabled = true;
            if (!memcard1_path.empty()) { seed.memcard1_path = memcard1_path; seed.has_memcard1_path = true; }
            if (!memcard2_path.empty()) { seed.memcard2_path = memcard2_path; seed.has_memcard2_path = true; }
            seed.language = resolved_language; seed.has_language = true;
            seed.p1_device = p1_device; seed.has_p1_device = true;
            seed.p2_device = p2_device; seed.has_p2_device = true;
            seed.p1_mode = p1_mode; seed.has_p1_mode = true;
            seed.p2_mode = p2_mode; seed.has_p2_mode = true;
            seed.deadzone = resolved_deadzone >= 0 ? resolved_deadzone : 12000;
            seed.has_deadzone = true;
            seed.window_width = g_video_win_w; seed.has_window_width = true;

            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

            /* Launcher opens at the same 4:3 size the game will, so there's no
             * jarring resize on LAUNCH. The dense dashboard needs a usable
             * minimum, so the launcher floor is 1280 wide even if the game
             * window is set smaller. */
            int lwin_w = g_video_win_w < 1280 ? 1280 : g_video_win_w;
            int lwin_h = 0;
            clamp_window_aspect(&lwin_w, &lwin_h, 4, 3);
            std::string lwin_title = (game_name.empty() ? std::string("PSX") : game_name)
                                     + " \xE2\x80\x94 Launcher";
            SDL_Window* lwin = SDL_CreateWindow(
                lwin_title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                lwin_w, lwin_h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
            psx_launcher::Result lr = psx_launcher::Result::Unavailable;
            if (lwin) {
                SDL_GLContext lctx = SDL_GL_CreateContext(lwin);
                if (lctx) {
                    SDL_GL_MakeCurrent(lwin, lctx);
                    SDL_GL_SetSwapInterval(1);
                    std::string assets = exe_dir_from_argv(argv[0]).string();
                    psx_launcher::GameInfo ginfo;
                    ginfo.name             = game_name.empty() ? nullptr : game_name.c_str();
                    ginfo.expected_serial  = game_id.empty()   ? nullptr : game_id.c_str();
                    ginfo.expected_crc     = game_disc_crc;
                    ginfo.has_expected_crc = game_has_disc_crc;
                    ginfo.allow_hybrid     = ctrl_allow_hybrid;
                    ginfo.lock_mode        = ctrl_lock_mode;
                    ginfo.locked_mode      = p1_mode;  /* force the game's declared mode (default_mode) */
                    ginfo.lock_device      = ctrl_lock_device;
                    ginfo.ws_offered       = ws_offered;
                    ginfo.ws_ultrawide_offered = ws_ultrawide_offered;
                    for (const auto& lo : lang_menu_options)
                        ginfo.languages.push_back({ lo.code, lo.label });
                    lr = psx_launcher::run(lwin, lctx, seed, ginfo, assets.c_str());
                    SDL_GL_DeleteContext(lctx);
                }
                SDL_DestroyWindow(lwin);
            }
            /* Reset GL attributes so the emulator window starts from defaults. */
            SDL_GL_ResetAttributes();

            if (lr == psx_launcher::Result::Quit) {
                std::fprintf(stdout, "psxrecomp: launcher closed; exiting.\n");
                return 0;
            }
            if (lr == psx_launcher::Result::Launch) {
                g_video_renderer  = seed.renderer;
                g_video_scale     = seed.supersampling;
                g_video_aa        = seed.antialiasing;
                g_video_texfilter = seed.texture_filter;
                g_video_screen    = seed.screen_kind;
                g_auto_skip_fmv   = seed.auto_skip_fmv ? 1 : 0;
                g_turbo_loads_enabled = seed.turbo_loads ? 1 : 0;
                fast_boot = seed.fast_boot;
                bios_hle  = seed.bios_hle;
                g_fullscreen      = seed.fullscreen ? 1 : 0;
                g_frame_interpolation = seed.frame_interpolation ? 1 : 0;
                g_frame_interpolation_fps = seed.frame_interpolation_fps;
                g_video_aspect_num = seed.aspect_num;
                g_video_aspect_den = seed.aspect_den;
                g_audio_spu_hq    = seed.spu_hq;
                skip_launcher_setting = seed.skip_launcher;
                if (seed.has_bios_path) {
                    settings_bios_storage = seed.bios_path.string();
                    bios_path = settings_bios_storage.c_str();
                }
                if (seed.has_disc_path) resolved_disc = seed.disc_path;
                memcard1_enabled = seed.memcard1_enabled;
                memcard2_enabled = seed.memcard2_enabled;
                if (seed.has_memcard1_path) memcard1_path = seed.memcard1_path;
                if (seed.has_memcard2_path) memcard2_path = seed.memcard2_path;
                if (seed.has_language) resolved_language = seed.language;
                p1_device = seed.p1_device; p2_device = seed.p2_device;
                p1_mode = seed.p1_mode; p2_mode = seed.p2_mode;
                if (seed.has_deadzone) resolved_deadzone = seed.deadzone;
                g_video_win_w = seed.window_width;
                /* Persist the user's choices next to the exe. */
                PSXRecompV4::save_user_settings(
                    exe_dir_from_argv(argv[0]) / "settings.toml", seed);
            }
        }
    }
#endif

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

    std::filesystem::path resolved_bios = resolve_bios_for_runtime(bios_path, argv[0]);
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
    std::string disc_path_str    = resolved_disc.string();

    std::fprintf(stdout, "psxrecomp runtime: loading BIOS from %s\n", bios_path_str.c_str());
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
    /* Select the renderer backend BEFORE gpu_init() (which runs gr_init ->
     * the backend's init on the VRAM buffer). Software is the default and the
     * fallback; an unavailable OpenGL backend reverts to software. */
    gr_set_backend(g_video_renderer == 2 ? GR_BACKEND_VULKAN :
                   g_video_renderer == 1 ? GR_BACKEND_OPENGL : GR_BACKEND_SOFTWARE);
    std::fprintf(stdout, "psxrecomp: renderer backend requested: %s\n",
                 g_video_renderer == 2 ? "vulkan" :
                 g_video_renderer == 1 ? "opengl" : "software");
    gpu_init();
    /* Internal-resolution supersampling (SSAA). Must follow gpu_init (which
     * runs sw_renderer_init). scale==1 is a no-op; >1 allocates the hi-res
     * VRAM mirror. */
    if (g_video_scale < 1) g_video_scale = 1;
    if (g_video_scale > SW_MAX_INTERNAL_SCALE) g_video_scale = SW_MAX_INTERNAL_SCALE;
    gr_set_scale(g_video_scale);
    g_video_scale = gr_scale(); /* reflect any clamp / alloc fallback */
    gr_set_texture_filter(g_video_texfilter);
    /* Display aspect. Identity at the default 4:3. The present letterbox uses
     * this aspect; native-wide fills it with a genuinely wider frame (no
     * stretch), squash mode stretches the 4:3 frame into it. */
    gl_renderer_set_display_aspect(g_video_aspect_num, g_video_aspect_den);
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
    /* Seed per-player device routing from the resolved [controller] config.
     * SDL controller handles are opened later (after SDL_Init); here we only
     * set the PSX-visible connection + pad type so the BIOS sees the right
     * ports during early boot. */
    set_player_device(g_players[0], p1_device, p1_mode);
    set_player_device(g_players[1], p2_device, p2_mode);
    for (int s = 0; s < 2; s++) {
        /* Dev-any-input keeps P1 connected even with no assigned controller so the
         * keyboard / any plugged-in controller can drive port 1 standalone. */
        const bool dev_p1 = (dev_any_input_enabled() && s == 0);
        sio_set_pad_connected(s, (g_players[s].kind != 0 || dev_p1) ? 1 : 0);
        sio_set_pad_analog(s, pad_mode_boot_analog(g_players[s].mode), 0x80, 0x80, 0x80, 0x80);
    }
    /* SPU float-shadow gate must be set before spu_init() (which runs
     * spu_shadow_reset()). Default OFF; PSX_AUDIO_SHADOW env overrides. */
    spu_shadow_set_enabled(g_audio_spu_hq ? 1 : 0);
    if (g_audio_spu_hq)
        std::fprintf(stdout, "psxrecomp: SPU float-shadow enabled (verified-enhancement)\n");
    spu_init();
    cdrom_init(disc_path_str.empty() ? NULL : disc_path_str.c_str());
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
    {
        int divisor = 1; /* default: authentic 1x timing */
        if (disc_speed == "instant") divisor = 0;
        else if (disc_speed == "4x") divisor = 4;
        else if (disc_speed == "2x") divisor = 2;
        /* Store for post-BIOS application; boot always runs at 1x so the
         * BIOS disc-init sequence sees correct timing. */
        cdrom_set_game_speed(divisor);
        if (instant_rate > 0) cdrom_set_instant_rate(instant_rate);
        if (divisor != 1)
            std::fprintf(stdout, "psxrecomp: disc_speed=%s (applied post-BIOS, "
                         "instant budget %d/frame)\n",
                         disc_speed.c_str(), cdrom_get_instant_rate());
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
    /* ...but HIDAPI's Xbox sub-driver is OFF by default on Windows (Xbox pads are
     * normally RAWINPUT/XInput there). With RAWINPUT disabled above, a PHYSICAL
     * Xbox One/Series controller would be claimed by nobody -> not a GameController
     * -> zero input (PS5 DualSense works regardless: its HIDAPI driver is on by
     * default). Enable the HIDAPI Xbox driver so HIDAPI handles Xbox pads too. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    load_input_config(argv[0]);
    /* The launcher / settings.toml / game.toml deadzone (when set) is the
     * user-facing authority; apply it over the input.ini value here, after
     * load_input_config has read input.ini. */
    if (resolved_deadzone >= 0)
        controller_deadzone = std::max(0, std::min(32767, resolved_deadzone));
    refresh_player_devices();  /* open SDL handles to match the player config */
#ifndef PSX_SDL_NO_AUDIO
    audio_trace_init();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want;
        SDL_AudioSpec have;
        SDL_zero(want);
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        const bool legacy = audio_legacy_mode();
        if (!legacy)
            want.callback = sdl_drc_callback;  /* pull model: bridge resamples + DRC */
        sdl_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                               legacy ? 0 : SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
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
            SDL_PauseAudioDevice(sdl_audio_device, 0);
        }
    }
#endif

    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (g_video_renderer == 1) win_flags |= SDL_WINDOW_OPENGL;
    if (g_video_renderer == 2) win_flags |= SDL_WINDOW_VULKAN;
    /* Fullscreen on launch (launcher "Fullscreen on launch" toggle). DESKTOP
     * fullscreen keeps the desktop resolution and letterboxes the image, matching
     * the in-game F11 / Alt+Enter hotkey behaviour. */
    if (g_fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
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

    /* Sync-to-host-refresh: with SDL PRESENTVSYNC on, a fixed 59.94 Hz wall-clock
     * pacer fights a 60.00 Hz panel — rendered frames slip onto an uneven vblank
     * count (2/3/1 beat) that reads as moving-object judder. If the panel is
     * within ~2% of 60 Hz, nudge the pacer to the exact panel period so the pacer
     * and vsync agree and 30fps content pads to a steady 2 refreshes each.
     * Non-~60Hz panels keep the PSX rate (vsync then governs; wrong-speed sim is
     * worse than a benign slow beat). */
    {
        SDL_DisplayMode dm;
        int disp_idx = SDL_GetWindowDisplayIndex(sdl_window);
        if (disp_idx >= 0 && SDL_GetCurrentDisplayMode(disp_idx, &dm) == 0 && dm.refresh_rate > 0) {
            double host_hz = (double)dm.refresh_rate;
            g_host_refresh_hz = host_hz;
            if (host_hz >= 58.8 && host_hz <= 61.2) {
                g_frame_period_ms = 1000.0 / host_hz;
                std::printf("psxrecomp: sync-to-host-refresh: pacing to %d Hz panel "
                            "(%.4f ms/frame)\n", dm.refresh_rate, g_frame_period_ms);
            } else {
                std::printf("psxrecomp: host panel %d Hz not ~60 Hz; keeping PSX "
                            "59.94 Hz pacing\n", dm.refresh_rate);
            }
        }
    }

    /* OpenGL backend: create the GL context now. On failure, relabel the
     * facade back to software (rasterization already runs through software in
     * this phase) and fall through to the SDL_Renderer present path below. */
    if (g_video_renderer == 1) {
        gl_renderer_set_swap_interval(g_video_vsync);   /* applied at context init */
        g_gl_active = (gl_renderer_init_context(sdl_window) != 0);
        if (!g_gl_active) gr_set_backend(GR_BACKEND_SOFTWARE);
        /* The GL backend establishes its real internal scale HERE (raster init),
         * which is AFTER the earlier `g_video_scale = gr_scale()` sync (that ran
         * before this and so saw the default scale 1). Re-sync now so the staging
         * buffer below is sized for the true scale and the native-wide present
         * (which uses gr_scale()) matches it — otherwise sdl_pixel_buf is
         * undersized and the wide readback overflows it. */
        g_video_scale = gr_scale();
        gl_renderer_set_interpolation(g_frame_interpolation, g_host_refresh_hz,
                                      (double)g_frame_interpolation_fps);
    }
    /* Vulkan backend: create the instance/device/swapchain on the
     * SDL_WINDOW_VULKAN window. On failure, fall back to software (vkb_init
     * already initialized the software renderer on the shared VRAM array). */
    if (g_video_renderer == 2) {
        vk_renderer_set_present_mode(g_video_vsync);
        g_vk_active = (vk_renderer_init_context(sdl_window) != 0);
        if (!g_vk_active) gr_set_backend(GR_BACKEND_SOFTWARE);
        g_video_scale = gr_scale();
    }
    latency_ring_set_backend(g_vk_active ? "vulkan" : g_gl_active ? "opengl" : "software");
    latency_ring_set_present_mode(g_video_vsync);
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
    /* Vsync off (g_video_vsync==0) drops PRESENTVSYNC for lowest display
     * latency; the wall-clock pacer still holds 59.94Hz (may tear). */
    Uint32 rflags = SDL_RENDERER_ACCELERATED |
                    (g_video_vsync != 0 ? SDL_RENDERER_PRESENTVSYNC : 0u);
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
     * unchanged. */
    g_logical_w = 480 * g_video_aspect_num * g_video_scale / g_video_aspect_den;
    SDL_RenderSetLogicalSize(sdl_renderer, g_logical_w, 480 * g_video_scale);
  }

    /* Staging buffer + backing texture are sized for the internal resolution
     * (640x512 native, times the supersampling factor). */
    sdl_pixel_buf = (uint32_t*)std::malloc(
        (size_t)640 * g_video_scale * 512 * g_video_scale * sizeof(uint32_t));
    if (!sdl_pixel_buf) {
        std::fprintf(stderr, "failed to allocate %dx staging buffer\n", g_video_scale);
        return 1;
    }

  if (!g_gl_active && !g_vk_active) {
    sdl_texture = SDL_CreateTexture(
        sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        640 * g_video_scale, 512 * g_video_scale
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
    /* Wire the sljit JIT host-helper table (cpu-relative => position-independent
     * shards; prerequisite for the persisted sljit shard cache). Harmless when
     * sljit isn't the active backend. */
    overlay_sljit_init_helpers(&cpu);

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
        const bool boot_skip =
            (bios_hle && !bios_hle_keep_intro) || fast_boot;
        psx_bios_hle_configure(bios_hle ? 1 : 0,
                               (boot_skip && game_entry_pc != 0) ? 1 : 0);
        std::fprintf(stdout, "psxrecomp: bios_backend=%s  bios_boot=%s\n",
                     psx_bios_hle_backend_name(),
                     psx_bios_hle_boot_skip_enabled()
                         ? "HLE (shell skipped)" : "LLE (real intro)");
    }

    /* R3000A reset state. */
    cpu.pc = 0xBFC00000u;
    cpu.cop0[12] = 0x00400000u; /* SR: BEV=1 (boot exception vectors) */

    /* User save states (F1-F12 / Shift+F1-F12): slots live in the per-game
     * memcard/save dir, keyed by entry_pc + guarded by the boot_state integrity
     * key (codegen hash/abi/ver) so a stale/foreign state can never load. */
    if (game_entry_pc != 0) {
        savestate_configure(memcard_dir.string().c_str(),
                            memory_get_bios_checksum(), game_entry_pc);
    }

    /* Let memory subsystem see SR for cache-isolation checks. */
    memory_set_sr_ptr(&cpu.cop0[12]);

    /* Wire debug server to CPU state for register queries. */
    debug_server_set_cpu(&cpu);

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

    psx_scheduler_run(&cpu);
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
    { extern uint64_t g_slice_fired, g_slice_irq_taken, g_dirty_ram_insns_run;
      extern uint32_t g_slice_exit_pc, g_slice_exit_reason, g_slice_exit_iter;
      extern uint32_t g_slice_exit_dispatchable, g_slice_exit_dirty, g_slice_exit_in_text, g_slice_exit_want;
      std::fprintf(stdout, "psxrecomp runtime: [slice diag] slice_fired=%llu slice_irq_taken=%llu dirty_insns=%llu exit_pc=0x%08X reason=%u iter=%u dispatchable=%u dirty=%u in_text=%u want=%u\n",
                   (unsigned long long)g_slice_fired, (unsigned long long)g_slice_irq_taken,
                   (unsigned long long)g_dirty_ram_insns_run, g_slice_exit_pc, g_slice_exit_reason,
                   g_slice_exit_iter, g_slice_exit_dispatchable, g_slice_exit_dirty,
                   g_slice_exit_in_text, g_slice_exit_want); }

    shutdown_runtime();
    if (g_gl_active) gl_renderer_shutdown();
    SDL_DestroyTexture(sdl_texture);   /* NULL-safe in GL mode */
    SDL_DestroyRenderer(sdl_renderer); /* NULL-safe in GL mode */
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();

    return 0;
}
