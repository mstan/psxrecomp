#include "ws_present_layout.h"

#include <assert.h>

static int rect_equal(PsxPresentRect a, PsxPresentRect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

int main(void) {
    PsxPresentLayout gameplay;
    PsxPresentLayout menu;
    PsxPresentLayout fmv;

    psx_present_layout_compute(2560, 1440, 32, 9, 1, 0, &gameplay);
    psx_present_layout_compute(2560, 1440, 32, 9, 1, 1, &menu);
    psx_present_layout_compute(2560, 1440, 32, 9, 1, 1, &fmv);

    /* Frame content may change only the inner transform. Gameplay, menus and
     * FMV must retain one identical configured outer presentation aspect. */
    assert(rect_equal(gameplay.outer, menu.outer));
    assert(rect_equal(gameplay.outer, fmv.outer));
    assert(gameplay.outer.x == 0 && gameplay.outer.y == 360);
    assert(gameplay.outer.w == 2560 && gameplay.outer.h == 720);
    assert(rect_equal(gameplay.content, gameplay.outer));
    assert(menu.content.x == 800 && menu.content.y == 360);
    assert(menu.content.w == 960 && menu.content.h == 720);
    assert(rect_equal(menu.content, fmv.content));

    psx_present_layout_compute(2520, 1080, 21, 9, 1, 0, &gameplay);
    psx_present_layout_compute(2520, 1080, 21, 9, 1, 1, &menu);
    assert(rect_equal(gameplay.outer, menu.outer));
    assert(gameplay.outer.x == 0 && gameplay.outer.y == 0);
    assert(gameplay.outer.w == 2520 && gameplay.outer.h == 1080);
    assert(menu.content.x == 540 && menu.content.y == 0);
    assert(menu.content.w == 1440 && menu.content.h == 1080);

    /* Opt-in preserves the existing scene-dependent outer viewport for titles
     * that have not selected the separated policy. */
    psx_present_layout_compute(2560, 1440, 32, 9, 0, 0, &gameplay);
    psx_present_layout_compute(2560, 1440, 32, 9, 0, 1, &menu);
    assert(!rect_equal(gameplay.outer, menu.outer));
    assert(rect_equal(menu.content, menu.outer));

    return 0;
}
