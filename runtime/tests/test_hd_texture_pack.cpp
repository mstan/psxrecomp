#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "hd_texture_pack.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
std::string key_name(uint32_t texture_hash, uint32_t palette_hash) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << texture_hash << '-'
        << palette_hash << ".png";
    return out.str();
}

std::string padded_key_name(uint32_t texture_hash, uint32_t palette_hash) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setw(8) << std::setfill('0')
        << texture_hash << '-' << palette_hash << ".png";
    return out.str();
}

void touch(const fs::path& path) {
    std::ofstream output(path, std::ios::binary);
    output.put('\0');
}

uint32_t png_crc(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

void append_be32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 24));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void append_png_chunk(std::vector<uint8_t>& png, const char type[4],
                      const std::vector<uint8_t>& payload) {
    append_be32(png, static_cast<uint32_t>(payload.size()));
    const size_t crc_start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    append_be32(png, png_crc(png.data() + crc_start, png.size() - crc_start));
}

/* Generate an original, tiny RGBA PNG in the test itself. The single stored
 * DEFLATE block keeps the fixture independent of an encoder library. */
void write_test_png(const fs::path& path, uint32_t width, uint32_t height) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1u + width * 4u));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); /* PNG filter: None */
        for (uint32_t x = 0; x < width; ++x) {
            raw.push_back(static_cast<uint8_t>(x * 3u));
            raw.push_back(static_cast<uint8_t>(y * 17u));
            raw.push_back(0xA5u);
            /* Exercise packs that use 7-bit alpha (127 = opaque),
             * not conventional 8-bit PNG opacity.  Keep the fixture aligned
             * with the shipping assets so the renderer's alpha contract stays
             * covered by the pack test. */
            raw.push_back(0x7Fu);
        }
    }
    check(raw.size() <= 65535u, "generated PNG fits one stored DEFLATE block");

    std::vector<uint8_t> zlib{0x78u, 0x01u, 0x01u};
    const uint16_t length = static_cast<uint16_t>(raw.size());
    const uint16_t inverse = static_cast<uint16_t>(~length);
    zlib.push_back(static_cast<uint8_t>(length));
    zlib.push_back(static_cast<uint8_t>(length >> 8));
    zlib.push_back(static_cast<uint8_t>(inverse));
    zlib.push_back(static_cast<uint8_t>(inverse >> 8));
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t value : raw) { s1 = (s1 + value) % 65521u; s2 = (s2 + s1) % 65521u; }
    append_be32(zlib, (s2 << 16) | s1);

    std::vector<uint8_t> png{0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    std::vector<uint8_t> ihdr;
    append_be32(ihdr, width); append_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8u, 6u, 0u, 0u, 0u});
    append_png_chunk(png, "IHDR", ihdr);
    append_png_chunk(png, "IDAT", zlib);
    append_png_chunk(png, "IEND", {});
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
}

struct TempTree {
    fs::path path;

    TempTree() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("psxrecomp_hd_pack_" + std::to_string(tick));
        fs::create_directories(path);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct Fixture {
    TempTree temp;
    fs::path root;
    fs::path replacements;
    std::vector<uint16_t> vram;
    std::vector<uint16_t> upload_words;
    std::vector<uint16_t> wrap_words;
    uint32_t upload_hash = 0;
    uint32_t wrap_hash = 0;
    uint32_t palette4_hash = 0;
    uint32_t palette8_hash = 0;

    Fixture()
        : root(temp.path / "pack"),
          replacements(root / "Example-texture-replacements"),
          vram(1024u * 512u, 0),
          upload_words(8u * 4u),
          wrap_words(4u * 4u) {
        fs::create_directories(replacements);
        for (unsigned i = 0; i < upload_words.size(); ++i)
            upload_words[i] = static_cast<uint16_t>(0x1200u + i * 17u);
        for (unsigned i = 0; i < wrap_words.size(); ++i)
            wrap_words[i] = static_cast<uint16_t>(0xA000u + i * 13u);
        upload_hash = hd_texture_crc32_words_le(upload_words.data(), upload_words.size());
        wrap_hash = hd_texture_crc32_words_le(wrap_words.data(), wrap_words.size());

        /* Both palettes wrap at the right edge, proving full-CLUT gathering. */
        for (unsigned i = 0; i < 16; ++i)
            vram[7u * 1024u + ((1020u + i) & 1023u)] =
                static_cast<uint16_t>(0x0100u + i * 3u);
        for (unsigned i = 0; i < 256; ++i)
            vram[8u * 1024u + ((900u + i) & 1023u)] =
                static_cast<uint16_t>(0x2000u + i * 5u);
        palette4_hash = hd_texture_hash_clut(
            vram.data(), vram.size(), 1020, 7, HD_TEXTURE_DEPTH_4BPP);
        palette8_hash = hd_texture_hash_clut(
            vram.data(), vram.size(), 900, 8, HD_TEXTURE_DEPTH_8BPP);

        /* Native upload is 8 words * 4 pixels/word by 4 rows. The 64x8 PNG is
         * a valid uniform 2x replacement for the live GL aspect check. */
        write_test_png(replacements / key_name(upload_hash, palette4_hash), 64, 8);
        touch(replacements / key_name(upload_hash, palette8_hash));
        touch(replacements / key_name(wrap_hash, 0));
        touch(replacements / "not-a-pack-key.png");
        touch(replacements / "123456789-1.png"); /* component is > 8 hex digits */

        std::ofstream hashes(root / "Hashes.ini");
        hashes << "[Textures]\n"
               << std::hex << upload_hash << '-' << palette4_hash
               << " = Test/Logical/Four bit.png\n"
               << "# comments and sections are ignored\n";
    }
};

HdTexturePack* open_pack(const fs::path& root) {
    HdTexturePack* pack = nullptr;
    char error[512]{};
    check(hd_texture_pack_create(root.string().c_str(), &pack,
                                 error, sizeof(error)) == 1,
          error[0] ? error : "pack opens");
    return pack;
}

HdTextureDrawQuery query4(const std::vector<uint16_t>& vram,
                          uint8_t u_first,
                          uint8_t u_last) {
    HdTextureDrawQuery query{};
    query.page_x = 10;
    query.page_y = 20;
    query.depth = HD_TEXTURE_DEPTH_4BPP;
    query.u_first = u_first;
    query.u_last = u_last;
    query.v_first = 0;
    query.v_last = 3;
    query.clut_x = 1020;
    query.clut_y = 7;
    query.vram = vram.data();
    query.vram_word_count = vram.size();
    return query;
}

void test_crc_exact_little_endian() {
    const uint16_t ascii_words[] = {0x3231, 0x3433, 0x3635, 0x3837};
    check(hd_texture_crc32_words_le(ascii_words, 4) == 0x9AE0DAAFu,
          "CRC32 is IEEE/zlib over explicit little-endian word bytes");
    check(hd_texture_crc32_words_le(nullptr, 0) == 0,
          "empty CRC32 is the IEEE empty-buffer value");
}

void test_scan_mapping(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;

    HdTexturePackInfo info{};
    hd_texture_pack_get_info(pack, &info);
    check(info.unique_key_count == 3,
          "scanner accepts only valid numeric PNG keys");
    check(info.ambiguous_key_count == 0, "fixture has no ambiguous aliases");
    check(info.logical_mapping_count == 1, "Hashes.ini logical map is parsed");
    check(std::string(info.replacement_root).find("-texture-replacements") !=
              std::string::npos,
          "pack root resolves its single replacement child");

    HdTexturePackEntry entry{};
    check(hd_texture_pack_lookup(pack, fixture.upload_hash,
                                 fixture.palette4_hash, &entry) ==
              HD_TEXTURE_LOOKUP_FOUND,
          "numeric texture/palette key lookup succeeds");
    check(std::string(entry.logical_path) == "Test/Logical/Four bit.png",
          "lookup exposes the logical path from Hashes.ini");
    hd_texture_pack_destroy(pack);

    /* The replacement directory is also a valid direct root; sibling
     * Hashes.ini remains discoverable. */
    pack = open_pack(fixture.replacements);
    if (pack) {
        check(hd_texture_pack_lookup(pack, fixture.upload_hash,
                                     fixture.palette4_hash, &entry) ==
                  HD_TEXTURE_LOOKUP_FOUND && entry.logical_path[0] != '\0',
              "direct replacement root finds sibling Hashes.ini");
        hd_texture_pack_destroy(pack);
    }

#ifdef _WIN32
    _putenv_s("PSXRECOMP_HD_TEXTURE_ROOT", fixture.root.string().c_str());
#else
    setenv("PSXRECOMP_HD_TEXTURE_ROOT", fixture.root.string().c_str(), 1);
#endif
    pack = nullptr;
    char error[128]{};
    check(hd_texture_pack_create(nullptr, &pack, error, sizeof(error)) == 1,
          "environment root is used when explicit path is absent");
    hd_texture_pack_destroy(pack);
#ifdef _WIN32
    _putenv_s("PSXRECOMP_HD_TEXTURE_ROOT", "");
#else
    unsetenv("PSXRECOMP_HD_TEXTURE_ROOT");
#endif
}

void test_palette_match_and_invalidation(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    uint32_t hash = 0;
    check(hd_texture_pack_track_upload(pack, 10, 20, 8, 4,
                                       fixture.upload_words.data(),
                                       fixture.upload_words.size(), &hash) == 1 &&
              hash == fixture.upload_hash,
          "upload tracking returns exact content hash");

    HdTextureDrawQuery four = query4(fixture.vram, 0, 31);
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &four, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "4bpp page/depth/UV query matches a containing upload");
    check(match.entry.palette_hash == fixture.palette4_hash &&
              match.source_word_x == 0 && match.source_y == 0,
          "match reports palette key and upload-relative origin");

    /* The live GP0 adapter derives page base/depth from texpage. Page X/Y are
     * fixed hardware bases, so UV 40/20 reaches the upload at VRAM 10/20 in
     * 4bpp (four pixels per word). */
    check(hd_texture_pack_match_draw(
              pack, 0x0000u, 1020, 7, 40, 71, 20, 23,
              fixture.vram.data(), fixture.vram.size(), &match) ==
              HD_TEXTURE_LOOKUP_FOUND && match.source_word_x == 0 &&
              match.source_y == 0,
          "live texpage/CLUT/UV adapter resolves a 4bpp replacement hit");
    check(hd_texture_pack_match_draw(
              pack, 0x0180u, 1020, 7, 0, 0, 0, 0,
              fixture.vram.data(), fixture.vram.size(), &match) ==
              HD_TEXTURE_LOOKUP_ERROR,
          "reserved texpage depth fails closed");

    /* Entry 20 is outside a 16-color CLUT and must not alter a 4bpp match. */
    const size_t outside4 = 7u * 1024u + ((1020u + 20u) & 1023u);
    fixture.vram[outside4] ^= 0x55AAu;
    check(hd_texture_pack_match(pack, &four, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "4bpp hashes exactly 16 CLUT entries, not adjacent VRAM");
    fixture.vram[outside4] ^= 0x55AAu;
    const size_t inside4 = 7u * 1024u + ((1020u + 15u) & 1023u);
    fixture.vram[inside4] ^= 1u;
    check(hd_texture_pack_match(pack, &four, &match) == HD_TEXTURE_LOOKUP_NONE,
          "changing the sixteenth 4bpp CLUT entry rejects the palette");
    fixture.vram[inside4] ^= 1u;

    HdTextureDrawQuery eight{};
    eight.page_x = 10;
    eight.page_y = 20;
    eight.depth = HD_TEXTURE_DEPTH_8BPP;
    eight.u_first = 0;
    eight.u_last = 15;
    eight.v_first = 0;
    eight.v_last = 3;
    eight.clut_x = 900;
    eight.clut_y = 8;
    eight.vram = fixture.vram.data();
    eight.vram_word_count = fixture.vram.size();
    check(hd_texture_pack_match(pack, &eight, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "8bpp query matches using the complete 256-entry CLUT");
    const size_t entry200 = 8u * 1024u + ((900u + 200u) & 1023u);
    fixture.vram[entry200] ^= 1u;
    check(hd_texture_pack_match(pack, &eight, &match) == HD_TEXTURE_LOOKUP_NONE,
          "changing CLUT entry 200 rejects an 8bpp palette");
    fixture.vram[entry200] ^= 1u;

    /* Intersect invalidation preserves disjoint surviving fragments. */
    hd_texture_pack_invalidate(pack, 14, 20, 4, 4);
    HdTextureDrawQuery left = query4(fixture.vram, 0, 15);
    check(hd_texture_pack_match(pack, &left, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "non-intersected split of an upload remains matchable");
    check(hd_texture_pack_match(pack, &four, &match) == HD_TEXTURE_LOOKUP_NONE,
          "query crossing an invalidated upload fragment falls back");
    hd_texture_pack_invalidate(pack, 11, 21, 1, 1);
    check(hd_texture_pack_match(pack, &left, &match) == HD_TEXTURE_LOOKUP_NONE,
          "single-word intersect invalidation punches a real residency hole");

    hd_texture_pack_destroy(pack);
}

void test_tracking_savestate_continuity(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    check(hd_texture_pack_track_upload(pack, 10, 20, 8, 4,
                                       fixture.upload_words.data(),
                                       fixture.upload_words.size(), nullptr) == 1,
          "savestate fixture upload tracks");
    hd_texture_pack_invalidate(pack, 14, 20, 4, 4);
    const HdTextureDrawQuery left = query4(fixture.vram, 0, 15);
    const HdTextureDrawQuery full = query4(fixture.vram, 0, 31);
    HdTextureMatch before{};
    check(hd_texture_pack_match(pack, &left, &before) ==
              HD_TEXTURE_LOOKUP_FOUND,
          "savestate fixture retains a split upload fragment");

    uint8_t* state = nullptr;
    size_t state_size = 0;
    check(hd_texture_pack_tracking_state_save(
              pack, &state, &state_size) == 1 && state && state_size > 24,
          "upload residency serializes to a non-empty portable wire");
    check(hd_texture_pack_tracking_state_check(state, state_size) == 1,
          "serialized upload residency passes side-effect-free preflight");

    hd_texture_pack_reset_tracking(pack);
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &left, &match) == HD_TEXTURE_LOOKUP_NONE,
          "full-VRAM restore reset removes host upload identity");
    check(hd_texture_pack_tracking_state_load(pack, state, state_size) == 1,
          "saved upload residency restores after the VRAM reset");
    check(hd_texture_pack_tracking_upload_count(pack) == 1,
          "restore recovers the exact live upload count");
    check(hd_texture_pack_match(pack, &left, &match) ==
              HD_TEXTURE_LOOKUP_FOUND &&
              match.upload_serial == before.upload_serial,
          "restored tracker preserves replacement match and upload identity");
    check(hd_texture_pack_match(pack, &full, &match) == HD_TEXTURE_LOOKUP_NONE,
          "restored tracker preserves the invalidated residency hole");
    std::vector<uint8_t> corrupt(state, state + state_size);
    corrupt[20] = 1; /* reserved header word */
    check(hd_texture_pack_tracking_state_check(corrupt.data(), corrupt.size()) == 0,
          "corrupt tracker header fails closed");
    check(hd_texture_pack_tracking_state_check(state, state_size - 1) == 0,
          "truncated tracker wire fails closed");
    check(hd_texture_pack_tracking_state_load(
              pack, corrupt.data(), corrupt.size()) == 0 &&
              hd_texture_pack_match(pack, &left, &match) ==
                  HD_TEXTURE_LOOKUP_FOUND,
          "failed tracker load leaves the current residency untouched");
    hd_texture_pack_invalidate(pack, 10, 20, 1, 1);
    check(hd_texture_pack_match(pack, &left, &match) == HD_TEXTURE_LOOKUP_NONE,
          "restored tracker rebuilds invalidation bounds before GPU draws");
    std::free(state);
    hd_texture_pack_destroy(pack);

    state = nullptr;
    state_size = 0;
    check(hd_texture_pack_tracking_state_save(
              nullptr, &state, &state_size) == 1 && state_size == 24 &&
              hd_texture_pack_tracking_state_check(state, state_size) == 1 &&
              hd_texture_pack_tracking_state_load(nullptr, state, state_size) == 1,
          "renderer-independent states carry a valid empty HD tracker");
    std::free(state);
}

void test_wrapping(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    uint32_t hash = 0;
    check(hd_texture_pack_track_upload(pack, 1022, 510, 4, 4,
                                       fixture.wrap_words.data(),
                                       fixture.wrap_words.size(), &hash) == 1,
          "upload crossing both VRAM edges is tracked");
    HdTextureDrawQuery query{};
    query.page_x = 1022;
    query.page_y = 510;
    query.depth = HD_TEXTURE_DEPTH_16BPP;
    query.u_first = 0;
    query.u_last = 3;
    query.v_first = 0;
    query.v_last = 3;
    query.vram = fixture.vram.data();
    query.vram_word_count = fixture.vram.size();
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &query, &match) == HD_TEXTURE_LOOKUP_FOUND,
          "wrapped page/UV footprint is covered by one logical upload");
    check(match.entry.texture_hash == fixture.wrap_hash,
          "wrapped match preserves original logical upload hash");
    hd_texture_pack_invalidate(pack, 0, 0, 1, 1);
    check(hd_texture_pack_match(pack, &query, &match) == HD_TEXTURE_LOOKUP_NONE,
          "physical-edge invalidation intersects a wrapped upload");
    hd_texture_pack_destroy(pack);
}

void test_ambiguity_fallback() {
    TempTree temp;
    const fs::path direct = temp.path / "ambiguous-texture-replacements";
    fs::create_directories(direct);
    uint16_t word = 0;
    uint32_t hash = 0;
    for (unsigned value = 0; value <= 0xFFFFu; ++value) {
        word = static_cast<uint16_t>(value);
        hash = hd_texture_crc32_words_le(&word, 1);
        if (hash != 0 && hash < 0x10000000u) break;
    }
    check(hash < 0x10000000u, "found a short numeric CRC for alias test");
    touch(direct / key_name(hash, 0));
    touch(direct / padded_key_name(hash, 0));

    HdTexturePack* pack = open_pack(direct);
    if (!pack) return;
    HdTexturePackEntry entry{};
    check(hd_texture_pack_lookup(pack, hash, 0, &entry) ==
              HD_TEXTURE_LOOKUP_AMBIGUOUS,
          "numeric filename aliases are rejected as ambiguous");

    std::vector<uint16_t> vram(1024u * 512u, 0);
    check(hd_texture_pack_track_upload(pack, 30, 40, 1, 1,
                                       &word, 1, nullptr) == 1,
          "ambiguous fixture upload tracks");
    HdTextureDrawQuery query{};
    query.page_x = 30;
    query.page_y = 40;
    query.depth = HD_TEXTURE_DEPTH_16BPP;
    query.vram = vram.data();
    query.vram_word_count = vram.size();
    HdTextureMatch match{};
    check(hd_texture_pack_match(pack, &query, &match) ==
              HD_TEXTURE_LOOKUP_AMBIGUOUS,
          "draw ambiguity fails closed instead of choosing enumeration order");
    hd_texture_pack_destroy(pack);
}

void test_bounded_async_decode(Fixture& fixture) {
    HdTexturePack* pack = open_pack(fixture.root);
    if (!pack) return;
    check(hd_texture_pack_track_upload(pack, 10, 20, 8, 4,
                                       fixture.upload_words.data(),
                                       fixture.upload_words.size(), nullptr) == 1,
          "decode fixture upload tracks");
    int status = hd_texture_pack_request_decode(
        pack, fixture.upload_hash, fixture.palette4_hash);
    check(status == HD_TEXTURE_LOOKUP_NONE,
          "first decode request queues work instead of decoding synchronously");

    HdTexturePixels pixels{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        status = hd_texture_pack_acquire_decoded(
            pack, fixture.upload_hash, fixture.palette4_hash, &pixels);
        if (status == HD_TEXTURE_LOOKUP_FOUND) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    check(status == HD_TEXTURE_LOOKUP_FOUND,
          "bounded worker publishes a decoded replacement");
    check(pixels.rgba && pixels.width == 64 && pixels.height == 8 &&
              pixels.stride == 64u * 4u,
          "decoded lease exposes expected RGBA dimensions");
    hd_texture_pack_destroy(pack);
    check(pixels.rgba && pixels.rgba[2] == 0xA5u && pixels.rgba[3] == 0x7Fu,
          "decoded lease preserves the pack's 7-bit opaque alpha");
    hd_texture_pixels_release(&pixels);
}

} // namespace

int main() {
    test_crc_exact_little_endian();
    Fixture fixture;
    test_scan_mapping(fixture);
    test_palette_match_and_invalidation(fixture);
    test_tracking_savestate_continuity(fixture);
    test_wrapping(fixture);
    test_ambiguity_fallback();
    test_bounded_async_decode(fixture);

    if (failures) {
        std::fprintf(stderr, "test_hd_texture_pack: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("PASS: external HD texture pack scan/hash/tracker semantics");
    return 0;
}
