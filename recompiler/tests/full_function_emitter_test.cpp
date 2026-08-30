#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "../src/bios_address_model.h"
#include "../src/config_loader.h"
#include "../src/full_function_emitter.h"
#include "../src/function_discovery.h"
#include "../src/pgxp_hook_emitter.h"

using PSXRecompV4::BiosAddressModel;
using PSXRecompV4::BiosAddrCopy;
using PSXRecompV4::BiosConfig;
using PSXRecompV4::DiscoveredFunction;
using PSXRecompV4::DiscoveryResult;
using PSXRecompV4::EmitStats;
using PSXRecompV4::FullFunctionEmitter;
using PSXRecompV4::FunctionDiscovery;

namespace {

constexpr uint32_t kBase = 0xBFC00000u;
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void append_word(std::vector<uint8_t>& rom, uint32_t word) {
    rom.push_back(static_cast<uint8_t>(word));
    rom.push_back(static_cast<uint8_t>(word >> 8));
    rom.push_back(static_cast<uint8_t>(word >> 16));
    rom.push_back(static_cast<uint8_t>(word >> 24));
}

DiscoveredFunction function_at(uint32_t entry, uint32_t end,
                               std::initializer_list<uint32_t> leaders) {
    DiscoveredFunction fn{};
    fn.entry_addr = entry;
    fn.normalized_addr = entry & 0x1FFFFFFFu;
    fn.end_addr = end;
    fn.instruction_count = (end - entry) / 4u + 1u;
    fn.termination_reason = "jr_ra";
    fn.discovered_by = "synthetic test";
    fn.block_leaders.assign(leaders.begin(), leaders.end());
    return fn;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

struct RunResult {
    EmitStats stats;
    std::string dispatch;
    std::string full;
};

RunResult run_case(const char* name, const std::vector<uint32_t>& words,
                   std::vector<DiscoveredFunction> functions) {
    std::vector<uint8_t> rom;
    for (uint32_t word : words) append_word(rom, word);

    DiscoveryResult discovery{};
    discovery.ok = true;
    discovery.functions = std::move(functions);

    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const auto out_dir = std::filesystem::temp_directory_path() /
                         ("psxrecomp-full-emitter-" + std::string(name) + "-" +
                          std::to_string(nonce));
    std::filesystem::create_directories(out_dir);

    const std::string stem = "Test";
    RunResult result;
    result.stats = FullFunctionEmitter::emit(
        rom, kBase, kBase + static_cast<uint32_t>(rom.size()) - 1u,
        discovery, "synthetic", out_dir.string(), stem);
    result.dispatch = read_file(out_dir / (stem + "_dispatch.c"));
    result.full = read_file(out_dir / (stem + "_full.c"));
    std::filesystem::remove_all(out_dir);
    return result;
}

void expect_entry_absent(const RunResult& result, uint32_t normalized,
                         const char* message) {
    char needle[96];
    std::snprintf(needle, sizeof(needle),
                  "{ 0x%08Xu, Test_func_%08X }", normalized, normalized);
    expect(result.dispatch.find(needle) == std::string::npos, message);
}

void expect_dispatch_key_absent(const RunResult& result, uint32_t normalized,
                                const char* message) {
    char needle[32];
    std::snprintf(needle, sizeof(needle), "{ 0x%08Xu,", normalized);
    expect(result.dispatch.find(needle) == std::string::npos, message);
}

void delay_slot_load_falls_back() {
    const auto result = run_case(
        "delay-slot",
        {
            0x10000001u,  // beq zero,zero,+1
            0x8D280000u,  // lw t0,0(t1) -- branch delay slot
            0x01001021u,  // addu v0,t0,zero -- dependent successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 16u, {kBase, kBase + 8u})});
    expect(result.stats.functions_interpreted == 1,
           "delay-slot load marks the function for interpretation");
    expect(result.stats.functions_emitted == 0,
           "delay-slot load function is not emitted");
    expect_entry_absent(result, 0x00000500u,
                        "delay-slot load function is absent from dispatch");
    expect_dispatch_key_absent(result, 0x00000508u,
                               "delay-slot successor continuation is absent from dispatch");
}

void label_split_load_falls_back() {
    const auto result = run_case(
        "label-split",
        {
            0x8D280000u,  // lw t0,0(t1)
            0x01001021u,  // addu v0,t0,zero -- dependent labeled successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase, kBase + 4u})});
    expect(result.stats.functions_interpreted == 1,
           "label-split load marks the function for interpretation");
    expect_entry_absent(result, 0x00000500u,
                        "label-split function is absent from dispatch");
    expect_dispatch_key_absent(result, 0x00000504u,
                               "label-split continuation is absent from dispatch");
}

void fragment_split_load_falls_back() {
    const auto result = run_case(
        "fragment-split",
        {
            0x8D280000u,  // fragment 1: lw t0,0(t1)
            0x01001021u,  // fragment 2: dependent successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {
            function_at(kBase, kBase, {kBase}),
            function_at(kBase + 4u, kBase + 12u, {kBase + 4u}),
        });
    expect(result.stats.functions_interpreted == 1,
           "fragment-split load marks only its owning function for interpretation");
    expect(result.stats.functions_emitted == 1,
           "unaffected neighboring fragment remains native");
    expect_entry_absent(result, 0x00000500u,
                        "fragment-split load owner is absent from dispatch");
}

void noncomplementary_lwl_falls_back() {
    const auto result = run_case(
        "lwl-dependent",
        {
            0x89280000u,  // lwl t0,0(t1)
            0x01001021u,  // addu v0,t0,zero -- dependent, not lwr
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase})});
    expect(result.stats.functions_interpreted == 1,
           "non-complementary LWL dependency falls back");
    expect_entry_absent(result, 0x00000500u,
                        "non-complementary LWL function is absent from dispatch");
}

void complementary_lwl_lwr_stays_native() {
    const auto result = run_case(
        "lwl-lwr",
        {
            0x89280000u,  // lwl t0,0(t1)
            0x99280003u,  // lwr t0,3(t1) -- architectural forwarding pair
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase})});
    expect(result.stats.functions_interpreted == 0,
           "complementary LWL/LWR does not fall back");
    expect(result.stats.functions_emitted == 1,
           "complementary LWL/LWR remains native");
    expect(result.dispatch.find("static int dispatch_find(uint32_t phys)") !=
               std::string::npos,
           "emitted dispatch uses the cached exact lookup helper");
    expect(result.dispatch.find("dispatch_table[index_plus_one - 1u].addr == phys") !=
               std::string::npos,
           "dispatch cache hits revalidate the exact table address");
}

void destructive_pgxp_writes_are_emitted() {
    std::string code = "cpu->gpr[2] = cpu->gpr[3] & cpu->gpr[4];";
    PSXRecomp::append_pgxp_hooks(0x00641024u, code); /* and v0,v1,a0 */
    expect(code.find("PGXP_GPR_WRITE(2u)") != std::string::npos,
           "AND emits byte-identical-safe PGXP destination invalidation");

    code = "cpu->gpr[2] = cpu->gpr[3] < 7;";
    PSXRecomp::append_pgxp_hooks(0x28620007u, code); /* slti v0,v1,7 */
    expect(code.find("PGXP_GPR_WRITE(2u)") != std::string::npos,
           "SLTI emits PGXP destination invalidation");

    code = "cpu->gpr[2] = cpu->gte_ctrl[3];";
    PSXRecomp::append_pgxp_hooks(0x48421800u, code); /* cfc2 v0,c3 */
    expect(code.find("PGXP_COP2") != std::string::npos,
           "CFC2 updates the GPR shadow instead of leaving it stale");

    code = "cpu->gpr[31] = 0x80012348u;";
    PSXRecomp::append_pgxp_hooks(0x0C0048D2u, code); /* jal 0x80012348 */
    expect(code.find("PGXP_GPR_WRITE(31u)") != std::string::npos,
           "JAL invalidates byte-identical link-register provenance");

    code = "cpu->gpr[2] = cpu->cop0[12];";
    PSXRecomp::append_pgxp_hooks(0x40026000u, code); /* mfc0 v0,Status */
    expect(code.find("PGXP_GPR_WRITE(2u)") != std::string::npos,
           "MFC0 invalidates destination provenance");
}

void delayed_pgxp_shadow_hooks_are_emitted() {
    const auto result = run_case(
        "pgxp-load-delay",
        {
            0x8D280000u,  // lw t0,0(t1)
            0x01001021u,  // addu v0,t0,zero -- reads the old t0
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase})});
    expect(result.stats.functions_emitted == 1,
           "dependent load pair remains native in the full emitter");
    expect(result.full.find("PGXP_LOAD_DELAYED(0x8D280000u") !=
               std::string::npos,
           "full emitter stages the load shadow before the dependent successor");
    expect(result.full.find("PGXP_LOAD_COMMIT(8u, psx_ldd_BFC00000);") !=
               std::string::npos,
           "full emitter commits the shadow after delayed writeback");
}

}  // namespace

int main() {
    BiosConfig config{};
    config.config_path = "test://full-function-emitter";
    config.load_address = kBase;
    config.text_size = 0x1000u;
    BiosAddrCopy copy;
    copy.name = "synthetic RAM-backed BIOS code";
    copy.rom_lo = 0x1FC00000u;
    copy.rom_hi = 0x1FC01000u;
    copy.ram_lo = 0x00000500u;
    copy.runtime_base = 0x00000500u;
    copy.key_is_ram = true;
    config.address_copies.push_back(copy);
    BiosAddressModel model = BiosAddressModel::from_config(config);
    FunctionDiscovery::set_address_model(&model);
    FullFunctionEmitter::set_address_model(&model);

    delay_slot_load_falls_back();
    label_split_load_falls_back();
    fragment_split_load_falls_back();
    noncomplementary_lwl_falls_back();
    complementary_lwl_lwr_stays_native();
    destructive_pgxp_writes_are_emitted();
    delayed_pgxp_shadow_hooks_are_emitted();

    if (failures != 0) {
        std::fprintf(stderr, "%d full-function emitter test(s) failed\n", failures);
        return 1;
    }
    std::puts("Full-function emitter fail-closed tests passed");
    return 0;
}
