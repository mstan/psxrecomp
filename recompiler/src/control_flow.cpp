#include "control_flow.h"
#include <algorithm>
#include <fmt/format.h>

namespace PSXRecomp {

ControlFlowAnalyzer::ControlFlowAnalyzer(const PS1Executable& exe) : exe_(exe) {}

bool ControlFlowAnalyzer::is_control_flow(uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3F;

    // SPECIAL opcode (0x00) - check funct for jr/jalr
    if (opcode == 0x00) {
        uint32_t funct = instr & 0x3F;
        return (funct == 0x08 || funct == 0x09); // jr or jalr
    }

    // REGIMM opcode (0x01) - bltz, bgez, bltzal, bgezal
    if (opcode == 0x01) {
        return true;
    }

    // Branch instructions
    if (opcode >= 0x04 && opcode <= 0x07) {
        return true; // beq, bne, blez, bgtz
    }

    // Branch likely variants (0x14-0x17)
    if (opcode >= 0x14 && opcode <= 0x17) {
        return true; // beql, bnel, blezl, bgtzl
    }

    // Jump instructions
    if (opcode == 0x02 || opcode == 0x03) {
        return true; // j, jal
    }

    return false;
}

uint32_t ControlFlowAnalyzer::get_branch_target(uint32_t pc, uint32_t instr) {
    // Branch target = (PC + 4) + (sign_extend(offset) << 2)
    int16_t offset = (int16_t)(instr & 0xFFFF);
    int32_t byte_offset = ((int32_t)offset) << 2;
    return (pc + 4) + byte_offset;
}

uint32_t ControlFlowAnalyzer::get_jump_target(uint32_t pc, uint32_t instr) {
    // Jump target = (PC+4)[31:28] | (instr_index << 2)
    uint32_t instr_index = instr & 0x03FFFFFF;
    uint32_t target = (instr_index << 2);
    uint32_t upper_bits = (pc + 4) & 0xF0000000;
    return upper_bits | target;
}

ControlFlowInstr ControlFlowAnalyzer::analyze_instruction(uint32_t addr, uint32_t instr) {
    ControlFlowInstr cf;
    cf.address = addr;
    cf.instruction = instr;
    cf.type = ControlFlowType::None;
    cf.target = 0;
    cf.has_delay_slot = false;
    cf.is_likely = false;

    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    uint32_t funct = instr & 0x3F;

    // SPECIAL opcode - jr/jalr
    if (opcode == 0x00) {
        if (funct == 0x08) { // jr
            cf.type = (rs == 31) ? ControlFlowType::Return : ControlFlowType::JumpRegister;
            cf.has_delay_slot = true;
            cf.mnemonic = "jr";
            return cf;
        } else if (funct == 0x09) { // jalr
            cf.type = ControlFlowType::JumpLinkReg;
            cf.has_delay_slot = true;
            cf.mnemonic = "jalr";
            return cf;
        }
    }

    // REGIMM opcode - bltz, bgez, bltzal, bgezal
    if (opcode == 0x01) {
        cf.type = ControlFlowType::Branch;
        cf.target = get_branch_target(addr, instr);
        cf.has_delay_slot = true;
        cf.mnemonic = (rt == 0x00) ? "bltz" :
                      (rt == 0x01) ? "bgez" :
                      (rt == 0x10) ? "bltzal" : "bgezal";
        return cf;
    }

    // Branch instructions
    switch (opcode) {
        case 0x04: // beq
            cf.type = ControlFlowType::Branch;
            cf.target = get_branch_target(addr, instr);
            cf.has_delay_slot = true;
            cf.mnemonic = "beq";
            break;

        case 0x05: // bne
            cf.type = ControlFlowType::Branch;
            cf.target = get_branch_target(addr, instr);
            cf.has_delay_slot = true;
            cf.mnemonic = "bne";
            break;

        case 0x06: // blez
            cf.type = ControlFlowType::Branch;
            cf.target = get_branch_target(addr, instr);
            cf.has_delay_slot = true;
            cf.mnemonic = "blez";
            break;

        case 0x07: // bgtz
            cf.type = ControlFlowType::Branch;
            cf.target = get_branch_target(addr, instr);
            cf.has_delay_slot = true;
            cf.mnemonic = "bgtz";
            break;

        // Branch likely variants
        case 0x14: // beql
            cf.type = ControlFlowType::Branch;
            cf.target = get_branch_target(addr, instr);
            cf.has_delay_slot = true;
            cf.is_likely = true;
            cf.mnemonic = "beql";
            break;

        case 0x15: // bnel
            cf.type = ControlFlowType::Branch;
            cf.target = get_branch_target(addr, instr);
            cf.has_delay_slot = true;
            cf.is_likely = true;
            cf.mnemonic = "bnel";
            break;

        case 0x16: // blezl
            cf.type = ControlFlowType::Branch;
            cf.target = get_branch_target(addr, instr);
            cf.has_delay_slot = true;
            cf.is_likely = true;
            cf.mnemonic = "blezl";
            break;

        case 0x17: // bgtzl
            cf.type = ControlFlowType::Branch;
            cf.target = get_branch_target(addr, instr);
            cf.has_delay_slot = true;
            cf.is_likely = true;
            cf.mnemonic = "bgtzl";
            break;

        // Jump instructions
        case 0x02: // j
            cf.type = ControlFlowType::Jump;
            cf.target = get_jump_target(addr, instr);
            cf.has_delay_slot = true;
            cf.mnemonic = "j";
            break;

        case 0x03: // jal
            cf.type = ControlFlowType::JumpLink;
            cf.target = get_jump_target(addr, instr);
            cf.has_delay_slot = true;
            cf.mnemonic = "jal";
            break;
    }

    return cf;
}

std::set<uint32_t> ControlFlowAnalyzer::find_block_boundaries(const Function& func) {
    std::set<uint32_t> boundaries;

    // Function start is always a boundary
    boundaries.insert(func.start_addr);

    // Scan all instructions in function
    uint32_t addr = func.start_addr;
    while (addr < func.end_addr) {
        auto instr_opt = exe_.read_word(addr);
        if (!instr_opt.has_value()) {
            break;
        }

        uint32_t instr = *instr_opt;

        if (is_control_flow(instr)) {
            ControlFlowInstr cf = analyze_instruction(addr, instr);

            // If branch/jump has a target, target is a boundary
            if (cf.target != 0 && cf.target >= func.start_addr && cf.target < func.end_addr) {
                boundaries.insert(cf.target);
            }

            // Instruction after delay slot is a boundary (for branches, not jumps)
            if (cf.type == ControlFlowType::Branch ||
                cf.type == ControlFlowType::JumpLink ||
                cf.type == ControlFlowType::JumpLinkReg) {
                uint32_t fall_through = addr + 8; // After delay slot
                if (fall_through < func.end_addr) {
                    boundaries.insert(fall_through);
                }
            }

            // For unconditional jump/return, next instruction is unreachable
            // (unless it's a jump target from elsewhere)
            if (cf.type == ControlFlowType::Jump ||
                cf.type == ControlFlowType::JumpRegister ||
                cf.type == ControlFlowType::Return) {
                uint32_t after_delay = addr + 8;
                if (after_delay < func.end_addr) {
                    boundaries.insert(after_delay);
                }
            }
        }

        addr += 4;
    }

    return boundaries;
}

std::map<uint32_t, BasicBlock> ControlFlowAnalyzer::build_basic_blocks(
    const Function& func,
    const std::set<uint32_t>& boundaries) {

    std::map<uint32_t, BasicBlock> blocks;
    std::vector<uint32_t> boundary_vec(boundaries.begin(), boundaries.end());
    std::sort(boundary_vec.begin(), boundary_vec.end());

    for (size_t i = 0; i < boundary_vec.size(); i++) {
        BasicBlock block;
        block.start_addr = boundary_vec[i];

        // End address is the next boundary (or function end)
        if (i + 1 < boundary_vec.size()) {
            block.end_addr = boundary_vec[i + 1] - 4;
        } else {
            block.end_addr = func.end_addr - 4;
        }

        block.instruction_count = (block.end_addr - block.start_addr) / 4 + 1;
        block.is_entry = (block.start_addr == func.start_addr);
        block.is_exit = false;
        block.is_loop_header = false;

        // Analyze exit instruction (last instruction in block)
        auto exit_instr_opt = exe_.read_word(block.end_addr);
        if (exit_instr_opt.has_value()) {
            block.exit_instr = analyze_instruction(block.end_addr, *exit_instr_opt);

            // If last instruction is not control flow, check if it's a delay slot
            // (i.e., the instruction before it is a branch/jump with delay slot)
            if (block.exit_instr.type == ControlFlowType::None &&
                block.end_addr > block.start_addr) {
                auto branch_instr_opt = exe_.read_word(block.end_addr - 4);
                if (branch_instr_opt.has_value()) {
                    ControlFlowInstr branch_cf = analyze_instruction(block.end_addr - 4, *branch_instr_opt);
                    if (branch_cf.type != ControlFlowType::None && branch_cf.has_delay_slot) {
                        block.exit_instr = branch_cf;
                    }
                }
            }

            block.is_exit = (block.exit_instr.type == ControlFlowType::Return);
        } else {
            block.exit_instr.type = ControlFlowType::None;
        }

        blocks[block.start_addr] = block;
    }

    return blocks;
}

void ControlFlowAnalyzer::link_basic_blocks(std::map<uint32_t, BasicBlock>& blocks) {
    for (auto& [addr, block] : blocks) {
        ControlFlowInstr& cf = block.exit_instr;

        // Add successors based on control flow type
        if (cf.type == ControlFlowType::Branch) {
            // Branch has two successors: target and fall-through
            if (cf.target != 0 && blocks.count(cf.target)) {
                block.successors.push_back(cf.target);
                blocks[cf.target].predecessors.push_back(addr);
            }

            // Fall-through (after delay slot)
            uint32_t fall_through = cf.address + 8;
            if (blocks.count(fall_through)) {
                block.successors.push_back(fall_through);
                blocks[fall_through].predecessors.push_back(addr);
            }
        } else if (cf.type == ControlFlowType::Jump) {
            // Unconditional jump - only target
            if (cf.target != 0 && blocks.count(cf.target)) {
                block.successors.push_back(cf.target);
                blocks[cf.target].predecessors.push_back(addr);
            }
        } else if (cf.type == ControlFlowType::JumpLink || cf.type == ControlFlowType::JumpLinkReg) {
            // Function call - continues after delay slot
            uint32_t fall_through = cf.address + 8;
            if (blocks.count(fall_through)) {
                block.successors.push_back(fall_through);
                blocks[fall_through].predecessors.push_back(addr);
            }
        } else if (cf.type == ControlFlowType::JumpRegister && cf.type != ControlFlowType::Return) {
            // Register jump (not return) - unknown target, no successors
        } else if (cf.type == ControlFlowType::Return) {
            // Return - no successors
        } else {
            // No control flow - fall through to next block
            uint32_t next_block = block.end_addr + 4;
            if (blocks.count(next_block)) {
                block.successors.push_back(next_block);
                blocks[next_block].predecessors.push_back(addr);
            }
        }
    }
}

std::vector<std::pair<uint32_t, uint32_t>> ControlFlowAnalyzer::detect_loops(
    const std::map<uint32_t, BasicBlock>& blocks) {

    std::vector<std::pair<uint32_t, uint32_t>> loops;

    for (const auto& [addr, block] : blocks) {
        // Check each successor
        for (uint32_t successor : block.successors) {
            // Back edge: successor address < current block address
            if (successor <= addr) {
                loops.push_back({successor, addr}); // (header, back_edge_source)

                // Mark loop header
                if (blocks.count(successor)) {
                    const_cast<BasicBlock&>(blocks.at(successor)).is_loop_header = true;
                }
            }
        }
    }

    return loops;
}

ControlFlowGraph ControlFlowAnalyzer::analyze_function(const Function& func) {
    ControlFlowGraph cfg;
    cfg.function_start = func.start_addr;
    cfg.function_end = func.end_addr;

    // Step 1: Find basic block boundaries
    std::set<uint32_t> boundaries = find_block_boundaries(func);

    // Step 2: Build basic blocks
    cfg.blocks = build_basic_blocks(func, boundaries);

    // Step 3: Link blocks (build CFG edges)
    link_basic_blocks(cfg.blocks);

    // Step 4: Detect loops
    cfg.loops = detect_loops(cfg.blocks);
    cfg.loop_count = static_cast<int>(cfg.loops.size());

    // Step 5: Create block order list
    for (const auto& [addr, block] : cfg.blocks) {
        cfg.block_order.push_back(addr);
    }
    std::sort(cfg.block_order.begin(), cfg.block_order.end());

    return cfg;
}

std::map<uint32_t, ControlFlowGraph> ControlFlowAnalyzer::analyze_all_functions(
    const std::vector<Function>& functions) {

    std::map<uint32_t, ControlFlowGraph> all_cfgs;

    fmt::print("\n=== Control Flow Analysis ===\n\n");
    fmt::print("Analyzing {} functions...\n", functions.size());

    int total_blocks = 0;
    int total_loops = 0;

    for (const Function& func : functions) {
        ControlFlowGraph cfg = analyze_function(func);
        total_blocks += static_cast<int>(cfg.blocks.size());
        total_loops += cfg.loop_count;
        all_cfgs[func.start_addr] = cfg;
    }

    fmt::print("✓ Created {} basic blocks\n", total_blocks);
    fmt::print("✓ Detected {} loops\n", total_loops);
    fmt::print("✓ Average {:.1f} blocks per function\n\n",
               static_cast<double>(total_blocks) / functions.size());

    return all_cfgs;
}

} // namespace PSXRecomp
