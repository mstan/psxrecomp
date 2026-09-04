// test_disc_path_resolve.cpp — either member of a disc dump is a valid pick.
//
// The contract under test (runtime/include/disc_path.h): whichever file the
// player hands us — the .cue, a conventional raw image, or Steam's .car raw
// image — we mount the same disc and read
// identity/CRC from the same data track. Regression anchor for the old
// behavior, which swapped a .cue for its same-named .bin unconditionally and
// silently dropped the cue's CD-DA tracks.

#include "cue_sheet.h"
#include "disc_path.h"
#include "iso_reader.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using PSXRecompV4::resolve_disc_path;

namespace {

int g_checks = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::abort();
    }
}

void write_sectors(const fs::path& path, int count, uint8_t marker) {
    std::ofstream out(path, std::ios::binary);
    std::vector<uint8_t> sector(2352, marker);
    for (int i = 0; i < count; ++i)
        out.write(reinterpret_cast<const char*>(sector.data()), (std::streamsize)sector.size());
}

void write_text(const fs::path& path, const std::string& body) {
    std::ofstream out(path);
    out << body;
}

bool same(const fs::path& a, const fs::path& b) {
    return a.lexically_normal() == b.lexically_normal();
}

// ---- 1. single-file dump: cue and bin are interchangeable -------------------
void test_single_file_dump(const fs::path& root) {
    const fs::path dir = root / "single";
    fs::create_directories(dir);
    write_sectors(dir / "game.bin", 4, 0x11);
    write_text(dir / "game.cue",
               "FILE \"game.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n");

    const auto from_cue = resolve_disc_path(dir / "game.cue");
    check(from_cue.from_cue, "single: picking the cue mounts the cue");
    check(same(from_cue.mount, dir / "game.cue"), "single: cue mount path");
    check(same(from_cue.data, dir / "game.bin"), "single: cue -> bin data track");

    const auto from_bin = resolve_disc_path(dir / "game.bin");
    check(from_bin.upgraded_to_cue, "single: picking the bin finds the owning cue");
    check(same(from_bin.mount, dir / "game.cue"), "single: bin upgrades to the cue");

    // The whole point: both picks agree on what to read identity/CRC from.
    check(same(from_cue.data, from_bin.data), "single: both picks share a data track");

    write_sectors(dir / "Tomba! (USA).bin", 4, 0x12);
    write_text(dir / "Tomba! (USA).cue",
               "FILE \"Tomba! (USA).bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n");

    const auto bang_from_cue = resolve_disc_path(dir / "Tomba! (USA).cue");
    check(bang_from_cue.from_cue, "single: cue path with ! mounts the cue");
    check(same(bang_from_cue.data, dir / "Tomba! (USA).bin"),
          "single: cue path with ! resolves the data track");

    const auto bang_from_bin = resolve_disc_path(dir / "Tomba! (USA).bin");
    check(bang_from_bin.upgraded_to_cue,
          "single: bin path with ! upgrades to owning cue");
    check(same(bang_from_cue.data, bang_from_bin.data),
          "single: ! cue/bin picks share a data track");
}

// ---- 2. redump multi-track: picking any track file finds the cue ------------
void test_multi_track_dump(const fs::path& root) {
    const fs::path dir = root / "multi";
    fs::create_directories(dir);
    write_sectors(dir / "Game (Track 1).bin", 4, 0x11);
    write_sectors(dir / "Game (Track 2).bin", 3, 0x22);
    write_text(dir / "Game.cue",
               "FILE \"Game (Track 1).bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n"
               "FILE \"Game (Track 2).bin\" BINARY\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 00 00:00:00\n"
               "    INDEX 01 00:00:01\n");

    const auto from_cue = resolve_disc_path(dir / "Game.cue");
    check(from_cue.from_cue, "multi: cue mounts as a cue");
    check(same(from_cue.data, dir / "Game (Track 1).bin"), "multi: data track is track 1");

    // Picking the DATA track's bin: must upgrade to the cue, or the audio track
    // disappears. This is the case the old normalize_disc_path_for_launch got
    // wrong in the other direction.
    const auto from_data_bin = resolve_disc_path(dir / "Game (Track 1).bin");
    check(from_data_bin.upgraded_to_cue, "multi: data bin upgrades to the cue");
    check(same(from_data_bin.mount, dir / "Game.cue"), "multi: data bin -> Game.cue");

    // Picking an AUDIO track's bin still resolves identity to the data track,
    // so the verify verdict does not depend on which file was clicked.
    const auto from_audio_bin = resolve_disc_path(dir / "Game (Track 2).bin");
    check(from_audio_bin.upgraded_to_cue, "multi: audio bin upgrades to the cue");
    check(same(from_audio_bin.data, dir / "Game (Track 1).bin"),
          "multi: audio bin still identifies via the data track");

    // And the mounted TOC really does carry both tracks, for every pick.
    for (const fs::path& pick : {dir / "Game.cue",
                                 dir / "Game (Track 1).bin",
                                 dir / "Game (Track 2).bin"}) {
        PS1::ISOReader reader;
        check(reader.Open(resolve_disc_path(pick).mount.string()), "multi: mount opens");
        check(reader.TrackCount() == 2, "multi: both tracks present regardless of pick");
        check(reader.TrackIsAudio(2), "multi: track 2 is audio regardless of pick");
        reader.Close();
    }
}

// ---- 3. broken cue: fall back to the sibling image instead of failing -------
void test_cue_with_missing_bin(const fs::path& root) {
    const fs::path dir = root / "broken";
    fs::create_directories(dir);
    write_sectors(dir / "game.bin", 4, 0x11);
    // The cue names a payload that is not there (renamed dump, partial copy).
    write_text(dir / "game.cue",
               "FILE \"game (Track 01).bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n");

    const auto r = resolve_disc_path(dir / "game.cue");
    check(r.cue_fallback, "broken cue: reports the fallback");
    check(same(r.mount, dir / "game.bin"), "broken cue: falls back to the sibling bin");
    check(same(r.data, dir / "game.bin"), "broken cue: identity reads the sibling bin");
    check(!r.note.empty(), "broken cue: explains itself");
}

// ---- 4. no cue at all: a bare image mounts as itself ------------------------
void test_bare_image(const fs::path& root) {
    const fs::path dir = root / "bare";
    fs::create_directories(dir);
    write_sectors(dir / "game.bin", 4, 0x11);

    const auto r = resolve_disc_path(dir / "game.bin");
    check(!r.from_cue, "bare: no cue to upgrade to");
    check(same(r.mount, dir / "game.bin"), "bare: mounts itself");
    check(same(r.data, dir / "game.bin"), "bare: identity reads itself");
    check(r.note.empty(), "bare: nothing to report");

    // .iso is equally acceptable.
    write_sectors(dir / "game2.iso", 4, 0x33);
    const auto iso = resolve_disc_path(dir / "game2.iso");
    check(same(iso.mount, dir / "game2.iso"), "bare: iso mounts itself");

    // Steam ships some PlayStation games as extension-renamed raw images. The
    // extension is packaging, not a different on-disc format: accept the file
    // directly without requiring the player to rename t_data_u.car to .bin.
    write_sectors(dir / "t_data_u.car", 4, 0x55);
    const auto car = resolve_disc_path(dir / "t_data_u.car");
    check(same(car.mount, dir / "t_data_u.car"), "bare: Steam car mounts itself");
    check(same(car.data, dir / "t_data_u.car"), "bare: Steam car identifies itself");
    PS1::ISOReader car_reader;
    check(car_reader.Open(car.mount.string()), "bare: Steam car opens as a raw image");
    check(car_reader.TrackCount() == 1, "bare: Steam car exposes one data track");
    car_reader.Close();

    // A CHD embeds its own TOC. Even if a same-stem cue exists, never replace
    // the selected compressed image with that cue or its external payload.
    write_text(dir / "compressed.chd", "not-a-real-chd");
    write_sectors(dir / "compressed.bin", 4, 0x44);
    write_text(dir / "compressed.cue",
               "FILE \"compressed.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n");
    const auto chd = resolve_disc_path(dir / "compressed.chd");
    check(same(chd.mount, dir / "compressed.chd"), "bare: chd mounts itself");
    check(same(chd.data, dir / "compressed.chd"), "bare: chd identifies itself");
    check(!chd.upgraded_to_cue, "bare: chd never upgrades to a cue");
}

// ---- 5. surface-syntax tolerance in the shared cue parser -------------------
void test_cue_syntax_tolerance(const fs::path& root) {
    const fs::path dir = root / "syntax";
    fs::create_directories(dir);
    write_sectors(dir / "game.bin", 4, 0x11);

    // Lowercase keywords, unquoted filename, CRLF line endings.
    write_text(dir / "lower.cue",
               "file game.bin binary\r\n"
               "  track 01 mode2/2352\r\n"
               "    index 01 00:00:00\r\n");
    const auto lower = PSXRecompV4::parse_cue_sheet(dir / "lower.cue");
    check(lower.usable(), "syntax: lowercase + unquoted + CRLF parses");
    check(same(lower.files.front().path, dir / "game.bin"), "syntax: resolves the payload");

    // THE BLACK-SCREEN CASE. The old mount-side parser matched the literal
    // uppercase "BINARY" and required a quoted filename, so a cue like the one
    // above made ISOReader::Open() return false -- while the verify side
    // (identify_disc) uppercased the line, found the payload, and happily
    // reported "Disc verified". The launcher therefore showed a green badge and
    // the runtime then booted with an EMPTY DRIVE: a black screen that went
    // away if you picked the .bin instead. Both sides share one parser now, so
    // pin that they agree: this cue must MOUNT, not just parse.
    {
        PS1::ISOReader r;
        check(r.Open((dir / "lower.cue").string()),
              "syntax: a cue the verify side accepts must also MOUNT (black-screen regression)");
        check(r.TrackCount() >= 1, "syntax: mounted cue exposes its data track");
        r.Close();
    }

    // A WAVE payload has no fixed sector geometry — refuse rather than guess.
    write_text(dir / "wave.cue",
               "FILE \"game.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n"
               "FILE \"track2.wav\" WAVE\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 01 00:00:00\n");
    const auto wave = PSXRecompV4::parse_cue_sheet(dir / "wave.cue");
    check(wave.has_non_binary_file(), "syntax: WAVE payload is flagged");
    check(!wave.usable(), "syntax: WAVE cue is not mountable");

    PS1::ISOReader reader;
    check(!reader.Open((dir / "wave.cue").string()), "syntax: ISOReader refuses a WAVE cue");

    // ...and the resolver recovers to the sibling bin rather than dying.
    const auto r = resolve_disc_path(dir / "wave.cue");
    check(same(r.mount, dir / "game.bin"), "syntax: WAVE cue falls back to the bin");
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "psxrecomp-disc-path-test";
    fs::remove_all(root);
    fs::create_directories(root);

    test_single_file_dump(root);
    test_multi_track_dump(root);
    test_cue_with_missing_bin(root);
    test_bare_image(root);
    test_cue_syntax_tolerance(root);

    fs::remove_all(root);
    std::printf("disc_path_resolve: %d checks passed\n", g_checks);
    return 0;
}
