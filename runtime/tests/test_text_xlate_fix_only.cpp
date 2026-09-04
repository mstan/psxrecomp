#include "text_xlate.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static std::array<uint8_t, 2u * 1024u * 1024u> g_ram{};
static std::array<uint16_t, 1024u * 512u> g_vram{};

extern "C" {
uint64_t s_frame_count;

uint8_t* memory_get_ram_ptr(void) { return g_ram.data(); }
uint16_t gr_vram_read(int x, int y) {
    return g_vram[static_cast<size_t>(y) * 1024u + static_cast<size_t>(x)];
}
void gr_vram_write(int x, int y, uint16_t pixel) {
    g_vram[static_cast<size_t>(y) * 1024u + static_cast<size_t>(x)] = pixel;
}
void dirty_ram_text_bless(uint32_t, const uint8_t*, uint32_t) {}
}

static bool set_test_language(void) {
#ifdef _WIN32
    return _putenv_s("PSX_LANG", "en") == 0;
#else
    return setenv("PSX_LANG", "en", 1) == 0;
#endif
}

int main(void) {
    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("psxrecomp-text-xlate-fix-only-" + std::to_string(nonce));
    const fs::path translations = root / "translations";
    const fs::path table = translations / "pixels.toml";
    std::error_code ec;

    if (!set_test_language() || !fs::create_directories(translations, ec)) {
        std::cerr << "failed to prepare test directory\n";
        return 1;
    }

    {
        std::ofstream out(table);
        out << "[[vram_patch]]\n"
               "x = 4\n"
               "y = 5\n"
               "w = 1\n"
               "h = 1\n"
               "src_hex = \"3412\"\n"
               "en_hex = \"7856\"\n";
        if (!out) {
            std::cerr << "failed to write test table\n";
            fs::remove_all(root, ec);
            return 1;
        }
    }

    g_vram[5u * 1024u + 4u] = 0x1234u;
    text_xlate_init(root.string().c_str(), "en");
    text_xlate_vram_upload(0, 0, 16, 16);

    const bool passed = g_vram[5u * 1024u + 4u] == 0x5678u;
    fs::remove_all(root, ec);
    if (!passed) {
        std::cerr << "fix-only VRAM patch table was ignored\n";
        return 1;
    }
    std::cout << "PASS: fix-only translation table loaded\n";
    return 0;
}
