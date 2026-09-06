#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>

#include "gpu_render.h"
#include "gpu_gl_renderer.h"
#include "gpu_vk_renderer.h"
#include "gpu_ng_renderer.h"

#define VRAM_W 1024
#define VRAM_H 512
#define VRAM_PIXELS (VRAM_W * VRAM_H)

typedef struct Options {
    GrBackend backend;
    const char *backend_name;
    const char *out_dir;
    int warmup;
    int repeats;
    int iters;
    int scale;
    int skip_wide;
    int contracts;
    int present;
} Options;

typedef struct BackendRuntime {
    SDL_Window *window;
} BackendRuntime;

void renderer_probe_set_vram(uint16_t *vram);

static uint16_t g_vram[VRAM_PIXELS];
static uint16_t g_readback[VRAM_PIXELS];

static uint16_t rgb555(int r, int g, int b)
{
    return (uint16_t)(((r & 31) << 0) | ((g & 31) << 5) | ((b & 31) << 10));
}

static uint16_t rgb555_masked(int r, int g, int b)
{
    return (uint16_t)(rgb555(r, g, b) | 0x8000u);
}

static uint16_t texpage_16bpp(int x, int y, int abr)
{
    return (uint16_t)(((x / 64) & 0x0f) | (((y / 256) & 1) << 4) | ((abr & 3) << 5) | (2 << 7));
}

static uint64_t hash_vram(const uint16_t *data)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < VRAM_PIXELS * sizeof(uint16_t); i++) {
        h ^= (uint64_t)p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static double now_ms(void)
{
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static int ensure_dir(const char *path)
{
    if (!path || !path[0]) return -1;
    if (SDL_CreateDirectory(path)) return 0;
    if (SDL_GetError()[0] == '\0') return 0;
    return -1;
}

static void upload_texture_patterns(void)
{
    enum { TX = 768, TY = 0, TW = 64, TH = 64 };
    uint16_t tex[TW * TH];
    uint16_t clut[256];
    uint16_t indexed4[8 * 32], indexed8[16 * 32];

    for (int i = 0; i < 256; i++) {
        clut[i] = rgb555((i * 3) & 31, (31 - i * 2) & 31, (i * 5) & 31);
    }
    clut[0] = 0; /* transparent palette entry */
    gr_vram_transfer_in(0, 480, 256, 1, clut);
    for (int i = 0; i < 8 * 32; ++i) indexed4[i] = (uint16_t)(0x3210u + ((i & 3) * 0x4444u));
    for (int i = 0; i < 16 * 32; ++i) indexed8[i] = (uint16_t)((i & 255) | (((i + 1) & 255) << 8));
    gr_vram_transfer_in(896, 0, 8, 32, indexed4);
    gr_vram_transfer_in(960, 0, 16, 32, indexed8);

    for (int y = 0; y < TH; y++) {
        for (int x = 0; x < TW; x++) {
            int r = (x * 31) / (TW - 1);
            int g = (y * 31) / (TH - 1);
            int b = ((x ^ y) * 31) / 63;
            tex[y * TW + x] = rgb555(r, g, b) | (((x ^ y) & 8) ? 0x8000u : 0);
        }
    }
    gr_vram_transfer_in(TX, TY, TW, TH, tex);
}

static void reset_render_state(void)
{
    gr_set_texture_filter(0);
    gr_set_draw_area(0, 0, VRAM_W - 1, VRAM_H - 1);
    gr_set_draw_offset(0, 0);
    gr_set_texture_window(0);
    gr_set_color_modulation(128, 128, 128, 1);
    gr_set_semi_transparency(0, 0);
    gr_set_mask_bits(0, 0);
    gr_set_precise_triangle(0, 0, 0, 0, 0, 0, 0);
    gr_set_perspective_triangle(0, 1.0f, 1.0f, 1.0f);
}

static void draw_scene(int iter)
{
    const uint16_t tp0 = texpage_16bpp(768, 0, 0);
    const uint16_t tp1 = texpage_16bpp(768, 0, 1);
    (void)iter;

    reset_render_state();
    gr_fill_rect(0, 0, VRAM_W, VRAM_H, 0);
    upload_texture_patterns();

    gr_draw_flat_rect(16, 16, 80, 40, rgb555(31, 0, 0));
    gr_draw_flat_rect(36, 32, 80, 48, rgb555(0, 24, 0));
    gr_draw_line(8, 8, 200, 64, rgb555(31, 31, 0));
    gr_draw_shaded_line(8, 72, rgb555(0, 0, 31), 200, 96, rgb555(31, 31, 31));

    gr_draw_flat_triangle(32, 128, 112, 92, 148, 188, rgb555(0, 0, 31));
    gr_draw_gouraud_triangle(160, 96, rgb555(31, 0, 0),
                             248, 128, rgb555(0, 31, 0),
                             192, 208, rgb555(0, 0, 31));

    gr_draw_textured_rect(272, 24, 48, 48, 0, 0, 0, 0, tp0);
    gr_draw_textured_rect_scaled(336, 24, 72, 40, 0, 0, 63, 31, 0, 0, tp0);
    gr_draw_textured_triangle(272, 104, 0, 0,
                              368, 112, 63, 0,
                              316, 204, 24, 63,
                              0, 0, tp0);

    gr_set_color_modulation(96, 160, 224, 0);
    gr_draw_shaded_textured_triangle(424, 96, 0, 0, 0x804020,
                                     520, 112, 63, 0, 0x20e050,
                                     468, 204, 24, 63, 0x2040ff,
                                     0, 0, tp0, 0);
    gr_set_color_modulation(128, 128, 128, 1);

    gr_draw_flat_rect(552, 24, 80, 48, rgb555(0, 0, 18));
    gr_set_semi_transparency(1, 0);
    gr_draw_flat_rect(568, 40, 80, 48, rgb555(31, 0, 0));
    gr_set_semi_transparency(1, 1);
    gr_draw_textured_rect(640, 40, 48, 48, 8, 8, 0, 0, tp1);
    gr_set_semi_transparency(0, 0);

    gr_set_semi_transparency(1, 2);
    gr_draw_flat_rect(576, 48, 32, 32, rgb555(8, 8, 8));
    gr_set_semi_transparency(1, 3);
    gr_draw_flat_rect(608, 48, 32, 32, rgb555(16, 16, 16));
    gr_set_semi_transparency(0, 0);
    gr_draw_textured_rect(16, 380, 32, 32, 0, 0, 0, 480, 14);
    gr_draw_textured_rect(80, 380, 32, 32, 0, 0, 0, 480, 15 | (1 << 7));
    gr_set_texture_window(1 | (1 << 5) | (1 << 10) | (1 << 15));
    gr_draw_textured_rect(144, 380, 32, 32, 0, 0, 0, 480, 14);
    gr_set_texture_window(0);

    gr_set_mask_bits(1, 0);
    gr_draw_flat_rect(32, 232, 80, 48, rgb555_masked(31, 31, 0));
    gr_set_mask_bits(0, 1);
    gr_draw_flat_rect(56, 248, 80, 48, rgb555(0, 31, 31));
    gr_set_mask_bits(0, 0);

    gr_copy_rect(16, 16, 176, 232, 96, 64);
    gr_copy_rect(272, 24, 320, 232, 64, 64);
    gr_draw_textured_rect(408, 232, 64, 64, 0, 232, 0, 0, texpage_16bpp(320, 0, 0));

}


/* A regression test, outside the timed workload. Exact upload colors avoid
 * conflating rasterization/color rounding differences with wide coherence. */
static int wide_smoke_check(const char *backend_name)
{
    int S = gr_scale(), W = 384 * S, H = 512 * S, PITCH = W + 8;
    uint32_t *wide = NULL, *present = NULL;
    uint32_t small[4] = {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
    int ow = 0, oh = 0;
    int is_vk = strcmp(backend_name, "vulkan") == 0;
    int result = -1;
    if (!gr_wide_supported()) return 0;
    wide = malloc((size_t)W * H * sizeof(*wide));
    present = malloc((size_t)PITCH * 64 * S * sizeof(*present));
    if (!wide || !present) goto done;
    reset_render_state();
    gr_fill_rect(0, 0, VRAM_W, VRAM_H, 0);
    gr_wide_configure(384, 32);
    gr_wide_set_target(0);
    gr_wide_clear(0, 0, 512, rgb555(2, 2, 2));
    gr_draw_flat_rect(100, 32, 8, 8, rgb555(31, 0, 0));
    gr_draw_flat_rect(100, 320, 8, 8, rgb555(31, 0, 0));
    /* These uploads are not mirrored geometry: the centre must refresh from
     * canonical VRAM, and the revealed margins must stay untouched. */
    if (is_vk) {
        gr_vram_write(100, 32, rgb555(0, 31, 0));
        gr_vram_write(100, 320, rgb555(0, 31, 0));
        ow = 123; oh = 456;
        if (gr_wide_dump_full(small, 0, &ow, &oh, 0) != 0 ||
            gr_wide_dump_full(small, 1, &ow, &oh, 0) != 0 ||
            gr_wide_dump_full(small, W * H - 1, &ow, &oh, 0) != 0 ||
            small[0] != 0xdeadbeef || ow != 123 || oh != 456) {
            fprintf(stderr, "wide capacity regression\n");
            goto done;
        }
    }
    if (gr_wide_dump_full(wide, W * H, &ow, &oh, 0) != W * H || ow != W || oh != H) {
        fprintf(stderr, "%s wide full dimensions regression\n", backend_name);
        goto done;
    }
    if (is_vk && (wide[32 * S * W + 132 * S] != 0xff00f800 ||
                  wide[320 * S * W + 132 * S] != 0xff00f800 ||
                  wide[32 * S * W] == 0xff000000 || wide[32 * S * W + W - 1] == 0xff000000)) {
        fprintf(stderr, "wide full canonical/margins regression: %08x %08x\n",
                wide[32 * S * W + 132 * S], wide[320 * S * W + 132 * S]);
        goto done;
    }
    /* Mutate after full dump to ensure band readback independently refreshes. */
    if (is_vk) gr_vram_write(100, 32, rgb555(0, 0, 31));
    for (int i = 0; i < PITCH * 64 * S; ++i) present[i] = 0xdeadbeef;
    if (gr_render_wide_display(present, PITCH * (int)sizeof(uint32_t), 0, 0, 64) != W * 64 * S ||
        (is_vk && present[32 * S * PITCH + 132 * S] != 0xff0000f8)) {
        fprintf(stderr, "%s wide band canonical regression: %08x\n", backend_name, present[32 * S * PITCH + 132 * S]);
        goto done;
    }
    for (int y = 0; y < 64 * S; ++y)
        for (int x = W; x < PITCH; ++x)
            if (present[y * PITCH + x] != 0xdeadbeef) goto done;
    result = 0;
done:
    gr_wide_disable_target();
    gr_wide_configure(0, 0);
    free(wide);
    free(present);
    return result;
}
static int dump_raw(const char *out_dir, const char *backend, int repeat, const uint16_t *data)
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s-r%d.vram16", out_dir, backend, repeat);
    if (n <= 0 || n >= (int)sizeof(path)) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t wrote = fwrite(data, sizeof(uint16_t), VRAM_PIXELS, f);
    int close_ok = fclose(f);
    return (wrote == VRAM_PIXELS && close_ok == 0) ? 0 : -1;
}

/* Small analytical contracts run outside timing. They catch an inert backend,
 * ignored masks, wrong CLUT decoding and broken wrapped/odd-width transfers
 * without treating another renderer's rasterization as a hardware oracle. */
static int expect_pixel(int x, int y, uint16_t expected, const char *operation)
{
    uint16_t actual = gr_vram_read(x, y);
    if (actual == expected) return 0;
    fprintf(stderr, "%s at (%d,%d): expected %04x, got %04x\n", operation, x, y, expected, actual);
    return -1;
}

static int pixel_contract_check(void)
{
    const uint16_t red = rgb555(31, 0, 0), green = rgb555(0, 31, 0);
    const uint16_t odd[] = {1, 2, 3, 4, 5, 6};
    uint16_t copy[6] = {0};
    uint16_t palette[16] = {0};
    uint16_t indices = 0x0010;
    reset_render_state();
    gr_fill_rect(0, 0, VRAM_W, VRAM_H, 0);
    gr_vram_transfer_in(1023, 511, 3, 2, odd);
    gr_vram_transfer_out(1023, 511, 3, 2, copy);
    if (memcmp(odd, copy, sizeof(odd))) {
        fprintf(stderr, "wrapped odd-width transfer regression\n");
        return -1;
    }
    gr_copy_rect(1023,511,110,110,3,2);
    gr_vram_transfer_out(110,110,3,2,copy);
    if (memcmp(odd,copy,sizeof(odd))) {
        fprintf(stderr, "wrapped source VRAM copy regression\n");
        return -1;
    }
    gr_draw_flat_rect(20, 20, 3, 2, red);
    if (expect_pixel(20,20,red,"flat rect") || expect_pixel(22,21,red,"flat rect extent") ||
        expect_pixel(23,21,0,"flat rect exclusive edge")) return -1;
    gr_set_mask_bits(1, 0);
    gr_draw_flat_rect(20, 20, 1, 1, green);
    gr_set_mask_bits(0, 1);
    gr_draw_flat_rect(20, 20, 1, 1, red);
    if (expect_pixel(20,20,(uint16_t)(green | 0x8000),"mask protection")) return -1;
    gr_set_mask_bits(0, 0);
    gr_set_draw_area(31,31,32,32);
    gr_set_draw_offset(10,10);
    /* gpu.c applies GP0 drawing offsets before calling this facade. */
    gr_draw_flat_rect(30,30,4,4,red);
    if (expect_pixel(30,30,0,"draw area clipping") || expect_pixel(31,31,red,"offset not applied twice")) return -1;
    reset_render_state();
    palette[1] = green;
    gr_vram_transfer_in(0,480,16,1,palette);
    gr_vram_transfer_in(768,0,1,1,&indices);
    gr_draw_flat_rect(40,40,2,1,red);
    gr_draw_textured_rect(40,40,2,1,0,0,0,480,12); /* 4-bit page x=768 */
    if (expect_pixel(40,40,red,"transparent CLUT entry") || expect_pixel(41,40,green,"4-bit CLUT")) return -1;
    gr_copy_rect(40,40,60,60,2,1);
    if (expect_pixel(60,60,red,"VRAM copy") || expect_pixel(61,60,green,"VRAM copy extent")) return -1;
    {
        const uint16_t back = rgb555(12,12,12), front = rgb555(8,8,8);
        const int expected[] = {10,20,4,14};
        for (int mode = 0; mode < 4; ++mode) {
            gr_set_semi_transparency(0,0);
            gr_draw_flat_rect(80,80,4,4,back);
            gr_set_semi_transparency(1,mode);
            gr_draw_flat_rect(80,80,4,4,front);
            for (int y = 80; y < 84; ++y)
                for (int x = 80; x < 84; ++x)
                    if (expect_pixel(x,y,rgb555(expected[mode],expected[mode],expected[mode]),"semi-transparent rectangle")) return -1;
        }
        gr_set_semi_transparency(0,0);
        gr_draw_flat_rect(80,80,4,4,back);
        gr_set_semi_transparency(1,1);
        gr_draw_flat_triangle(80,80,84,80,80,84,front);
        gr_draw_flat_triangle(84,80,84,84,80,84,front);
        for (int y = 80; y < 84; ++y)
            for (int x = 80; x < 84; ++x)
                if (expect_pixel(x,y,rgb555(20,20,20),"shared triangle edge blends once")) return -1;
    }
    reset_render_state();
    {
        uint16_t texel = (uint16_t)(green | 0x8000);
        gr_vram_transfer_in(768,0,1,1,&texel);
        gr_draw_textured_rect(90,90,1,1,0,0,0,0,texpage_16bpp(768,0,0));
        /* Existing software drops texture STP on write; the new backend's
         * contract preserves it, as native Vulkan's texture shader does. */
        uint16_t stored = gr_backend() == GR_BACKEND_SOFTWARE ? green : texel;
        if (expect_pixel(90,90,stored,"textured mask bit")) return -1;
        gr_draw_flat_rect(92,92,1,1,red);
        gr_set_semi_transparency(1,0);
        gr_draw_textured_rect(92,92,1,1,0,0,0,0,texpage_16bpp(768,0,0));
        uint16_t blended = rgb555(15,15,0);
        if (gr_backend() != GR_BACKEND_SOFTWARE) blended |= 0x8000;
        if (expect_pixel(92,92,blended,"textured blending preserves STP")) return -1;
        gr_set_semi_transparency(0,0);
        /* A following partial fill must not discard pending GPU writes from
         * the CPU mirror, which is consumed by savestates and debug tools. */
        gr_draw_flat_rect(91,91,1,1,green);
        gr_fill_rect(100,100,1,1,red);
        if (expect_pixel(91,91,green,"partial fill retains prior GPU writes")) return -1;
    }
    reset_render_state();
    return 0;
}

static int init_backend(const Options *opt, BackendRuntime *rt)
{
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    gr_set_backend(opt->backend);
    renderer_probe_set_vram(g_vram);
    gr_init(g_vram);
    gr_set_scale(opt->scale);

    if (opt->backend == GR_BACKEND_SOFTWARE) {
        return 0;
    }

    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
    if (opt->backend == GR_BACKEND_OPENGL) {
        flags |= SDL_WINDOW_OPENGL;
    } else if (opt->backend == GR_BACKEND_VULKAN || opt->backend == GR_BACKEND_NOGRAPHICS) {
        flags |= SDL_WINDOW_VULKAN;
    }
    rt->window = SDL_CreateWindow("psx-renderer-probe", 640, 480, flags);
    if (!rt->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    if (opt->backend == GR_BACKEND_OPENGL) {
        gl_renderer_set_swap_interval(0);
        if (!gl_renderer_init_context(rt->window)) {
            fprintf(stderr, "gl_renderer_init_context failed\n");
            return -1;
        }
    } else if (opt->backend == GR_BACKEND_VULKAN) {
        vk_renderer_set_present_mode(0);
        if (!vk_renderer_init_context(rt->window)) {
            fprintf(stderr, "vk_renderer_init_context failed\n");
            return -1;
        }
    } else if (opt->backend == GR_BACKEND_NOGRAPHICS) {
        ng_renderer_set_present_mode(0);
        if (!ng_renderer_init_context(rt->window)) {
            fprintf(stderr, "ng_renderer_init_context failed\n");
            return -1;
        }
        gr_set_backend(GR_BACKEND_NOGRAPHICS);
        gr_init(g_vram);
        gr_set_scale(opt->scale);
    }
    if (gr_backend() != opt->backend) {
        fprintf(stderr, "Unexpected backend fallback\n");
        return -1;
    }
    return 0;
}

static void shutdown_backend(const Options *opt, BackendRuntime *rt)
{
    if (opt->backend == GR_BACKEND_OPENGL) {
        gl_renderer_shutdown();
    } else if (opt->backend == GR_BACKEND_VULKAN) {
        vk_renderer_shutdown();
    } else if (opt->backend == GR_BACKEND_NOGRAPHICS) {
        gr_set_backend(GR_BACKEND_SOFTWARE);
        ng_renderer_shutdown();
    }
    if (rt->window) SDL_DestroyWindow(rt->window);
    SDL_Quit();
}

static int run_probe(const Options *opt)
{
    BackendRuntime rt;
    memset(&rt, 0, sizeof(rt));
    if (ensure_dir(opt->out_dir) != 0) {
        fprintf(stderr, "failed to create output dir '%s': %s\n", opt->out_dir, SDL_GetError());
        return 2;
    }
    if (init_backend(opt, &rt) != 0) {
        shutdown_backend(opt, &rt);
        return 3;
    }

    if (gr_scale() != opt->scale) {
        fprintf(stderr, "Requested scale %d, effective scale %d\n", opt->scale, gr_scale());
        shutdown_backend(opt, &rt);
        return 5;
    }
    if (opt->contracts && pixel_contract_check() != 0) {
        shutdown_backend(opt, &rt);
        return 5;
    }
    if (!opt->skip_wide && wide_smoke_check(opt->backend_name) != 0) {
        shutdown_backend(opt, &rt);
        return 5;
    }

    for (int i = 0; i < opt->warmup; i++) {
        draw_scene(i);
        gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, g_readback);
    }

    printf("{\"backend\":\"%s\",\"effective_backend\":%d,\"scale\":%d,\"warmup\":%d,\"iters\":%d,\"repeats\":[",
           opt->backend_name, (int)gr_backend(), opt->scale, opt->warmup, opt->iters);
    fflush(stdout);

    for (int r = 0; r < opt->repeats; r++) {
        double start = now_ms();
        for (int i = 0; i < opt->iters; i++) {
            draw_scene(i);
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, g_readback);
        }
        double elapsed = now_ms() - start;
        uint64_t h = hash_vram(g_readback);
        if (dump_raw(opt->out_dir, opt->backend_name, r, g_readback) != 0) {
            fprintf(stderr, "failed to write raw VRAM for repeat %d\n", r);
            shutdown_backend(opt, &rt);
            return 4;
        }
        printf("%s{\"repeat\":%d,\"wall_ms\":%.6f,\"hash64\":\"%016" PRIx64 "\"}",
               r ? "," : "", r, elapsed, h);
        fflush(stdout);
    }
    printf("]}\n");

    if (opt->present) {
        int presented = 0;
        if (opt->backend == GR_BACKEND_NOGRAPHICS) {
            uint32_t *cpu_frame = malloc(320u * 240u * sizeof(uint32_t));
            if (!cpu_frame) { shutdown_backend(opt, &rt); return 6; }
            for (int i = 0; i < 320 * 240; ++i) cpu_frame[i] = 0xff204080u;
            presented = ng_renderer_present_vram(0,0,320,240,0,1);
            SDL_SetWindowSize(rt.window, 800, 600);
            SDL_PumpEvents();
            presented &= ng_renderer_present_vram(0,0,320,240,1,1);
            ng_renderer_present_cpu(cpu_frame,320,240,0,1);
            free(cpu_frame);
            ng_renderer_present_blank();
        } else if (opt->backend == GR_BACKEND_VULKAN) {
            presented = vk_renderer_present_vram(0,0,320,240,0,1);
        }
        if (!presented) {
            fprintf(stderr, "Presentation smoke failed/unsupported\n");
            shutdown_backend(opt, &rt);
            return 6;
        }
    }

    shutdown_backend(opt, &rt);
    return 0;
}

static int parse_backend(const char *s, GrBackend *out)
{
    if (strcmp(s, "software") == 0 || strcmp(s, "sw") == 0) { *out = GR_BACKEND_SOFTWARE; return 0; }
    if (strcmp(s, "opengl") == 0 || strcmp(s, "gl") == 0) { *out = GR_BACKEND_OPENGL; return 0; }
    if (strcmp(s, "vulkan") == 0 || strcmp(s, "vk") == 0) { *out = GR_BACKEND_VULKAN; return 0; }
    if (strcmp(s, "vulkan_nographics") == 0 || strcmp(s, "ng") == 0) { *out = GR_BACKEND_NOGRAPHICS; return 0; }
    return -1;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --backend software|opengl|vulkan|vulkan_nographics [--out DIR] [--warmup N] [--repeat N] [--iters N] [--scale N] [--skip-wide] [--contracts] [--present]\n", argv0);
}

int main(int argc, char **argv)
{
    Options opt;
    memset(&opt, 0, sizeof(opt));
    opt.backend = GR_BACKEND_SOFTWARE;
    opt.backend_name = "software";
    opt.out_dir = "renderer_probe_out";
    opt.warmup = 3;
    opt.repeats = 5;
    opt.iters = 20;
    opt.scale = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            opt.backend_name = argv[++i];
            if (parse_backend(opt.backend_name, &opt.backend) != 0) {
                usage(argv[0]);
                return 64;
            }
            if (opt.backend == GR_BACKEND_SOFTWARE) opt.backend_name = "software";
            else if (opt.backend == GR_BACKEND_OPENGL) opt.backend_name = "opengl";
            else if (opt.backend == GR_BACKEND_NOGRAPHICS) opt.backend_name = "vulkan_nographics";
            else opt.backend_name = "vulkan";
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            opt.out_dir = argv[++i];
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            opt.warmup = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            opt.repeats = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            opt.iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            opt.scale = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--skip-wide") == 0) {
            opt.skip_wide = 1;
        } else if (strcmp(argv[i], "--contracts") == 0) {
            opt.contracts = 1;
        } else if (strcmp(argv[i], "--present") == 0) {
            opt.present = 1;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    if (opt.warmup < 0 || opt.repeats <= 0 || opt.iters <= 0 || opt.scale <= 0) {
        usage(argv[0]);
        return 64;
    }
    return run_probe(&opt);
}
