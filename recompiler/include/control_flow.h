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
    std::vector<uint32_t> predecessors; // Blocks that jump to this one

    // Block properties
    bool is_entry;              // Function entry point
    bool is_exit;               // Contains return instruction
    bool is_loop_header;        // Target of back edge (loop entry)
};

// Control flow graph for a function
struct ControlFlowGraph {
    uint32_t function_start;
    uint32_t function_end;
    std::map<uint32_t, BasicBlock> blocks; // Map: block start address -> block
    std::vector<uint32_t> block_order;     // Blocks in address order

    // Loop information
    std::vector<std::pair<uint32_t, uint32_t>> loops; // (header, back_edge_source)
    int loop_count;
};

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

    // Find all basic block boundaries in a function
    std::set<uint32_t> find_block_boundaries(const Function& func);

    // Build basic blocks from boundaries
    std::map<uint32_t, BasicBlock> build_basic_blocks(
        const Function& func,
        const std::set<uint32_t>& boundaries);

    // Link basic blocks (build CFG edges)
    void link_basic_blocks(std::map<uint32_t, BasicBlock>& blocks);

    // Detect loops (back edges in CFG)
    std::vector<std::pair<uint32_t, uint32_t>> detect_loops(
        const std::map<uint32_t, BasicBlock>& blocks);
};

} // namespace PSXRecomp
