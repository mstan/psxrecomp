/* display_scanout.h — small, renderer-neutral CRTC presentation helpers. */

#ifndef PSXRECOMP_DISPLAY_SCANOUT_H
#define PSXRECOMP_DISPLAY_SCANOUT_H

#include <stdint.h>

/* PAL active video is 288 lines per field and 576 rows when the interlaced
 * display bit is set. CPU presentation buffers must preserve that full
 * canvas even though PS1 VRAM itself remains 512 rows and wraps on access. */
#define PSX_DISPLAY_PRESENT_MAX_HEIGHT 576u

typedef struct {
    uint32_t canvas_height;
    uint32_t canvas_origin_y;
    uint32_t source_skip_y;
    uint32_t source_height;
    int offset_y;
    int range_set;
    int valid;
} PsxDisplayVerticalLayout;

/* Depth24 staging expands the visible source into the full active-video
 * canvas. Every presentation backend must therefore consume the canvas
 * height, while ordinary 15-bit display keeps its source height. */
static inline uint32_t psx_display_present_height(
    int depth24, uint32_t source_height, uint32_t canvas_height)
{
    return depth24 && canvas_height != 0u ? canvas_height : source_height;
}

static inline uint32_t psx_display_interlaced_rows(
    uint32_t rows, int interlaced)
{
    return interlaced ? rows * 2u : rows;
}

/* Canvas-origin clipping belongs only to staged depth24 presentation.
 * Ordinary 15-bit renderers read their source rectangle directly. */
static inline uint32_t psx_display_clip_source_height(
    int depth24, uint32_t source_height, uint32_t canvas_height,
    uint32_t canvas_origin_y)
{
    if (!depth24)
        return source_height;
    if (canvas_origin_y >= canvas_height)
        return 0u;
    const uint32_t available = canvas_height - canvas_origin_y;
    return source_height < available ? source_height : available;
}

/* Preserve the legacy default for an unset/degenerate GP1(07h) range. A
 * programmed increasing range that has no active-video intersection owns a
 * zero height instead, so presentation fails closed to black. */
static inline uint32_t psx_display_source_height(
    PsxDisplayVerticalLayout layout, uint32_t fallback_height)
{
    return layout.range_set ? layout.source_height : fallback_height;
}

/* Intersect a GP1(07h) range with the PAL or NTSC active region. The canvas
 * preserves the television scanout position without discarding visible VRAM
 * rows. source_skip_y owns rows before the active region; canvas_origin_y owns
 * black rows before the visible source. */
static inline PsxDisplayVerticalLayout psx_display_vertical_layout(
    int pal, uint32_t raw_y1, uint32_t raw_y2)
{
    const int ymin = pal ? 20 : 16;
    const int ymax = pal ? 308 : 256;
    const int centre = pal ? 0xA3 : 0x88;
    PsxDisplayVerticalLayout out = {0};
    int y1 = (int)raw_y1;
    int y2 = (int)raw_y2;

    if (raw_y2 <= raw_y1)
        return out;
    out.range_set = 1;
    out.canvas_height = (uint32_t)(ymax - ymin);
    if (y1 < ymin) y1 = ymin;
    if (y1 > ymax) y1 = ymax;
    if (y2 < ymin) y2 = ymin;
    if (y2 > ymax) y2 = ymax;
    if (y2 <= y1)
        return out;

    out.canvas_origin_y = (uint32_t)(y1 - ymin);
    out.source_skip_y = raw_y1 < (uint32_t)ymin
        ? (uint32_t)ymin - raw_y1 : 0u;
    out.source_height = (uint32_t)(y2 - y1);
    out.offset_y = ((y1 + y2) - (centre * 2)) / 2;
    out.valid = 1;
    return out;
}

/* Return the vertical position of a GP1(07h) display range relative to the
 * broadcast centre. Positive values move the scanout down. The limits and
 * centres are the PS1 PAL/NTSC active regions documented by psx-spx. */
static inline int psx_display_vertical_offset(int pal, uint32_t raw_y1,
                                              uint32_t raw_y2)
{
    PsxDisplayVerticalLayout layout = psx_display_vertical_layout(
        pal, raw_y1, raw_y2);
    return layout.valid ? layout.offset_y : 0;
}

#endif
