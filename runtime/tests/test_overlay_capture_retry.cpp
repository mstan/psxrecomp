#include "code_provider.h"
#include "dirty_ram_interp.h"
#include "overlay_capture.h"

#include <SDL.h>

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern "C" int overlay_capture_test_write_state(void);
extern "C" unsigned overlay_capture_test_write_attempts(void);
extern "C" int overlay_capture_test_provider_pending(void);

extern "C" {
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
uint64_t g_dirty_ram_insns_run = 0;
uint64_t g_dirty_window_dispatches = 0;
uint64_t s_frame_count = 0;
uint32_t g_overlay_region_floor = 0x00010000u;
}

namespace {

uint8_t g_ram[2u * 1024u * 1024u]{};
uint32_t g_dirty_pages[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
int g_provider_calls = 0;
int g_provider_accepts = 0;

int provider_available() { return 1; }
int provider_request() {
    ++g_provider_calls;
    if (g_provider_calls == 1) return 0;  // inject one spawn failure
    ++g_provider_accepts;
    return 1;
}
int provider_busy() { return 0; }

const CodeProvider kProvider = {
    "capture-retry-test", provider_available, provider_request,
    provider_busy, nullptr,
};

bool bit_is_set(const uint32_t *bitmap, uint32_t phys) {
    const uint32_t word = phys >> 2;
    return ((bitmap[word >> 5] >> (word & 31u)) & 1u) != 0;
}

void set_exec(uint32_t phys) {
    const uint32_t word = phys >> 2;
    g_dirty_ram_exec_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    g_dirty_ram_dispatch_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    const uint32_t page = phys >> 12;
    g_dirty_ram_exec_page_bitmap[page >> 5] |= 1u << (page & 31u);
    g_dirty_pages[page >> 5] |= 1u << (page & 31u);
}

bool wait_for_failed_write() {
    for (int i = 0; i < 200; ++i) {
        overlay_autocapture_tick();
        if (overlay_capture_test_write_state() == 0 &&
            overlay_capture_test_write_attempts() >= 1u)
            return true;
        SDL_Delay(5);
    }
    return false;
}

bool wait_for_provider_pending() {
    for (int i = 0; i < 200; ++i) {
        overlay_autocapture_tick();
        if (overlay_capture_test_provider_pending()) return true;
        SDL_Delay(5);
    }
    return false;
}

std::string read_all(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

int fail(const char *message, const std::filesystem::path &root) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    overlay_capture_wait_pending();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    SDL_Quit();
    return 1;
}

}  // namespace

extern "C" uint8_t *memory_get_ram_ptr(void) { return g_ram; }
extern "C" uint32_t dirty_ram_get_bitmap_word_count(void) {
    return DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS;
}
extern "C" uint32_t dirty_ram_get_bitmap_word(uint32_t index) {
    return index < DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS ? g_dirty_pages[index] : 0u;
}
extern "C" int cdrom_load_in_progress(void) { return 0; }
extern "C" int fntrace_is_game_started(void) { return 1; }
extern "C" void overlay_loader_check_cache(uint32_t, uint32_t,
                                             const uint8_t *) {}
extern "C" int overlay_loader_registered_count(void) { return 0; }
extern "C" const CodeProvider *code_provider_active(void) { return &kProvider; }
extern "C" uint32_t crc32_compute(const uint8_t *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash ? hash : 1u;
}

int main() {
    SDL_SetMainReady();
    if (SDL_Init(0) != 0) {
        std::fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    const auto root = std::filesystem::temp_directory_path() /
        ("psxrecomp-capture-retry-" + std::to_string(
            static_cast<unsigned long long>(SDL_GetPerformanceCounter())));
    const auto capture = root / "overlay_captures.json";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    overlay_capture_set_path(capture.string().c_str());
    overlay_capture_set_enabled(1);
    overlay_autocapture_set_enabled(1);
    g_ram[0x10000] = 0x11;
    overlay_capture_on_dma(0x10000u, 4u, &g_ram[0x10000]);
    set_exec(0x10000u);
    g_dirty_window_dispatches = 128u;
    g_dirty_ram_insns_run = 100000u;
    s_frame_count = 120u;
    overlay_autocapture_tick();

    if (!wait_for_failed_write())
        return fail("failed periodic write was not retained", root);
    if (bit_is_set(g_dirty_ram_exec_pc_bitmap, 0x10000u))
        return fail("accepted snapshot did not begin a new live epoch", root);

    /* This evidence belongs to the newer live epoch and must survive retrying
     * the immutable old snapshot. */
    set_exec(0x10004u);
    std::filesystem::create_directories(root);
    s_frame_count = 1000u;
    overlay_autocapture_tick();
    if (!wait_for_provider_pending())
        return fail("durable retry did not retain failed provider request", root);
    if (!bit_is_set(g_dirty_ram_exec_pc_bitmap, 0x10004u))
        return fail("old snapshot retry erased newer live evidence", root);

    const std::string first_epoch = read_all(capture);
    if (first_epoch.find("0x80010000") == std::string::npos ||
        first_epoch.find("0x80010004") != std::string::npos)
        return fail("retried file did not preserve the immutable old epoch", root);

    /* Retry only the provider start. No RAM snapshot or evidence clear should
     * occur, and acceptance must be recorded exactly once. */
    s_frame_count = 2000u;
    overlay_autocapture_tick();
    if (g_provider_calls != 2 || g_provider_accepts != 1 ||
        overlay_capture_test_provider_pending())
        return fail("provider spawn failure was not retried exactly once", root);
    if (!bit_is_set(g_dirty_ram_exec_pc_bitmap, 0x10004u))
        return fail("provider retry mutated the newer live epoch", root);

    overlay_capture_wait_pending();
    std::filesystem::remove_all(root, ignored);
    SDL_Quit();
    std::puts("PASS: periodic capture and provider failures retain additive epochs");
    return 0;
}
