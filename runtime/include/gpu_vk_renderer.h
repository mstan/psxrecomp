#ifndef PSX_GPU_VK_RENDERER_H
#define PSX_GPU_VK_RENDERER_H

/* gpu_vk_renderer.c — hardware Vulkan renderer backend.
 *
 * Selected via [video] renderer = "vulkan".  Like the GL backend it is
 * GPU-authoritative: VRAM lives as a Vulkan image and present blits straight
 * from it to the swapchain (no per-frame readback).  Vulkan is loaded ENTIRELY
 * at runtime through SDL_Vulkan_LoadLibrary + vkGetInstanceProcAddr, so the exe
 * has no link-time vulkan-1 dependency and gracefully falls back to software on
 * a machine without a Vulkan ICD.
 *
 * The backend supplies its vtable through vk_backend_get() (NULL until a context
 * is created and the pipeline is ready). */

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Create the Vulkan instance/device/swapchain on a window made with
 * SDL_WINDOW_VULKAN.  Returns 1 on success, 0 to fall back to software. */
int  vk_renderer_init_context(struct SDL_Window *win);

/* Tear down all Vulkan objects (idempotent). */
void vk_renderer_shutdown(void);

/* Present the displayed VRAM region straight from the GPU VRAM image to the
 * swapchain (15-bit frames; deterministic, no readback). Returns 1 on success,
 * 0 if the Vulkan path is inactive (caller keeps the CPU present path). */
int  vk_renderer_present_vram(int disp_x, int disp_y, int w, int h,
                              int linear, int force_4_3);

/* GPU-direct native-wide present (mirrors gl_renderer_present_wide_fbo). */
int  vk_renderer_present_wide(int disp_x, int disp_y, int disp_h, int linear);

/* Present an ARGB8888 image as a letterboxed quad (24-bit FMV frames and the
 * display-disabled clear). force_4_3 pillarboxes at native 4:3. */
void vk_renderer_present_cpu(const uint32_t *pixels, int src_w, int src_h,
                             int linear, int force_4_3);
void vk_renderer_present_blank(void);

/* Sync the GPU VRAM image down into the CPU VRAM mirror (screenshots, debug
 * server, 24-bit present). No-op when the Vulkan path is inactive. */
void vk_renderer_sync_cpu(void);

/* After savestate restore: push CPU VRAM into the Vulkan image (see GL twin). */
void vk_renderer_restage_vram_after_savestate(void);

/* Set the present policy: 1=tear-free, 0=IMMEDIATE (lowest latency, may tear),
 * -1=MAILBOX. Tear-free prefers MAILBOX because the frontend already paces
 * frames; unsupported modes fall back to FIFO (always available). */
void vk_renderer_set_present_mode(int mode);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_VK_RENDERER_H */
