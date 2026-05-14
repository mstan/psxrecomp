/* beetle_main.cpp — psx-beetle entry point.
 * Standalone harness around Beetle PSX libretro core. SDL window for
 * Beetle's framebuffer, keyboard input, TCP debug server on port 4380. */

#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
int  beetle_init(const char* bios_path);
int  beetle_init_with_disc(const char* bios_path, const char* disc_path);
void beetle_shutdown(void);
void beetle_run_frame(uint16_t pad1_buttons);
int  beetle_get_framebuffer(uint32_t **out_pixels, unsigned *out_w, unsigned *out_h);

void beetle_debug_server_init(int port);
void beetle_debug_server_poll(void);
void beetle_debug_server_shutdown(void);
int  beetle_debug_server_get_input_override(void);
}

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

static uint16_t pad_from_keyboard(void) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    uint16_t b = 0xFFFF;
    if (keys[SDL_SCANCODE_UP])      b &= ~PAD_UP;
    if (keys[SDL_SCANCODE_DOWN])    b &= ~PAD_DOWN;
    if (keys[SDL_SCANCODE_LEFT])    b &= ~PAD_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])   b &= ~PAD_RIGHT;
    if (keys[SDL_SCANCODE_RETURN])  b &= ~PAD_START;
    if (keys[SDL_SCANCODE_RSHIFT])  b &= ~PAD_SELECT;
    if (keys[SDL_SCANCODE_X])       b &= ~PAD_CROSS;
    if (keys[SDL_SCANCODE_Z])       b &= ~PAD_SQUARE;
    if (keys[SDL_SCANCODE_S])       b &= ~PAD_CIRCLE;
    if (keys[SDL_SCANCODE_A])       b &= ~PAD_TRIANGLE;
    if (keys[SDL_SCANCODE_Q])       b &= ~PAD_L1;
    if (keys[SDL_SCANCODE_W])       b &= ~PAD_R1;
    if (keys[SDL_SCANCODE_E])       b &= ~PAD_L2;
    if (keys[SDL_SCANCODE_R])       b &= ~PAD_R2;
    return b;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    const char* bios_path = "bios/SCPH1001.BIN";
    const char* disc_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--disc") && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (argv[i][0] != '-') {
            bios_path = argv[i];
        }
    }

    std::fprintf(stdout, "psx-beetle: loading BIOS from %s\n", bios_path);
    if (disc_path) {
        std::fprintf(stdout, "psx-beetle: loading disc from %s\n", disc_path);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow(
        "psx-beetle — Beetle PSX",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_SHOWN);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Software renderer only — see main.cpp note. GPU-driver-side hangs in
     * SDL_RenderPresent reproducibly wedge the SDL main thread mid-frame. */
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Texture* tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 512);
    if (!tex) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return 1;
    }

    if (beetle_init_with_disc(bios_path, disc_path) != 0) {
        std::fprintf(stderr, "beetle_init failed\n");
        return 1;
    }

#ifndef PSX_NO_DEBUG_TOOLS
#ifdef DEFAULT_DEBUG_PORT
    beetle_debug_server_init(DEFAULT_DEBUG_PORT);
#else
    beetle_debug_server_init(4380);
#endif
#endif

    static uint32_t pixels[640 * 512];

    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
#ifndef PSX_NO_DEBUG_TOOLS
                beetle_debug_server_shutdown();
#endif
                beetle_shutdown();
                SDL_DestroyTexture(tex);
                SDL_DestroyRenderer(ren);
                SDL_DestroyWindow(win);
                SDL_Quit();
                return 0;
            }
        }

#ifndef PSX_NO_DEBUG_TOOLS
        beetle_debug_server_poll();
        int override = beetle_debug_server_get_input_override();
#else
        int override = -1;
#endif
        uint16_t pad = (override >= 0) ? (uint16_t)override : pad_from_keyboard();

        beetle_run_frame(pad);

        uint32_t* fb = nullptr;
        unsigned w = 0, h = 0;
        if (beetle_get_framebuffer(&fb, &w, &h) && fb && w && h) {
            if (w > 640) w = 640;
            if (h > 512) h = 512;
            for (unsigned y = 0; y < h; y++) {
                for (unsigned x = 0; x < w; x++) {
                    pixels[y * w + x] = fb[y * w + x] | 0xFF000000u;
                }
            }
            SDL_UpdateTexture(tex, NULL, pixels, (int)(w * sizeof(uint32_t)));
            SDL_Rect dst = { 0, 0, 640, 480 };
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, &dst);
            SDL_RenderPresent(ren);
        }

        /* Wall-clock pacing to PSX-native 59.94 Hz for apples-to-apples
         * comparison with psx-runtime. Press TAB on the Beetle window to
         * sustain unlocked rate (turbo). */
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        if (keys && keys[SDL_SCANCODE_TAB]) continue;
        constexpr double FRAME_MS = 1000.0 / 59.94;
        static Uint64 deadline = 0;
        Uint64 freq = SDL_GetPerformanceFrequency();
        Uint64 period = (Uint64)((double)freq * (FRAME_MS / 1000.0));
        Uint64 now = SDL_GetPerformanceCounter();
        if (deadline == 0 || now >= deadline + period) {
            deadline = now + period;
        } else {
            while (SDL_GetPerformanceCounter() < deadline) {
                Uint64 left = deadline - SDL_GetPerformanceCounter();
                Uint64 ms = (left * 1000) / freq;
                if (ms >= 2) SDL_Delay((Uint32)(ms - 1));
                else break;
            }
            while (SDL_GetPerformanceCounter() < deadline) { /* spin */ }
            deadline += period;
        }
    }
}
