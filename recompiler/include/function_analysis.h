#pragma once

#include <cstdint>
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
};

struct FunctionAnalysisResult {
    std::vector<Function> functions;
    int total_instructions;
    int jr_ra_count;
    int prologue_count;
    int call_discovered_count = 0; // Functions found via JAL call-target following
};

class FunctionAnalyzer {
public:
    explicit FunctionAnalyzer(const PS1Executable& exe);

    // Scan entire executable for function boundaries
    FunctionAnalysisResult analyze();

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

private:
    const PS1Executable& exe_;

    // Forced entry points (from add_forced_entry())
    std::vector<uint32_t> forced_entry_points_;

    // Find function start by scanning backward from jr $ra
    uint32_t find_function_start(uint32_t return_addr);

    // Detect if a region is likely a data section masquerading as code
    bool is_likely_data_section(uint32_t start_addr, uint32_t end_addr) const;
};

} // namespace PSXRecomp
