#ifndef PSX_GPU_INTERLACE_H
#define PSX_GPU_INTERLACE_H

/* GP0 raster primitives skip the active field only in 480i mode with
 * drawing-to-display prohibited. VRAM transfer commands do not use this rule.
 * Return -1 for progressive drawing, otherwise the native VRAM row parity
 * that must remain unchanged. The display origin participates in that parity. */
static inline int psx_gpu_raster_skipped_row(unsigned interlace,
        unsigned high_vertical_resolution, unsigned draw_to_display,
        unsigned display_y, unsigned field) {
    if (!interlace || !high_vertical_resolution || draw_to_display)
        return -1;
    return (int)((display_y + field) & 1u);
}

#endif
