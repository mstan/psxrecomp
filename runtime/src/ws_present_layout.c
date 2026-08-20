#include "ws_present_layout.h"

#include <stdint.h>

static PsxPresentRect fit_aspect(PsxPresentRect bounds, int num, int den) {
    PsxPresentRect result = bounds;
    if (bounds.w <= 0 || bounds.h <= 0) {
        result.w = 0;
        result.h = 0;
        return result;
    }
    if (num <= 0 || den <= 0) {
        num = 4;
        den = 3;
    }

    int fit_w = bounds.w;
    int fit_h = (int)(((int64_t)bounds.w * den) / num);
    if (fit_h > bounds.h) {
        fit_h = bounds.h;
        fit_w = (int)(((int64_t)bounds.h * num) / den);
    }
    if (fit_w < 1) fit_w = 1;
    if (fit_h < 1) fit_h = 1;

    result.x = bounds.x + (bounds.w - fit_w) / 2;
    result.y = bounds.y + (bounds.h - fit_h) / 2;
    result.w = fit_w;
    result.h = fit_h;
    return result;
}

void psx_present_layout_compute(int drawable_w, int drawable_h,
                                int target_num, int target_den,
                                int fixed_outer_aspect, int content_4_3,
                                PsxPresentLayout *out) {
    if (!out) return;

    PsxPresentRect drawable = {0, 0, drawable_w, drawable_h};
    if (fixed_outer_aspect) {
        out->outer = fit_aspect(drawable, target_num, target_den);
        out->content = content_4_3
            ? fit_aspect(out->outer, 4, 3)
            : out->outer;
    } else {
        /* Legacy compatibility: scene classification selected the presented
         * aspect itself, so a native-content frame replaces the target canvas
         * with a 4:3 viewport. */
        out->outer = content_4_3
            ? fit_aspect(drawable, 4, 3)
            : fit_aspect(drawable, target_num, target_den);
        out->content = out->outer;
    }
}
