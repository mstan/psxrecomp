#ifndef PSXRECOMP_WS_PRESENT_LAYOUT_H
#define PSXRECOMP_WS_PRESENT_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxPresentRect {
    int x;
    int y;
    int w;
    int h;
} PsxPresentRect;

typedef struct PsxPresentLayout {
    PsxPresentRect outer;
    PsxPresentRect content;
} PsxPresentLayout;

/* Compute the configured outer canvas and the content rect independently.
 *
 * INVARIANT: the OUTER viewport aspect is a function of user configuration
 * ONLY. Nothing derived from frame content may change it when
 * fixed_outer_aspect is enabled. Scene classification may only select the
 * centered 4:3 content-safe rect inside that canvas.
 *
 * fixed_outer_aspect is opt-in so games that have not selected the separated
 * policy retain the legacy scene-dependent 4:3 presentation behavior. */
void psx_present_layout_compute(int drawable_w, int drawable_h,
                                int target_num, int target_den,
                                int fixed_outer_aspect, int content_4_3,
                                PsxPresentLayout *out);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_WS_PRESENT_LAYOUT_H */
