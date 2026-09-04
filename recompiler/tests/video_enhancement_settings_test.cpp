/* video_enhancement_settings_test — [video] geometry_correction /
 * perspective_texturing plumbing.
 *
 * These two knobs are the opt-in for the sub-pixel vertex precision and
 * perspective-correct UV enhancements (psxrecomp issue #92). The underlying
 * GTE/GPU machinery has its own unit coverage in the runtime suite; what this
 * test pins is the part that decides whether it is ever switched on:
 *
 *   1. both default OFF (the faithful floor) when game.toml says nothing;
 *   2. game.toml [video] turns them on;
 *   3. settings.toml (the player's file) parses them;
 *   4. save_user_settings round-trips them — a launcher save must not silently
 *      drop a hand-edited key, which would look like "the setting does nothing".
 */
#include "config_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static fs::path write_temp(const std::string& name, const std::string& body) {
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
    return p;
}

/* game.toml needs the fields load_game_config() requires; keep this to the
 * documented minimum plus whichever [video] body the case under test needs.
 *
 * [runtime] is required even when empty: parse_runtime_block() early-returns
 * on a config with no [runtime] table, so a game.toml carrying [video] alone
 * would silently ignore every video key. Every shipped game.toml has both. */
static fs::path write_game_toml(const std::string& name,
                                const std::string& video_block) {
    return write_temp(name,
        "[game]\n"
        "name = \"probe\"\n"
        "exe = \"probe.exe\"\n"
        "load_address = \"0x80010000\"\n"
        "entry_pc = \"0x80010000\"\n"
        "text_size = \"0x1000\"\n"
        "[recompiler]\n"
        "seeds = \"seeds.json\"\n"
        "[runtime]\n"
        + video_block);
}

/* BOTH default off — the faithful floor — but for different reasons, and the
 * distinction matters if anyone reconsiders these later. geometry_correction is
 * off because it is BROKEN at the coverage the runtime can reach (it moves
 * vertices and splits shared edges) and has no launcher control at all.
 * perspective_texturing is off because it is a deliberate departure from hardware
 * output validated on only one title and renderer; it is structurally safe and
 * players opt in from the launcher. See docs/ENHANCEMENTS.md G1.8/G1.9. */
static void test_defaults_off() {
    fs::path p = write_game_toml("psxrecomp_pgxp_default.toml", "");
    auto gc = PSXRecompV4::load_game_config(p);
    check(!gc.runtime.video_geometry_correction,
          "geometry_correction defaults OFF (known to crack meshes)");
    check(!gc.runtime.video_perspective_texturing,
          "perspective_texturing defaults OFF (faithful floor; opt-in)");
    fs::remove(p);
}

static void test_game_toml_opt_in() {
    fs::path p = write_game_toml("psxrecomp_pgxp_on.toml",
        "[video]\n"
        "window_width = 1920\n"
        "geometry_correction = true\n"
        "perspective_texturing = true\n");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_window_width == 1920,
          "[video] window_width is honoured");
    check(gc.runtime.video_geometry_correction,
          "[video] geometry_correction = true is honoured");
    check(gc.runtime.video_perspective_texturing,
          "[video] perspective_texturing = true is honoured");
    fs::remove(p);
}

static void test_game_window_width_validation() {
    fs::path p = write_game_toml("psxrecomp_window_too_small.toml",
        "[video]\n"
        "window_width = 639\n");
    bool rejected = false;
    try {
        (void)PSXRecompV4::load_game_config(p);
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "[video] window_width rejects values below 640");
    fs::remove(p);
}

/* The two knobs are independent: a title may want stable geometry without
 * changing texture mapping (or the reverse) — so a single flag would be wrong. */
static void test_knobs_independent() {
    /* Each knob must be settable against the other's default: geometry ON while
     * perspective is explicitly opted OUT proves neither key implies the other. */
    fs::path p = write_game_toml("psxrecomp_pgxp_geom_only.toml",
        "[video]\n"
        "geometry_correction = true\n"
        "perspective_texturing = false\n");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_geometry_correction,
          "geometry_correction turns on independently");
    check(!gc.runtime.video_perspective_texturing,
          "perspective_texturing can be opted OUT while geometry is on");
    fs::remove(p);

    /* And the converse direction: perspective_texturing alone must turn on
     * WITHOUT dragging the broken geometry knob on with it. This is the case that
     * matters most in practice — it is what the launcher checkbox does. */
    fs::path q = write_game_toml("psxrecomp_pgxp_persp_only.toml",
        "[video]\n"
        "perspective_texturing = true\n");
    auto gq = PSXRecompV4::load_game_config(q);
    check(gq.runtime.video_perspective_texturing,
          "perspective_texturing alone turns on");
    check(!gq.runtime.video_geometry_correction,
          "perspective_texturing does NOT drag geometry_correction on with it");
    fs::remove(q);
}

static void test_user_settings_read() {
    fs::path p = write_temp("psxrecomp_pgxp_settings.toml",
        "[video]\n"
        "geometry_correction = true\n"
        "perspective_texturing = false\n"
        "[audio]\n"
        "frequency = 48000\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "settings.toml parses");
    check(us.has_geometry_correction && us.geometry_correction,
          "settings.toml geometry_correction = true read");
    check(us.has_perspective_texturing && !us.perspective_texturing,
          "settings.toml perspective_texturing = false read (explicit off)");
    check(us.has_audio_freq && us.audio_freq == 48000,
          "settings.toml audio frequency read");
    fs::remove(p);
}

/* An absent key must stay absent, so layering leaves the game.toml value alone
 * instead of forcing it off. */
static void test_user_settings_absent_key() {
    fs::path p = write_temp("psxrecomp_pgxp_settings_empty.toml",
        "[video]\n"
        "supersampling = 2\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.has_geometry_correction,
          "absent geometry_correction leaves has_* false");
    check(!us.has_perspective_texturing,
          "absent perspective_texturing leaves has_* false");
    fs::remove(p);
}

static void test_user_settings_round_trip() {
    PSXRecompV4::UserSettings out;
    out.geometry_correction = true;   out.has_geometry_correction = true;
    out.perspective_texturing = true; out.has_perspective_texturing = true;
    out.audio_freq = 48000;           out.has_audio_freq = true;

    fs::path p = fs::temp_directory_path() / "psxrecomp_pgxp_roundtrip.toml";
    check(PSXRecompV4::save_user_settings(p, out), "save_user_settings writes");

    auto back = PSXRecompV4::load_user_settings(p);
    check(!back.parse_error, "written settings.toml re-parses");
    check(back.has_geometry_correction && back.geometry_correction,
          "geometry_correction survives a save/load round trip");
    check(back.has_perspective_texturing && back.perspective_texturing,
          "perspective_texturing survives a save/load round trip");
    check(back.has_audio_freq && back.audio_freq == 48000,
          "audio frequency survives a save/load round trip");
    fs::remove(p);
}

int main() {
    test_defaults_off();
    test_game_toml_opt_in();
    test_game_window_width_validation();
    test_knobs_independent();
    test_user_settings_read();
    test_user_settings_absent_key();
    test_user_settings_round_trip();

    if (failures) {
        std::fprintf(stderr, "video_enhancement_settings_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("PASS: [video] geometry_correction / perspective_texturing "
                "plumbing\n");
    return 0;
}
