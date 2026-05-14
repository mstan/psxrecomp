/* main.cpp — Phase 3 runtime entry point.
 *
 * Loads BIOS ROM, initializes CPU state + SDL display, calls into
 * the recompiled reset vector. BIOS drives execution; SDL presents
 * VRAM at each vblank via callback from gpu_vblank_tick().
 */

#include "cpu_state.h"
#include "psx_interpreter.h"
#include "cdrom.h"
#include "gpu.h"
#include "sio.h"
#include "spu.h"
#include "memcard.h"
#include "debug_server.h"
#include "crash_trace.h"
#include "config_loader.h"
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#ifndef PSX_DEFAULT_BIOS_PATH
#define PSX_DEFAULT_BIOS_PATH "bios/SCPH1001.BIN"
#endif
#ifndef PSX_WINDOW_TITLE
#define PSX_WINDOW_TITLE "psxrecomp-v4"
#endif
#ifndef DEFAULT_DEBUG_PORT
#error DEFAULT_DEBUG_PORT must be defined by the runtime target.
#endif

extern "C" uint64_t gte_get_exec_count(void);

/* memory.c */
extern "C" void memory_init(const char* bios_path);
extern "C" void memory_set_sr_ptr(const uint32_t *p);

/* dma.c */
extern "C" void dma_init(void);

/* mdec.c */
extern "C" void mdec_init(void);

/* timers.c */
extern "C" void timers_init(void);

/* interrupts.c */
extern "C" void interrupts_init(void);
extern "C" uint32_t psx_read_word(uint32_t addr);
extern "C" void     psx_write_word(uint32_t addr, uint32_t val);
extern "C" uint16_t psx_read_half(uint32_t addr);
extern "C" void     psx_write_half(uint32_t addr, uint16_t val);
extern "C" uint8_t  psx_read_byte(uint32_t addr);
extern "C" void     psx_write_byte(uint32_t addr, uint8_t val);

/* ---- SDL state ---- */
static SDL_Window*   sdl_window;
static SDL_Renderer* sdl_renderer;
static SDL_Texture*  sdl_texture;
static uint32_t      sdl_pixel_buf[640 * 512]; /* ARGB8888 staging buffer */
static SDL_AudioDeviceID sdl_audio_device;
static int16_t       sdl_audio_buf[2048 * 2];

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

static std::filesystem::path resolve_bios_path(const char* requested, const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(requested);
    if (fs::exists(p, ec)) {
        fs::path abs = fs::absolute(p, ec);
        return ec ? p : abs;
    }
    if (p.is_absolute()) return p;

    std::vector<fs::path> roots;
    roots.push_back(fs::current_path());
    if (argv0 && argv0[0]) roots.push_back(fs::absolute(argv0, ec).parent_path());

    for (const fs::path& root : roots) {
        fs::path found = find_upward(root, p);
        if (!found.empty()) return found / p;
    }
    return p;
}

// Fallback memcard directory used when no game config (or its [runtime]
// block) specifies one. Prefer the executable's directory so saves live
// next to the binary; fall back to the current working directory.
static std::filesystem::path default_memcard_dir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe_dir;
    if (argv0 && argv0[0]) {
        exe_dir = fs::absolute(argv0, ec).parent_path();
        if (ec) exe_dir.clear();
    }
    if (exe_dir.empty()) exe_dir = fs::current_path();
    return exe_dir;
}

static void shutdown_runtime(void) {
    memcard_flush_all();
    if (sdl_audio_device) {
        SDL_ClearQueuedAudio(sdl_audio_device);
        SDL_CloseAudioDevice(sdl_audio_device);
        sdl_audio_device = 0;
    }
    debug_server_shutdown();
}

static void sdl_audio_pump(void) {
    if (!sdl_audio_device) return;

    const uint32_t bytes_per_frame = sizeof(int16_t) * 2u;
    const uint32_t max_queue_bytes = 44100u * bytes_per_frame / 5u;
    if (SDL_GetQueuedAudioSize(sdl_audio_device) > max_queue_bytes) return;

    static double sample_accum = 0.0;
    sample_accum += 44100.0 / 60.0;
    int frames = (int)sample_accum;
    sample_accum -= (double)frames;
    if (frames <= 0) return;
    if (frames > 2048) frames = 2048;

    spu_render(sdl_audio_buf, frames);
    SDL_QueueAudio(sdl_audio_device, sdl_audio_buf,
                   (uint32_t)frames * bytes_per_frame);
}

/* Convert PS1 16-bit color (1-5-5-5) to ARGB8888 */
static inline uint32_t psx16_to_argb(uint16_t c) {
    uint32_t r = (c & 0x1F) << 3;
    uint32_t g = ((c >> 5) & 0x1F) << 3;
    uint32_t b = ((c >> 10) & 0x1F) << 3;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* PS1 digital pad button bits (active-low: 0=pressed, 1=released).
 * Bit 0 = SELECT, Bit 3 = START, Bit 4 = UP, Bit 5 = RIGHT,
 * Bit 6 = DOWN, Bit 7 = LEFT, Bit 8 = L2, Bit 9 = R2,
 * Bit 10 = L1, Bit 11 = R1, Bit 12 = TRIANGLE, Bit 13 = CIRCLE,
 * Bit 14 = CROSS, Bit 15 = SQUARE */
#define PAD_SELECT   (1 << 0)
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

static void update_pad_from_keyboard(void) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    uint16_t buttons = 0xFFFF; /* all released */

    if (keys[SDL_SCANCODE_UP])      buttons &= ~PAD_UP;
    if (keys[SDL_SCANCODE_DOWN])    buttons &= ~PAD_DOWN;
    if (keys[SDL_SCANCODE_LEFT])    buttons &= ~PAD_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])   buttons &= ~PAD_RIGHT;
    if (keys[SDL_SCANCODE_RETURN])  buttons &= ~PAD_START;
    if (keys[SDL_SCANCODE_RSHIFT])  buttons &= ~PAD_SELECT;
    if (keys[SDL_SCANCODE_X])       buttons &= ~PAD_CROSS;
    if (keys[SDL_SCANCODE_Z])       buttons &= ~PAD_SQUARE;
    if (keys[SDL_SCANCODE_S])       buttons &= ~PAD_CIRCLE;
    if (keys[SDL_SCANCODE_A])       buttons &= ~PAD_TRIANGLE;
    if (keys[SDL_SCANCODE_Q])       buttons &= ~PAD_L1;
    if (keys[SDL_SCANCODE_W])       buttons &= ~PAD_R1;
    if (keys[SDL_SCANCODE_E])       buttons &= ~PAD_L2;
    if (keys[SDL_SCANCODE_R])       buttons &= ~PAD_R2;

    sio_set_pad_state(buttons);
}

/* PSX native vblank cadence: NTSC ≈ 59.94 Hz. Wall-clock target keeps
 * audio sample generation (735 samples/vblank * 60 = 44100/sec) matched
 * to the SDL audio device drain rate, eliminating queue overflow drops
 * and underruns. Uncapped, the host runs the simulation at whatever
 * speed it can — typically several × realtime — and audio glitches. */
static constexpr double PSX_FRAME_PERIOD_MS = 1000.0 / 59.94;

/* Called from gpu_vblank_tick() at each simulated vblank. */
static void sdl_vblank_present(void) {
    /* Debug server: pause gate, poll commands, record frame, check watchpoints. */
    debug_server_wait_if_paused();
    debug_server_poll();
    debug_server_record_frame();
    debug_server_check_watchpoints();

    /* Check debug server input override. */
    int override = debug_server_get_input_override();

    /* Pump SDL events to prevent window freeze. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            shutdown_runtime();
            std::exit(0);
        }
    }

    /* Sample keyboard state and feed into SIO controller.
     * Debug server input override takes priority if active. */
    uint16_t pad_buttons_this_frame;
    if (override >= 0) {
        pad_buttons_this_frame = (uint16_t)override;
    } else {
        update_pad_from_keyboard();
        pad_buttons_this_frame = sio_get_pad_buttons();
    }
    sio_set_pad_state(pad_buttons_this_frame);
    sdl_audio_pump();

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

    /* ---- Display from our VRAM ---- */
    uint32_t w = 0, h = 0;
    {
        GpuDisplayInfo di;
        gpu_get_display_info(&di);
        if (di.disabled || di.width == 0 || di.height == 0) return;
        const uint16_t* vram = gpu_get_vram();
        w = di.width; h = di.height;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t vy = (di.display_y + y) & 511;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t vx = (di.display_x + x) & 1023;
                sdl_pixel_buf[y * w + x] = psx16_to_argb(vram[vy * 1024 + vx]);
            }
        }
    }

    /* Update only the active display rectangle. The backing texture is fixed
     * at 640x512, while games can switch to smaller modes such as 320x224 for
     * FMV; presenting the full texture would leave the active image stuck in
     * the upper-left portion of the window. */
    SDL_Rect src = { 0, 0, (int)w, (int)h };
    SDL_UpdateTexture(sdl_texture, &src, sdl_pixel_buf, (int)(w * sizeof(uint32_t)));

    SDL_Rect dst = { 0, 0, 640, 480 };
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, &src, &dst);
    SDL_RenderPresent(sdl_renderer);

    /* Wall-clock pacing: hold each simulated vblank to ~16.68 ms so the
     * BIOS runs at PSX-native 59.94 Hz. Uses SDL's high-resolution
     * counter — works around MinGW std::thread quirks. If a frame ran
     * long the deadline resyncs to "now" rather than queueing catch-up
     * frames. */
    {
        static Uint64 next_deadline = 0;
        Uint64 freq = SDL_GetPerformanceFrequency();
        Uint64 period = (Uint64)((double)freq * (PSX_FRAME_PERIOD_MS / 1000.0));
        Uint64 now = SDL_GetPerformanceCounter();
        if (next_deadline == 0 || now >= next_deadline + period) {
            next_deadline = now + period;  /* first frame, or fell behind */
        } else {
            while (SDL_GetPerformanceCounter() < next_deadline) {
                Uint64 remaining_ticks = next_deadline - SDL_GetPerformanceCounter();
                Uint64 remaining_ms = (remaining_ticks * 1000) / freq;
                if (remaining_ms >= 2) {
                    SDL_Delay((Uint32)(remaining_ms - 1));
                } else {
                    /* Final spin for sub-ms accuracy. */
                    break;
                }
            }
            while (SDL_GetPerformanceCounter() < next_deadline) {
                /* spin */
            }
            next_deadline += period;
        }
    }
}

int main(int argc, char** argv) {
    /* Force line-buffered output so messages appear even if killed. */
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::fprintf(stderr, "psxrecomp-v4: main() entered\n");
    std::fflush(stderr);

    /* Install crash handlers early so they catch issues during init too.
     * Writes psx_last_run_report.json on signal/SEH/atexit/fail-fast. */
    psx_crash_trace_install_handlers();

    const char* bios_path = PSX_DEFAULT_BIOS_PATH;
    const char* game_config_path = nullptr;
    /* Parse args.
     *   --bios <path>       override the compile-time BIOS path
     *   --game <toml>       load a game config (single source of truth for
     *                       disc / memcard / window title / debug port)
     *   <positional>        deprecated alias for --bios
     * No --disc / --memcard-dir / --game-root flags: those are config-driven. */
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
        } else if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            game_config_path = argv[++i];
        } else if (argv[i][0] != '-') {
            bios_path = argv[i];
        }
    }

    std::filesystem::path resolved_bios = resolve_bios_path(bios_path, argv[0]);
    std::filesystem::path memcard_dir;
    std::filesystem::path resolved_disc;
    std::string window_title = PSX_WINDOW_TITLE;
    uint16_t   debug_port    = (uint16_t)DEFAULT_DEBUG_PORT;
    std::string game_name;
    std::string game_id;

    if (game_config_path) {
        try {
            const auto gc = PSXRecompV4::load_game_config(game_config_path);
            game_name = gc.name;
            game_id   = gc.id;
            if (!gc.discs.empty()) resolved_disc = gc.discs.front();
            if (gc.runtime.has_memcard_dir)  memcard_dir   = gc.runtime.memcard_dir;
            if (gc.runtime.has_window_title) window_title  = gc.runtime.window_title;
            if (gc.runtime.has_debug_port)   debug_port    = gc.runtime.debug_port;
            std::fprintf(stdout, "psxrecomp-v4: loaded game config %s (%s, %s)\n",
                         game_config_path, game_name.c_str(), game_id.c_str());
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "psxrecomp-v4: failed to load --game %s: %s\n",
                         game_config_path, ex.what());
            return 1;
        }
    }

    if (memcard_dir.empty()) {
        memcard_dir = default_memcard_dir(argv[0]);
    }

    std::string bios_path_str    = resolved_bios.string();
    std::string memcard_dir_str  = memcard_dir.string();
    std::string disc_path_str    = resolved_disc.string();

    std::fprintf(stdout, "psxrecomp-v4 runtime: loading BIOS from %s\n", bios_path_str.c_str());
    memory_init(bios_path_str.c_str());
    gpu_init();
    dma_init();
    mdec_init();
    timers_init();
    interrupts_init();
    sio_init();
    sio_connect_pad(0);  /* Controller on port 1 */
    spu_init();
    cdrom_init(disc_path_str.empty() ? NULL : disc_path_str.c_str());
    memcard_init(memcard_dir_str.c_str());
    std::atexit(memcard_flush_all);
    debug_server_init(debug_port);

    /* ---- SDL init ---- */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want;
        SDL_AudioSpec have;
        SDL_zero(want);
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        sdl_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (sdl_audio_device) {
            SDL_PauseAudioDevice(sdl_audio_device, 0);
        }
    }

    sdl_window = SDL_CreateWindow(
        window_title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_SHOWN
    );
    if (!sdl_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Software renderer only: the accelerated path lets SDL_RenderPresent
     * block inside the GPU driver, which has reproducibly hung the main
     * thread mid-frame after extended uptime (TombaRecomp/ISSUES.md #6).
     * Software costs a few % more CPU but cannot stall in the GPU stack. */
    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
    if (!sdl_renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    sdl_texture = SDL_CreateTexture(
        sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        640, 512
    );
    if (!sdl_texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureScaleMode(sdl_texture, SDL_ScaleModeNearest);

    /* Register vblank presentation callback. */
    gpu_set_vblank_callback(sdl_vblank_present);

    /* Initialize CPU state. */
    CPUState cpu;
    std::memset(&cpu, 0, sizeof(cpu));

    /* Wire memory function pointers. */
    cpu.read_word  = psx_read_word;
    cpu.write_word = psx_write_word;
    cpu.read_half  = psx_read_half;
    cpu.write_half = psx_write_half;
    cpu.read_byte  = psx_read_byte;
    cpu.write_byte = psx_write_byte;

    /* R3000A reset state. */
    cpu.pc = 0xBFC00000u;
    cpu.cop0[12] = 0x00400000u; /* SR: BEV=1 (boot exception vectors) */

    /* Let memory subsystem see SR for cache-isolation checks. */
    memory_set_sr_ptr(&cpu.cop0[12]);

    /* Wire debug server to CPU state for register queries. */
    debug_server_set_cpu(&cpu);

    /* Execute. */
    std::fprintf(stdout, "psxrecomp-v4 runtime: executing from PC=0x%08X\n", cpu.pc);

#if defined(PSX_ORACLE_BUILD)
    std::fprintf(stdout, "psxrecomp-v4 ORACLE: interpreter mode (port %d)\n", DEFAULT_DEBUG_PORT);
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
    psx_dispatch(&cpu, cpu.pc);
#endif

    /* If we reach here, all execution completed without MMIO abort. */
    std::fprintf(stdout, "psxrecomp-v4 runtime: execution completed, PC=0x%08X\n", cpu.pc);

    shutdown_runtime();
    SDL_DestroyTexture(sdl_texture);
    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();

    return 0;
}
