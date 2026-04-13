/* main.cpp — Phase 3 runtime entry point.
 *
 * Loads BIOS ROM, initializes CPU state + SDL display, calls into
 * the recompiled reset vector. BIOS drives execution; SDL presents
 * VRAM at each vblank via callback from gpu_vblank_tick().
 */

#include "cpu_state.h"
#include "gpu.h"
#include "sio.h"
#include "memcard.h"
#include "debug_server.h"
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

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
    if (override >= 0) {
        sio_set_pad_state((uint16_t)override);
    } else {
        update_pad_from_keyboard();
    }

    GpuDisplayInfo di;
    gpu_get_display_info(&di);

    if (di.disabled || di.width == 0 || di.height == 0) return;

    const uint16_t* vram = gpu_get_vram();
    uint32_t w = di.width;
    uint32_t h = di.height;

    /* Blit the display area from VRAM into the staging buffer. */
    for (uint32_t y = 0; y < h; y++) {
        uint32_t vy = (di.display_y + y) & 511;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t vx = (di.display_x + x) & 1023;
            sdl_pixel_buf[y * w + x] = psx16_to_argb(vram[vy * 1024 + vx]);
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

    const char* bios_path = "bios/SCPH1001.BIN";
    if (argc > 1) bios_path = argv[1];

    std::fprintf(stdout, "psxrecomp-v4 runtime: loading BIOS from %s\n", bios_path);
    memory_init(bios_path);
    gpu_init();
    dma_init();
    timers_init();
    interrupts_init();
    sio_init();
    sio_connect_pad(0);  /* Controller on port 1 */
    memcard_init(".");   /* Look for card1.mcd / card2.mcd in working directory */
    debug_server_init(4370);

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
    psx_dispatch(&cpu, cpu.pc);

    /* If we reach here, all execution completed without MMIO abort. */
    std::fprintf(stdout, "psxrecomp-v4 runtime: execution completed, PC=0x%08X\n", cpu.pc);

    debug_server_shutdown();
    SDL_DestroyTexture(sdl_texture);
    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();

    return 0;
}
