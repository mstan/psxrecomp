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
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" uint64_t gte_get_exec_count(void);

/* memory.c */
extern "C" void memory_init(const char* bios_path);
extern "C" void memory_set_sr_ptr(const uint32_t *p);

/* dma.c */
extern "C" void dma_init(void);

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
            debug_server_shutdown();
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

    /* Update texture and present. */
    SDL_UpdateTexture(sdl_texture, NULL, sdl_pixel_buf, (int)(w * sizeof(uint32_t)));

    SDL_Rect dst = { 0, 0, 640, 480 };
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, &dst);
    SDL_RenderPresent(sdl_renderer);
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

    const char* bios_path = "bios/SCPH1001.BIN";
    /* Parse positional. First non-flag arg = bios_path. */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            bios_path = argv[i];
        }
    }

    std::fprintf(stdout, "psxrecomp-v4 runtime: loading BIOS from %s\n", bios_path);
    memory_init(bios_path);
    gpu_init();
    dma_init();
    timers_init();
    interrupts_init();
    sio_init();
    sio_connect_pad(0);  /* Controller on port 1 */
    spu_init();
    cdrom_init(NULL);    /* No disc for BIOS-only boot */
    memcard_init(".");   /* Look for card1.mcd / card2.mcd in working directory */
#ifdef DEFAULT_DEBUG_PORT
    debug_server_init(DEFAULT_DEBUG_PORT);
#else
    debug_server_init(4370);
#endif

    /* ---- SDL init ---- */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    sdl_window = SDL_CreateWindow(
        "psxrecomp-v4 — BIOS",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_SHOWN
    );
    if (!sdl_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
    if (!sdl_renderer) {
        /* Fall back to software renderer. */
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
    }
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

    debug_server_shutdown();
    SDL_DestroyTexture(sdl_texture);
    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();

    return 0;
}
