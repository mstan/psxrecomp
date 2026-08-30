#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include "ps1_exe_parser.h"
#include "mips_decoder.h"

namespace PSXRecomp {

// A basic block is a maximal straight-line sequence of instructions with:
//   - A single entry point (the first instruction)
//   - A single exit point (the last instruction, which is a branch/jump or end-of-code)
//   - No branches/jumps except at the end
// MIPS delay slots: the instruction after a branch/jump is part of the same block.
struct BasicBlock {
    uint32_t start_addr;          // Address of first instruction
    uint32_t end_addr;            // Address of last instruction (inclusive)
    uint32_t instr_count;         // Total instructions including delay slot
    bool     has_delay_slot;      // Ends with branch/jump (delay slot follows)
    uint32_t delay_slot_addr;     // Address of delay slot (0 if none)

    // Control flow edges
    std::vector<uint32_t> successors;    // Addresses of possible next blocks
    std::vector<uint32_t> predecessors;  // Addresses of blocks that jump here

    // Convenience helpers
    bool contains(uint32_t addr) const {
        return addr >= start_addr && addr <= end_addr;
    }
};

class BasicBlockAnalyzer {
public:
    explicit BasicBlockAnalyzer(const PS1Executable& exe);

    // Find all basic blocks in the executable.
    // Returns blocks sorted by start_addr.
    std::vector<BasicBlock> analyze();

    // After analyze(), find the block containing addr.
    // Returns nullptr if not found.
    const BasicBlock* find_block(uint32_t addr) const;

    // Build predecessor lists for all blocks.
    // Call this after analyze() to populate block.predecessors.
    static void build_predecessors(std::vector<BasicBlock>& blocks);

private:
    const PS1Executable& exe_;

    // Decoded instruction cache (filled by analyze())
    std::vector<DecodedInstruction> decoded_;

    // Map from address → index in decoded_
    std::unordered_map<uint32_t, size_t> addr_to_idx_;

    // Resulting blocks (populated by analyze())
    std::vector<BasicBlock> blocks_;

    // Map from start_addr → block index (for find_block)
    std::unordered_map<uint32_t, size_t> addr_to_block_;

    // Collect all block leader addresses.
    // A leader is:
    //   1. The entry point of the executable
    //   2. Any branch/jump target
    //   3. Any instruction immediately after a branch/jump (+ delay slot)
    std::vector<uint32_t> find_leaders();

    // Build a single block starting at start_addr.
    BasicBlock build_block(uint32_t start_addr);
};

} // namespace PSXRecomp
