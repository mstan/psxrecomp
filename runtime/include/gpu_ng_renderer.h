#ifndef PSX_GPU_NG_RENDERER_H
#define PSX_GPU_NG_RENDERER_H

#include "gpu_render.h"
struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Supplemental experimental backend; never aliases native Vulkan. Failure to
 * load its optional DLL or create a device leaves ng_backend_get() NULL. */
int ng_renderer_init_context(struct SDL_Window *window);
/* Build + DLL presence only; device eligibility is checked during init. */
int ng_renderer_available(void);
void ng_renderer_shutdown(void);
const GpuRenderBackend *ng_backend_get(void);
int ng_renderer_present_vram(int dx, int dy, int w, int h, int linear, int force_4_3);
int ng_renderer_present_wide(int dx, int dy, int dh, int linear);
void ng_renderer_present_cpu(const uint32_t *pixels, int w, int h, int linear, int force_4_3);
void ng_renderer_present_blank(void);
void ng_renderer_sync_cpu(void);
void ng_renderer_restage_vram_after_savestate(void);
void ng_renderer_set_present_mode(int mode);
int ng_perf_json(char *out, int cap, int count);

#ifdef __cplusplus
}
#endif
#endif
