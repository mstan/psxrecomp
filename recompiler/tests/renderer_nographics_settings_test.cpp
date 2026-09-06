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

static void test_renderer_parser_contract(void) {
    int out = -1;
    check(PSXRecompV4::video_renderer_parse("software", &out) &&
              out == PSXRecompV4::VIDEO_RENDERER_SOFTWARE,
          "software parses to ID 0");
    check(PSXRecompV4::video_renderer_parse("opengl", &out) &&
              out == PSXRecompV4::VIDEO_RENDERER_OPENGL,
          "opengl parses to ID 1");
    check(PSXRecompV4::video_renderer_parse("vulkan", &out) &&
              out == PSXRecompV4::VIDEO_RENDERER_VULKAN,
          "legacy vulkan parses to native ID 2");
    check(PSXRecompV4::video_renderer_parse("vulkan_nographics", &out) &&
              out == PSXRecompV4::VIDEO_RENDERER_NOGRAPHICS,
          "vulkan_nographics parses to ID 3");
    out = 123;
    check(!PSXRecompV4::video_renderer_parse("nogfx", &out) && out == 123,
          "invalid renderer string is rejected without changing output");
    check(std::string(PSXRecompV4::video_renderer_name(
              PSXRecompV4::VIDEO_RENDERER_VULKAN)) == "vulkan",
          "native Vulkan keeps serialized name vulkan");
    check(std::string(PSXRecompV4::video_renderer_name(
              PSXRecompV4::VIDEO_RENDERER_NOGRAPHICS)) ==
              "vulkan_nographics",
          "NoGraphics Vulkan serializes separately");
    check(std::string(PSXRecompV4::video_renderer_name(99)) == "opengl",
          "unknown renderer serializes to default OpenGL");
}

static void test_user_settings_renderer_roundtrip(void) {
    fs::path legacy = write_temp("psxrecomp_renderer_legacy.toml",
        "[video]\nrenderer = \"vulkan\"\n");
    auto old = PSXRecompV4::load_user_settings(legacy);
    check(!old.parse_error, "legacy settings.toml parses");
    check(old.has_renderer && old.renderer == PSXRecompV4::VIDEO_RENDERER_VULKAN,
          "settings renderer=vulkan remains native ID 2");
    fs::remove(legacy);

    fs::path ng = write_temp("psxrecomp_renderer_ng.toml",
        "[video]\nrenderer = \"vulkan_nographics\"\n");
    auto parsed = PSXRecompV4::load_user_settings(ng);
    check(!parsed.parse_error, "NoGraphics settings.toml parses");
    check(parsed.has_renderer &&
              parsed.renderer == PSXRecompV4::VIDEO_RENDERER_NOGRAPHICS,
          "settings renderer=vulkan_nographics reads ID 3");
    fs::remove(ng);

    fs::path bad = write_temp("psxrecomp_renderer_bad.toml",
        "[video]\nrenderer = \"nogfx\"\nsupersampling = 2\n");
    auto invalid = PSXRecompV4::load_user_settings(bad);
    check(!invalid.parse_error, "invalid renderer does not poison whole settings file");
    check(!invalid.has_renderer, "invalid renderer remains absent");
    check(invalid.has_supersampling && invalid.supersampling == 2,
          "other settings still load after invalid renderer");
    fs::remove(bad);

    PSXRecompV4::UserSettings out;
    out.renderer = PSXRecompV4::VIDEO_RENDERER_NOGRAPHICS;
    out.has_renderer = true;
    fs::path roundtrip = fs::temp_directory_path() /
                         "psxrecomp_renderer_roundtrip.toml";
    check(PSXRecompV4::save_user_settings(roundtrip, out),
          "save_user_settings writes NoGraphics renderer");
    std::string body;
    {
        std::ifstream f(roundtrip, std::ios::binary);
        body.assign(std::istreambuf_iterator<char>(f),
                    std::istreambuf_iterator<char>());
    }
    check(body.find("renderer          = \"vulkan_nographics\"") !=
              std::string::npos,
          "settings save writes vulkan_nographics token");
    auto back = PSXRecompV4::load_user_settings(roundtrip);
    check(!back.parse_error && back.has_renderer &&
              back.renderer == PSXRecompV4::VIDEO_RENDERER_NOGRAPHICS,
          "NoGraphics renderer survives save/load round trip");
    fs::remove(roundtrip);
}

static void test_game_config_offer_gate(void) {
    fs::path inherited = write_game_toml("psxrecomp_renderer_offer_default.toml",
        "[video]\noffer_vulkan = true\n");
    auto gi = PSXRecompV4::load_game_config(inherited);
    check(gi.vulkan_offered, "game config offer_vulkan parses true");
    check(gi.vulkan_nographics_offered,
          "NoGraphics offer defaults true beside offered native Vulkan");
    fs::remove(inherited);

    fs::path optout = write_game_toml("psxrecomp_renderer_offer_optout.toml",
        "[video]\noffer_vulkan = true\noffer_vulkan_nographics = false\n");
    auto go = PSXRecompV4::load_game_config(optout);
    check(go.vulkan_offered, "native Vulkan offer remains true with NoGraphics opt-out");
    check(!go.vulkan_nographics_offered,
          "offer_vulkan_nographics=false hides experimental backend");
    fs::remove(optout);
}

static void test_runtime_renderer_parse(void) {
    fs::path p = write_game_toml("psxrecomp_runtime_renderer_ng.toml",
        "[video]\nrenderer = \"vulkan_nographics\"\n");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_renderer == PSXRecompV4::VIDEO_RENDERER_NOGRAPHICS,
          "runtime [video] renderer parses vulkan_nographics");
    fs::remove(p);

    fs::path bad = write_game_toml("psxrecomp_runtime_renderer_bad.toml",
        "[video]\nrenderer = \"nogfx\"\n");
    bool rejected = false;
    try {
        (void)PSXRecompV4::load_game_config(bad);
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "runtime [video] renderer rejects invalid strings");
    fs::remove(bad);
}

int main(void) {
    test_renderer_parser_contract();
    test_user_settings_renderer_roundtrip();
    test_game_config_offer_gate();
    test_runtime_renderer_parse();

    if (failures) {
        std::fprintf(stderr,
                     "renderer_nographics_settings_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("PASS: NoGraphics renderer settings compatibility\n");
    return 0;
}
