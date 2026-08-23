#include "ws_ui_group.h"

#include <assert.h>
#include <stdint.h>

static int32_t scale_about(int32_t x, int32_t anchor,
                           int32_t numerator, int32_t denominator) {
    int32_t delta = x - anchor;
    int32_t scaled =
        (delta * numerator +
         (delta >= 0 ? denominator / 2 : -denominator / 2)) / denominator;
    return anchor + scaled;
}

int main(void) {
    const int display_width = 384;

    /* {key, x, width, y, height} — anchor and root are outputs. */
    WsUiGroupItem text[] = {
        {1, 120, 16, 100, 16}, {1, 136, 16, 100, 16}, {1, 152, 16, 100, 16},
        {1, 168, 16, 100, 16}, {1, 184, 16, 100, 16}, {1, 200, 16, 100, 16},
        {1, 216, 16, 100, 16}, {1, 232, 16, 100, 16}, {1, 248, 16, 100, 16},
    };
    ws_ui_group_assign(text, 9, display_width, 0);
    for (int i = 0; i < 9; i++) assert(text[i].anchor == 192);
    assert(scale_about(184, text[3].anchor, 4, 7) ==
           scale_about(184, text[4].anchor, 4, 7));

    WsUiGroupItem monkeys[] = {
        {2, 300, 16, 100, 16}, {2, 320, 16, 100, 16},
        {2, 340, 16, 100, 16}, {2, 360, 16, 100, 16},
    };
    ws_ui_group_assign(monkeys, 4, display_width, 0);
    for (int i = 0; i < 4; i++) assert(monkeys[i].anchor == 384);
    for (int i = 0; i < 3; i++) {
        int32_t a = scale_about(monkeys[i].x, monkeys[i].anchor, 3, 4);
        int32_t b = scale_about(monkeys[i + 1].x,
                                monkeys[i + 1].anchor, 3, 4);
        assert(b - a == 15);
    }

    WsUiGroupItem edge_runs[] = {
        {3, 20, 16, 100, 16}, {3, 36, 16, 100, 16},
        {3, 330, 16, 100, 16}, {3, 346, 16, 100, 16},
    };
    ws_ui_group_assign(edge_runs, 4, display_width, 0);
    assert(edge_runs[0].anchor == 0 && edge_runs[1].anchor == 0);
    assert(edge_runs[2].anchor == 384 && edge_runs[3].anchor == 384);

    WsUiGroupItem dense[] = {{4, 8, 16, 100, 16}, {5, 340, 16, 100, 16}};
    ws_ui_group_assign(dense, 2, display_width, 1);
    assert(dense[0].anchor == 192 && dense[1].anchor == 192);

    /* A digit drawn on top of its background box is ONE element even though
     * the group key (CLUT / texpage / Y band / poly-vs-rect family) differs.
     * A primitive elsewhere on screen that merely shares the same columns is
     * NOT — overlap has to be judged in 2-D or union-find chains the whole HUD
     * into a single run and the corners collapse toward the centre. */
    WsUiGroupItem stacked[] = {
        { 9, 200, 90, 200, 20, 0, 0},  /* background box                  */
        {10, 210, 16, 204, 12, 0, 0},  /* digit sitting on the box        */
        {12, 204, 60, 191,  8, 0, 0},  /* label stacked directly above it */
        {11, 205, 24,  10, 16, 0, 0},  /* top row, same columns           */
    };
    ws_ui_group_assign(stacked, 4, display_width, 0);
    assert(stacked[0].root == stacked[1].root);
    /* One scanline of seam between the label and the box is still one
     * element — this is the WipEout 3 lap readout that flew apart. */
    assert(stacked[2].root == stacked[0].root);
    assert(stacked[3].root != stacked[0].root);

    /* Submission adjacency. WipEout 3's speed/shield readout ends at x=288 and
     * the meter bar it belongs to starts at x=306 with a different CLUT, so
     * neither shared columns nor a matching key can join them; only being
     * consecutive in the ordering-table walk while touching can. The bottom
     * -left cluster must NOT chain in through the same rule, so the item before
     * the readout is a far-away neighbour. Coordinates are the captured ones,
     * with display_width 508 rather than the 384 used above. */
    WsUiGroupItem submitted[] = {
        {0x9a4d,  26, 32, 228, 16, 0, 0},  /* bottom-left, 66 px clear     */
        {0xd82d, 277, 11, 234,  8, 0, 0},  /* last readout glyph           */
        {0xb938, 306,112, 236,  6, 0, 0},  /* meter bar, gap 18, rows meet */
        {0xf3e1, 418, 16, 236,  8, 0, 0},  /* bar end cap                  */
    };
    ws_ui_group_assign(submitted, 4, 508, 0);
    assert(submitted[1].root == submitted[2].root);   /* readout joins meters */
    assert(submitted[2].root == submitted[3].root);
    assert(submitted[0].root != submitted[1].root);   /* left stays separate  */
    assert(submitted[1].anchor == 508 && submitted[0].anchor == 0);

    /* The same two primitives NOT consecutive in the walk must not join --
     * otherwise the rule degenerates into plain proximity and chains the whole
     * bottom row into one centre-anchored run. */
    WsUiGroupItem apart[] = {
        {0xd82d, 277, 11, 234, 8, 0, 0},
        {0x1111,  10,  4,  10, 4, 0, 0},   /* unrelated, breaks adjacency */
        {0xb938, 306,112, 236, 6, 0, 0},
    };
    ws_ui_group_assign(apart, 3, 508, 0);
    assert(apart[0].root != apart[2].root);

    assert(ws_ui_anchor_for_bounds(8, 32, display_width) == 0);
    assert(ws_ui_anchor_for_bounds(344, 32, display_width) == 384);
    return 0;
}
