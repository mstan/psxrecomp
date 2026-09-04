/* rewind_settings_test — [video] rewind opt-in plumbing.
 *
 * Local rewind keeps whole-machine snapshots (2 MB main RAM + 1 MB VRAM +
 * 512 KB SPU RAM, uncompressed) in a ring and captures one every
 * rewind_interval frames. At the default depth that is a few hundred MB
 * resident plus a periodic multi-megabyte copy, so the feature is opt-in and
 * nothing is allocated until a player asks for it.
 *
 * What this pins is the decision layer, not the ring itself:
 *
 *   1. `rewind` defaults OFF, and stays off when settings.toml says nothing;
 *   2. settings.toml turns it on, and can spell an explicit off;
 *   3. an absent key stays absent, so layering cannot force a value;
 *   4. save_user_settings round-trips it — a launcher save that dropped the key
 *      would read as "the toggle does nothing";
 *   5. the tuning keys stay independent of the enable.
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

/* The default is the whole point of the change: a host that has never opened
 * the settings window must not be paying for the ring. */
static void test_default_off() {
    PSXRecompV4::UserSettings fresh;
    check(!fresh.rewind, "rewind defaults OFF in UserSettings");
    check(!fresh.has_rewind, "a fresh UserSettings has no opinion on rewind");

    fs::path p = write_temp("psxrecomp_rewind_absent.toml",
        "[video]\n"
        "supersampling = 2\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "settings.toml parses");
    check(!us.has_rewind, "absent rewind leaves has_rewind false");
    check(!us.rewind, "absent rewind leaves the value off");
    fs::remove(p);
}

static void test_opt_in_and_explicit_off() {
    fs::path on = write_temp("psxrecomp_rewind_on.toml",
        "[video]\n"
        "rewind = true\n");
    auto us_on = PSXRecompV4::load_user_settings(on);
    check(us_on.has_rewind && us_on.rewind,
          "settings.toml rewind = true is read");
    fs::remove(on);

    /* Explicit false must be distinguishable from absent: one is a player who
     * turned it off, the other is a file that never mentioned it. */
    fs::path off = write_temp("psxrecomp_rewind_off.toml",
        "[video]\n"
        "rewind = false\n");
    auto us_off = PSXRecompV4::load_user_settings(off);
    check(us_off.has_rewind, "explicit rewind = false still sets has_rewind");
    check(!us_off.rewind, "explicit rewind = false reads as off");
    fs::remove(off);
}

static void test_round_trip() {
    PSXRecompV4::UserSettings out;
    out.rewind = true;            out.has_rewind = true;
    out.rewind_depth = 100;       out.has_rewind_depth = true;
    out.rewind_interval = 4;      out.has_rewind_interval = true;

    fs::path p = fs::temp_directory_path() / "psxrecomp_rewind_roundtrip.toml";
    check(PSXRecompV4::save_user_settings(p, out), "save_user_settings writes");

    auto back = PSXRecompV4::load_user_settings(p);
    check(!back.parse_error, "written settings.toml re-parses");
    check(back.has_rewind && back.rewind,
          "rewind survives a save/load round trip");
    check(back.has_rewind_depth && back.rewind_depth == 100,
          "rewind_depth survives a save/load round trip");
    check(back.has_rewind_interval && back.rewind_interval == 4,
          "rewind_interval survives a save/load round trip");
    fs::remove(p);
}

/* Depth/interval describe a ring that only exists once rewind is on, but they
 * are still their own keys: a player may tune them while it is off and expect
 * the values to be there when they turn it on. */
static void test_tuning_independent_of_enable() {
    fs::path p = write_temp("psxrecomp_rewind_tuning_only.toml",
        "[video]\n"
        "rewind_depth = 150\n"
        "rewind_interval = 8\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.has_rewind, "tuning keys alone do not imply an enable");
    check(us.has_rewind_depth && us.rewind_depth == 150,
          "rewind_depth is read without rewind being set");
    check(us.has_rewind_interval && us.rewind_interval == 8,
          "rewind_interval is read without rewind being set");
    fs::remove(p);
}

int main() {
    test_default_off();
    test_opt_in_and_explicit_off();
    test_round_trip();
    test_tuning_independent_of_enable();

    if (failures) {
        std::fprintf(stderr, "rewind_settings_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: [video] rewind opt-in plumbing\n");
    return 0;
}
