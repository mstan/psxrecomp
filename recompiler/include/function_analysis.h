#pragma once

#include <cstdint>
#include <set>
#include <utility>
#include <vector>
#include <string>
#include "ps1_exe_parser.h"

namespace PSXRecomp {

struct Function {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t size;
    bool has_prologue;
    bool has_epilogue;
    int32_t stack_frame_size; // Size of stack frame (if has prologue)
    std::string name; // Optional name (for now, just "func_<addr>")
    bool is_data_section = false; // True if this "function" is actually a data section
    // Overlapping-alias entry: nonzero means start_addr is an INTERIOR address of
    // the host function beginning at alias_walk_lo. The CFG covers the host's
    // full range [alias_walk_lo, end_addr) and the emitted C function enters via
    // a goto to start_addr's block. The host function is emitted unchanged —
    // aliases never cap or truncate it (the mid-function-seed hazard class).
    uint32_t alias_walk_lo = 0;
    // All alias entries sharing this host (including start_addr). Injected as
    // block leaders so every sibling alias gets an identical CFG, letting the
    // emitter generate ONE shared body per host with an entry switch instead
    // of duplicating the host's blocks per alias.
    std::vector<uint32_t> alias_group_entries;
    // Exact-entry composites retain the source producer that owns this code.
    // Zero means an ordinary single-image function.
    uint32_t producer_lo = 0;
    uint32_t producer_hi = 0;
};

struct AbsorbedEntry {
    uint32_t addr;
    uint32_t host_start;
    uint32_t host_end;
    uint32_t source_addr;
    bool resolved_indirect;
};

struct FunctionAnalysisResult {
    std::vector<Function> functions;
    int total_instructions;
    int jr_ra_count;
    int prologue_count;
    int call_discovered_count = 0; // Functions found via JAL call-target following
    int strong_prologue_count = 0; // Functions found from prologues with saved $ra
    int bios_thunk_count = 0; // Packed A0/B0/C0 BIOS dispatch thunks
    int state_continuation_count = 0; // Split entries after calls to SaveState-style helpers
    int pointer_table_entry_count = 0; // Function entries found from executable pointer tables
    // Statically proven direct/constant-register transfer targets that the
    // FINAL exact-entry partition reaches inside another function. These are
    // safe overlapping aliases: the source edge and host reachability were
    // both observed by the analyzer (not inferred from a raw byte envelope).
    std::vector<AbsorbedEntry> absorbed_entries;
    // Instruction PCs reached by the FINAL exact-entry partition. Consumers
    // use this to reject hostless `interior` seeds that merely fall inside a
    // function's coarse [start,end) envelope or an unreachable data hole.
    std::set<uint32_t> exact_reachable_pcs;
};

// A canonical, bounds-checked MIPS jump table recovered from a jr $rN site.
// `table_base` and each pair's first value are the runtime addresses stored by
// the guest; the pair's second value is the corresponding address in the
// active executable image (normally identical, except for BIOS shell remaps).
struct ExactJumpTable {
    uint32_t table_base = 0;
    uint32_t table_count = 0;
    std::vector<std::pair<uint32_t, uint32_t>> targets;
};

using ExactAddressMapper = uint32_t (*)(uint32_t, const PS1Executable&);

// Recognize only the canonical bounded-switch dependency chain:
// sltiu/beq guard -> sll index,2 -> addu table address -> lw target -> jr.
// The table constant may use either same-register or cross-register
// `lui source; addiu base,source,lo`. `producer_lo/producer_hi` bound both
// table storage and case code for composite images; zero/zero means the full
// executable. Every dependency, table word, and target must pass the hard
// safety checks; otherwise the whole table is rejected.
bool resolve_exact_bounded_jump_table(
    const PS1Executable& exe,
    uint32_t entry,
    uint32_t hard_cap,
    uint32_t jr_pc,
    uint32_t jr_rs,
    ExactJumpTable& table,
    ExactAddressMapper runtime_to_image = nullptr,
    uint32_t producer_lo = 0,
    uint32_t producer_hi = 0);

class FunctionAnalyzer {
public:
    explicit FunctionAnalyzer(const PS1Executable& exe);

    // Scan entire executable for function boundaries
    FunctionAnalysisResult analyze();

    // Analyze only explicit entry points and callable direct-JAL targets
    // reachable from them. Used by runtime-loaded overlays and opt-in main-EXE
    // reachable discovery. Unresolved jalr/indirect targets do not mint
    // functions; evidence-backed entries must be supplied explicitly.
    FunctionAnalysisResult analyze_exact_entries(
        const std::vector<uint32_t>& entries,
        const std::vector<std::pair<uint32_t, uint32_t>>& producer_ranges = {},
        const std::set<uint32_t>& cross_call_allow = {});

    // Add a forced entry point address that is treated as a function start
    // even if it has no standard ADDIU $sp prologue. The function will be
    // included in the analysis result with has_prologue = false.
    void add_forced_entry(uint32_t addr);

    // Check if instruction is jr $ra (return)
    static bool is_jr_ra(uint32_t instr);

    // Check if instruction is function prologue (addiu $sp, $sp, -N)
    static bool is_prologue(uint32_t instr, int32_t& stack_size);

    // Check if instruction is epilogue (addiu $sp, $sp, +N)
    static bool is_epilogue(uint32_t instr, int32_t& stack_size);

    // Check if instruction is a branch or jump (has a delay slot)
    static bool is_branch_or_jump(uint32_t instr);

    // Check if a raw word decodes as a plausible R3000 instruction. Used to
    // validate scanned data pointers before promoting/aliasing them.
    static bool is_valid_mips_word(uint32_t instr);

private:
    const PS1Executable& exe_;

    // Forced entry points (from add_forced_entry())
    std::vector<uint32_t> forced_entry_points_;

    // Find function start by scanning backward from jr $ra
    uint32_t find_function_start(uint32_t return_addr);

    // Detect PSY-Q style BIOS dispatch thunks:
    //   addiu/ori rN, $zero, {0xA0,0xB0,0xC0}
    //   jr        rN
    //   addiu/ori $t1, $zero, function_index
    bool is_bios_dispatch_thunk(uint32_t addr, uint32_t& jr_addr_out) const;

    // Detect if a region is likely a data section masquerading as code
    bool is_likely_data_section(uint32_t start_addr, uint32_t end_addr) const;
};

} // namespace PSXRecomp
