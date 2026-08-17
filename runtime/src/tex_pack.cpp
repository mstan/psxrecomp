/* tex_pack.cpp — HD texture replacement / dumping. See tex_pack.h for the
 * on-disk format and the Beetle PSX HW compatibility contract.
 *
 * Phase 1 scope: upload tracking, texture + palette hashing, and the dumper.
 * Nothing here touches rendering yet — the point of this stage is to prove our
 * filenames match Beetle's byte-for-byte before any shader work is done.
 *
 * C++ rather than C purely for <filesystem> (directory creation and the pack
 * scan), the same reason text_xlate.cpp is C++; the API is extern "C".
 */

#include "tex_pack.h"

#include "gpu.h"
#include "crc32.h"
#include "png_write.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <cstdio>
#include <cstring>

namespace {

constexpr int FB_WIDTH  = 1024;
constexpr int FB_HEIGHT = 512;

/* Cap on simultaneously tracked uploads. A PS1 title keeps far fewer textures
 * resident than this (VRAM is 1 MiB total); the cap only bounds the worst case
 * where a game streams uploads without ever overwriting them. */
constexpr size_t MAX_UPLOADS = 8192;

/* ---- state ------------------------------------------------------------- */

struct Upload {
    int      x, y, w, h;   /* VRAM halfword coords */
    uint32_t hash;
    std::vector<uint16_t> pixels;  /* w*h, as uploaded — the hash preimage */
};

/* Memoised CLUT hash. Games re-draw from the same CLUT thousands of times per
 * frame; without this the palette CRC dominates the per-primitive cost. */
struct PalMemo {
    int      x, y, n;
    uint32_t hash;
};

struct State {
    std::mutex mu;

    bool replace_on = false;
    bool dump_on    = false;

    std::filesystem::path pack_dir;
    std::filesystem::path dump_dir;
    bool dump_dir_ready = false;

    std::vector<Upload>  uploads;
    std::vector<PalMemo> pal_memo;

    /* Sorted (hash << 32) | palette_hash key sets. */
    std::vector<uint64_t> known;    /* files present in the pack on disk       */
    std::vector<uint64_t> dumped;   /* keys written this session (dedup)       */
    std::vector<uint64_t> matched;  /* pack keys a draw has actually asked for */

    /* GP0(E2h), decoded. mask == 0 on both axes means "no texture window". */
    int tw_mask_x = 0, tw_mask_y = 0, tw_off_x = 0, tw_off_y = 0;

    /* Diagnostics. */
    uint64_t n_uploads = 0, n_upload_dedup = 0, n_kills = 0;
    uint64_t n_prims = 0, n_pal_hash = 0, n_pal_memo_hit = 0;
    uint64_t n_dump_written = 0, n_dump_failed = 0;
};

State g;

/* ---- small helpers ----------------------------------------------------- */

bool rects_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return false;
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

bool key_set_contains(const std::vector<uint64_t> &s, uint64_t k) {
    return std::binary_search(s.begin(), s.end(), k);
}

/* Returns true if the key was newly inserted. */
bool key_set_insert(std::vector<uint64_t> &s, uint64_t k) {
    auto it = std::lower_bound(s.begin(), s.end(), k);
    if (it != s.end() && *it == k) return false;
    s.insert(it, k);
    return true;
}

uint64_t pack_key(uint32_t hash, uint32_t pal) {
    return ((uint64_t)hash << 32) | (uint64_t)pal;
}

/* ---- hashing ----------------------------------------------------------- */

/* CRC32 of a CLUT row read straight out of VRAM, X wrapped exactly as the
 * hardware wraps a CLUT that runs off the right edge. This is Beetle's
 * vram_mirror fallback path in get_palette(), which is what modern Beetle
 * almost always takes — its rect-tracker path reads the same bytes whenever it
 * hits, so the two agree. */
uint32_t palette_hash(int clut_x, int clut_y, int n) {
    if (n <= 0) return 0;

    for (const PalMemo &m : g.pal_memo) {
        if (m.x == clut_x && m.y == clut_y && m.n == n) {
            g.n_pal_memo_hit++;
            return m.hash;
        }
    }

    const uint16_t *vram = gpu_get_vram();
    if (!vram) return 0;

    uint16_t row[256];
    if (n > 256) n = 256;
    const int py = clut_y & (FB_HEIGHT - 1);
    for (int i = 0; i < n; i++)
        row[i] = vram[py * FB_WIDTH + ((clut_x + i) & (FB_WIDTH - 1))];

    const uint32_t h = crc32_compute((const uint8_t *)row, (size_t)n * sizeof(uint16_t));
    g.n_pal_hash++;

    if (g.pal_memo.size() < 64) g.pal_memo.push_back(PalMemo{clut_x, clut_y, n, h});
    return h;
}

/* ---- the sampled VRAM rect --------------------------------------------- *
 * Reproduces Beetle's hd_texture_vram derivation (rhi_lib_vulkan.c, the
 * render_state.texture_mode != TextureMode_None block) so a draw is matched
 * against the same tracked uploads it would be there.
 *
 * `lim` is already post-wrap: gpu_uv.h's psx_uv_axis_limits collapses an axis
 * that crosses a 256 boundary to the full 0..255 range, which is exactly the
 * `max_u > 255` "assume the whole page is hit" case on Beetle's side. */
void sampled_vram_rect(const int lim[4], int base_x, int base_y, int shift,
                       int *rx, int *ry, int *rw, int *rh) {
    if (g.tw_mask_x == 0 && g.tw_mask_y == 0) {
        const bool wraps = (lim[0] == 0 && lim[2] == 255) ||
                           (lim[1] == 0 && lim[3] == 255);
        if (wraps) {
            *rx = base_x; *ry = base_y;
            *rw = 256 >> shift; *rh = 256;
            return;
        }
        const int min_u = lim[0] >> shift;
        const int max_u = (lim[2] + (1 << shift) - 1) >> shift;   /* round up */
        *rx = base_x + min_u;
        *ry = base_y + lim[1];
        /* The -1 is Beetle's, not a slip: without it a right-edge tile fails
         * the containment test against the upload it came from. Kept so our
         * match set is the same one Beetle produces, 0-width cases included. */
        *rw = (max_u - min_u + 1) - 1;
        *rh = lim[3] - lim[1] + 1;
    } else {
        /* Windowed: the sampled set is base | any subset of the free bits, i.e.
         * a rect of (free + 1) texels starting at base. */
        const int free_u = (~(g.tw_mask_x * 8)) & 0xFF;
        const int free_v = (~(g.tw_mask_y * 8)) & 0xFF;
        const int base_u = (g.tw_off_x & g.tw_mask_x) * 8;
        const int base_v = (g.tw_off_y & g.tw_mask_y) * 8;
        *rx = base_x + (base_u >> shift);
        *ry = base_y + base_v;
        *rw = (free_u + 1) >> shift;
        *rh = free_v + 1;
    }
}

/* ---- dumping ----------------------------------------------------------- */

/* PS1 1555 -> RGBA8 with Beetle's tri-state alpha convention: a fully black,
 * unset-STP texel is the transparent one the hardware discards; any other
 * unset-STP texel is opaque; STP set is semi-transparent. */
void expand_texel(uint16_t c, uint8_t *out) {
    const int r = ((c >> 0) & 0x1F) * 255 / 31;
    const int gg = ((c >> 5) & 0x1F) * 255 / 31;
    const int b = ((c >> 10) & 0x1F) * 255 / 31;
    int a;
    if ((c >> 15) & 1)              a = 127;
    else if (r == 0 && gg == 0 && b == 0) a = 0;
    else                            a = 255;
    out[0] = (uint8_t)r; out[1] = (uint8_t)gg; out[2] = (uint8_t)b; out[3] = (uint8_t)a;
}

void dump_upload(const Upload &up, int depth, int clut_x, int clut_y, uint32_t pal_hash) {
    const int shift = (depth == 0) ? 2 : (depth == 1) ? 1 : 0;
    const int ppp   = 1 << shift;                  /* texels per VRAM word */
    const int bpp   = 16 / ppp;
    const int mask  = (1 << bpp) - 1;

    const uint32_t out_w = (uint32_t)up.w * (uint32_t)ppp;
    const uint32_t out_h = (uint32_t)up.h;
    if (out_w == 0 || out_h == 0) return;

    uint16_t clut[256];
    const bool palettised = (depth != 2);
    if (palettised) {
        const uint16_t *vram = gpu_get_vram();
        if (!vram) return;
        const int n  = (depth == 0) ? 16 : 256;
        const int py = clut_y & (FB_HEIGHT - 1);
        for (int i = 0; i < n; i++)
            clut[i] = vram[py * FB_WIDTH + ((clut_x + i) & (FB_WIDTH - 1))];
    }

    std::vector<uint8_t> rgba((size_t)out_w * out_h * 4);
    size_t bi = 0;
    for (size_t wi = 0; wi < up.pixels.size(); wi++) {
        const uint16_t word = up.pixels[wi];
        for (int p = 0; p < ppp; p++) {
            const uint16_t sub = (uint16_t)((word >> (p * bpp)) & mask);
            expand_texel(palettised ? clut[sub] : sub, &rgba[bi]);
            bi += 4;
        }
    }

    if (!g.dump_dir_ready) {
        std::error_code ec;
        std::filesystem::create_directories(g.dump_dir, ec);
        g.dump_dir_ready = true;   /* one attempt; a failure surfaces per-file */
    }

    char name[64];
    /* Beetle tags a direct-colour texture -0 so the loader's palette-hash-0
     * lookup round-trips. We always resolve a CLUT for palettised modes, so the
     * "-missing" spelling it falls back to never arises here. */
    std::snprintf(name, sizeof name, "%x-%x.png", (unsigned)up.hash, (unsigned)pal_hash);

    const std::filesystem::path path = g.dump_dir / name;
    FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) { g.n_dump_failed++; return; }
    const int ok = png_write_rgba(f, rgba.data(), out_w, out_h);
    std::fclose(f);
    if (ok) g.n_dump_written++; else g.n_dump_failed++;
}

}  // namespace

/* ---- public API -------------------------------------------------------- */

extern "C" void tex_pack_init(const char *disc_path, int enable_replace,
                              int enable_dump, const char *dir_override) {
    std::lock_guard<std::mutex> lk(g.mu);

    g.replace_on = false;
    g.dump_on    = false;
    g.uploads.clear();
    g.pal_memo.clear();
    g.known.clear();
    g.dumped.clear();
    g.matched.clear();
    g.dump_dir_ready = false;

    if ((!enable_replace && !enable_dump) || !disc_path || !disc_path[0]) return;

    const std::filesystem::path disc(disc_path);
    const std::string stem = disc.stem().string();
    if (stem.empty()) return;

    std::filesystem::path parent =
        (dir_override && dir_override[0]) ? std::filesystem::path(dir_override)
                                          : disc.parent_path();

    g.pack_dir = parent / (stem + "-texture-replacements");
    g.dump_dir = parent / (stem + "-texture-dump");
    g.replace_on = enable_replace != 0;
    g.dump_on    = enable_dump != 0;

    /* Index the pack: filenames alone tell us which (texture, palette) pairs
     * have a replacement, so the draw path can answer "is there one?" without
     * touching the disk. */
    if (g.replace_on) {
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(g.pack_dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            const std::string fn = e.path().filename().string();
            unsigned h = 0, p = 0;
            char ext[16] = {0};
            if (std::sscanf(fn.c_str(), "%x-%x.%15s", &h, &p, ext) != 3) continue;
            if (std::strcmp(ext, "png") != 0) continue;   /* v1 decodes PNG only */
            key_set_insert(g.known, pack_key(h, p));
        }
    }

    std::printf("[tex_pack] replace=%d dump=%d pack=%s (%zu entries) dump_dir=%s\n",
                (int)g.replace_on, (int)g.dump_on, g.pack_dir.string().c_str(),
                g.known.size(), g.dump_dir.string().c_str());
}

extern "C" void tex_pack_shutdown(void) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.replace_on = false;
    g.dump_on    = false;
    g.uploads.clear();
    g.pal_memo.clear();
}

extern "C" int tex_pack_active(void) {
    return (g.replace_on || g.dump_on) ? 1 : 0;
}

extern "C" void tex_pack_set_texture_window(uint32_t raw) {
    if (!tex_pack_active()) return;
    std::lock_guard<std::mutex> lk(g.mu);
    g.tw_mask_x = (int)(raw & 0x1F);
    g.tw_mask_y = (int)((raw >> 5) & 0x1F);
    g.tw_off_x  = (int)((raw >> 10) & 0x1F);
    g.tw_off_y  = (int)((raw >> 15) & 0x1F);
}

namespace {

/* Caller holds g.mu. Split out so the upload path (which invalidates and then
 * inserts under one lock) cannot re-enter the non-recursive mutex. */
void invalidate_locked(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;

    const size_t before = g.uploads.size();
    g.uploads.erase(std::remove_if(g.uploads.begin(), g.uploads.end(),
                                   [&](const Upload &u) {
                                       return rects_overlap(u.x, u.y, u.w, u.h, x, y, w, h);
                                   }),
                    g.uploads.end());
    g.n_kills += before - g.uploads.size();

    g.pal_memo.erase(std::remove_if(g.pal_memo.begin(), g.pal_memo.end(),
                                    [&](const PalMemo &m) {
                                        return rects_overlap(m.x, m.y, m.n, 1, x, y, w, h);
                                    }),
                     g.pal_memo.end());
}

}  // namespace

extern "C" void tex_pack_invalidate(int x, int y, int w, int h) {
    if (!tex_pack_active()) return;
    std::lock_guard<std::mutex> lk(g.mu);
    invalidate_locked(x, y, w, h);
}

extern "C" void tex_pack_on_upload(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!tex_pack_active()) return;
    if (w <= 0 || h <= 0 || !pixels) return;

    std::lock_guard<std::mutex> lk(g.mu);

    /* A full-VRAM transfer is a savestate restore, not a texture; Beetle drops
     * these too, and tracking one would alias every subsequent lookup. */
    if (w == FB_WIDTH && h == FB_HEIGHT) {
        invalidate_locked(0, 0, FB_WIDTH, FB_HEIGHT);
        return;
    }

    invalidate_locked(x, y, w, h);

    if (g.uploads.size() >= MAX_UPLOADS) return;

    const size_t n = (size_t)w * (size_t)h;
    const uint32_t hash = crc32_compute((const uint8_t *)pixels, n * sizeof(uint16_t));

    for (const Upload &u : g.uploads) {
        if (u.hash == hash && u.w == w && u.h == h) { g.n_upload_dedup++; return; }
    }

    Upload up;
    up.x = x; up.y = y; up.w = w; up.h = h;
    up.hash = hash;
    up.pixels.assign(pixels, pixels + n);
    g.uploads.push_back(std::move(up));
    g.n_uploads++;
}

extern "C" void tex_pack_on_textured_prim(const int lim[4], uint16_t clut_x,
                                          uint16_t clut_y, uint16_t texpage) {
    if (!tex_pack_active() || !lim) return;

    int depth = (texpage >> 7) & 3;
    if (depth > 2) depth = 2;
    const int shift  = (depth == 0) ? 2 : (depth == 1) ? 1 : 0;
    const int base_x = (texpage & 0xF) * 64;
    const int base_y = ((texpage >> 4) & 1) * 256;

    std::lock_guard<std::mutex> lk(g.mu);
    g.n_prims++;

    const int pal_n = (depth == 0) ? 16 : (depth == 1) ? 256 : 0;
    const uint32_t pal = palette_hash(clut_x, clut_y, pal_n);

    int rx, ry, rw, rh;
    sampled_vram_rect(lim, base_x, base_y, shift, &rx, &ry, &rw, &rh);

    for (const Upload &up : g.uploads) {
        if (!rects_overlap(up.x, up.y, up.w, up.h, rx, ry, rw, rh)) continue;

        const uint64_t key = pack_key(up.hash, pal);
        if (g.replace_on && key_set_contains(g.known, key)) key_set_insert(g.matched, key);
        if (g.dump_on && key_set_insert(g.dumped, key))
            dump_upload(up, depth, clut_x, clut_y, pal);
    }
}

/* A filesystem path is not a JSON string. On Windows every separator in
 * "C:\Users\..." reads as an escape, and "\U" is not a legal one, so a raw %s
 * of a path produced a response no JSON parser would accept -- which is why
 * this whole surface silently could not be read from the wire. */
static std::string json_escape(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)(unsigned char)c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

extern "C" int tex_pack_debug_json(const char *subcmd, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    std::lock_guard<std::mutex> lk(g.mu);
    int n = 0;

    if (!subcmd || !std::strcmp(subcmd, "stats")) {
        const std::string pack_dir = json_escape(g.pack_dir.string());
        const std::string dump_dir = json_escape(g.dump_dir.string());
        n = std::snprintf(out, (size_t)cap,
            "{\"replace\":%d,\"dump\":%d,\"pack_dir\":\"%s\",\"dump_dir\":\"%s\","
            "\"pack_entries\":%zu,\"pack_matched\":%zu,\"live_uploads\":%zu,"
            "\"uploads\":%llu,\"upload_dedup\":%llu,\"kills\":%llu,\"prims\":%llu,"
            "\"pal_hash\":%llu,\"pal_memo_hit\":%llu,\"dumped\":%zu,"
            "\"dump_written\":%llu,\"dump_failed\":%llu}",
            (int)g.replace_on, (int)g.dump_on,
            pack_dir.c_str(), dump_dir.c_str(),
            g.known.size(), g.matched.size(), g.uploads.size(),
            (unsigned long long)g.n_uploads, (unsigned long long)g.n_upload_dedup,
            (unsigned long long)g.n_kills, (unsigned long long)g.n_prims,
            (unsigned long long)g.n_pal_hash, (unsigned long long)g.n_pal_memo_hit,
            g.dumped.size(),
            (unsigned long long)g.n_dump_written, (unsigned long long)g.n_dump_failed);
    } else if (!std::strcmp(subcmd, "uploads")) {
        n = std::snprintf(out, (size_t)cap, "[");
        for (size_t i = 0; i < g.uploads.size() && n < cap - 64; i++) {
            const Upload &u = g.uploads[i];
            n += std::snprintf(out + n, (size_t)(cap - n),
                               "%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"hash\":\"%x\"}",
                               i ? "," : "", u.x, u.y, u.w, u.h, (unsigned)u.hash);
        }
        n += std::snprintf(out + n, (size_t)(cap - n), "]");
    } else if (!std::strcmp(subcmd, "dumped") || !std::strcmp(subcmd, "missing")) {
        /* "dumped": what we wrote. "missing": pack entries no draw has asked
         * for yet — the authoring view of what the tracker still can't see. */
        const bool want_missing = !std::strcmp(subcmd, "missing");
        const std::vector<uint64_t> &src = want_missing ? g.known : g.dumped;
        bool first = true;
        n = std::snprintf(out, (size_t)cap, "[");
        for (size_t i = 0; i < src.size() && n < cap - 32; i++) {
            if (want_missing && key_set_contains(g.matched, src[i])) continue;
            n += std::snprintf(out + n, (size_t)(cap - n), "%s\"%x-%x\"",
                               first ? "" : ",",
                               (unsigned)(src[i] >> 32), (unsigned)(src[i] & 0xFFFFFFFFu));
            first = false;
        }
        n += std::snprintf(out + n, (size_t)(cap - n), "]");
    }

    if (n < 0) n = 0;
    if (n > cap) n = cap;
    return n;
}
