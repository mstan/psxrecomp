#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>

namespace PSXRecomp::TexPackDetail {

/* Conservative spatial rejection for the HD-texture upload tracker.
 *
 * PS1 VRAM coordinates start inside 1024x512, but a transfer rectangle may
 * extend by another full VRAM width/height before the renderer applies its
 * wrapping rules.  Cover that entire linear coordinate domain.  Rectangles
 * outside the documented domain are deliberately treated as "maybe" rather
 * than risking a false negative.
 *
 * Counts, rather than bits, make overlapping tracked uploads exact to remove.
 * A positive tile is only a candidate: callers still perform their existing
 * exact rectangle test before invalidating anything. */
class RectTileIndex {
public:
    static constexpr int kTileShift = 4;       /* 16x16 halfword tiles */
    static constexpr int kWidth = 2048;
    static constexpr int kHeight = 1024;
    static constexpr int kCols = kWidth >> kTileShift;
    static constexpr int kRows = kHeight >> kTileShift;

    void clear() {
        std::memset(refs_, 0, sizeof(refs_));
        unindexed_ = 0;
    }

    void add(int x, int y, int w, int h) {
        Bounds b;
        if (!bounds(x, y, w, h, &b)) {
            if (w > 0 && h > 0) ++unindexed_;
            return;
        }
        for (int ty = b.y0; ty <= b.y1; ++ty) {
            for (int tx = b.x0; tx <= b.x1; ++tx) {
                uint16_t &ref = refs_[ty * kCols + tx];
                assert(ref != UINT16_MAX);
                ++ref;
            }
        }
    }

    void remove(int x, int y, int w, int h) {
        Bounds b;
        if (!bounds(x, y, w, h, &b)) {
            if (w > 0 && h > 0) {
                assert(unindexed_ != 0);
                if (unindexed_ != 0) --unindexed_;
            }
            return;
        }
        for (int ty = b.y0; ty <= b.y1; ++ty) {
            for (int tx = b.x0; tx <= b.x1; ++tx) {
                uint16_t &ref = refs_[ty * kCols + tx];
                assert(ref != 0);
                if (ref != 0) --ref;
            }
        }
    }

    bool maybe_overlaps(int x, int y, int w, int h) const {
        if (w <= 0 || h <= 0) return false;
        Bounds b;
        if (unindexed_ != 0 || !bounds(x, y, w, h, &b)) return true;
        for (int ty = b.y0; ty <= b.y1; ++ty)
            for (int tx = b.x0; tx <= b.x1; ++tx)
                if (refs_[ty * kCols + tx] != 0) return true;
        return false;
    }

private:
    struct Bounds { int x0, y0, x1, y1; };

    static bool bounds(int x, int y, int w, int h, Bounds *out) {
        if (w <= 0 || h <= 0) return false;
        const int64_t x1 = static_cast<int64_t>(x) + w;
        const int64_t y1 = static_cast<int64_t>(y) + h;
        if (x < 0 || y < 0 || x1 > kWidth || y1 > kHeight) return false;
        out->x0 = x >> kTileShift;
        out->y0 = y >> kTileShift;
        out->x1 = static_cast<int>((x1 - 1) >> kTileShift);
        out->y1 = static_cast<int>((y1 - 1) >> kTileShift);
        return true;
    }

    uint16_t refs_[kCols * kRows]{};
    uint32_t unindexed_ = 0;
};

}  // namespace PSXRecomp::TexPackDetail
