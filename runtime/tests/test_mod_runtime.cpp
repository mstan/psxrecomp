#include "mod_runtime.h"
#include "mod_packages.h"
#include "psx_sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::array<uint8_t, 2 * 1024 * 1024> ram;
static int failures;
static int activation_calls;
static int plugin_calls;

extern "C" uint8_t psx_read_byte(uint32_t address) {
    return ram[address & 0x1fffffu];
}

extern "C" void psx_write_byte(uint32_t address, uint8_t value) {
    ram[address & 0x1fffffu] = value;
}

extern "C" uint16_t psx_read_half(uint32_t address) {
    const uint32_t offset = address & 0x1fffffu;
    return (uint16_t)(ram[offset] | ((uint16_t)ram[offset + 1] << 8));
}

extern "C" void psx_write_half(uint32_t address, uint16_t value) {
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
}

extern "C" uint32_t psx_read_word(uint32_t address) {
    const uint32_t offset = address & 0x1fffffu;
    return (uint32_t)ram[offset] |
           ((uint32_t)ram[offset + 1] << 8) |
           ((uint32_t)ram[offset + 2] << 16) |
           ((uint32_t)ram[offset + 3] << 24);
}

extern "C" void psx_write_word(uint32_t address, uint32_t value) {
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
    ram[offset + 2] = (uint8_t)(value >> 16);
    ram[offset + 3] = (uint8_t)(value >> 24);
}

extern "C" uint32_t psx_mod_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t, uint32_t) {
    return 0;
}
extern "C" int psx_ws_x_margin(void) { return 0; }

extern "C" void dirty_ram_mark_executable_range(uint32_t, uint32_t) {}
extern "C" int fntrace_is_game_started(void) { return 1; }

static void test_vblank_plugin(void) {
    plugin_calls++;
}

static void test_activation_plugin(void) {
    activation_calls++;
}

static void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

static void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

static std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-runtime-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const std::vector<uint8_t> stock(8 * 2352, 0);
    std::vector<uint8_t> overlay(3000);
    for (size_t i = 0; i < overlay.size(); ++i)
        overlay[i] = (uint8_t)(i * 17u + 3u);
    const fs::path stock_path = root / "stock.bin";
    write_bytes(stock_path, stock);
    write_bytes(root / "audio.bin", std::vector<uint8_t>(2 * 2352, 0x5a));
    const fs::path cue_path = root / "stock.cue";
    write_text(cue_path,
        "FILE \"stock.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"audio.bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:00:00\n");
    write_bytes(root / "packages/runtime.test/1.0.0/assets/overlay.bin",
                overlay);
    write_text(root / "packages/runtime.test/1.0.0/manifest.toml",
        "format_version = 5\n"
        "id = \"runtime.test\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Runtime Test\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-RUNTIME\"\n"
        "disc_sha256 = \"" + sha256_hex(stock) + "\"\n"
        "[[feature]]\n"
        "id = \"main-code\"\n"
        "name = \"Main Code\"\n"
        "[[feature]]\n"
        "id = \"disc-byte\"\n"
        "name = \"Disc Byte\"\n"
        "[[feature]]\n"
        "id = \"asset-overlay\"\n"
        "name = \"Asset Overlay\"\n"
        "[[feature]]\n"
        "id = \"user-byte\"\n"
        "name = \"User Byte\"\n"
        "[[feature]]\n"
        "id = \"dynamic-main\"\n"
        "name = \"Dynamic Main\"\n"
        "[[feature]]\n"
        "id = \"sparse-main\"\n"
        "name = \"Sparse Main\"\n"
        "[[feature]]\n"
        "id = \"sparse-flag\"\n"
        "name = \"Sparse Flag\"\n"
        "[[feature]]\n"
        "id = \"sparse-disc\"\n"
        "name = \"Sparse Disc\"\n"
        "[[feature]]\n"
        "id = \"vblank-plugin\"\n"
        "name = \"VBlank Plugin\"\n"
        "[[option]]\n"
        "feature = \"dynamic-main\"\n"
        "id = \"count\"\n"
        "label = \"Count\"\n"
        "type = \"integer\"\n"
        "min = 0\n"
        "max = 254\n"
        "default = 42\n"
        "[[option]]\n"
        "feature = \"sparse-main\"\n"
        "id = \"frames\"\n"
        "label = \"Frames\"\n"
        "type = \"integer\"\n"
        "min = 0\n"
        "max = 99\n"
        "default = 2\n"
        "[[patch]]\n"
        "feature = \"main-code\"\n"
        "target = \"main_exe\"\n"
        "address = 2147487744\n"
        "expected = \"01020304\"\n"
        "replace = \"a1a2a3a4\"\n"
        "[[patch]]\n"
        "feature = \"disc-byte\"\n"
        "target = \"disc_raw\"\n"
        "offset = 4714\n"
        "expected = \"aa\"\n"
        "replace = \"bb\"\n"
        "[[patch]]\n"
        "feature = \"user-byte\"\n"
        "target = \"disc_user\"\n"
        "offset = 6154\n"
        "expected = \"cc\"\n"
        "replace = \"dd\"\n"
        "[[patch]]\n"
        "feature = \"dynamic-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488000\n"
        "expected = \"0000\"\n"
        "replace_from = { option = \"count\", encoding = \"u16le\" }\n"
        "[[patch]]\n"
        "feature = \"dynamic-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488002\n"
        "expected = \"0100\"\n"
        "replace_from = { option = \"count\", encoding = \"u16le\", addend = 1 }\n"
        "[[patch]]\n"
        "feature = \"sparse-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 0, option = \"frames\", encoding = \"u8\" }]\n"
        "when_integer = { option = \"frames\", op = \"gt\", value = 0 }\n"
        "[[patch]]\n"
        "feature = \"sparse-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 0, replace = \"01\" }, "
        "{ offset = 2, replace = \"00\" }]\n"
        "when_integer = { option = \"frames\", op = \"eq\", value = 0 }\n"
        "[[patch]]\n"
        "feature = \"sparse-flag\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 1, replace = \"42\" }]\n"
        "[[patch]]\n"
        "feature = \"sparse-disc\"\n"
        "target = \"disc_user\"\n"
        "offset = 6164\n"
        "expected = \"11223344\"\n"
        "fields = [{ offset = 0, replace = \"aa\" }, "
        "{ offset = 2, replace = \"bb\" }]\n"
        "[[overlay]]\n"
        "feature = \"asset-overlay\"\n"
        "target = \"disc_raw\"\n"
        "offset = 11408\n"
        "file = \"assets/overlay.bin\"\n"
        "sha256 = \"" + sha256_hex(overlay) + "\"\n"
        "expected_sha256 = \"" +
            sha256_hex(std::vector<uint8_t>(overlay.size(), 0)) + "\"\n"
        "[[plugin]]\n"
        "feature = \"vblank-plugin\"\n"
        "id = \"runtime.test-vblank\"\n");
    write_text(root / "state.toml",
        "format_version = 2\n"
        "[[package]]\n"
        "id = \"runtime.test\"\n"
        "version = \"1.0.0\"\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"main-code\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"disc-byte\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"asset-overlay\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"user-byte\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"dynamic-main\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "count = 42\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-main\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "frames = 0\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-flag\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-disc\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"vblank-plugin\"\n"
        "enabled = true\n");

    std::string error;
    PSXRecompV4::mod_clear_plugins_for_tests();
    check(PSXRecompV4::mod_register_activation_plugin(
              "runtime.test-vblank", test_activation_plugin),
          "runtime test activation hook must register");
    check(PSXRecompV4::mod_register_vblank_plugin(
              "runtime.test-vblank", test_vblank_plugin),
          "runtime test plugin must register");
    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error),
          "CUE and its data-track BIN must have the same mod target identity");
    mod_runtime_activate_plugins();
    check(activation_calls == 1,
          "resolved trusted plugin must activate before runtime startup");
    mod_runtime_on_vblank();
    check(plugin_calls == 1,
          "resolved trusted plugin must run on guest VBlank");

    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_dispatch(0x80001000);
    check(ram[0x1000] == 1, "patch must wait for the configured entry point");
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4,
          "main-EXE patch must apply before entry execution");
    check(ram[0x1100] == 42 && ram[0x1101] == 0 &&
              ram[0x1102] == 43 && ram[0x1103] == 0,
          "dynamic main-EXE patches must encode all sites before entry");
    check(ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "adjacent sparse fields must compose while preserving guard-only "
          "bytes");

    /* A full-machine savestate replaces main RAM after the entry-point plan
     * has already applied. Loading a stock checkpoint must not silently turn
     * the currently enabled main-EXE features back off. */
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_savestate_loaded();
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "savestate restore must reapply the complete enabled main plan");

    /* Loading a checkpoint that already carries plan replacements must not
     * re-store / re-dirty those pages (enhanced 8 MB softlock class). */
    ram[0x1000] = 0xa1; ram[0x1001] = 0xa2; ram[0x1002] = 0xa3; ram[0x1003] = 0xa4;
    ram[0x1100] = 42; ram[0x1101] = 0;
    ram[0x1102] = 43; ram[0x1103] = 0;
    ram[0x1200] = 1; ram[0x1201] = 0x42;
    ram[0x1202] = 0; ram[0x1203] = 0x32;
    mod_runtime_on_savestate_loaded();
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "already-patched savestate must keep plan bytes without reapply");

    /* Disc overlays can LoadExe the boot EXE already rewritten. Entry must
     * accept planned replacements in RAM (not only stock expected). */
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error), error.c_str());
    ram[0x1000] = 0xa1; ram[0x1001] = 0xa2; ram[0x1002] = 0xa3; ram[0x1003] = 0xa4;
    ram[0x1100] = 42; ram[0x1101] = 0;
    ram[0x1102] = 43; ram[0x1103] = 0;
    ram[0x1200] = 1; ram[0x1201] = 0x42;
    ram[0x1202] = 0; ram[0x1203] = 0x32;
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "entry apply must accept disc-prepatched main EXE replacements");

    std::array<uint8_t, 2352> sector{};
    sector[10] = 0xaa;
    mod_runtime_patch_disc_sector(2, 1, sector.data(), (uint32_t)sector.size());
    check(sector[10] == 0xaa, "disc overlay must stay off during reference reads");
    mod_runtime_enable_disc_patches();
    mod_runtime_patch_disc_sector(2, 1, sector.data(), (uint32_t)sector.size());
    check(sector[10] == 0xbb, "raw disc overlay must patch matching sectors");

    std::array<uint8_t, 2352> overlay_sector{};
    mod_runtime_patch_disc_sector(
        4, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector[1999] == 0 &&
              overlay_sector[2000] == overlay[0] &&
              overlay_sector[2351] == overlay[351],
          "file overlay must patch the tail of its first sector");
    overlay_sector.fill(0);
    mod_runtime_patch_disc_sector(
        5, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector.front() == overlay[352] &&
              overlay_sector.back() == overlay[2703],
          "file overlay must patch complete middle sectors");
    overlay_sector.fill(0);
    mod_runtime_patch_disc_sector(
        6, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector[0] == overlay[2704] &&
              overlay_sector[295] == overlay[2999] &&
              overlay_sector[296] == 0,
          "file overlay must patch the head of its final sector");

    std::array<uint8_t, 2352> mode2_sector{};
    mode2_sector[15] = 2;
    mode2_sector[18] = 0;
    mode2_sector[24 + 10] = 0xcc;
    mode2_sector[24 + 20] = 0x11;
    mode2_sector[24 + 21] = 0x22;
    mode2_sector[24 + 22] = 0x33;
    mode2_sector[24 + 23] = 0x44;
    mod_runtime_patch_disc_sector(
        3, 1, mode2_sector.data(), (uint32_t)mode2_sector.size());
    check(mode2_sector[24 + 10] == 0xdd,
          "disc_user operations must apply to raw Mode2 Form1 user data");
    check(mode2_sector[24 + 20] == 0xaa &&
              mode2_sector[24 + 21] == 0x22 &&
              mode2_sector[24 + 22] == 0xbb &&
              mode2_sector[24 + 23] == 0x44,
          "sparse disc writes must validate a complete guard and modify only "
          "owned fields");
    std::array<uint8_t, 2352> audio_sector{};
    audio_sector[24 + 10] = 0xcc;
    mod_runtime_patch_disc_sector(
        3, 1, audio_sector.data(), (uint32_t)audio_sector.size());
    check(audio_sector[24 + 10] == 0xcc,
          "disc_user operations must not modify CDDA/non-data sectors");

    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 2; ram[0x1103] = 0; /* second dynamic guard is wrong */
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 1 && ram[0x1003] == 4 &&
              ram[0x1100] == 0 && ram[0x1101] == 0,
          "one failed generated guard must leave the complete main plan untouched");

    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x33; /* guard-only byte is wrong */
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 1 && ram[0x1003] == 4 &&
              ram[0x1200] == 2 && ram[0x1201] == 0 &&
              ram[0x1202] == 1 && ram[0x1203] == 0x33,
          "a failed sparse guard-only byte must leave the complete main plan "
          "untouched");

    /* Loading can be requested while the boot executable is still running,
     * before the configured game entry point has dispatched. The restored
     * checkpoint itself supplies the bytes used to validate and apply the
     * plan in that case. */
    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_savestate_loaded();
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "pre-entry savestate restore must validate and apply the main plan");

    mod_runtime_enable_disc_patches();
    std::array<uint8_t, 2352> bad_sparse_disc{};
    bad_sparse_disc[15] = 2;
    bad_sparse_disc[18] = 0;
    bad_sparse_disc[24 + 10] = 0xcc;
    bad_sparse_disc[24 + 20] = 0x11;
    bad_sparse_disc[24 + 21] = 0x99; /* guard-only byte is wrong */
    bad_sparse_disc[24 + 22] = 0x33;
    bad_sparse_disc[24 + 23] = 0x44;
    mod_runtime_patch_disc_sector(
        3, 1, bad_sparse_disc.data(),
        (uint32_t)bad_sparse_disc.size());
    check(bad_sparse_disc[24 + 10] == 0xcc &&
              bad_sparse_disc[24 + 20] == 0x11 &&
              bad_sparse_disc[24 + 22] == 0x33,
          "a failed sparse disc guard must leave every write in the sector "
          "untouched");

    fs::remove_all(root, ec);
    if (failures) return 1;
    std::cout << "mod runtime tests passed\n";
    return 0;
}
