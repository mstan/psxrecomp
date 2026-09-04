#include "ws_ui_group.h"

#include <stdlib.h>

int32_t ws_ui_anchor_for_bounds(int32_t x, int32_t width,
                                int32_t display_width) {
    const int32_t twice_center = 2 * x + width;
    if (3 * twice_center < 2 * display_width) return 0;
    if (3 * twice_center > 4 * display_width) return display_width;
    return display_width / 2;
}

static int32_t axis_gap(int32_t a0, int32_t a1, int32_t b0, int32_t b1) {
    if (a1 < b0) return b0 - a1;
    if (b1 < a0) return a0 - b1;
    return 0;
}

static int32_t interval_gap(const WsUiGroupItem *a,
                            const WsUiGroupItem *b) {
    return axis_gap(a->x, a->x + a->width, b->x, b->x + b->width);
}

/* Are the two primitives one visual element -- drawn over or directly against
 * each other -- as opposed to merely close? interval_gap() cannot tell those
 * apart, but they are very different signals: horizontal adjacency is a weak
 * hint that two runs might belong together, whereas sharing screen columns
 * while touching vertically means a glyph on its background box, a bar in its
 * frame, or a label sitting on the readout it annotates. Those must never be
 * anchored independently.
 *
 * Both halves of the test are load-bearing, and each was learned from a real
 * failure on WipEout 3's race HUD at 32:9:
 *
 *  - Drop the column test and everything joins by key alone, so a digit and
 *    the box under it (different CLUT, different poly-vs-rect family) get
 *    independent thirds anchors and are dragged to opposite screen edges.
 *  - Drop the row test and X alone merges the top-left lap counter with the
 *    bottom-left lap timer 200 scanlines below. Union is transitive, so that
 *    chained all 71 HUD primitives into one run spanning [24,486], which
 *    anchors centre and pulls the corners inward.
 *  - Demand a strict row INTERSECTION and stacked rows are missed: the lap
 *    readout draws its digits at y=[219,227] and the box they label at
 *    y=[228,244] -- a one-pixel seam. The box then anchored centre while its
 *    own digits anchored left, landing 500 screen pixels apart. Hence the
 *    STACK_GAP slack rather than a bare intersection. */
static int same_element(const WsUiGroupItem *a, const WsUiGroupItem *b) {
    if (a->x >= b->x + b->width || b->x >= a->x + a->width)
        return 0;   /* no shared columns */
    return axis_gap(a->y, a->y + a->height, b->y, b->y + b->height) <=
           WS_UI_GROUP_STACK_GAP;
}

/* Close enough on screen to be part of the same widget: horizontally within a
 * glyph-gap, vertically overlapping or all but touching. Weaker than
 * same_element(), which additionally demands shared columns. */
static int touching(const WsUiGroupItem *a, const WsUiGroupItem *b) {
    return interval_gap(a, b) <= WS_UI_GROUP_JOIN_GAP &&
           axis_gap(a->y, a->y + a->height, b->y, b->y + b->height) <=
               WS_UI_GROUP_STACK_GAP;
}

/* The three ways two primitives are one HUD element.
 *
 * The third is submission adjacency: consecutive in the ordering-table walk
 * AND touching. The array arrives in submission order -- gpu_ws_prepass_linked_
 * list fills it as it walks the OT, and the max_rank filter compacts forward --
 * so "adjacent index" needs no extra field.
 *
 * It is what the shared-columns test was standing in for. WipEout 3's
 * speed/shield readout ends at x=288 and the meter bar it belongs to starts at
 * x=306: no shared column, so same_element() cannot fire, and the digit font's
 * CLUT differs from the bar's, so the key join cannot fire either. The readout
 * was left as its own run; its bbox midpoint, 256.5, landed in the middle third
 * against a screen centre of 254; and it stayed pinned near screen centre while
 * its own meters went to the right edge. A 2 px gap in 4:3 opens to 64 frame-
 * buffer pixels at 16:9, 110 at 21:9 and 159 at 32:9.
 *
 * No anchor threshold can fix that. The genuinely centred "pit ... check"
 * message sits FARTHER RIGHT (midpoint 300.5) than the mis-anchored readout, so
 * any threshold that pushed the readout into the right third would drag the
 * message there first and tear it apart. Submission order is the only signal
 * that separates them: one widget's draw code emits its parts back to back.
 *
 * The gap and row tests keep it narrow -- consecutive packets that are far
 * apart on screen are simply the next widget. Simulated over six captures this
 * forms exactly one union that changes an anchor, and that union is the defect;
 * the bottom-left cluster does not chain in, because its nearest consecutive
 * neighbour pair is 66 px apart. */
static int items_join(const WsUiGroupItem *items, size_t i, size_t j) {
    const WsUiGroupItem *a = &items[i], *b = &items[j];
    if (same_element(a, b)) return 1;
    if (a->key == b->key && interval_gap(a, b) <= WS_UI_GROUP_JOIN_GAP) return 1;
    if (j == i + 1 && touching(a, b)) return 1;
    return 0;
}

static size_t root_of(size_t *parent, size_t index) {
    while (parent[index] != index) {
        parent[index] = parent[parent[index]];
        index = parent[index];
    }
    return index;
}

void ws_ui_group_assign(WsUiGroupItem *items, size_t count,
                        int32_t display_width, int dense_menu) {
    if (!items || count == 0 || display_width <= 0) return;
    if (dense_menu) {
        for (size_t i = 0; i < count; i++) {
            items[i].anchor = display_width / 2;
            items[i].root = (uint32_t)i;   /* diag: no runs formed */
        }
        return;
    }

    size_t *parent = (size_t *)malloc(count * sizeof(*parent));
    if (!parent) {
        for (size_t i = 0; i < count; i++) {
            items[i].anchor = ws_ui_anchor_for_bounds(
                items[i].x, items[i].width, display_width);
            items[i].root = (uint32_t)i;   /* diag: each item stands alone */
        }
        return;
    }
    for (size_t i = 0; i < count; i++) parent[i] = i;

    /* Transitive union is important for text: each adjacent pair is close,
     * even when the complete string spans more than one thirds region.
     *
     * Two ways to join, and the second is not a relaxation of the first:
     *
     *  - SAME KEY and within JOIN_GAP — a run of glyphs from one font/CLUT.
     *  - SAME ELEMENT, whatever the key — see same_element(). Requiring a
     *    matching key here was a defect: the key folds in CLUT, texpage, a 24px
     *    Y band and the poly-vs-rect family, so a digit and the box it sits on,
     *    or a bar and its frame, could never merge. They then got INDEPENDENT
     *    thirds anchors and were dragged toward opposite screen edges as the
     *    frame widened.
     *  - SUBMISSION-ADJACENT and touching — see items_join().
     *
     * Observed on WipEout 3's race HUD at 32:9: the lap readout's digits at
     * [137,202] anchored left while the box beneath them at [138,202] anchored
     * centre, putting the box ~500 screen pixels from the digits it belongs to.
     * That is the HUD pulling itself apart, and it gets worse the wider the
     * aspect. */
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (!items_join(items, i, j)) continue;
            size_t ri = root_of(parent, i);
            size_t rj = root_of(parent, j);
            if (ri != rj) parent[rj] = ri;
        }
    }

    for (size_t i = 0; i < count; i++) {
        size_t root = root_of(parent, i);
        int32_t min_x = items[i].x;
        int32_t max_x = items[i].x + items[i].width;
        for (size_t j = 0; j < count; j++) {
            if (root_of(parent, j) != root) continue;
            if (items[j].x < min_x) min_x = items[j].x;
            if (items[j].x + items[j].width > max_x)
                max_x = items[j].x + items[j].width;
        }
        items[i].anchor =
            ws_ui_anchor_for_bounds(min_x, max_x - min_x, display_width);
        items[i].root = (uint32_t)root;
    }
    free(parent);
}
