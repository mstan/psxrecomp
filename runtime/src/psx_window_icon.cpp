/* psx_window_icon.cpp — stage-time PNG → SDL_SetWindowIcon (Win/Linux/macOS). */

#include "psx_window_icon.h"
#include "psx_sdl.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"

namespace fs = std::filesystem;

static fs::path exe_dir_from_argv0(const char *argv0)
{
    fs::path exe_dir;
    std::error_code ec;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n > 0 && n < (DWORD)(sizeof(buf) / sizeof(buf[0])))
        exe_dir = fs::path(std::wstring(buf, buf + n)).parent_path();
#endif
    if (exe_dir.empty()) {
        const char *appimg = std::getenv("APPIMAGE");
        if (appimg && appimg[0]) {
            exe_dir = fs::absolute(appimg, ec).parent_path();
            if (ec) exe_dir.clear();
        }
    }
    if (exe_dir.empty() && argv0 && argv0[0]) {
        exe_dir = fs::absolute(argv0, ec).parent_path();
        if (ec) exe_dir.clear();
    }
    if (exe_dir.empty()) {
        const char *base = SDL_GetBasePath();
        if (base) {
            exe_dir = fs::path(base);
            SDL_free((void *)base);
        }
    }
    if (exe_dir.empty()) exe_dir = fs::path(".");
    return exe_dir;
}

static fs::path find_icon_png(const char *argv0)
{
    const fs::path exe_dir = exe_dir_from_argv0(argv0);
    const fs::path candidates[] = {
        exe_dir / "assets" / "psxrecomp.png",
        exe_dir / "psxrecomp.png",
        exe_dir / ".." / "assets" / "psxrecomp.png",
    };
    for (const fs::path &p : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

/* Resolve once and keep it: both the game window and the launcher ask, and
 * the answer cannot change within a run. */
extern "C" const char *psx_window_icon_path(const char *argv0)
{
    static std::string cached;
    static bool resolved = false;
    if (!resolved) {
        cached = find_icon_png(argv0).string();
        resolved = true;
    }
    return cached.c_str();
}

extern "C" void psx_apply_window_icon(SDL_Window *window, const char *argv0)
{
    if (!window) return;
    const fs::path png(psx_window_icon_path(argv0));
    if (png.empty()) return;

    FILE *f = std::fopen(png.string().c_str(), "rb");
    if (!f) return;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return;
    }
    long sz = std::ftell(f);
    if (sz <= 0) {
        std::fclose(f);
        return;
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return;
    }
    std::string buf(static_cast<size_t>(sz), '\0');
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fclose(f);
        return;
    }
    std::fclose(f);

    int w = 0, h = 0, comp = 0;
    stbi_uc *pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc *>(buf.data()), (int)buf.size(), &w, &h, &comp, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return;
    }

#if defined(PSX_SDL3)
    SDL_Surface *surf =
        SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (surf) {
        SDL_SetWindowIcon(window, surf);
        SDL_DestroySurface(surf);
    }
#else
    SDL_Surface *surf = SDL_CreateRGBSurfaceFrom(
        pixels, w, h, 32, w * 4,
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff
#else
        0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000
#endif
    );
    if (surf) {
        SDL_SetWindowIcon(window, surf);
        SDL_FreeSurface(surf);
    }
#endif
    stbi_image_free(pixels);
}
