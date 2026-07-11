#ifndef PSXRECOMP_PRESENT_RING_H
#define PSXRECOMP_PRESENT_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Present-classification ring: one entry per present decision in
 * sdl_vblank_present, for EVERY backend (software, GL, Vulkan). Records how
 * the frame was classified (FMV/full-2D 4:3 pillarbox, native-wide, or the
 * canonical read) and whether a native-wide present fell back to the
 * canonical width — the fallback is invisible on screen except as a
 * horizontal stretch, so it must be observable in data. Always-on; queried
 * via the TCP debug server "present_ring" command. */

typedef enum {
    PRES_PATH_BLANK     = 0,  /* display disabled — blank frame */
    PRES_PATH_NATIVE_43 = 1,  /* FMV / full-2D frame, 4:3 pillarbox */
    PRES_PATH_WIDE      = 2,  /* native-wide surface (canonical + EXTRA) */
    PRES_PATH_CANONICAL = 3,  /* canonical read; stretches when the window
                                 is wider than the display aspect */
} PresPath;

typedef struct {
    uint32_t frame;         /* s_frame_count at present time */
    uint16_t disp_w, disp_h;/* GPU display size */
    uint16_t present_w;     /* width actually presented */
    uint16_t nw_extra;      /* native-wide growth configured this frame */
    uint8_t  path;          /* PresPath */
    uint8_t  wide_fellback; /* wide present attempted but surface missing */
    uint8_t  game_mode;     /* ws gameplay-vs-2D classification */
    uint8_t  native_43;     /* gpu_ws_present_native_43() at present time */
    int32_t  tag_delta;     /* frames since newest sprite tag (clamped) */
    uint16_t gte_verts;     /* RTPS/RTPT verts, last completed frame */
    uint16_t ovh_prims;     /* overhanging polys, last completed frame (the
                               2D-only-scene classifier's world signal) */
} PresRingEntry;

uint64_t present_ring_total(void);
int      present_ring_get(uint64_t seq, PresRingEntry* out);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PRESENT_RING_H */
