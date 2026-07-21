#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "code_generator.h"
#include "config_loader.h"
#include "control_flow.h"
#include "fmt/format.h"
#include "gte_register_classification.h"
#include "recompiler_patch.h"

namespace fs = std::filesystem;
using PSXRecompV4::RecompilerPatch;

namespace {

int failures = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        fmt::print("PASS  {}\n", name);
    } else {
        fmt::print(stderr, "FAIL  {}\n", name);
        ++failures;
    }
}

template <typename Fn>
void check_throws(Fn&& fn, const std::string& needle,
                  const std::string& name) {
    try {
        fn();
        check(false, name + " (did not throw)");
    } catch (const std::exception& e) {
        check(std::string(e.what()).find(needle) != std::string::npos,
              name + " (" + e.what() + ")");
    }
}

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

void write_word(PSXRecomp::PS1Executable& exe, uint32_t address,
                uint32_t word) {
    const size_t offset = static_cast<size_t>(address - exe.header.load_address);
    exe.code_data[offset + 0] = static_cast<uint8_t>(word);
    exe.code_data[offset + 1] = static_cast<uint8_t>(word >> 8);
    exe.code_data[offset + 2] = static_cast<uint8_t>(word >> 16);
    exe.code_data[offset + 3] = static_cast<uint8_t>(word >> 24);
}

std::string generate_first_instruction(uint32_t first_word,
                                       std::vector<RecompilerPatch> patches,
                                       bool overlay_mode,
                                       PSXRecomp::CodeGenConfig config = {}) {
    constexpr uint32_t base = 0x80010000u;
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = base;
    exe.header.initial_pc = base;
    exe.header.file_size = 20;
    append_word(exe.code_data, first_word);
    append_word(exe.code_data, 0x00000000u); // branch/nonbranch delay-slot nop
    append_word(exe.code_data, 0x24030001u); // addiu v1, zero, 1
    append_word(exe.code_data, 0x03E00008u); // jr ra
    append_word(exe.code_data, 0x00000000u); // return delay-slot nop

    PSXRecompV4::apply_recompiler_patches_to_executable(
        exe, patches, overlay_mode);

    PSXRecomp::Function function{};
    function.start_addr = base;
    function.end_addr = base + 20;
    function.size = 20;
    function.name = "patch_test";

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    config.overlay_mode = overlay_mode;
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg).full_code;
}

std::string base_config() {
    return R"toml([game]
name = "Patch Test"
id = "TEST-00000"
exe = "TEST.EXE"
load_address = "0x80010000"
entry_pc = "0x80010000"
text_size = "0x1000"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"
)toml";
}

fs::path write_config(const fs::path& root, const std::string& suffix,
                      const std::string& patch_tables) {
    const fs::path path = root / ("game-" + suffix + ".toml");
    std::ofstream file(path, std::ios::binary);
    file << base_config() << patch_tables;
    file.close();
    return path;
}

void parser_tests(const fs::path& root) {
    const auto valid = write_config(root, "valid", R"toml(
[[recompiler.patch]]
id = "gameplay-rate"
address = "0x80012340"
expected = "0x24020002"
replacement = "0x24020001"
note = "Test-only fixture"
)toml");
    const auto config = PSXRecompV4::load_game_config(valid);
    check(config.recompiler_patches.size() == 1,
          "parser accepts one guarded patch");
    check(config.recompiler_patches[0].id == "gameplay-rate" &&
          config.recompiler_patches[0].address == 0x80012340u &&
          config.recompiler_patches[0].expected == 0x24020002u &&
          config.recompiler_patches[0].replacement == 0x24020001u,
          "parser preserves patch fields");

    const auto duplicate_address = write_config(root, "duplicate-address", R"toml(
[[recompiler.patch]]
id = "first"
address = "0x80012340"
expected = "0x24020002"
replacement = "0x24020001"

[[recompiler.patch]]
id = "alias"
address = "0xA0012340"
expected = "0x24020002"
replacement = "0x24020001"
)toml");
    check_throws(
        [&] { (void)PSXRecompV4::load_game_config(duplicate_address); },
        "physical address", "parser rejects duplicate aliased addresses");

    const auto duplicate_id = write_config(root, "duplicate-id", R"toml(
[[recompiler.patch]]
id = "same-id"
address = "0x80012340"
expected = "0x24020002"
replacement = "0x24020001"

[[recompiler.patch]]
id = "same-id"
address = "0x80012344"
expected = "0x24030002"
replacement = "0x24030001"
)toml");
    check_throws([&] { (void)PSXRecompV4::load_game_config(duplicate_id); },
                 "duplicate [[recompiler.patch]] id",
                 "parser rejects duplicate IDs");

    const auto unaligned = write_config(root, "unaligned", R"toml(
[[recompiler.patch]]
id = "unaligned"
address = "0x80012342"
expected = "0x24020002"
replacement = "0x24020001"
)toml");
    check_throws([&] { (void)PSXRecompV4::load_game_config(unaligned); },
                 "instruction-aligned", "parser rejects unaligned sites");

    const auto negsub = write_config(root, "negsub", R"toml(
[widescreen.cull]
negsub_sites = ["0x80012340"]
)toml");
    const auto negsub_config = PSXRecompV4::load_game_config(negsub);
    check(negsub_config.ws_cull_negsub_sites ==
              std::vector<uint32_t>{0x80012340u},
          "parser preserves negsub cull sites");

    const auto vxrange = write_config(root, "vxrange", R"toml(
[widescreen.cull]
vxrange_sites = ["0x80012340"]
)toml");
    const auto vxrange_config = PSXRecompV4::load_game_config(vxrange);
    check(vxrange_config.ws_cull_vxrange_sites ==
              std::vector<uint32_t>{0x80012340u},
          "parser preserves masked-u16 X-window sites");

    const auto depth = write_config(root, "depth", R"toml(
[widescreen.cull]
depth_sites = ["0x80012340"]
)toml");
    const auto depth_config = PSXRecompV4::load_game_config(depth);
    check(depth_config.ws_cull_depth_sites ==
              std::vector<uint32_t>{0x80012340u},
          "parser preserves depth-bound sites");

    const auto range = write_config(root, "range-cull", R"toml(
[widescreen.cull]
range_sites = ["0x80012340"]
)toml");
    const auto range_config = PSXRecompV4::load_game_config(range);
    check(range_config.ws_cull_range_sites ==
              std::vector<uint32_t>{0x80012340u},
          "parser preserves explicit range cull sites");
}

void capture_history_config_tests(const fs::path& root) {
    const auto valid = write_config(root, "capture-history", R"toml(
[runtime]
overlay_cache = true
overlay_capture_history = true
overlay_capture_persist_dir = ".aot_capture_history/TEST-00000"
)toml");
    const auto cfg = PSXRecompV4::load_game_config(valid);
    check(cfg.runtime.overlay_capture_history,
          "parser enables durable overlay capture history");
    check(cfg.runtime.overlay_capture_persist_dir ==
              ".aot_capture_history/TEST-00000",
          "parser preserves project-relative capture history directory");

    const auto escaping = write_config(root, "capture-history-escape", R"toml(
[runtime]
overlay_capture_persist_dir = "../outside"
)toml");
    check_throws([&] { (void)PSXRecompV4::load_game_config(escaping); },
                 "must stay inside the project",
                 "parser rejects escaping capture history directory");

    const auto absolute = write_config(root, "capture-history-absolute", R"toml(
[runtime]
overlay_capture_persist_dir = "C:/outside"
)toml");
    check_throws([&] { (void)PSXRecompV4::load_game_config(absolute); },
                 "must be project-relative",
                 "parser rejects absolute capture history directory");
}

void codegen_tests() {
    constexpr uint32_t original = 0x24020002u;    // addiu v0, zero, 2
    constexpr uint32_t replacement = 0x24020001u; // addiu v0, zero, 1
    const RecompilerPatch physical_alias{
        "gameplay-rate", 0x00010000u, original, replacement, ""};

    const std::string applied =
        generate_first_instruction(original, {physical_alias}, false);
    check(applied.find("0x24020001") != std::string::npos &&
          applied.find("cpu->gpr[2] = 1;") != std::string::npos,
          "codegen applies exact patch through physical alias");

    check_throws(
        [&] { (void)generate_first_instruction(0x24020003u,
                                               {physical_alias}, false); },
        "wrong game revision or stale patch",
        "main codegen fails on stale opcode guard");

    const std::string overlay =
        generate_first_instruction(0x24020003u, {physical_alias}, true);
    check(overlay.find("0x24020003") != std::string::npos &&
          overlay.find("0x24020001") == std::string::npos,
          "overlay nonmatching variant remains unchanged");

    constexpr uint32_t beq_v0_zero = 0x10400002u;
    constexpr uint32_t bne_v0_zero = 0x14400002u;
    const RecompilerPatch branch_patch{
        "feature-branch", 0x80010000u, beq_v0_zero, bne_v0_zero, ""};
    const std::string branch =
        generate_first_instruction(beq_v0_zero, {branch_patch}, false);
    check(branch.find("cpu->gpr[2] != cpu->gpr[0]") != std::string::npos,
          "patch is applied before control-flow analysis");

    std::vector<RecompilerPatch> merged{physical_alias};
    PSXRecompV4::merge_recompiler_patches(merged, {physical_alias});
    check(merged.size() == 1,
          "config merge deduplicates an identical patch");

    const RecompilerPatch conflicting_id{
        "gameplay-rate", 0x00010004u, original, replacement, ""};
    check_throws(
        [&] {
            PSXRecompV4::merge_recompiler_patches(merged, {conflicting_id});
        },
        "conflicting recompiler patches",
        "config merge rejects cross-config ID conflicts");

    PSXRecomp::CodeGenConfig negsub_config;
    negsub_config.ws_cull_negsub_sites.insert(0x80010000u);
    const std::string negsub = generate_first_instruction(
        0x00041023u, {}, false, negsub_config); // subu v0,zero,a0
    check(negsub.find("cpu->gpr[2] = 0u - cpu->gpr[4] - "
                      "(uint32_t)psx_ws_x_margin()") != std::string::npos,
          "codegen emits configured negsub horizontal low-edge widen");

    const std::string overlay_mismatch = generate_first_instruction(
        0x00041021u, {}, true, negsub_config); // addu v0,zero,a0
    check(overlay_mismatch.find("ws cull negsub") == std::string::npos,
          "overlay nonmatching negsub variant remains unchanged");

    PSXRecomp::CodeGenConfig vxrange_config;
    vxrange_config.ws_cull_vxrange_sites.insert(0x80010000u);
    const std::string vxrange = generate_first_instruction(
        0x2C820140u, {}, false, vxrange_config); // sltiu v0,a0,0x140
    check(vxrange.find("cpu->gpr[2] = psx_ws_cull_vxrange(cpu->gpr[4], 320)") !=
              std::string::npos,
          "codegen routes masked-u16 X-window sites through shared helper");

    const std::string vxrange_overlay_mismatch = generate_first_instruction(
        0x24820140u, {}, true, vxrange_config); // addiu v0,a0,0x140
    check(vxrange_overlay_mismatch.find("ws cull masked-u16") == std::string::npos,
          "overlay nonmatching masked-u16 variant remains unchanged");

    PSXRecomp::CodeGenConfig depth_config;
    depth_config.ws_cull_depth_sites.insert(0x80010000u);
    const std::string signed_depth = generate_first_instruction(
        0x28827FFFu, {}, false, depth_config); // slti v0,a0,0x7fff
    check(signed_depth.find("psx_ws_depth_bound(32767)") != std::string::npos,
          "codegen emits signed aspect-scaled depth bound");

    const std::string unsigned_depth = generate_first_instruction(
        0x2C82FFFFu, {}, false, depth_config); // sltiu v0,a0,-1
    check(unsigned_depth.find("psx_ws_depth_bound(-1)") != std::string::npos,
          "unsigned depth emit preserves MIPS immediate sign extension");

    const std::string depth_overlay_mismatch = generate_first_instruction(
        0x24827FFFu, {}, true, depth_config); // addiu v0,a0,0x7fff
    check(depth_overlay_mismatch.find("ws cull depth") == std::string::npos,
          "overlay nonmatching depth variant remains unchanged");

    PSXRecomp::CodeGenConfig range_config;
    range_config.ws_cull_range_sites.insert(0x80010000u);
    const std::string range = generate_first_instruction(
        0x2C8201C1u, {}, false, range_config); // sltiu v0,a0,0x1c1
    check(range.find("2*psx_ws_x_margin()") != std::string::npos,
          "native range emit widens by both horizontal margins");
}

void gte_codegen_classification_tests() {
    bool all_ok = true;
    for (uint8_t reg = 0; reg < 32; ++reg) {
        const auto expect = [&](uint32_t word, const std::string& call,
                                bool needs_helper, const char *kind) {
            const std::string code = generate_first_instruction(word, {}, false);
            const bool has_helper = code.find(call) != std::string::npos;
            if (has_helper != needs_helper) {
                fmt::print(stderr,
                           "FAIL  full-game GTE {} reg {} helper={} expected={}\n",
                           kind, reg, has_helper, needs_helper);
                all_ok = false;
            }
        };

        const uint32_t cop2 = 0x12u << 26;
        expect(cop2 | (0x00u << 21) | (2u << 16) | (uint32_t(reg) << 11),
               fmt::format("gte_read_data(cpu, {})", reg),
               PSXRecompGTERegisters::data_read_needs_helper(reg), "MFC2");
        expect(cop2 | (0x02u << 21) | (2u << 16) | (uint32_t(reg) << 11),
               fmt::format("gte_read_ctrl(cpu, {})", reg),
               PSXRecompGTERegisters::ctrl_read_needs_helper(reg), "CFC2");
        expect(cop2 | (0x04u << 21) | (2u << 16) | (uint32_t(reg) << 11),
               fmt::format("gte_write_data(cpu, {}", reg),
               PSXRecompGTERegisters::data_write_needs_helper(reg), "MTC2");
        expect(cop2 | (0x06u << 21) | (2u << 16) | (uint32_t(reg) << 11),
               fmt::format("gte_write_ctrl(cpu, {}", reg),
               PSXRecompGTERegisters::ctrl_write_needs_helper(reg), "CTC2");
        expect((0x32u << 26) | (3u << 21) | (uint32_t(reg) << 16),
               fmt::format("gte_write_data(cpu, {}", reg),
               PSXRecompGTERegisters::data_write_needs_helper(reg), "LWC2");
        expect((0x3Au << 26) | (3u << 21) | (uint32_t(reg) << 16),
               fmt::format("gte_read_data(cpu, {})", reg),
               PSXRecompGTERegisters::data_read_needs_helper(reg), "SWC2");
    }
    check(all_ok, "full-game GTE helper classification covers all registers");
}

void jump_table_producer_codegen_test() {
    constexpr uint32_t base = 0x80010000u;
    constexpr uint32_t entry = base + 0x500u;
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = base;
    exe.header.initial_pc = entry;
    exe.header.file_size = 0x1000u;
    exe.code_data.resize(0x1000u, 0u);

    write_word(exe, base + 0x500u, 0x3C088001u); // lui t0,0x8001
    write_word(exe, base + 0x504u, 0x25100A00u); // addiu s0,t0,0x0a00
    write_word(exe, base + 0x50Cu, 0x2C620003u); // sltiu v0,v1,3
    write_word(exe, base + 0x510u, 0x1040001Bu); // beq v0,zero,+0x580
    write_word(exe, base + 0x518u, 0x00031080u); // sll v0,v1,2
    write_word(exe, base + 0x51Cu, 0x00501021u); // addu v0,v0,s0
    write_word(exe, base + 0x520u, 0x8C420000u); // lw v0,0(v0)
    write_word(exe, base + 0x528u, 0x00400008u); // jr v0
    const uint32_t cases[] = {
        base + 0x540u, base + 0x550u, base + 0x560u};
    for (size_t i = 0; i < 3u; i++) {
        write_word(exe, cases[i], 0x24020001u + static_cast<uint32_t>(i));
        write_word(exe, cases[i] + 4u, 0x03E00008u);
        write_word(exe, base + 0xA00u + static_cast<uint32_t>(i * 4u),
                   cases[i]);
    }
    write_word(exe, base + 0x580u, 0x03E00008u);
    write_word(exe, base + 0x590u,
               0x08000000u | ((entry >> 2) & 0x03FFFFFFu));

    PSXRecomp::Function function{};
    function.start_addr = entry;
    function.end_addr = base + 0x588u;
    function.size = function.end_addr - function.start_addr;
    function.name = "producer_switch";
    function.producer_lo = base + 0x400u;
    function.producer_hi = base + 0x900u; // table belongs to adjacent producer

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto bounded_cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator bounded_generator(exe);
    const std::string bounded =
        bounded_generator.generate_function(function, bounded_cfg).full_code;
    check(bounded.find("/* jump table") == std::string::npos,
          "codegen cannot resurrect an adjacent-producer jump table");

    function.producer_lo = 0u;
    function.producer_hi = 0u;
    const auto single_image_cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator single_image_generator(exe);
    const std::string single_image = single_image_generator
        .generate_function(function, single_image_cfg).full_code;
    check(single_image.find("/* jump table") != std::string::npos,
          "codegen still emits the same table for a single owned image");

    // Exercise the actual overlapping-alias emitter path. The alias begins at
    // a later block which jumps back through the host's complete table setup,
    // so an unbounded alias really would expose the adjacent producer bytes.
    // Producer ownership must survive Function -> alias CFG -> shared body.
    PSXRecomp::Function alias = function;
    alias.start_addr = base + 0x590u;
    alias.end_addr = base + 0x598u;
    alias.size = alias.end_addr - alias.start_addr;
    alias.name = "producer_switch_alias";
    alias.alias_walk_lo = entry;
    alias.alias_group_entries = {alias.start_addr};
    alias.producer_lo = base + 0x400u;
    alias.producer_hi = base + 0x900u;

    const auto alias_cfg = analyzer.analyze_function(alias);
    check(alias_cfg.producer_lo == alias.producer_lo &&
          alias_cfg.producer_hi == alias.producer_hi,
          "alias CFG retains its host producer bounds");
    PSXRecomp::CodeGenerator alias_generator(exe);
    const auto alias_generated = alias_generator.generate_alias_group(
        {&alias}, alias_cfg, "");
    check(!alias_generated.empty() &&
          alias_generated.front().full_code.find("/* jump table") ==
              std::string::npos,
          "alias emitter cannot expose an adjacent producer jump table");

    alias.producer_lo = 0u;
    alias.producer_hi = 0u;
    const auto unbounded_alias_cfg = analyzer.analyze_function(alias);
    PSXRecomp::CodeGenerator unbounded_alias_generator(exe);
    const auto unbounded_alias_generated =
        unbounded_alias_generator.generate_alias_group(
            {&alias}, unbounded_alias_cfg, "");
    check(!unbounded_alias_generated.empty() &&
          unbounded_alias_generated.front().full_code.find("/* jump table") !=
              std::string::npos,
          "alias regression fixture reaches the table when ownership is absent");
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        fmt::format("psxrecomp-patch-test-{}", reinterpret_cast<uintptr_t>(&failures));
    fs::remove_all(root);
    fs::create_directories(root);

    try {
        parser_tests(root);
        capture_history_config_tests(root);
        codegen_tests();
        gte_codegen_classification_tests();
        jump_table_producer_codegen_test();
    } catch (const std::exception& e) {
        fmt::print(stderr, "FAIL  unexpected exception: {}\n", e.what());
        ++failures;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    fmt::print("\nGuarded recompiler patches: {}\n",
               failures == 0 ? "all tests passed" : "failures detected");
    return failures == 0 ? 0 : 1;
}
