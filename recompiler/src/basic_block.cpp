#include "basic_block.h"
#include <algorithm>

namespace PSXRecomp {

BasicBlockAnalyzer::BasicBlockAnalyzer(const PS1Executable& exe)
    : exe_(exe)
{
}

// ---------------------------------------------------------------------------
// find_leaders: collect all basic block entry point addresses
// ---------------------------------------------------------------------------
std::vector<uint32_t> BasicBlockAnalyzer::find_leaders() {
    std::vector<uint32_t> leaders;

    // Leader 1: entry point of the executable
    leaders.push_back(exe_.entry_point());

    for (size_t i = 0; i < decoded_.size(); ++i) {
        const auto& instr = decoded_[i];

        if (instr.is_branch || instr.is_jump) {
            // The instruction after the delay slot is a leader (fall-through target).
            // Delay slot is at i+1, so i+2 is the next block's start.
            uint32_t fall_through = instr.address + 8; // skip instr + delay slot
            if (fall_through < exe_.end_address()) {
                leaders.push_back(fall_through);
            }

            // The branch/jump target is also a leader.
            if (instr.is_branch && instr.branch_target != 0 &&
                instr.branch_target >= exe_.load_address() &&
                instr.branch_target < exe_.end_address()) {
                leaders.push_back(instr.branch_target);
            }
            if (instr.is_jump && instr.jump_target != 0 &&
                instr.jump_target >= exe_.load_address() &&
                instr.jump_target < exe_.end_address()) {
                leaders.push_back(instr.jump_target);
            }
        }
    }

    // Sort and deduplicate
    std::sort(leaders.begin(), leaders.end());
    leaders.erase(std::unique(leaders.begin(), leaders.end()), leaders.end());
    return leaders;
}

// ---------------------------------------------------------------------------
// build_block: construct a BasicBlock starting at start_addr
// ---------------------------------------------------------------------------
BasicBlock BasicBlockAnalyzer::build_block(uint32_t start_addr) {
    BasicBlock block{};
    block.start_addr    = start_addr;
    block.end_addr      = start_addr;
    block.instr_count   = 0;
    block.has_delay_slot = false;
    block.delay_slot_addr = 0;

    auto it = addr_to_idx_.find(start_addr);
    if (it == addr_to_idx_.end()) {
        return block; // Address not in executable range
    }

    size_t idx = it->second;
    bool in_delay_slot = false;

    for (size_t i = idx; i < decoded_.size(); ++i) {
        const auto& instr = decoded_[i];
        block.end_addr = instr.address;
        block.instr_count++;

        if (in_delay_slot) {
            // This instruction is the delay slot — block ends after it
            block.delay_slot_addr = instr.address;
            break;
        }

        if (instr.is_delay_slot_user) {
            // Next instruction is the delay slot
            block.has_delay_slot = true;
            in_delay_slot = true;
            continue;
        }

        // Non-delay-slot end conditions:
        // 1. jr $ra without delay slot consideration (already handled above)
        // 2. Current instruction is followed by a leader in another block
        // Check if the NEXT instruction is a known leader (i.e., new block starts there)
        if (i + 1 < decoded_.size()) {
            uint32_t next_addr = decoded_[i + 1].address;
            // If next_addr is a leader (different block), end here
            if (addr_to_block_.count(next_addr) || next_addr == start_addr) {
                // We'll check this via leader set in analyze()
                // For now: end block if next address is in our block list
                // (This pass is before block map is built, so we check leaders differently)
            }
        }

        // If no successor info yet, fall through to next instruction
    }

    // Compute successors based on terminator instruction
    if (block.instr_count > 0) {
        const auto* term_it = addr_to_idx_.count(block.end_addr)
            ? &decoded_[addr_to_idx_.at(block.end_addr)]
            : nullptr;

        // Walk back to the branch/jump instruction (before delay slot)
        const DecodedInstruction* branch_instr = nullptr;
        if (block.has_delay_slot && block.instr_count >= 2) {
            uint32_t branch_addr = block.end_addr - 4; // instruction before delay slot
            auto bit = addr_to_idx_.find(branch_addr);
            if (bit != addr_to_idx_.end()) {
                branch_instr = &decoded_[bit->second];
            }
        } else if (term_it && (term_it->is_branch || term_it->is_jump)) {
            branch_instr = term_it;
        }

        if (branch_instr) {
            uint32_t fall_through = block.end_addr + 4; // after delay slot

            if (branch_instr->is_branch) {
                // Conditional: fall-through + taken target
                if (fall_through < exe_.end_address())
                    block.successors.push_back(fall_through);
                if (branch_instr->branch_target >= exe_.load_address() &&
                    branch_instr->branch_target < exe_.end_address())
                    block.successors.push_back(branch_instr->branch_target);
            } else if (branch_instr->is_jump) {
                if (branch_instr->is_jr_ra) {
                    // Function return — no successor within this function
                } else if (branch_instr->opcode == 0x03) {
                    // JAL — control returns here; fall-through is successor
                    if (fall_through < exe_.end_address())
                        block.successors.push_back(fall_through);
                } else if (branch_instr->jump_target >= exe_.load_address() &&
                           branch_instr->jump_target < exe_.end_address()) {
                    block.successors.push_back(branch_instr->jump_target);
                }
            }
        } else if (term_it) {
            // Straight-line end: fall through to next instruction
            uint32_t fall_through = block.end_addr + 4;
            if (fall_through < exe_.end_address())
                block.successors.push_back(fall_through);
        }
    }

    return block;
}

// ---------------------------------------------------------------------------
// analyze: main entry point
// ---------------------------------------------------------------------------
std::vector<BasicBlock> BasicBlockAnalyzer::analyze() {
    blocks_.clear();
    addr_to_block_.clear();
    decoded_.clear();
    addr_to_idx_.clear();

    // Step 1: Decode all instructions
    decoded_ = MipsDecoder::decode_executable(exe_);
    addr_to_idx_.reserve(decoded_.size());
    for (size_t i = 0; i < decoded_.size(); ++i) {
        addr_to_idx_[decoded_[i].address] = i;
    }

    // Step 2: Find all leader addresses
    auto leaders = find_leaders();

    // Step 3: Build blocks — for each leader, scan forward until the next leader
    //         We need to know where blocks end, so use leaders as boundaries.
    // Reserve to avoid reallocation while pointers into decoded_ are live.
    blocks_.reserve(leaders.size());
    for (size_t li = 0; li < leaders.size(); ++li) {
        uint32_t start = leaders[li];
        uint32_t next_leader = (li + 1 < leaders.size())
            ? leaders[li + 1]
            : exe_.end_address();

        // Only create a block if start_addr is in the decoded range
        if (addr_to_idx_.count(start) == 0) continue;

        BasicBlock block{};
        block.start_addr  = start;
        block.end_addr    = start;
        block.instr_count = 0;
        block.has_delay_slot = false;
        block.delay_slot_addr = 0;

        size_t idx = addr_to_idx_.at(start);
        bool   in_delay_slot = false;
        bool   block_ended   = false;

        for (size_t i = idx; i < decoded_.size() && !block_ended; ++i) {
            const auto& instr = decoded_[i];

            // Stop if we've reached the next leader (and we're not in a delay slot)
            if (!in_delay_slot && instr.address >= next_leader && instr.address != start) {
                break;
            }

            block.end_addr = instr.address;
            block.instr_count++;

            if (in_delay_slot) {
                block.delay_slot_addr = instr.address;
                block_ended = true;
                break;
            }

            if (instr.is_delay_slot_user) {
                block.has_delay_slot = true;
                in_delay_slot = true;
                // Continue loop to pick up the delay slot instruction
                continue;
            }

            // Check if next instruction is a leader → block ends here
            if (i + 1 < decoded_.size()) {
                uint32_t next_addr = decoded_[i + 1].address;
                if (next_addr >= next_leader) {
                    block_ended = true;
                }
            } else {
                block_ended = true; // end of executable
            }
        }

        // Compute successors
        // Find the branch/jump instruction (before delay slot if present)
        const DecodedInstruction* branch_instr = nullptr;
        if (block.has_delay_slot && block.instr_count >= 2) {
            uint32_t branch_addr = block.delay_slot_addr - 4;
            auto bit = addr_to_idx_.find(branch_addr);
            if (bit != addr_to_idx_.end()) {
                branch_instr = &decoded_[bit->second];
            }
        } else {
            auto eit = addr_to_idx_.find(block.end_addr);
            if (eit != addr_to_idx_.end()) {
                const auto& end_instr = decoded_[eit->second];
                if (end_instr.is_branch || end_instr.is_jump) {
                    branch_instr = &end_instr;
                }
            }
        }

        if (branch_instr) {
            // Instruction after the delay slot (or after the branch if no delay slot)
            uint32_t after_branch = block.has_delay_slot
                ? block.delay_slot_addr + 4
                : block.end_addr + 4;

            if (branch_instr->is_branch) {
                // Conditional: two successors
                if (after_branch < exe_.end_address())
                    block.successors.push_back(after_branch);
                if (branch_instr->branch_target >= exe_.load_address() &&
                    branch_instr->branch_target < exe_.end_address())
                    block.successors.push_back(branch_instr->branch_target);
            } else { // jump
                if (branch_instr->is_jr_ra) {
                    // Return: no intra-function successors
                } else if (branch_instr->opcode == 0x03) { // JAL
                    // JAL: fall-through after delay slot
                    if (after_branch < exe_.end_address())
                        block.successors.push_back(after_branch);
                } else if (branch_instr->jump_target >= exe_.load_address() &&
                           branch_instr->jump_target < exe_.end_address()) {
                    block.successors.push_back(branch_instr->jump_target);
                } else if (branch_instr->opcode == 0x08 || branch_instr->opcode == 0x09) {
                    // JR/JALR to register — dynamic target, can't statically determine
                }
            }
        } else {
            // Straight-line: fall through
            uint32_t fall_through = block.end_addr + 4;
            if (fall_through < exe_.end_address() && fall_through < next_leader) {
                // Only add if it's in range (typically the next block)
            }
            if (fall_through < exe_.end_address())
                block.successors.push_back(fall_through);
        }

        // Deduplicate successors
        std::sort(block.successors.begin(), block.successors.end());
        block.successors.erase(
            std::unique(block.successors.begin(), block.successors.end()),
            block.successors.end());

        size_t block_idx = blocks_.size();
        addr_to_block_[block.start_addr] = block_idx;
        blocks_.push_back(std::move(block));
    }

    // Step 4: Build predecessor lists
    build_predecessors(blocks_);

    return blocks_;
}

// ---------------------------------------------------------------------------
// build_predecessors: reverse the successor edges
// ---------------------------------------------------------------------------
void BasicBlockAnalyzer::build_predecessors(std::vector<BasicBlock>& blocks) {
    // Build addr → block index map
    std::unordered_map<uint32_t, size_t> addr_map;
    for (size_t i = 0; i < blocks.size(); ++i) {
        addr_map[blocks[i].start_addr] = i;
    }

    // For each block, add this block's start as a predecessor of each successor
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (uint32_t succ_addr : blocks[i].successors) {
            auto it = addr_map.find(succ_addr);
            if (it != addr_map.end()) {
                blocks[it->second].predecessors.push_back(blocks[i].start_addr);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// find_block: look up a block containing addr
// ---------------------------------------------------------------------------
const BasicBlock* BasicBlockAnalyzer::find_block(uint32_t addr) const {
    // First check if addr is an exact start address
    auto it = addr_to_block_.find(addr);
    if (it != addr_to_block_.end()) {
        return &blocks_[it->second];
    }

    // Linear scan for containment (slow but correct for occasional lookups)
    for (const auto& block : blocks_) {
        if (block.contains(addr)) {
            return &block;
        }
    }
    return nullptr;
}

} // namespace PSXRecomp
