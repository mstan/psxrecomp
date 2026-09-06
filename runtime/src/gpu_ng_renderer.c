/* Optional NoGraphicsAPI DLL loader. The native Vulkan implementation remains
 * independent. All GPU work is owned by the DLL; this file only bridges SDL
 * window handles and the versioned C entry points. */
#include "gpu_ng_renderer.h"
#include "gpu_ng_abi.h"
#include <stdio.h>
#include <wchar.h>

#if defined(PSX_HAVE_NOGRAPHICS) && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "psx_sdl.h"
#if !defined(PSX_SDL3)
#include <SDL_syswm.h>
#endif

static HMODULE s_module;
static const PsxNgApi *s_api;
static struct SDL_Window *s_window;
static int s_present_mode = 1;

static int library_path(wchar_t *path, DWORD capacity)
{
    DWORD length = GetModuleFileNameW(NULL, path, capacity);
    wchar_t *slash;
    if (!length || length >= capacity) return 0;
    slash = wcsrchr(path, L'\\');
    if (!slash || (size_t)(slash - path) + 1 + sizeof(PSX_NG_LIBRARY_NAME) >= capacity) return 0;
    wcscpy(slash + 1, L"psx_nographics.dll");
    return 1;
}

int ng_renderer_available(void)
{
    wchar_t path[32768];
    DWORD attrs;
    if (!library_path(path, (DWORD)(sizeof(path) / sizeof(path[0])))) return 0;
    attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int valid_api(const PsxNgApi *a)
{
    const GpuRenderBackend *b;
    if (!a || a->version != PSX_NG_ABI_VERSION || a->struct_size != sizeof(*a) ||
        !a->init_context || !a->shutdown || !a->resize || !a->present_vram ||
        !a->present_wide || !a->present_cpu || !a->present_blank || !a->sync_cpu ||
        !a->restage || !a->set_present_mode || !a->backend) return 0;
    b = a->backend;
    return b->name && b->init && b->set_scale && b->scale &&
        b->set_texture_filter && b->texture_filter && b->set_semi_transparency &&
        b->set_mask_bits && b->set_texture_window && b->set_color_modulation &&
        b->fill_rect && b->copy_rect && b->draw_flat_triangle && b->draw_gouraud_triangle &&
        b->draw_textured_triangle && b->draw_shaded_textured_triangle &&
        b->draw_flat_rect && b->draw_textured_rect && b->draw_textured_rect_scaled &&
        b->draw_line && b->draw_shaded_line && b->render_display && b->render_display_hires &&
        b->vram_write && b->vram_read && b->vram_transfer_in && b->vram_transfer_out &&
        b->set_draw_area && b->get_draw_area && b->set_draw_offset;
}

static void refresh_size(void)
{
    int w = 0, h = 0;
    if (!s_api || !s_window) return;
#if defined(PSX_SDL3)
    SDL_GetWindowSizeInPixels(s_window, &w, &h);
#else
    SDL_GetWindowSize(s_window, &w, &h);
#endif
    s_api->resize(w, h);
}

int ng_renderer_init_context(struct SDL_Window *window)
{
    /* Resolve only beside the executable, never through the current directory
     * or PATH. Windows x64 uses the same C calling convention for both compilers. */
    wchar_t path[32768];
    PsxNgGetApi get_api;
    const PsxNgApi *api;
    HWND hwnd = NULL;
    int width = 0, height = 0;
    ng_renderer_shutdown();
    if (!window) return 0;
    if (!library_path(path, (DWORD)(sizeof(path) / sizeof(path[0])))) return 0;
    s_module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!s_module) {
        fprintf(stderr, "psxrecomp: NoGraphicsAPI DLL unavailable (Windows error %lu)\n", (unsigned long)GetLastError());
        return 0;
    }
    get_api = (PsxNgGetApi)GetProcAddress(s_module, "psx_ng_get_api");
    api = get_api ? get_api(PSX_NG_ABI_VERSION) : NULL;
    if (!valid_api(api)) {
        fprintf(stderr, "psxrecomp: NoGraphicsAPI DLL has an incompatible renderer ABI\n");
        FreeLibrary(s_module); s_module = NULL;
        return 0;
    }
#if defined(PSX_SDL3)
    hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    SDL_GetWindowSizeInPixels(window, &width, &height);
#else
    {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS)
            hwnd = info.info.win.window;
    }
    SDL_GetWindowSize(window, &width, &height);
#endif
    if (!hwnd || !api->init_context(hwnd, width, height)) {
        fprintf(stderr, "psxrecomp: NoGraphicsAPI device/presentation initialization failed\n");
        api->shutdown();
        FreeLibrary(s_module); s_module = NULL;
        return 0;
    }
    s_api = api;
    s_window = window;
    s_api->set_present_mode(s_present_mode);
    return 1;
}

void ng_renderer_shutdown(void)
{
    if (s_api && gr_backend() == GR_BACKEND_NOGRAPHICS) {
        s_api->sync_cpu();
        gr_set_backend(GR_BACKEND_SOFTWARE);
    }
    if (s_api) s_api->shutdown();
    s_api = NULL; s_window = NULL;
    if (s_module) FreeLibrary(s_module);
    s_module = NULL;
}
const GpuRenderBackend *ng_backend_get(void) { return s_api ? s_api->backend : NULL; }
int ng_renderer_present_vram(int x,int y,int w,int h,int l,int f) { refresh_size(); return s_api ? s_api->present_vram(x,y,w,h,l,f) : 0; }
int ng_renderer_present_wide(int x,int y,int h,int l) { refresh_size(); return s_api ? s_api->present_wide(x,y,h,l) : 0; }
void ng_renderer_present_cpu(const uint32_t *p,int w,int h,int l,int f) { refresh_size(); if (s_api) s_api->present_cpu(p,w,h,l,f); }
void ng_renderer_present_blank(void) { refresh_size(); if (s_api) s_api->present_blank(); }
void ng_renderer_sync_cpu(void) { if (s_api) s_api->sync_cpu(); }
void ng_renderer_restage_vram_after_savestate(void) { if (s_api) s_api->restage(); }
void ng_renderer_set_present_mode(int mode) { s_present_mode = mode; if (s_api) s_api->set_present_mode(mode); }

/* Backend-specific counters have not been implemented. An empty array keeps
 * debug tooling truthful instead of attributing native Vulkan counters here. */
int ng_perf_json(char *out, int cap, int count)
{
    (void)count;
    if (!out || cap < 3) return 0;
    out[0] = '['; out[1] = ']'; out[2] = '\0';
    return 2;
}

#else
int ng_renderer_available(void) { return 0; }
int ng_renderer_init_context(struct SDL_Window *w) { (void)w; return 0; }
void ng_renderer_shutdown(void) {}
const GpuRenderBackend *ng_backend_get(void) { return NULL; }
int ng_renderer_present_vram(int x,int y,int w,int h,int l,int f) { (void)x;(void)y;(void)w;(void)h;(void)l;(void)f;return 0; }
int ng_renderer_present_wide(int x,int y,int h,int l) { (void)x;(void)y;(void)h;(void)l;return 0; }
void ng_renderer_present_cpu(const uint32_t *p,int w,int h,int l,int f) { (void)p;(void)w;(void)h;(void)l;(void)f; }
void ng_renderer_present_blank(void) {}
void ng_renderer_sync_cpu(void) {}
void ng_renderer_restage_vram_after_savestate(void) {}
void ng_renderer_set_present_mode(int m) { (void)m; }
int ng_perf_json(char *out, int cap, int count) { (void)out; (void)cap; (void)count; return 0; }
#endif
