#include "tex_pack_rect_index.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using PSXRecomp::TexPackDetail::RectTileIndex;

struct Rect { int x, y, w, h; };

static bool overlaps(const Rect &a, const Rect &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static uint32_t rng_state = 0xC001CAFEu;
static uint32_t next_random() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int fail(const char *what, int iteration) {
    std::fprintf(stderr, "tex_pack_rect_index_test: %s at iteration %d\n",
                 what, iteration);
    return 1;
}

int main() {
    RectTileIndex index;
    const Rect aligned{32, 48, 16, 16};
    index.add(aligned.x, aligned.y, aligned.w, aligned.h);
    if (!index.maybe_overlaps(32, 48, 1, 1)) return fail("missed occupied tile", 0);
    if (index.maybe_overlaps(16, 48, 16, 16)) return fail("crossed tile boundary", 0);
    index.remove(aligned.x, aligned.y, aligned.w, aligned.h);
    if (index.maybe_overlaps(32, 48, 1, 1)) return fail("remove left occupancy", 0);

    /* Unknown coordinate domains must degrade to a scan, never a false miss. */
    index.add(4096, 4096, 8, 8);
    if (!index.maybe_overlaps(0, 0, 1, 1)) return fail("unindexed rect was hidden", 0);
    index.remove(4096, 4096, 8, 8);
    if (index.maybe_overlaps(0, 0, 1, 1)) return fail("unindexed remove stuck", 0);

    std::vector<Rect> live;
    for (int iteration = 1; iteration <= 20000; ++iteration) {
        if (live.empty() || (next_random() & 3u) != 0) {
            Rect r;
            r.x = static_cast<int>(next_random() % 1985u);
            r.y = static_cast<int>(next_random() % 961u);
            r.w = 1 + static_cast<int>(next_random() % 64u);
            r.h = 1 + static_cast<int>(next_random() % 64u);
            live.push_back(r);
            index.add(r.x, r.y, r.w, r.h);
        } else {
            const size_t victim = next_random() % live.size();
            const Rect r = live[victim];
            index.remove(r.x, r.y, r.w, r.h);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(victim));
        }

        Rect q;
        q.x = static_cast<int>(next_random() % 1985u);
        q.y = static_cast<int>(next_random() % 961u);
        q.w = 1 + static_cast<int>(next_random() % 64u);
        q.h = 1 + static_cast<int>(next_random() % 64u);
        bool exact = false;
        for (const Rect &r : live) {
            if (overlaps(r, q)) { exact = true; break; }
        }
        if (exact && !index.maybe_overlaps(q.x, q.y, q.w, q.h))
            return fail("false-negative overlap", iteration);
    }

    index.clear();
    if (index.maybe_overlaps(0, 0, 2048, 1024)) return fail("clear failed", 20001);
    return 0;
}
