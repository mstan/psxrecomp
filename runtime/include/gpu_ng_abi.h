#ifndef PSX_GPU_NG_ABI_H
#define PSX_GPU_NG_ABI_H

/* Versioned C-only boundary between the runtime and the optional MSVC-built
 * NoGraphicsAPI DLL. No C++ objects, allocation ownership or CRT handles cross
 * this boundary. Both modules must use this exact header and 64-bit Windows. */
#include "gpu_render.h"
#include <stdint.h>

#define PSX_NG_ABI_VERSION 1u
#define PSX_NG_LIBRARY_NAME "psx_nographics.dll"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxNgApi {
    uint32_t version;
    uint32_t struct_size;
    const GpuRenderBackend *backend;
    int  (*init_context)(void *hwnd, int width, int height);
    void (*shutdown)(void);
    void (*resize)(int width, int height);
    int  (*present_vram)(int dx, int dy, int w, int h, int linear, int force_4_3);
    int  (*present_wide)(int dx, int dy, int dh, int linear);
    void (*present_cpu)(const uint32_t *pixels, int w, int h, int linear, int force_4_3);
    void (*present_blank)(void);
    void (*sync_cpu)(void);
    void (*restage)(void);
    void (*set_present_mode)(int mode);
} PsxNgApi;

typedef const PsxNgApi *(*PsxNgGetApi)(uint32_t version);

#ifdef __cplusplus
}
#endif
#endif
