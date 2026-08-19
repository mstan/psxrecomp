/* tex_pack.cpp — HD texture replacement / dumping. See tex_pack.h for the
 * on-disk format and the Beetle PSX HW compatibility contract.
 *
 * Phase 1 was upload tracking, texture + palette hashing, and the dumper: prove
 * our filenames match Beetle's byte-for-byte before any shader work. That held,
 * and the counters said so (292 pack entries indexed, 80 asked for by a draw in
 * a single race) — but "matched" only ever meant "a draw wanted this key". No
 * pixels were substituted, because nothing here decoded a replacement and no
 * renderer sampled one, so a pack could look like it was working while changing
 * nothing on screen.
 *
 * Phase 2 closes that: decode a matched entry (lazily, on first use), and hand
 * the draw path the image plus the mapping from texture-page UVs into it.
 * Binding and sampling is the backend's job — see gr_set_replacement.
 *
 * C++ rather than C purely for <filesystem> (directory creation and the pack
 * scan), the same reason text_xlate.cpp is C++; the API is extern "C".
 */

#include "tex_pack.h"

#include "gpu.h"
#include "crc32.h"
#include "png_write.h"

/* Declarations only — STB_IMAGE_IMPLEMENTATION is compiled in
 * psx_window_icon.cpp. Its STBI_NO_STDIO must be matched here or the
 * declarations disagree with the definitions that exist: that build has no
 * stbi_load(), only the from-memory entry point, so we read the file. */
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"

#include <algorithm>
#include <chrono>
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

    /* Texture hashes the pack must NOT replace, from an optional exclude.txt
     * in the pack directory (one lowercase %x texture hash per line, '#'
     * comments). Matched on the TEXTURE hash alone, so one line covers every
     * palette variant of the same image.
     *
     * This exists because a single bad entry can be catastrophic rather than
     * cosmetic: WipEout 3 draws all of its text from four font atlases, so
     * replacing those with images that render wrong erases every glyph in the
     * game — menus, HUD, everything — while the rest of the pack is fine. Being
     * able to drop one texture beats disabling the whole pack, and keeps the
     * pack itself unmodified. */
    std::vector<uint32_t> excluded;

    /* Sorted (hash << 32) | palette_hash key sets. */
    std::vector<uint64_t> known;    /* files present in the pack on disk       */
    std::vector<uint64_t> dumped;   /* keys written this session (dedup)       */
    std::vector<uint64_t> matched;  /* pack keys a draw has actually asked for */

    /* GP0(E2h), decoded. mask == 0 on both axes means "no texture window". */
    int tw_mask_x = 0, tw_mask_y = 0, tw_off_x = 0, tw_off_y = 0;

    /* Distinct sample rects per texture, in TEXEL coordinates relative to the
     * texture page (lim, exactly as the primitive addressed it).
     *
     * Authoring a replacement for an ATLAS needs to know where its cells are,
     * and a proportional font atlas has no grid to infer: the dumped bitmap
     * packs rows with no separating scanline and each glyph is a different
     * width, so reading cells back out of the image is guesswork. The game
     * already states every cell exactly, once per draw — this just records the
     * distinct ones. Authoring aid; nothing in the replace path reads it. */
    struct UvRect {
        uint64_t key;                    /* (tex hash << 32) | palette hash */
        uint16_t u0, v0, u1, v1;         /* inclusive texel bounds          */
        int16_t  origin_u, origin_v;     /* upload's texel origin in-page   */
        uint32_t hits;
    };
    std::vector<UvRect> uv_rects;

    /* Decoded replacement images, keyed like everything else. Loaded lazily on
     * the first draw that asks for one — indexing the pack reads filenames
     * only, so a 292-entry pack costs nothing until its textures are actually
     * on screen, and a pack far larger than VRAM never has to fit at once.
     * `pixels` empty with `tried` set means the file failed to decode; we
     * remember that so a broken entry is not re-opened every frame. */
    struct Repl {
        uint64_t key;
        int      w = 0, h = 0;        /* replacement image, pixels        */
        int      src_w = 0, src_h = 0;/* source texture, TEXELS           */
        int      origin_u = 0, origin_v = 0; /* upload origin in the page */
        bool     tried = false;
        bool     mask = false;        /* single ink colour: safe to recolour */
        int      ink_index = -1;      /* palette index this image inks        */
        uint8_t  img_max = 0;         /* full-ink max channel, 1..255         */
        int      dec_shift = 0;       /* depth shift it was analysed at       */
        int      ana_src_w = 0, ana_src_h = 0; /* texel grid it was analysed at */
        std::vector<uint8_t> pixels;  /* RGBA8, w*h*4                     */
        uint32_t gl_handle = 0;       /* backend-owned; 0 = not uploaded  */
    };
    std::vector<Repl> repl;

    /* Live override for substitution (tex_pack_replace_enabled). Separate from
     * replace_on so the pack stays indexed and the counters keep working while
     * the actual pixel swap is toggled for an A/B. */
    bool repl_apply = true;

    /* Which primitives were armed, and where they landed on screen. */
    struct Armed {
        uint64_t id;                     /* (tex hash << 32) | palette hash */
        int16_t  x0, y0, x1, y1;         /* primitive screen bounds         */
        uint32_t hits;
    };
    std::vector<Armed> armed;
    /* Session clock for the coverage report — how long the pack was actually
     * exercised, so a 10-second boot isn't read as "this pack covers 8%". */
    std::chrono::steady_clock::time_point started;

    /* Diagnostics. */
    uint64_t n_repl_hit = 0, n_repl_miss = 0, n_repl_decoded = 0, n_repl_failed = 0;
    /* Draws served by the palette-agnostic fallback, and draws it declined
     * because the pack image carries real colour structure. The second number
     * is the one that says whether the gate is doing its job. */
    uint64_t n_repl_recolour = 0, n_repl_multicol = 0;
    /* Why a lookup produced nothing. "hit + miss" alone cannot tell "the pack
     * has no image for this texture" from "no upload backs this primitive",
     * and those want opposite fixes. */
    uint64_t n_repl_calls = 0, n_repl_nocontain = 0, n_repl_notex = 0;
    /* Why containment fails, not just how often. Records the sample rect and
     * the upload that overlapped it MOST, so the two candidate causes can be
     * told apart: a rect spanning several uploads (best overlap is a strict
     * subset on one axis) versus a stale or re-uploaded record (best overlap
     * tiny or absent). Diagnostic only -- nothing in the replace path reads
     * it, and it is capped so a race cannot grow it without bound. */
    struct NoContain {
        int32_t  rx, ry, rw, rh;   /* the primitive's sample rect     */
        int32_t  ux, uy, uw, uh;   /* best-overlapping upload, or 0s  */
        uint32_t hash;             /* that upload's texture hash      */
        int32_t  ov;               /* overlap area, texels            */
        uint32_t hits;
    };
    std::vector<NoContain> nocontain;
    uint64_t n_uploads = 0, n_upload_dedup = 0, n_kills = 0;
    uint64_t n_restore_kept = 0;   /* uploads surviving savestate-restore revalidation */
    uint64_t n_state_calls = 0, n_state_dropped = 0; /* TEXPACK section apply telemetry */
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

/* Lowest pack key for this texture hash, or 0.
 *
 * pack_key puts the texture hash in the HIGH 32 bits and g.known is sorted
 * ascending, so a texture's palette variants form one contiguous run and this
 * lands on the lowest palette hash. That determinism is the point: the first
 * cut picked whichever filename directory_iterator happened to yield first,
 * and that order is unspecified -- so which variant donated a texture's shape
 * could differ between runs and between machines. */
uint64_t key_set_first_tex(const std::vector<uint64_t> &s, uint32_t hash) {
    const uint64_t lo = (uint64_t)hash << 32;
    auto it = std::lower_bound(s.begin(), s.end(), lo);
    if (it == s.end() || (uint32_t)(*it >> 32) != hash) return 0;
    return *it;
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

/* Config-owned exclusions ([video] hd_texture_exclude): merged with the pack
 * dir's optional exclude.txt so exclusions survive pack regeneration. */
extern "C" void tex_pack_add_excluded(uint32_t hash) {
    g.excluded.push_back(hash);
}

extern "C" void tex_pack_init(const char *disc_path, int enable_replace,
                              int enable_dump, const char *dir_override,
                              const char *pack_dir) {
    std::lock_guard<std::mutex> lk(g.mu);

    g.replace_on = false;
    g.dump_on    = false;
    g.uploads.clear();
    g.pal_memo.clear();
    g.known.clear();
    g.dumped.clear();
    g.matched.clear();
    g.dump_dir_ready = false;
    g.started        = std::chrono::steady_clock::now();

    if ((!enable_replace && !enable_dump) || !disc_path || !disc_path[0]) return;

    const std::filesystem::path disc(disc_path);
    const std::string stem = disc.stem().string();
    if (stem.empty()) return;

    std::filesystem::path parent =
        (dir_override && dir_override[0]) ? std::filesystem::path(dir_override)
                                          : disc.parent_path();

    /* A managed pack folder is addressed directly; the Beetle disc-stem
     * convention is the fallback for a pack dropped next to the disc. */
    g.pack_dir = (pack_dir && pack_dir[0])
                     ? std::filesystem::path(pack_dir)
                     : parent / (stem + "-texture-replacements");
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

        /* Optional per-pack exclusions. */
        if (FILE *f = std::fopen((g.pack_dir / "exclude.txt").string().c_str(), "r")) {
            char line[128];
            while (std::fgets(line, sizeof(line), f)) {
                char *s = line;
                while (*s == ' ' || *s == '\t') s++;
                if (*s == '#' || *s == '\n' || *s == '\r' || *s == '\0') continue;
                unsigned h = 0;
                if (std::sscanf(s, "%x", &h) == 1) g.excluded.push_back(h);
            }
            std::fclose(f);
        }
    }

    std::printf("[tex_pack] replace=%d dump=%d pack=%s (%zu entries) dump_dir=%s\n",
                (int)g.replace_on, (int)g.dump_on, g.pack_dir.string().c_str(),
                g.known.size(), g.dump_dir.string().c_str());
}

extern "C" void tex_pack_write_coverage(void) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.replace_on || g.pack_dir.empty() || g.known.empty()) return;

    /* Hand-rolled JSON: this is a flat object, and the runtime has no JSON
     * writer. The launcher reads it with nlohmann, which it already vendors. */
    const std::filesystem::path temp = g.pack_dir / "coverage.json.tmp";
    const std::filesystem::path final = g.pack_dir / "coverage.json";

    FILE *f = std::fopen(temp.string().c_str(), "wb");
    if (!f) return;

    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - g.started).count();

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"format_version\": 1,\n");
    std::fprintf(f, "  \"pack_entries\": %zu,\n", g.known.size());
    std::fprintf(f, "  \"matched\": %zu,\n", g.matched.size());
    std::fprintf(f, "  \"session_seconds\": %lld,\n", (long long)secs);

    /* The entries no draw ever asked for. This is the authoring view — it is
     * what tells a pack author whether a texture is genuinely unused or whether
     * the tracker simply never saw it. Capped so a pathological pack cannot
     * write an unbounded file. */
    std::fprintf(f, "  \"unmatched\": [");
    size_t written = 0;
    for (size_t i = 0; i < g.known.size() && written < 4096; i++) {
        if (key_set_contains(g.matched, g.known[i])) continue;
        std::fprintf(f, "%s\n    \"%x-%x\"", written ? "," : "",
                     (unsigned)(g.known[i] >> 32),
                     (unsigned)(g.known[i] & 0xFFFFFFFFu));
        written++;
    }
    std::fprintf(f, "%s]\n}\n", written ? "\n  " : "");
    std::fclose(f);

    /* Atomic replace, matching how mods/state.toml is persisted — a report
     * half-written by a crash on exit must not read as "0% coverage". */
    std::error_code ec;
    std::filesystem::rename(temp, final, ec);
    if (ec) std::filesystem::remove(temp, ec);
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

/* Savestate integration. The upload tracker is what makes HD substitution
 * possible -- a draw substitutes only when it lands inside a tracked upload --
 * and a savestate restore repopulates VRAM without uploads. Anything the game
 * uploads once (boot-time text strips, font atlases) therefore lost its HD
 * replacement after every load, until the game happened to re-upload it.
 *
 * The snapshot stores rects + hashes only (20 bytes/entry): pixels are
 * reconstructed from the RESTORED VRAM at apply time and verified against the
 * stored hash, so a kept entry is byte-exact by construction and the section
 * stays a few KB. Old states without the section simply skip this. */
extern "C" uint32_t tex_pack_state_bytes(void) {
    std::lock_guard<std::mutex> lk(g.mu);
    return (uint32_t)(4u + g.uploads.size() * 20u);
}

extern "C" void tex_pack_state_write(uint8_t *p) {
    std::lock_guard<std::mutex> lk(g.mu);
    const uint32_t n = (uint32_t)g.uploads.size();
    uint32_t off = 0;
    auto put32 = [&](uint32_t v) {
        p[off++] = (uint8_t)(v); p[off++] = (uint8_t)(v >> 8);
        p[off++] = (uint8_t)(v >> 16); p[off++] = (uint8_t)(v >> 24);
    };
    put32(n);
    for (const Upload &u : g.uploads) {
        put32((uint32_t)u.x); put32((uint32_t)u.y);
        put32((uint32_t)u.w); put32((uint32_t)u.h);
        put32(u.hash);
    }
}

extern "C" void tex_pack_state_apply(const uint8_t *p, uint64_t len,
                                     const uint16_t *vram) {
    if (!tex_pack_active() || !p || len < 4) return;
    /* The caller passes the savestate's OWN VRAM section. gpu_get_vram() is
     * the CPU mirror, which under the GL-authoritative pipeline is only kept
     * coherent where something forces it (CLUT rows) -- hashing tracked rects
     * against a stale mirror would silently drop every entry. */
    if (!vram) vram = gpu_get_vram();
    if (!vram) return;
    std::lock_guard<std::mutex> lk(g.mu);
    uint32_t off = 0;
    auto get32 = [&]() {
        uint32_t v = (uint32_t)p[off] | ((uint32_t)p[off+1] << 8) |
                     ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
        off += 4; return v;
    };
    g.n_state_calls++;
    const uint32_t n = get32();
    if (len < 4ull + (uint64_t)n * 20ull) return;
    g.uploads.clear();
    g.pal_memo.clear();
    size_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        const int x = (int)get32(), y = (int)get32();
        const int w = (int)get32(), h = (int)get32();
        const uint32_t hash = get32();
        if (w <= 0 || h <= 0 || x < 0 || y < 0 ||
            x + w > FB_WIDTH || y + h > FB_HEIGHT) continue;
        Upload up;
        up.x = x; up.y = y; up.w = w; up.h = h; up.hash = hash;
        up.pixels.resize((size_t)w * (size_t)h);
        for (int row = 0; row < h; row++)
            std::memcpy(up.pixels.data() + (size_t)row * w,
                        vram + ((size_t)(y + row) * FB_WIDTH + x),
                        (size_t)w * sizeof(uint16_t));
        if (crc32_compute((const uint8_t *)up.pixels.data(),
                          up.pixels.size() * sizeof(uint16_t)) != hash)
            continue;   /* VRAM diverged from this rect since it was saved */
        g.uploads.push_back(std::move(up));
        if (++kept >= MAX_UPLOADS) break;
    }
    g.n_restore_kept += kept;
    g.n_state_dropped += (uint64_t)n - kept;
}

extern "C" void tex_pack_invalidate(int x, int y, int w, int h) {
    if (!tex_pack_active()) return;
    std::lock_guard<std::mutex> lk(g.mu);
    invalidate_locked(x, y, w, h);
}

extern "C" void tex_pack_on_upload(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!tex_pack_active()) return;
    if (w <= 0 || h <= 0 || !pixels) return;

    std::lock_guard<std::mutex> lk(g.mu);

    /* A full-VRAM transfer is a savestate restore, not a texture. Beetle drops
     * these from tracking too -- but DROPPING EVERY TRACKED UPLOAD is what
     * made HD replacements (including the fonts, which are pack entries) fall
     * back to low-res after loading a state: substitution requires a draw to
     * land inside a tracked upload, the restore repopulates VRAM without any
     * uploads, and textures the game only uploads once (boot-time text strips)
     * never track again for the rest of the session.
     *
     * We can do better than dropping, exactly: each Upload carries its pixel
     * preimage, and this transfer carries the complete incoming VRAM. Keep an
     * entry iff its rect in the incoming image is byte-identical to the
     * preimage -- then its hash is still true of what VRAM now holds, which is
     * the precise condition substitution relies on. Anything the state differs
     * on is dropped as before. Palette memos stay dropped: they are keyed on
     * position and re-hash lazily from live VRAM, so correctness is unaffected.
     */
    if (w == FB_WIDTH && h == FB_HEIGHT) {
        size_t kept = 0;
        for (size_t i = 0; i < g.uploads.size(); ) {
            const Upload &u = g.uploads[i];
            bool same = (u.x >= 0 && u.y >= 0 &&
                         u.x + u.w <= FB_WIDTH && u.y + u.h <= FB_HEIGHT &&
                         u.pixels.size() == (size_t)u.w * (size_t)u.h);
            for (int row = 0; same && row < u.h; row++) {
                const uint16_t *inc = pixels + ((size_t)(u.y + row) * FB_WIDTH + u.x);
                if (std::memcmp(inc, u.pixels.data() + (size_t)row * u.w,
                                (size_t)u.w * sizeof(uint16_t)) != 0)
                    same = false;
            }
            if (same) { kept++; i++; }
            else      { g.n_kills++; g.uploads.erase(g.uploads.begin() + (ptrdiff_t)i); }
        }
        g.n_restore_kept += kept;
        g.pal_memo.clear();
        return;
    }

    invalidate_locked(x, y, w, h);

    if (g.uploads.size() >= MAX_UPLOADS) return;

    const size_t n = (size_t)w * (size_t)h;
    const uint32_t hash = crc32_compute((const uint8_t *)pixels, n * sizeof(uint16_t));

    for (const Upload &u : g.uploads) {
        /* Dedup only when the POSITION matches too. The game re-uploads some
         * textures (the UI text strips) at a rect one scanline off from the
         * first upload; deduping by content alone kept the STALE rect, so the
         * draw's origin was computed one texel high -- glyph tops cut off,
         * bottoms duplicated, and under LINEAR sampling that same one-texel
         * error is the neighbour-bleed seam. Same content at a new position is
         * a new upload; invalidate_locked above already retired any overlap. */
        if (u.hash == hash && u.w == w && u.h == h &&
            u.x == x && u.y == y) { g.n_upload_dedup++; return; }
    }

    Upload up;
    up.x = x; up.y = y; up.w = w; up.h = h;
    up.hash = hash;
    up.pixels.assign(pixels, pixels + n);
    g.uploads.push_back(std::move(up));
    g.n_uploads++;
}

/* Is this replacement a single-colour MASK -- one ink colour plus holes?
 *
 * This is the gate on palette-agnostic substitution, and it has to be a
 * property of the DATA, not a list of texture hashes we happen to know are
 * fonts. Recolouring means throwing the image's RGB away and taking the colour
 * from the live CLUT instead. That is exact when the image only ever had one
 * colour to begin with -- a glyph, an icon, a HUD symbol -- and wrong for
 * anything with internal colour structure, where it would flatten the artwork
 * to a single hue. So: allow it only where there is nothing to lose.
 *
 * Alpha here is meaning, not opacity (0 = colour index 0, the cutout hole), so
 * a pixel only counts as ink once it is past the same threshold the shader
 * discards on. Anti-aliased edges keep the ink colour and vary alpha, so they
 * do not make an image look multi-coloured. */
static bool image_is_mask(const std::vector<uint8_t> &px, uint8_t *out_max) {
    bool seen = false;
    uint8_t r0 = 0, g0 = 0, b0 = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i + 3] == 0) continue;             /* hole: carries no colour */
        if (!seen) { r0 = px[i]; g0 = px[i + 1]; b0 = px[i + 2]; seen = true; continue; }
        if (px[i] != r0 || px[i + 1] != g0 || px[i + 2] != b0) return false;
    }
    if (!seen) return false;
    /* The shader divides the pack pixel's intensity by this to recover the
     * antialiasing ramp, so a black ink would divide by zero. expand_texel
     * writes alpha 127 for an STP-set texel BEFORE testing for black, so a
     * black semi-transparent ink really can reach here. */
    uint8_t mx = r0 > g0 ? r0 : g0;
    if (b0 > mx) mx = b0;
    if (mx == 0) return false;
    *out_max = mx;
    return true;
}

/* The palette index this image inks, taken from the source texels rather than
 * assumed to be 1. Majority vote over texels whose image centre is inked, so a
 * stray hand-drawn pixel cannot swing it. -1 when nothing qualifies. */
static int mask_ink_index(const State::Repl &r, const Upload &up, int shift) {
    if (r.src_w <= 0 || r.src_h <= 0) return -1;
    const int k = r.w / (r.src_w > 0 ? r.src_w : 1);
    if (k <= 0 || r.w != k * r.src_w || r.h != k * r.src_h) return -1;
    const int per = 1 << shift, bpp = 16 >> shift, m = (1 << bpp) - 1;
    uint32_t cnt[256] = {0};
    for (int tv = 0; tv < r.src_h; tv++) {
        for (int tu = 0; tu < r.src_w; tu++) {
            const size_t si = (size_t)tv * up.w + (size_t)(tu >> shift);
            if (si >= up.pixels.size()) continue;
            const int idx = (up.pixels[si] >> ((tu & (per - 1)) * bpp)) & m;
            const size_t c = (((size_t)(tv * k + k / 2) * r.w)
                              + (size_t)(tu * k + k / 2)) * 4 + 3;
            if (c < r.pixels.size() && r.pixels[c]) cnt[idx]++;
        }
    }
    int best = -1;
    uint32_t bestn = 0;
    for (int i = 0; i < 256; i++) if (cnt[i] > bestn) { bestn = cnt[i]; best = i; }
    return bestn ? best : -1;
}

/* Decode a pack entry on first use. Caller holds g.mu. Returns null when there
 * is no file for this key or it could not be decoded; the failure is cached so
 * a broken entry is not reopened every frame. */
State::Repl *repl_get_locked(uint64_t key, const Upload &up, int shift,
                             int base_x, int base_y) {
    for (State::Repl &r : g.repl) {
        if (r.key == key)
            return (r.tried && r.pixels.empty()) ? nullptr : &r;
    }

    State::Repl r;
    r.key   = key;
    r.tried = true;
    r.src_w = up.w << shift;      /* upload width is halfwords; texels differ by depth */
    r.src_h = up.h;
    r.origin_u = (up.x - base_x) << shift;
    r.origin_v = up.y - base_y;

    char name[64];
    std::snprintf(name, sizeof(name), "%x-%x.png",
                  (unsigned)(key >> 32), (unsigned)(key & 0xFFFFFFFFu));
    const std::filesystem::path path = g.pack_dir / name;

    std::vector<uint8_t> file;
    if (FILE *f = std::fopen(path.string().c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (len > 0) {
            file.resize((size_t)len);
            if (std::fread(file.data(), 1, file.size(), f) != file.size())
                file.clear();
        }
        std::fclose(f);
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc *px = file.empty() ? nullptr
                               : stbi_load_from_memory(file.data(), (int)file.size(),
                                                       &w, &h, &comp, 4);
    if (px && w > 0 && h > 0) {
        r.w = w; r.h = h;
        r.pixels.assign(px, px + (size_t)w * h * 4);
        r.mask = image_is_mask(r.pixels, &r.img_max);
        if (r.mask) {
            r.ink_index = mask_ink_index(r, up, shift);
            if (r.ink_index < 0) r.mask = false;
        }
        r.dec_shift = shift;
        r.ana_src_w = r.src_w;
        r.ana_src_h = r.src_h;
        g.n_repl_decoded++;
    } else {
        g.n_repl_failed++;
    }
    if (px) stbi_image_free(px);

    g.repl.push_back(std::move(r));
    State::Repl &ins = g.repl.back();
    return ins.pixels.empty() ? nullptr : &ins;
}

extern "C" int tex_pack_lookup_replacement(const int lim[4], uint16_t clut_x,
                                           uint16_t clut_y, uint16_t texpage,
                                           TexPackRepl *out) {
    if (!out) return 0;
    if (!tex_pack_active() || !lim) return 0;

    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.replace_on || !g.repl_apply || g.known.empty()) return 0;

    int depth = (texpage >> 7) & 3;
    if (depth > 2) depth = 2;
    const int shift  = (depth == 0) ? 2 : (depth == 1) ? 1 : 0;
    const int base_x = (texpage & 0xF) * 64;
    const int base_y = ((texpage >> 4) & 1) * 256;

    const int pal_n = (depth == 0) ? 16 : (depth == 1) ? 256 : 0;
    const uint32_t pal = palette_hash(clut_x, clut_y, pal_n);

    int rx, ry, rw, rh;
    sampled_vram_rect(lim, base_x, base_y, shift, &rx, &ry, &rw, &rh);

    /* Which upload actually BACKS this primitive, not merely which ones its
     * sample rect brushes against.
     *
     * The matched-set bookkeeping in tex_pack_on_textured_prim intentionally
     * uses overlap, because that is Beetle's rule for "this pack entry got
     * asked for". Substituting pixels is a stricter question: the replacement
     * image covers exactly one upload's rect, and UVs outside it address
     * nothing. Taking the first OVERLAPPING upload that happened to have a pack
     * entry substituted an unrelated texture under the primitive's own UVs --
     * which is why every glyph vanished while the font's own key was still
     * reported unmatched. It was sampling someone else's image.
     *
     * So: require CONTAINMENT, and scan newest-first, since a later upload to
     * the same VRAM region is the live one. */
    g.n_repl_calls++;
    bool contained = false;
    for (size_t i = g.uploads.size(); i-- > 0; ) {
        const Upload &up = g.uploads[i];
        if (rx < up.x || ry < up.y ||
            rx + rw > up.x + up.w || ry + rh > up.y + up.h) continue;
        contained = true;
        uint64_t key = pack_key(up.hash, pal);
        int recolour = 0;
        if (!key_set_contains(g.known, key)) {
            /* No file for this exact CLUT. We still know WHICH image this is --
             * the texture hash identifies it -- we just do not know its colour.
             * So borrow the texture's lowest-keyed variant for its SHAPE and
             * resolve the colour from the live CLUT in the shader.
             *
             * A pack entry is keyed (texture, palette), but a game that
             * recolours by swapping the CLUT -- fading text, flashing a
             * selection, drawing a glyph once per outline offset -- walks
             * through far more palettes than any pack ships, and every one of
             * those draws would otherwise fall back to the original low-res
             * texture. Gated on the borrowed image being a single-colour mask,
             * where there is no colour structure to destroy. */
            /* Palettised draws only. A 15bpp primitive has pal_n == 0 and hence
             * pal == 0, so its exact key never matches and it would otherwise
             * fall through to a 4bpp-dumped image whose src_w is four times
             * wrong. There is also no CLUT to recolour from. */
            if (pal_n == 0) continue;
            const uint64_t donor = key_set_first_tex(g.known, up.hash);
            if (!donor) { g.n_repl_notex++; continue; }
            key = donor;
            recolour = 1;
        }
        bool skip = false;
        for (uint32_t ex : g.excluded) { if (ex == up.hash) { skip = true; break; } }
        if (skip) continue;

        State::Repl *r = repl_get_locked(key, up, shift, base_x, base_y);
        if (!r) { g.n_repl_miss++; return 0; }
        if (recolour) {
            /* continue, not return: the neighbouring key and exclude tests both
             * fall through to the next upload, and a newer non-mask upload must
             * not block an older one that matches on its exact key. */
            const int sw = up.w << shift;
            if (!r->mask || r->ink_index < 0 ||
                r->dec_shift != shift ||                    /* same texel depth */
                r->ana_src_w != sw || r->ana_src_h != up.h  /* same texel grid  */) {
                g.n_repl_multicol++;
                continue;
            }
            g.n_repl_recolour++;
        }

        out->pixels   = r->pixels.data();
        out->width    = r->w;
        out->height   = r->h;
        /* From the LIVE containing upload, not the entry's first decode. The
         * Repl caches the origin of whichever upload triggered its decode, but
         * origin is a property of THIS draw's upload and texture page. With
         * position-exact dedup above, `up` is the rect the draw actually
         * samples, so this is now correct where the first attempt (which read
         * the stale deduped rect) was not.
         *
         * src stays derived from the upload too: same texture content always
         * has the same dimensions, so this only ever differs from the cache by
         * the drifted position. */
        out->src_w    = up.w << shift;
        out->src_h    = up.h;
        out->origin_u = (up.x - base_x) << shift;
        out->origin_v = up.y - base_y;
        out->id       = r->key;
        out->gl_handle = &r->gl_handle;
        out->recolour  = recolour;
        /* Written on EVERY success path: TexPackRepl is uninitialised stack
         * in tex_pack_arm and glb_set_replacement reads it directly. */
        out->ink_index = recolour ? r->ink_index : 0;
        out->ref_max   = recolour ? (float)r->img_max / 255.0f : 1.0f;
        g.n_repl_hit++;
        return 1;
    }
    if (!contained) {
        g.n_repl_nocontain++;
        /* Record WHY, capped. Find the upload that overlapped this sample
         * rect most; its shape against the rect distinguishes "the rect spans
         * several uploads" from "no live record covers it any more". */
        if (g.nocontain.size() < 64) {
            int best = -1, best_ov = 0;
            for (size_t k = 0; k < g.uploads.size(); k++) {
                const Upload &u = g.uploads[k];
                const int ox = std::min(rx + rw, u.x + u.w) - std::max(rx, u.x);
                const int oy = std::min(ry + rh, u.y + u.h) - std::max(ry, u.y);
                if (ox <= 0 || oy <= 0) continue;
                if (ox * oy > best_ov) { best_ov = ox * oy; best = (int)k; }
            }
            bool seen = false;
            for (State::NoContain &nc : g.nocontain) {
                if (nc.rx == rx && nc.ry == ry && nc.rw == rw && nc.rh == rh) {
                    nc.hits++; seen = true; break;
                }
            }
            if (!seen) {
                State::NoContain nc {};
                nc.rx = rx; nc.ry = ry; nc.rw = rw; nc.rh = rh;
                if (best >= 0) {
                    const Upload &u = g.uploads[(size_t)best];
                    nc.ux = u.x; nc.uy = u.y; nc.uw = u.w; nc.uh = u.h;
                    nc.hash = u.hash; nc.ov = best_ov;
                }
                nc.hits = 1;
                g.nocontain.push_back(nc);
            }
        }
    }
    g.n_repl_miss++;
    return 0;
}

extern "C" int tex_pack_replace_enabled(int set) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (set >= 0) g.repl_apply = set != 0;
    return g.repl_apply ? 1 : 0;
}

extern "C" void tex_pack_note_armed(unsigned long long id,
                                    int x0, int y0, int x1, int y1) {
    std::lock_guard<std::mutex> lk(g.mu);
    for (State::Armed &a : g.armed) {
        if (a.id == id) {
            if (x0 < a.x0) a.x0 = (int16_t)x0;
            if (y0 < a.y0) a.y0 = (int16_t)y0;
            if (x1 > a.x1) a.x1 = (int16_t)x1;
            if (y1 > a.y1) a.y1 = (int16_t)y1;
            a.hits++;
            return;
        }
    }
    if (g.armed.size() >= 256) return;
    State::Armed a;
    a.id = id;
    a.x0 = (int16_t)x0; a.y0 = (int16_t)y0;
    a.x1 = (int16_t)x1; a.y1 = (int16_t)y1;
    a.hits = 1;
    g.armed.push_back(a);
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

        /* Census of distinct cells, only while dumping — this is an authoring
         * tool and the linear scan below is not something a play session
         * should carry. Capped; a font atlas needs a couple of hundred. */
        if (g.dump_on && g.uv_rects.size() < 4096) {
            const int ou = (up.x - base_x) << shift;
            const int ov = up.y - base_y;
            bool seen = false;
            for (State::UvRect &r : g.uv_rects) {
                if (r.key == key && r.u0 == lim[0] && r.v0 == lim[1] &&
                    r.u1 == lim[2] && r.v1 == lim[3]) { r.hits++; seen = true; break; }
            }
            if (!seen) {
                State::UvRect r;
                r.key = key;
                r.u0 = (uint16_t)lim[0]; r.v0 = (uint16_t)lim[1];
                r.u1 = (uint16_t)lim[2]; r.v1 = (uint16_t)lim[3];
                r.origin_u = (int16_t)ou; r.origin_v = (int16_t)ov;
                r.hits = 1;
                g.uv_rects.push_back(r);
            }
        }
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
            "\"dump_written\":%llu,\"dump_failed\":%llu,"
            "\"repl_images\":%zu,\"repl_decoded\":%llu,\"repl_failed\":%llu,"
            "\"repl_hit\":%llu,\"repl_miss\":%llu,"
            "\"repl_recolour\":%llu,\"repl_multicol\":%llu,"
            "\"repl_calls\":%llu,\"repl_nocontain\":%llu,"
            "\"restore_kept\":%llu,\"state_calls\":%llu,\"state_dropped\":%llu,"
            "\"repl_notex\":%llu}",
            (int)g.replace_on, (int)g.dump_on,
            pack_dir.c_str(), dump_dir.c_str(),
            g.known.size(), g.matched.size(), g.uploads.size(),
            (unsigned long long)g.n_uploads, (unsigned long long)g.n_upload_dedup,
            (unsigned long long)g.n_kills, (unsigned long long)g.n_prims,
            (unsigned long long)g.n_pal_hash, (unsigned long long)g.n_pal_memo_hit,
            g.dumped.size(),
            (unsigned long long)g.n_dump_written, (unsigned long long)g.n_dump_failed,
            g.repl.size(),
            (unsigned long long)g.n_repl_decoded, (unsigned long long)g.n_repl_failed,
            (unsigned long long)g.n_repl_hit, (unsigned long long)g.n_repl_miss,
            (unsigned long long)g.n_repl_recolour,
            (unsigned long long)g.n_repl_multicol,
            (unsigned long long)g.n_repl_calls,
            (unsigned long long)g.n_repl_nocontain,
            (unsigned long long)g.n_restore_kept,
            (unsigned long long)g.n_state_calls,
            (unsigned long long)g.n_state_dropped,
            (unsigned long long)g.n_repl_notex);
    } else if (!std::strcmp(subcmd, "uploads")) {
        n = std::snprintf(out, (size_t)cap, "[");
        for (size_t i = 0; i < g.uploads.size() && n < cap - 64; i++) {
            const Upload &u = g.uploads[i];
            n += std::snprintf(out + n, (size_t)(cap - n),
                               "%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"hash\":\"%x\"}",
                               i ? "," : "", u.x, u.y, u.w, u.h, (unsigned)u.hash);
        }
        n += std::snprintf(out + n, (size_t)(cap - n), "]");
    } else if (!std::strcmp(subcmd, "repl")) {
        /* Each decoded replacement with the numbers that address it. If a
         * texture draws blank, this is where to look first: src_w/src_h are
         * the SOURCE size in texels and origin_u/v its offset within the
         * texture page, and the shader maps (u - origin) / src into the image.
         * Wrong values there sample outside the image and discard. */
        n = std::snprintf(out, (size_t)cap, "[");
        for (size_t i = 0; i < g.repl.size() && n < cap - 160; i++) {
            const State::Repl &r = g.repl[i];
            n += std::snprintf(out + n, (size_t)(cap - n),
                "%s{\"tex\":\"%x-%x\",\"img_w\":%d,\"img_h\":%d,"
                "\"src_w\":%d,\"src_h\":%d,\"origin_u\":%d,\"origin_v\":%d,"
                "\"decoded\":%s}",
                i ? "," : "",
                (unsigned)(r.key >> 32), (unsigned)(r.key & 0xFFFFFFFFu),
                r.w, r.h, r.src_w, r.src_h, r.origin_u, r.origin_v,
                r.pixels.empty() ? "false" : "true");
        }
        n += std::snprintf(out + n, (size_t)(cap - n), "]");
    } else if (!std::strcmp(subcmd, "nocontain")) {
        /* Sample rects that no tracked upload contained, with the upload that
         * overlapped each one most. If ov is a large fraction of the rect and
         * the upload is short on one axis, the rect spans several uploads; if
         * ov is 0 or tiny, no live record covers it at all. */
        n = std::snprintf(out, (size_t)cap, "[");
        for (size_t i = 0; i < g.nocontain.size() && n < cap - 200; i++) {
            const State::NoContain &c = g.nocontain[i];
            n += std::snprintf(out + n, (size_t)(cap - n),
                "%s{\"rect\":[%d,%d,%d,%d],\"best\":[%d,%d,%d,%d],"
                "\"hash\":\"%x\",\"ov\":%d,\"hits\":%u}",
                i ? "," : "", c.rx, c.ry, c.rw, c.rh,
                c.ux, c.uy, c.uw, c.uh, c.hash, c.ov, c.hits);
        }
        n += std::snprintf(out + n, (size_t)(cap - n), "]");
    } else if (!std::strcmp(subcmd, "armed")) {
        /* Every texture that actually got substituted, with the screen box its
         * primitives covered. Cross-reference against where the HUD draws. */
        n = std::snprintf(out, (size_t)cap, "[");
        for (size_t i = 0; i < g.armed.size() && n < cap - 128; i++) {
            const State::Armed &a = g.armed[i];
            n += std::snprintf(out + n, (size_t)(cap - n),
                "%s{\"tex\":\"%x-%x\",\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,\"hits\":%u}",
                i ? "," : "",
                (unsigned)(a.id >> 32), (unsigned)(a.id & 0xFFFFFFFFu),
                a.x0, a.y0, a.x1, a.y1, a.hits);
        }
        n += std::snprintf(out + n, (size_t)(cap - n), "]");
    } else if (!std::strcmp(subcmd, "uvs")) {
        /* Every distinct cell a draw addressed, per texture. For an atlas this
         * IS the cell layout, stated by the game rather than inferred from the
         * bitmap. Only populated while dumping. */
        n = std::snprintf(out, (size_t)cap, "[");
        for (size_t i = 0; i < g.uv_rects.size() && n < cap - 128; i++) {
            const State::UvRect &r = g.uv_rects[i];
            n += std::snprintf(out + n, (size_t)(cap - n),
                "%s{\"tex\":\"%x-%x\",\"u0\":%u,\"v0\":%u,\"u1\":%u,\"v1\":%u,"
                "\"ou\":%d,\"ov\":%d,\"hits\":%u}",
                i ? "," : "",
                (unsigned)(r.key >> 32), (unsigned)(r.key & 0xFFFFFFFFu),
                r.u0, r.v0, r.u1, r.v1, r.origin_u, r.origin_v, r.hits);
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
