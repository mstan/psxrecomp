#include "function_analysis.h"
#include "ps1_exe_parser.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kLoad = 0x80010000u;
int failures = 0;

#define CHECK(cond, message) do {                                              \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL: %s\n", (message));                        \
        failures++;                                                            \
    }                                                                          \
} while (0)

void put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset + 0] = static_cast<uint8_t>(value);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
    data[offset + 2] = static_cast<uint8_t>(value >> 16);
    data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t jal(uint32_t target) {
    return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu);
}

std::vector<uint8_t> make_exe_buffer(uint32_t image_size,
                                     uint32_t entry = kLoad) {
    std::vector<uint8_t> data(2048u + image_size, 0);
    std::memcpy(data.data(), "PS-X EXE", 8);
    put32(data, 0x10, entry);
    put32(data, 0x18, kLoad);
    put32(data, 0x1C, image_size);
    return data;
}

PSXRecomp::PS1Executable parse(std::vector<uint8_t> data) {
    std::string error;
    auto exe = PSXRecomp::PS1ExeParser::parse_buffer(data, error);
    CHECK(exe.has_value(), error.c_str());
    return exe.value();
}

std::set<uint32_t> starts(const PSXRecomp::FunctionAnalysisResult& result) {
    std::set<uint32_t> out;
    for (const auto& function : result.functions) out.insert(function.start_addr);
    return out;
}

struct ApeSwitchFixture {
    std::vector<uint8_t> image;
    uint32_t entry = kLoad + 0x500u;
    uint32_t jr_pc = kLoad + 0x528u;
    uint32_t table = kLoad + 0xA00u;
    std::vector<uint32_t> cases = {
        kLoad + 0x540u, kLoad + 0x550u, kLoad + 0x560u};
};

ApeSwitchFixture make_ape_switch() {
    ApeSwitchFixture f;
    f.image = make_exe_buffer(0x1000u, f.entry);
    const size_t text = 2048u;
    put32(f.image, text + 0x500, 0x3C088001u); // lui t0,0x8001
    put32(f.image, text + 0x504, 0x25100A00u); // addiu s0,t0,0x0a00
    put32(f.image, text + 0x508, 0x00000000u);
    put32(f.image, text + 0x50C, 0x2C620003u); // sltiu v0,v1,3
    put32(f.image, text + 0x510, 0x1040001Bu); // beq v0,zero,+0x580
    put32(f.image, text + 0x514, 0x00000000u);
    put32(f.image, text + 0x518, 0x00031080u); // sll v0,v1,2
    put32(f.image, text + 0x51C, 0x00501021u); // addu v0,v0,s0
    put32(f.image, text + 0x520, 0x8C420000u); // lw v0,0(v0)
    put32(f.image, text + 0x524, 0x00000000u);
    put32(f.image, text + 0x528, 0x00400008u); // jr v0
    put32(f.image, text + 0x52C, 0x00000000u);
    for (size_t i = 0; i < f.cases.size(); i++) {
        size_t off = static_cast<size_t>(f.cases[i] - kLoad);
        put32(f.image, text + off, 0x24020001u + static_cast<uint32_t>(i));
        put32(f.image, text + off + 4u, 0x03E00008u);
        put32(f.image, text + off + 8u, 0x00000000u);
        put32(f.image, text + 0xA00u + i * 4u, f.cases[i]);
    }
    put32(f.image, text + 0x580, 0x03E00008u);
    put32(f.image, text + 0x584, 0x00000000u);
    return f;
}

bool resolves_ape_switch(const ApeSwitchFixture& f,
                         uint32_t entry,
                         uint32_t producer_lo = kLoad,
                         uint32_t producer_hi = kLoad + 0x1000u) {
    auto exe = parse(f.image);
    PSXRecomp::ExactJumpTable table;
    return PSXRecomp::resolve_exact_bounded_jump_table(
        exe, entry, kLoad + 0x1000u, f.jr_pc, 2u, table,
        nullptr, producer_lo, producer_hi);
}

} // namespace

int main() {
    auto image = make_exe_buffer(0x2000);
    const size_t text = 2048;

    // Root: direct call to a framed target, direct call to a frameless leaf,
    // unresolved jalr, direct call beyond the eventual bound, then return.
    put32(image, text + 0x00, jal(kLoad + 0x100));
    put32(image, text + 0x04, 0x00000000u);
    put32(image, text + 0x08, jal(kLoad + 0x300));
    put32(image, text + 0x0C, 0x00000000u);
    put32(image, text + 0x10, 0x0320F809u); // jalr $ra,$t9 (unresolved)
    put32(image, text + 0x14, 0x00000000u);
    put32(image, text + 0x18, jal(kLoad + 0x1100));
    put32(image, text + 0x1C, 0x00000000u);
    put32(image, text + 0x20, 0x03E00008u);
    put32(image, text + 0x24, 0x00000000u);

    // Callable direct target.
    put32(image, text + 0x100, 0x27BDFFF0u);
    put32(image, text + 0x104, 0x03E00008u);
    put32(image, text + 0x108, 0x27BD0010u);

    // Valid-looking callback not referenced by a direct call.
    put32(image, text + 0x200, 0x27BDFFF0u);
    put32(image, text + 0x204, 0x03E00008u);
    put32(image, text + 0x208, 0x27BD0010u);

    // A direct JAL is sufficient callable-boundary evidence even when the
    // target is a tiny frameless leaf.
    put32(image, text + 0x300, 0x24020001u); // addiu $v0,$zero,1
    put32(image, text + 0x304, 0x03E00008u);
    put32(image, text + 0x308, 0x00000000u);

    // Callable target outside the verified static bound.
    put32(image, text + 0x1100, 0x27BDFFF0u);
    put32(image, text + 0x1104, 0x03E00008u);
    put32(image, text + 0x1108, 0x27BD0010u);

    auto exe = parse(image);
    std::string error;
    CHECK(PSXRecomp::apply_static_analysis_bound(exe, 0x1000, error),
          "valid page-aligned analysis bound is accepted");
    CHECK(exe.code_size() == 0x1000 && exe.code_data.size() == 0x1000,
          "accepted bound truncates header and analysis payload together");

    PSXRecomp::FunctionAnalyzer analyzer(exe);
    const auto reachable = starts(analyzer.analyze_exact_entries({kLoad}));
    CHECK(reachable == std::set<uint32_t>({kLoad, kLoad + 0x100,
                                           kLoad + 0x300}),
          "reachable analysis follows framed and frameless direct JAL targets");
    CHECK(!reachable.count(kLoad + 0x200),
          "unseen indirect callback fails closed");
    CHECK(!reachable.count(kLoad + 0x1100),
          "direct target beyond verified bound fails closed");

    // A JAL-shaped word can target an embedded table whose first pointer also
    // happens to decode as a legal MIPS instruction. Three adjacent local
    // pointers are stronger data evidence; keep that target interpreted.
    auto pointer_image = make_exe_buffer(0x1000);
    put32(pointer_image, text + 0x00, jal(kLoad + 0x400));
    put32(pointer_image, text + 0x04, 0x00000000u);
    put32(pointer_image, text + 0x08, 0x03E00008u);
    put32(pointer_image, text + 0x0C, 0x00000000u);
    put32(pointer_image, text + 0x400, kLoad + 0x500);
    put32(pointer_image, text + 0x404, kLoad + 0x520);
    put32(pointer_image, text + 0x408, kLoad + 0x540);
    auto pointer_exe = parse(pointer_image);
    PSXRecomp::FunctionAnalyzer pointer_analyzer(pointer_exe);
    const auto pointer_starts = starts(
        pointer_analyzer.analyze_exact_entries({kLoad}));
    CHECK(!pointer_starts.count(kLoad + 0x400),
          "reachable analysis rejects a dense local pointer table target");

    // Discover call targets in stable rounds. An early root's JAL into the
    // middle of a later explicit root must remain absorbed by that host rather
    // than splitting/hard-capping it before the host is walked.
    auto interior_call_image = make_exe_buffer(0x1000);
    put32(interior_call_image, text + 0x00, jal(kLoad + 0x108));
    put32(interior_call_image, text + 0x04, 0x00000000u);
    put32(interior_call_image, text + 0x08, 0x03E00008u);
    put32(interior_call_image, text + 0x0C, 0x00000000u);
    put32(interior_call_image, text + 0x100, 0x24020001u);
    put32(interior_call_image, text + 0x104, 0x24420001u);
    put32(interior_call_image, text + 0x108, 0x24420001u);
    put32(interior_call_image, text + 0x10C, 0x03E00008u);
    put32(interior_call_image, text + 0x110, 0x00000000u);
    auto interior_call_exe = parse(interior_call_image);
    PSXRecomp::FunctionAnalyzer interior_call_analyzer(interior_call_exe);
    const auto interior_call_starts = starts(
        interior_call_analyzer.analyze_exact_entries(
            {kLoad, kLoad + 0x100}));
    CHECK(interior_call_starts ==
              std::set<uint32_t>({kLoad, kLoad + 0x100}),
          "direct call into an owned function stays an interior alias");

    // Ownership must also settle among roots discovered in the same round.
    // The initial root calls both A and an address B inside A. Neither address
    // is owned by the initial partition, but walking A without B as a cap
    // reaches B, so B is an alias rather than a sibling function.
    auto same_round_image = make_exe_buffer(0x1000);
    put32(same_round_image, text + 0x00, jal(kLoad + 0x100));
    put32(same_round_image, text + 0x04, 0x00000000u);
    put32(same_round_image, text + 0x08, jal(kLoad + 0x110));
    put32(same_round_image, text + 0x0C, 0x00000000u);
    put32(same_round_image, text + 0x10, 0x03E00008u);
    put32(same_round_image, text + 0x14, 0x00000000u);
    put32(same_round_image, text + 0x100, jal(kLoad + 0x110));
    put32(same_round_image, text + 0x104, 0x00000000u);
    put32(same_round_image, text + 0x108, 0x24420001u);
    put32(same_round_image, text + 0x10C, 0x24420001u);
    put32(same_round_image, text + 0x110, 0x03E00008u);
    put32(same_round_image, text + 0x114, 0x00000000u);
    auto same_round_exe = parse(same_round_image);
    PSXRecomp::FunctionAnalyzer same_round_analyzer(same_round_exe);
    const auto same_round_result =
        same_round_analyzer.analyze_exact_entries({kLoad});
    const auto same_round_starts = starts(same_round_result);
    CHECK(same_round_starts ==
              std::set<uint32_t>({kLoad, kLoad + 0x100}),
          "same-round call target inside a discovered host stays absorbed");
    bool same_round_hosted = false;
    for (const auto& absorbed : same_round_result.absorbed_entries) {
        same_round_hosted |= (absorbed.addr == kLoad + 0x110 &&
                              absorbed.host_start == kLoad + 0x100 &&
                              absorbed.addr < absorbed.host_end &&
                              absorbed.source_addr == kLoad + 0x100 &&
                              !absorbed.resolved_indirect);
    }
    CHECK(same_round_hosted,
          "every analyzer-proven absorbed target names its final CFG host");

    // Resolved constant-register calls carry the same reachable provenance as
    // direct JALs and can therefore become final hosted aliases too.
    auto indirect_alias_image = make_exe_buffer(0x1000);
    put32(indirect_alias_image, text + 0x00, jal(kLoad + 0x100));
    put32(indirect_alias_image, text + 0x04, 0x00000000u);
    put32(indirect_alias_image, text + 0x08, 0x03E00008u);
    put32(indirect_alias_image, text + 0x0C, 0x00000000u);
    put32(indirect_alias_image, text + 0x100, 0x3C088001u);
    put32(indirect_alias_image, text + 0x104, 0x25080110u);
    put32(indirect_alias_image, text + 0x108, 0x0100F809u);
    put32(indirect_alias_image, text + 0x10C, 0x00000000u);
    put32(indirect_alias_image, text + 0x110, 0x03E00008u);
    put32(indirect_alias_image, text + 0x114, 0x00000000u);
    auto indirect_alias_exe = parse(indirect_alias_image);
    PSXRecomp::FunctionAnalyzer indirect_alias_analyzer(indirect_alias_exe);
    const auto indirect_alias_result =
        indirect_alias_analyzer.analyze_exact_entries({kLoad});
    bool indirect_alias_hosted = false;
    for (const auto& absorbed : indirect_alias_result.absorbed_entries) {
        indirect_alias_hosted |= (absorbed.addr == kLoad + 0x110 &&
                                  absorbed.host_start == kLoad + 0x100 &&
                                  absorbed.source_addr == kLoad + 0x108 &&
                                  absorbed.resolved_indirect);
    }
    CHECK(indirect_alias_hosted,
          "resolved indirect call target exports final alias provenance");

    // The ordering can span rounds too: B is found first, then B discovers an
    // earlier A whose reachable body contains B. Once A exists, ownership must
    // be recomputed without retaining B as a stale hard cap.
    auto later_round_image = make_exe_buffer(0x1000);
    put32(later_round_image, text + 0x00, jal(kLoad + 0x120));
    put32(later_round_image, text + 0x04, 0x00000000u);
    put32(later_round_image, text + 0x08, 0x03E00008u);
    put32(later_round_image, text + 0x0C, 0x00000000u);
    put32(later_round_image, text + 0x100, 0x10000007u); // b 0x80010120
    put32(later_round_image, text + 0x104, 0x00000000u);
    put32(later_round_image, text + 0x120, jal(kLoad + 0x100));
    put32(later_round_image, text + 0x124, 0x00000000u);
    put32(later_round_image, text + 0x128, 0x03E00008u);
    put32(later_round_image, text + 0x12C, 0x00000000u);
    auto later_round_exe = parse(later_round_image);
    PSXRecomp::FunctionAnalyzer later_round_analyzer(later_round_exe);
    const auto later_round_starts = starts(
        later_round_analyzer.analyze_exact_entries({kLoad}));
    CHECK(later_round_starts ==
              std::set<uint32_t>({kLoad, kLoad + 0x100}),
          "later-discovered host retracts its stale interior hard cap");

    // R discovers A and B; A initially owns B. Walking that expanded A then
    // discovers X between them. X caps A before B and does not itself reach B,
    // so final ownership must reconsider B and restore it as a real root.
    auto invalidated_owner_image = make_exe_buffer(0x1000);
    put32(invalidated_owner_image, text + 0x00, jal(kLoad + 0x100));
    put32(invalidated_owner_image, text + 0x04, 0x00000000u);
    put32(invalidated_owner_image, text + 0x08, jal(kLoad + 0x140));
    put32(invalidated_owner_image, text + 0x0C, 0x00000000u);
    put32(invalidated_owner_image, text + 0x10, 0x03E00008u);
    put32(invalidated_owner_image, text + 0x14, 0x00000000u);
    put32(invalidated_owner_image, text + 0x100,
          0x08000000u | (((kLoad + 0x130) >> 2) & 0x03FFFFFFu));
    put32(invalidated_owner_image, text + 0x104, 0x00000000u);
    put32(invalidated_owner_image, text + 0x120, 0x03E00008u); // X
    put32(invalidated_owner_image, text + 0x124, 0x00000000u);
    put32(invalidated_owner_image, text + 0x130, 0x24420001u);
    put32(invalidated_owner_image, text + 0x134, 0x24420001u);
    put32(invalidated_owner_image, text + 0x138, 0x24420001u);
    put32(invalidated_owner_image, text + 0x13C, 0x24420001u);
    put32(invalidated_owner_image, text + 0x140, jal(kLoad + 0x120)); // B -> X
    put32(invalidated_owner_image, text + 0x144, 0x00000000u);
    put32(invalidated_owner_image, text + 0x148, 0x03E00008u);
    put32(invalidated_owner_image, text + 0x14C, 0x00000000u);
    auto invalidated_owner_exe = parse(invalidated_owner_image);
    PSXRecomp::FunctionAnalyzer invalidated_owner_analyzer(
        invalidated_owner_exe);
    const auto invalidated_owner_result =
        invalidated_owner_analyzer.analyze_exact_entries({kLoad});
    const auto invalidated_owner_starts = starts(invalidated_owner_result);
    CHECK(invalidated_owner_starts.count(kLoad + 0x100) &&
          invalidated_owner_starts.count(kLoad + 0x120) &&
          invalidated_owner_starts.count(kLoad + 0x140),
          "R/A/X/B ownership invalidation restores B as a final root");
    bool stale_b_alias = false;
    for (const auto& absorbed : invalidated_owner_result.absorbed_entries)
        stale_b_alias |= absorbed.addr == kLoad + 0x140;
    CHECK(!stale_b_alias,
          "R/A/X/B final ownership does not retain stale B alias");

    // Producer boundaries are hard CFG ownership boundaries. A call into the
    // next producer is rejected without approval and, when approved, becomes
    // its own root rather than being absorbed by fallthrough from producer A.
    auto producer_owner_image = make_exe_buffer(0x1000);
    put32(producer_owner_image, text + 0x00, jal(kLoad + 0x100));
    put32(producer_owner_image, text + 0x04, 0x00000000u);
    put32(producer_owner_image, text + 0x08, jal(kLoad + 0x108));
    put32(producer_owner_image, text + 0x0C, 0x00000000u);
    put32(producer_owner_image, text + 0x10, 0x03E00008u);
    put32(producer_owner_image, text + 0x14, 0x00000000u);
    put32(producer_owner_image, text + 0x100, 0x24420001u);
    put32(producer_owner_image, text + 0x104, 0x24420001u);
    put32(producer_owner_image, text + 0x108, 0x03E00008u);
    put32(producer_owner_image, text + 0x10C, 0x00000000u);
    auto producer_owner_exe = parse(producer_owner_image);
    const std::vector<std::pair<uint32_t, uint32_t>> ownership_ranges = {
        {kLoad, kLoad + 0x108}, {kLoad + 0x108, kLoad + 0x1000}};
    PSXRecomp::FunctionAnalyzer rejected_owner_analyzer(producer_owner_exe);
    const auto rejected_owner = rejected_owner_analyzer.analyze_exact_entries(
        {kLoad}, ownership_ranges, {});
    CHECK(!starts(rejected_owner).count(kLoad + 0x108) &&
          rejected_owner.absorbed_entries.empty(),
          "rejected cross-producer call is neither root nor alias");
    PSXRecomp::FunctionAnalyzer allowed_owner_analyzer(producer_owner_exe);
    const auto allowed_owner = allowed_owner_analyzer.analyze_exact_entries(
        {kLoad}, ownership_ranges, {kLoad + 0x108});
    CHECK(starts(allowed_owner).count(kLoad + 0x108),
          "approved cross-producer call remains a separate root");
    bool cross_absorbed = false;
    for (const auto& absorbed : allowed_owner.absorbed_entries)
        cross_absorbed |= absorbed.addr == kLoad + 0x108;
    CHECK(!cross_absorbed,
          "approved cross-producer target is never absorbed across boundary");

    // An uncovered composite gap has no producer identity. Two gap addresses
    // both returning producer -1 must not become a synthetic shared producer.
    auto producer_gap_image = make_exe_buffer(0x1000);
    put32(producer_gap_image, text + 0x00, 0x03E00008u);
    put32(producer_gap_image, text + 0x04, 0x00000000u);
    put32(producer_gap_image, text + 0x60, 0x24020001u);
    put32(producer_gap_image, text + 0x64, 0x03E00008u);
    put32(producer_gap_image, text + 0x68, 0x00000000u);
    const auto producer_gap_exe = parse(producer_gap_image);
    PSXRecomp::FunctionAnalyzer producer_gap_analyzer(producer_gap_exe);
    const std::vector<std::pair<uint32_t, uint32_t>> gap_ranges = {
        {kLoad, kLoad + 0x40}, {kLoad + 0x80, kLoad + 0x100}};
    const auto producer_gap_result = producer_gap_analyzer.analyze_exact_entries(
        {kLoad, kLoad + 0x60}, gap_ranges, {});
    CHECK(starts(producer_gap_result) == std::set<uint32_t>({kLoad}) &&
          !producer_gap_result.exact_reachable_pcs.count(kLoad + 0x60),
          "explicit roots in uncovered producer gaps fail closed");

    // R reaches S, S calls B, and B calls earlier X. X then caps R before S,
    // invalidating B's only source edge. Recomputing evidence oscillates, so the
    // analyzer must fail closed to explicit R instead of retaining B forever.
    auto stale_source_image = make_exe_buffer(0x1000);
    put32(stale_source_image, text + 0x00,
          0x08000000u | (((kLoad + 0x100) >> 2) & 0x03FFFFFFu));
    put32(stale_source_image, text + 0x04, 0x00000000u);
    put32(stale_source_image, text + 0x80, 0x03E00008u); // X
    put32(stale_source_image, text + 0x84, 0x00000000u);
    put32(stale_source_image, text + 0x100, jal(kLoad + 0x140)); // S -> B
    put32(stale_source_image, text + 0x104, 0x00000000u);
    put32(stale_source_image, text + 0x108, 0x03E00008u);
    put32(stale_source_image, text + 0x10C, 0x00000000u);
    put32(stale_source_image, text + 0x140, jal(kLoad + 0x80)); // B -> X
    put32(stale_source_image, text + 0x144, 0x00000000u);
    put32(stale_source_image, text + 0x148, 0x03E00008u);
    put32(stale_source_image, text + 0x14C, 0x00000000u);
    auto stale_source_exe = parse(stale_source_image);
    PSXRecomp::FunctionAnalyzer stale_source_analyzer(stale_source_exe);
    const auto stale_source_result =
        stale_source_analyzer.analyze_exact_entries({kLoad});
    CHECK(starts(stale_source_result) == std::set<uint32_t>({kLoad}) &&
          stale_source_result.absorbed_entries.empty(),
          "cyclic stale call evidence fails closed to explicit roots");

    // beq rN,rN is unconditional: PC+8 is not reachable and a hidden JAL
    // there cannot manufacture a function or absorbed alias.
    auto always_branch_image = make_exe_buffer(0x1000);
    put32(always_branch_image, text + 0x00,
          (0x04u << 26) | (8u << 21) | (8u << 16) | 0x1Fu); // -> +0x80
    put32(always_branch_image, text + 0x04, 0x00000000u);
    put32(always_branch_image, text + 0x08, jal(kLoad + 0x40));
    put32(always_branch_image, text + 0x0C, 0x00000000u);
    put32(always_branch_image, text + 0x40, 0x03E00008u);
    put32(always_branch_image, text + 0x44, 0x00000000u);
    put32(always_branch_image, text + 0x80, 0x03E00008u);
    put32(always_branch_image, text + 0x84, 0x00000000u);
    auto always_branch_exe = parse(always_branch_image);
    PSXRecomp::FunctionAnalyzer always_branch_analyzer(always_branch_exe);
    const auto always_branch_result =
        always_branch_analyzer.analyze_exact_entries({kLoad});
    CHECK(starts(always_branch_result) == std::set<uint32_t>({kLoad}) &&
          always_branch_result.absorbed_entries.empty(),
          "always-taken branch excludes unreachable fallthrough call");

    // bne rN,rN is never taken: its encoded target cannot contribute a hidden
    // call edge, while its ordinary delay slot and PC+8 remain reachable.
    auto never_branch_image = make_exe_buffer(0x1000);
    put32(never_branch_image, text + 0x00,
          (0x05u << 26) | (8u << 21) | (8u << 16) | 0x1Fu); // false -> +0x80
    put32(never_branch_image, text + 0x04, 0x00000000u);
    put32(never_branch_image, text + 0x08, 0x03E00008u);
    put32(never_branch_image, text + 0x0C, 0x00000000u);
    put32(never_branch_image, text + 0x40, 0x03E00008u);
    put32(never_branch_image, text + 0x44, 0x00000000u);
    put32(never_branch_image, text + 0x80, jal(kLoad + 0x40));
    put32(never_branch_image, text + 0x84, 0x00000000u);
    put32(never_branch_image, text + 0x88, 0x03E00008u);
    put32(never_branch_image, text + 0x8C, 0x00000000u);
    auto never_branch_exe = parse(never_branch_image);
    PSXRecomp::FunctionAnalyzer never_branch_analyzer(never_branch_exe);
    const auto never_branch_result =
        never_branch_analyzer.analyze_exact_entries({kLoad});
    CHECK(starts(never_branch_result) == std::set<uint32_t>({kLoad}) &&
          never_branch_result.absorbed_entries.empty(),
          "never-taken branch excludes unreachable target call");

    // bgezal $zero is the BAL alias: the target is call evidence and the
    // return continuation at PC+8 remains reachable.
    auto bal_image = make_exe_buffer(0x1000);
    put32(bal_image, text + 0x00,
          (0x01u << 26) | (0x11u << 16) | 0x1Fu); // bal +0x80
    put32(bal_image, text + 0x04, 0x00000000u);
    put32(bal_image, text + 0x08, jal(kLoad + 0x40));
    put32(bal_image, text + 0x0C, 0x00000000u);
    put32(bal_image, text + 0x10, 0x03E00008u);
    put32(bal_image, text + 0x14, 0x00000000u);
    put32(bal_image, text + 0x40, 0x03E00008u);
    put32(bal_image, text + 0x44, 0x00000000u);
    put32(bal_image, text + 0x80, 0x03E00008u);
    put32(bal_image, text + 0x84, 0x00000000u);
    auto bal_exe = parse(bal_image);
    PSXRecomp::FunctionAnalyzer bal_analyzer(bal_exe);
    const auto bal_starts = starts(bal_analyzer.analyze_exact_entries({kLoad}));
    CHECK(bal_starts.count(kLoad + 0x40) && bal_starts.count(kLoad + 0x80),
          "BAL preserves call target and return-continuation discovery");

    // A synthetic adjacent-producer envelope is not one linked image. The
    // caller supplies the cross-boundary targets that passed its independent
    // callable-CFG proof; other valid-looking targets stay with the interpreter.
    PSXRecomp::FunctionAnalyzer composite_analyzer(exe);
    const std::vector<std::pair<uint32_t, uint32_t>> producer_ranges = {
        {kLoad, kLoad + 0x80}, {kLoad + 0x80, kLoad + 0x1000}};
    const auto composite = starts(composite_analyzer.analyze_exact_entries(
        {kLoad}, producer_ranges, {kLoad + 0x100}));
    CHECK(composite.count(kLoad) && composite.count(kLoad + 0x100),
          "approved cross-producer call target is discovered");
    CHECK(!composite.count(kLoad + 0x300),
          "unapproved cross-producer JAL target fails closed");

    // A constant-register JR out of the current function is a statically
    // proven tail call.  Resolve the common lui/addiu/jr sequence without an
    // execution-derived seed.
    auto indirect_image = make_exe_buffer(0x1000, kLoad + 0x500);
    put32(indirect_image, text + 0x400, 0x24020001u);
    put32(indirect_image, text + 0x404, 0x03E00008u);
    put32(indirect_image, text + 0x408, 0x00000000u);
    put32(indirect_image, text + 0x500, 0x3C088001u); // lui $t0,0x8001
    put32(indirect_image, text + 0x504, 0x25080400u); // addiu $t0,$t0,0x400
    put32(indirect_image, text + 0x508, 0x01000008u); // jr $t0
    put32(indirect_image, text + 0x50C, 0x00000000u);
    auto indirect_exe = parse(indirect_image);
    PSXRecomp::FunctionAnalyzer indirect_analyzer(indirect_exe);
    const auto indirect = starts(
        indirect_analyzer.analyze_exact_entries({kLoad + 0x500}));
    CHECK(indirect.count(kLoad + 0x400) && indirect.count(kLoad + 0x500),
          "constant-register JR discovers its frameless tail-call target");

    // A jump enters at the low-half definition and skips the preceding LUI.
    // A raw backward scan would combine them; reaching-definition proof must
    // reject that synthetic indirect target.
    auto split_constant_image = make_exe_buffer(0x1000, kLoad + 0x500);
    put32(split_constant_image, text + 0x400, 0x03E00008u);
    put32(split_constant_image, text + 0x404, 0x00000000u);
    put32(split_constant_image, text + 0x500,
          0x08000000u | (((kLoad + 0x518) >> 2) & 0x03FFFFFFu));
    put32(split_constant_image, text + 0x504, 0x00000000u);
    put32(split_constant_image, text + 0x514, 0x3C088001u);
    put32(split_constant_image, text + 0x518, 0x25080400u);
    put32(split_constant_image, text + 0x51C, 0x01000008u);
    put32(split_constant_image, text + 0x520, 0x00000000u);
    auto split_constant_exe = parse(split_constant_image);
    PSXRecomp::FunctionAnalyzer split_constant_analyzer(split_constant_exe);
    const auto split_constant = starts(
        split_constant_analyzer.analyze_exact_entries({kLoad + 0x500}));
    CHECK(!split_constant.count(kLoad + 0x400),
          "indirect resolver rejects a definition skipped by an inbound edge");

    // The LUI executes in a JAL delay slot, then the callee may clobber t0
    // before returning to the low half. It is not a reaching definition for
    // the later JALR even though the raw words are adjacent.
    auto call_delay_image = make_exe_buffer(0x1000, kLoad + 0x500);
    put32(call_delay_image, text + 0x300, 0x03E00008u);
    put32(call_delay_image, text + 0x304, 0x00000000u);
    put32(call_delay_image, text + 0x400, 0x03E00008u);
    put32(call_delay_image, text + 0x404, 0x00000000u);
    put32(call_delay_image, text + 0x500, jal(kLoad + 0x300));
    put32(call_delay_image, text + 0x504, 0x3C088001u); // call delay LUI
    put32(call_delay_image, text + 0x508, 0x25080400u);
    put32(call_delay_image, text + 0x50C, 0x0100F809u);
    put32(call_delay_image, text + 0x510, 0x00000000u);
    put32(call_delay_image, text + 0x514, 0x03E00008u);
    put32(call_delay_image, text + 0x518, 0x00000000u);
    auto call_delay_exe = parse(call_delay_image);
    PSXRecomp::FunctionAnalyzer call_delay_analyzer(call_delay_exe);
    const auto call_delay = starts(
        call_delay_analyzer.analyze_exact_entries({kLoad + 0x500}));
    CHECK(call_delay.count(kLoad + 0x300) &&
          !call_delay.count(kLoad + 0x400),
          "indirect resolver rejects a LUI in a call delay slot");

    // Ape Escape's save-screen dispatcher builds its table base across
    // registers (lui t0; addiu s0,t0,lo). Prove the complete bounded switch,
    // then make every adversarial mutation fail closed as one whole table.
    auto ape_switch = make_ape_switch();
    auto ape_switch_exe = parse(ape_switch.image);
    PSXRecomp::ExactJumpTable ape_table;
    CHECK(PSXRecomp::resolve_exact_bounded_jump_table(
              ape_switch_exe, ape_switch.entry, kLoad + 0x1000u,
              ape_switch.jr_pc, 2u, ape_table) &&
          ape_table.table_base == ape_switch.table &&
          ape_table.table_count == ape_switch.cases.size() &&
          ape_table.targets.size() == ape_switch.cases.size(),
          "Ape cross-register bounded switch resolves exactly");
    PSXRecomp::FunctionAnalyzer ape_switch_analyzer(ape_switch_exe);
    const auto ape_result = ape_switch_analyzer.analyze_exact_entries(
        {ape_switch.entry});
    bool all_ape_cases_reachable = true;
    for (uint32_t target : ape_switch.cases)
        all_ape_cases_reachable &= ape_result.exact_reachable_pcs.count(target) != 0;
    CHECK(all_ape_cases_reachable,
          "exact reachable discovery follows every validated Ape case");

    auto ori_switch = make_ape_switch();
    put32(ori_switch.image, text + 0x504, 0x35100A00u); // ori s0,t0,0x0a00
    CHECK(!resolves_ape_switch(ori_switch, ori_switch.entry),
          "unproven ORI table constants fail closed");

    auto no_lw_nop_switch = make_ape_switch();
    no_lw_nop_switch.jr_pc = kLoad + 0x524u;
    put32(no_lw_nop_switch.image, text + 0x524, 0x00400008u);
    put32(no_lw_nop_switch.image, text + 0x528, 0x00000000u);
    CHECK(resolves_ape_switch(no_lw_nop_switch, no_lw_nop_switch.entry),
          "zero-NOP LW-to-JR schedule matches Python parity");

    auto two_lw_nop_switch = make_ape_switch();
    two_lw_nop_switch.jr_pc = kLoad + 0x52Cu;
    put32(two_lw_nop_switch.image, text + 0x528, 0x00000000u);
    put32(two_lw_nop_switch.image, text + 0x52C, 0x00400008u);
    CHECK(!resolves_ape_switch(two_lw_nop_switch,
                               two_lw_nop_switch.entry),
          "two-NOP LW-to-JR schedule fails closed");

    auto wrong_jr_switch = make_ape_switch();
    put32(wrong_jr_switch.image, text + 0x528,
          0x00400808u); // reserved rd field makes this noncanonical JR
    CHECK(!resolves_ape_switch(wrong_jr_switch, wrong_jr_switch.entry),
          "jump-table proof checks the exact JR encoding");

    auto noncanonical_lui_switch = make_ape_switch();
    put32(noncanonical_lui_switch.image, text + 0x500,
          0x3C288001u); // LUI's reserved rs field is nonzero
    CHECK(!resolves_ape_switch(noncanonical_lui_switch,
                               noncanonical_lui_switch.entry),
          "jump-table proof rejects noncanonical LUI reserved fields");

    auto cross_producer_switch = make_ape_switch();
    CHECK(!resolves_ape_switch(cross_producer_switch,
                               cross_producer_switch.entry,
                               kLoad + 0x400u, kLoad + 0x900u),
          "producer A cannot borrow table slots from adjacent producer B");

    auto producer_case_escape = make_ape_switch();
    put32(producer_case_escape.image, text + 0x540,
          0x08000000u | (((kLoad + 0xB20u) >> 2) & 0x03FFFFFFu));
    put32(producer_case_escape.image, text + 0x544, 0x00000000u);
    put32(producer_case_escape.image, text + 0xB20,
          0x08000000u | (((kLoad + 0x518u) >> 2) & 0x03FFFFFFu));
    put32(producer_case_escape.image, text + 0xB24, 0x00000000u);
    CHECK(!resolves_ape_switch(producer_case_escape,
                               producer_case_escape.entry,
                               kLoad + 0x400u, kLoad + 0xA20u),
          "case walk cannot escape through an adjacent producer");
    auto cross_producer_exe = parse(cross_producer_switch.image);
    PSXRecomp::FunctionAnalyzer cross_producer_analyzer(cross_producer_exe);
    const auto cross_producer_result =
        cross_producer_analyzer.analyze_exact_entries(
            {cross_producer_switch.entry},
            {{kLoad + 0x400u, kLoad + 0x900u},
             {kLoad + 0x900u, kLoad + 0x1000u}});
    CHECK(!cross_producer_result.exact_reachable_pcs.count(kLoad + 0x540u),
          "reachable discovery rejects a cross-producer table");
    CHECK(!cross_producer_result.functions.empty() &&
          cross_producer_result.functions.front().producer_lo ==
              kLoad + 0x400u &&
          cross_producer_result.functions.front().producer_hi ==
              kLoad + 0x900u,
          "exact functions retain producer bounds for code generation");

    auto invalid_target_switch = make_ape_switch();
    put32(invalid_target_switch.image, text + 0x550, 0xFFFFFFFFu);
    CHECK(!resolves_ape_switch(invalid_target_switch,
                               invalid_target_switch.entry),
          "one invalid target instruction rejects the entire table");

    auto clobbered_switch = make_ape_switch();
    put32(clobbered_switch.image, text + 0x508,
          0x24100000u); // addiu s0,zero,0
    CHECK(!resolves_ape_switch(clobbered_switch, clobbered_switch.entry),
          "table-base clobber rejects the bounded switch");

    auto skipped_def_switch = make_ape_switch();
    put32(skipped_def_switch.image, text + 0x700,
          0x08000000u |
              (((kLoad + 0x504u) >> 2) & 0x03FFFFFFu));
    put32(skipped_def_switch.image, text + 0x704, 0x00000000u);
    CHECK(!resolves_ape_switch(skipped_def_switch,
                               skipped_def_switch.entry),
          "later host bytes cannot jump into and skip a protected definition");

    auto bad_bound_switch = make_ape_switch();
    put32(bad_bound_switch.image, text + 0x50C,
          0x2C820003u); // sltiu v0,a0,3: wrong index dependency
    CHECK(!resolves_ape_switch(bad_bound_switch, bad_bound_switch.entry),
          "unrelated SLTIU bound cannot validate a jump table");

    auto outside_target_switch = make_ape_switch();
    put32(outside_target_switch.image, text + 0xA04, kLoad + 0x2000u);
    CHECK(!resolves_ape_switch(outside_target_switch,
                               outside_target_switch.entry),
          "one out-of-image table target rejects the entire table");

    // A packed BIOS thunk is also a backward-scan boundary.  The frameless
    // leaf after its delay slot must not be swallowed into the thunk run.
    auto thunk_image = make_exe_buffer(0x1000);
    put32(thunk_image, text + 0x600, 0x240A00B0u);
    put32(thunk_image, text + 0x604, 0x01400008u);
    put32(thunk_image, text + 0x608, 0x24090008u);
    put32(thunk_image, text + 0x60C, 0x00000000u);
    put32(thunk_image, text + 0x610, 0x24020001u);
    put32(thunk_image, text + 0x614, 0x03E00008u);
    put32(thunk_image, text + 0x618, 0x00000000u);
    auto thunk_exe = parse(thunk_image);
    PSXRecomp::FunctionAnalyzer thunk_analyzer(thunk_exe);
    const auto thunk_starts = starts(thunk_analyzer.analyze());
    CHECK(thunk_starts.count(kLoad + 0x600) &&
          thunk_starts.count(kLoad + 0x610),
          "packed BIOS thunk bounds the following frameless leaf");

    PSXRecomp::FunctionAnalyzer seeded_analyzer(exe);
    const auto seeded = starts(
        seeded_analyzer.analyze_exact_entries({kLoad, kLoad + 0x200}));
    CHECK(seeded.count(kLoad + 0x200),
          "evidence-backed explicit callback seed is compiled");

    auto bad = parse(image);
    const auto original_size = bad.code_data.size();
    CHECK(!PSXRecomp::apply_static_analysis_bound(bad, 0x1800, error),
          "non-page-aligned truncation is rejected");
    CHECK(bad.code_size() == 0x2000 && bad.code_data.size() == original_size,
          "invalid bound leaves parsed image unchanged");
    CHECK(!PSXRecomp::apply_static_analysis_bound(bad, 0x3000, error),
          "oversized noncanonical bound is rejected");
    CHECK(!PSXRecomp::apply_static_analysis_bound(bad, 0, error),
          "zero bound is rejected");

    auto excludes_entry = parse(image);
    excludes_entry.header.initial_pc = kLoad + 0x1000;
    CHECK(!PSXRecomp::apply_static_analysis_bound(excludes_entry, 0x1000, error),
          "bound excluding entry point is rejected");

    auto rounded = parse(make_exe_buffer(0x1800));
    CHECK(PSXRecomp::apply_static_analysis_bound(rounded, 0x2000, error),
          "canonical generated final-page reservation remains compatible");
    CHECK(rounded.code_size() == 0x1800,
          "page reservation does not widen static analysis payload");

    if (failures) {
        std::fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("ALL PASS");
    return 0;
}
