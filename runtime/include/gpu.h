/* gpu.h — PS1 GPU hardware simulation (Phase 3).
 *
 * Implements GPUSTAT, GP0, GP1, and 1024x512 16-bit VRAM.
 * No rendering to screen — just correct hardware state transitions.
 */

#ifndef PSXRECOMP_V4_GPU_H
#define PSXRECOMP_V4_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     gpu_init(void);
uint32_t gpu_read_gpustat(void);   /* 0x1F801814 read */
uint32_t gpu_read_gpuread(void);   /* 0x1F801810 read */
void     gpu_write_gp0(uint32_t val);  /* 0x1F801810 write */
void     gpu_write_gp1(uint32_t val);  /* 0x1F801814 write */
void     gpu_vblank_tick(void);        /* Toggle LCF, called at each simulated vblank */

/* Display presentation accessors (Phase 3). */
const uint16_t* gpu_get_vram(void);    /* Pointer to 1024x512 16-bit VRAM */

typedef struct {
    uint32_t display_x, display_y;     /* VRAM start of display area (GP1(05h)) */
    uint32_t width, height;            /* Derived from display mode + ranges */
    int      disabled;                 /* GP1(03h) display disable flag */
} GpuDisplayInfo;

void gpu_get_display_info(GpuDisplayInfo* out);
uint64_t gpu_get_gp0_count(void);  /* Total GP0 writes since init */
void gpu_get_gp0_stats(uint64_t* nop, uint64_t* fill, uint64_t* draw, uint64_t* env, uint64_t* copy);

typedef struct {
    uint32_t left, top, right, bottom;
    int32_t offset_x, offset_y;
} GpuDrawArea;
void gpu_get_draw_area(GpuDrawArea* out);
uint16_t gpu_vram_peek(int x, int y);

/* Vblank presentation callback — called from gpu_vblank_tick(). */
typedef void (*gpu_vblank_cb)(void);
void gpu_set_vblank_callback(gpu_vblank_cb cb);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_GPU_H */
