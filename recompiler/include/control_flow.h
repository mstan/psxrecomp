#pragma once

#include <cstdint>
#include <vector>
#include <set>
#include <map>
#include <string>
#include "ps1_exe_parser.h"
#include "function_analysis.h"

namespace PSXRecomp {

// Types of control flow instructions
enum class ControlFlowType {
    None,           // Not a control flow instruction
    Branch,         // Conditional branch (beq, bne, blez, bgtz, bltz, bgez, etc.)
    Jump,           // Unconditional jump (j)
    JumpLink,       // Function call (jal)
    JumpRegister,   // Register jump (jr)
    JumpLinkReg,    // Register call (jalr)
    Return          // Function return (jr $ra)
};

// Information about a control flow instruction
struct ControlFlowInstr {
    uint32_t address;           // Address of the instruction
    uint32_t instruction;       // Raw instruction word
    ControlFlowType type;       // Type of control flow
    uint32_t target;            // Target address (0 if unknown/register-based)
    bool has_delay_slot;        // All MIPS branches/jumps have delay slot
    bool is_likely;             // Branch likely variant (delay slot conditional)
    std::string mnemonic;       // Instruction mnemonic (for debugging)
};

// A basic block: sequence of instructions with single entry and exit
struct BasicBlock {
    uint32_t start_addr;        // First instruction address
    uint32_t end_addr;          // Last instruction address (inclusive)
    int instruction_count;      // Number of instructions in block

    // Control flow information
    ControlFlowInstr exit_instr; // How this block exits (branch/jump/return)
    std::vector<uint32_t> successors; // Next basic blocks (targets + fall-through)
    std::vector<uint32_t> predecessors; // Unique in-graph sources, including unreachable ones

    // Block properties
    bool is_entry;              // Function entry point
    bool is_exit;               // Contains return instruction
    bool is_loop_header;        // Header dominates a reachable back-edge source
    bool is_reachable = false;  // Reachable through known edges from a declared entry
};

// Control flow graph for a function
struct ControlFlowGraph {
    uint32_t function_start;
    uint32_t function_end;
    uint32_t producer_lo = 0;
    uint32_t producer_hi = 0;
    std::map<uint32_t, BasicBlock> blocks; // Map: block start address -> block
    std::vector<uint32_t> block_order;     // Blocks in address order

    // Natural back edges in the KNOWN static graph, not backward addresses.
    // Missing indirect edges / runtime CPS entries mean this is NOT proof that
    // a block is private or that IRQ, I-cache or device checks may be removed.
    std::vector<std::pair<uint32_t, uint32_t>> loops; // (dominating header, source)
    int loop_count = 0; // Number of back edges, not distinct/nested loop bodies
};

// Rebuild reverse edges, static reachability and dominance-proven back edges.
// function_start, is_entry blocks and extra_entries are independent roots.
// Does not change block ownership, instruction ranges or successor ordering.
void rebuild_control_flow_metadata(
    ControlFlowGraph& cfg, const std::vector<uint32_t>& extra_entries = {});

class ControlFlowAnalyzer {
public:
    explicit ControlFlowAnalyzer(const PS1Executable& exe);

    // Analyze control flow for a single function
    ControlFlowGraph analyze_function(const Function& func);

    // Analyze control flow for all functions
    std::map<uint32_t, ControlFlowGraph> analyze_all_functions(
        const std::vector<Function>& functions);

    // Identify control flow instruction type and target
    static ControlFlowInstr analyze_instruction(uint32_t addr, uint32_t instr);

    // Check if instruction is a branch/jump
    static bool is_control_flow(uint32_t instr);

    // Get branch target address
    static uint32_t get_branch_target(uint32_t pc, uint32_t instr);

    // Get jump target address
    static uint32_t get_jump_target(uint32_t pc, uint32_t instr);

private:
    const PS1Executable& exe_;

    // One past the last address that DISCOVERY and BLOCK CONSTRUCTION may
    // reach for `func`: the function's own end, clamped to the image's
    // analysis bound (PS1Executable::analysis_end_address). The clamp keeps a
    // trailing delay-slot guard word — readable on purpose, but with no word
    // after it — from ever becoming a block leader or a block's exit
    // instruction. See the exe_tag comment in ps1_exe_parser.h.
    uint32_t analysis_walk_hi(const Function& func) const;

    // Find all basic block boundaries in a function
    std::set<uint32_t> find_block_boundaries(const Function& func);

    // Build basic blocks from boundaries
    std::map<uint32_t, BasicBlock> build_basic_blocks(
        const Function& func,
        const std::set<uint32_t>& boundaries);

    // Link basic blocks (build CFG edges)
    void link_basic_blocks(std::map<uint32_t, BasicBlock>& blocks);

};

} // namespace PSXRecomp
