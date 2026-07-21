#include "function_analysis.h"
#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <fmt/format.h>

namespace PSXRecomp {

FunctionAnalyzer::FunctionAnalyzer(const PS1Executable& exe) : exe_(exe) {}

void FunctionAnalyzer::add_forced_entry(uint32_t addr) {
    // Validate address is within the EXE range
    if (addr >= exe_.header.load_address && addr < exe_.end_address()) {
        forced_entry_points_.push_back(addr);
    }
}

bool FunctionAnalyzer::is_jr_ra(uint32_t instr) {
    // jr $ra: opcode=0, rs=31 ($ra), rt=0, rd=0, shamt=0, funct=8 (jr)
    // Format: 000000 11111 00000 00000 00000 001000
    // Hex: 0x03E00008
    return instr == 0x03E00008;
}

bool FunctionAnalyzer::is_prologue(uint32_t instr, int32_t& stack_size) {
    // addiu $sp, $sp, -N
    // Format: 001001 11101 11101 <16-bit signed immediate>
    // Opcode: 0x27 (addiu), rs=$sp (29), rt=$sp (29)
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    int16_t imm = (int16_t)(instr & 0xFFFF);

    if (opcode == 0x09 && rs == 29 && rt == 29 && imm < 0) {
        stack_size = -imm; // Store positive stack frame size
        return true;
    }
    return false;
}

bool FunctionAnalyzer::is_epilogue(uint32_t instr, int32_t& stack_size) {
    // addiu $sp, $sp, +N
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    int16_t imm = (int16_t)(instr & 0xFFFF);

    if (opcode == 0x09 && rs == 29 && rt == 29 && imm > 0) {
        stack_size = imm;
        return true;
    }
    return false;
}

bool FunctionAnalyzer::is_valid_mips_word(uint32_t instr) {
    if (instr == 0xFFFFFFFFu || instr == 0xFFFFFFFDu) return false;

    uint32_t opcode = (instr >> 26) & 0x3Fu;
    uint32_t funct = instr & 0x3Fu;
    uint32_t rt = (instr >> 16) & 0x1Fu;

    if (opcode == 0x00u) {
        switch (funct) {
        case 0x00u: case 0x02u: case 0x03u: case 0x04u:
        case 0x06u: case 0x07u: case 0x08u: case 0x09u:
        case 0x0Cu: case 0x0Du:
        case 0x10u: case 0x11u: case 0x12u: case 0x13u:
        case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu:
        case 0x20u: case 0x21u: case 0x22u: case 0x23u:
        case 0x24u: case 0x25u: case 0x26u: case 0x27u:
        case 0x2Au: case 0x2Bu:
            return true;
        default:
            return false;
        }
    }
    if (opcode == 0x01u) {
        return rt == 0x00u || rt == 0x01u || rt == 0x10u || rt == 0x11u;
    }

    switch (opcode) {
    case 0x02u: case 0x03u: case 0x04u: case 0x05u:
    case 0x06u: case 0x07u:
    case 0x08u: case 0x09u: case 0x0Au: case 0x0Bu:
    case 0x0Cu: case 0x0Du: case 0x0Eu: case 0x0Fu:
    case 0x10u: case 0x12u:
    case 0x20u: case 0x21u: case 0x22u: case 0x23u:
    case 0x24u: case 0x25u: case 0x26u:
    case 0x28u: case 0x29u: case 0x2Au: case 0x2Bu:
    case 0x2Eu:
    case 0x30u: case 0x32u: case 0x38u: case 0x3Au:
        return true;
    default:
        return false;
    }
}

bool FunctionAnalyzer::is_branch_or_jump(uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    // J, JAL
    if (opcode == 0x02 || opcode == 0x03) return true;
    // BEQ, BNE, BLEZ, BGTZ
    if (opcode >= 0x04 && opcode <= 0x07) return true;
    // REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL
    if (opcode == 0x01) return true;
    // SPECIAL: JR, JALR
    if (opcode == 0x00) {
        uint32_t funct = instr & 0x3F;
        if (funct == 0x08 || funct == 0x09) return true;
    }
    // COP1/COP2 branches (BC1F, BC1T, BC2F, BC2T) — opcode 0x11 or 0x12, rs=0x08
    if ((opcode == 0x11 || opcode == 0x12) && ((instr >> 21) & 0x1F) == 0x08) return true;
    return false;
}

static bool is_load_imm_zero_u16(uint32_t instr, uint32_t& rt_out, uint32_t& imm_out) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    if ((opcode != 0x09 && opcode != 0x0D) || rs != 0) {
        return false;
    }
    rt_out = (instr >> 16) & 0x1F;
    imm_out = instr & 0xFFFFu;
    return true;
}

bool FunctionAnalyzer::is_bios_dispatch_thunk(uint32_t addr, uint32_t& jr_addr_out) const {
    auto w0 = exe_.read_word(addr);
    auto w1 = exe_.read_word(addr + 4);
    auto w2 = exe_.read_word(addr + 8);
    if (!w0.has_value() || !w1.has_value() || !w2.has_value()) {
        return false;
    }

    uint32_t target_reg = 0;
    uint32_t vector = 0;
    if (!is_load_imm_zero_u16(*w0, target_reg, vector)) {
        return false;
    }
    if (vector != 0xA0u && vector != 0xB0u && vector != 0xC0u) {
        return false;
    }

    uint32_t op1 = (*w1 >> 26) & 0x3F;
    uint32_t rs1 = (*w1 >> 21) & 0x1F;
    uint32_t fn1 = *w1 & 0x3F;
    if (op1 != 0 || fn1 != 0x08u || rs1 != target_reg) {
        return false;
    }

    uint32_t index_reg = 0;
    uint32_t index = 0;
    if (!is_load_imm_zero_u16(*w2, index_reg, index) || index_reg != 9) {
        return false;
    }

    jr_addr_out = addr + 4;
    return true;
}

static bool is_sw_reg_base(uint32_t instr, uint32_t base_reg, uint32_t value_reg) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    return opcode == 0x2B && rs == base_reg && rt == value_reg;
}

static bool is_sw_reg_sp(uint32_t instr, uint32_t value_reg) {
    return is_sw_reg_base(instr, 29, value_reg);
}

static bool is_lui(uint32_t instr, uint32_t& rt_out) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    if (opcode != 0x0F) return false;
    rt_out = (instr >> 16) & 0x1F;
    return true;
}

static bool is_nop(uint32_t instr) {
    return instr == 0;
}

static uint32_t skip_leading_padding_nops(const PS1Executable& exe,
                                          uint32_t start_addr,
                                          uint32_t limit_addr) {
    constexpr uint32_t max_padding_bytes = 32;
    uint32_t addr = start_addr;
    uint32_t skipped = 0;

    while (addr + 4u <= limit_addr && skipped < max_padding_bytes) {
        auto word_opt = exe.read_word(addr);
        if (!word_opt.has_value() || !is_nop(*word_opt)) {
            break;
        }
        addr += 4u;
        skipped += 4u;
    }

    return addr;
}

static bool is_load_from_reg_base(uint32_t instr, uint32_t base_reg, uint32_t& rt_out) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    switch (opcode) {
        case 0x20: /* lb */
        case 0x21: /* lh */
        case 0x22: /* lwl */
        case 0x23: /* lw */
        case 0x24: /* lbu */
        case 0x25: /* lhu */
        case 0x26: /* lwr */
            break;
        default:
            return false;
    }
    if (rs != base_reg) return false;
    rt_out = rt;
    return true;
}

static bool is_load_imm_zero(uint32_t instr, uint32_t& rt_out) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    if ((opcode != 0x09 && opcode != 0x0D) || rs != 0) return false;
    rt_out = (instr >> 16) & 0x1F;
    return rt_out != 0;
}

static bool preprologue_window_is_valid(const PS1Executable& exe,
                                        uint32_t start_addr,
                                        uint32_t prologue_addr,
                                        uint32_t exe_start) {
    if (start_addr >= prologue_addr) return false;

    /* The candidate may start immediately after a previous return delay slot,
     * but it cannot itself be a branch delay slot. */
    if (start_addr >= exe_start + 4) {
        auto prev_opt = exe.read_word(start_addr - 4);
        if (prev_opt.has_value() && FunctionAnalyzer::is_branch_or_jump(*prev_opt)) {
            return false;
        }
    }

    std::set<uint32_t> global_base_regs;
    bool saw_global_load = false;

    for (uint32_t addr = start_addr; addr < prologue_addr; addr += 4) {
        auto word_opt = exe.read_word(addr);
        if (!word_opt.has_value()) return false;
        uint32_t instr = *word_opt;

        if (FunctionAnalyzer::is_branch_or_jump(instr)) return false;

        uint32_t rt = 0;
        if (is_lui(instr, rt) && rt != 0 && rt != 29 && rt != 31) {
            global_base_regs.insert(rt);
            continue;
        }

        if (is_load_imm_zero(instr, rt) && rt != 29 && rt != 31) {
            continue;
        }

        bool valid_load = false;
        for (uint32_t base_reg : global_base_regs) {
            uint32_t load_rt = 0;
            if (is_load_from_reg_base(instr, base_reg, load_rt) &&
                load_rt != 0 && load_rt != 29 && load_rt != 31) {
                valid_load = true;
                saw_global_load = true;
                break;
            }
        }
        if (valid_load) continue;

        return false;
    }

    return saw_global_load;
}

static bool find_preprologue_setup_start(const PS1Executable& exe,
                                         uint32_t prologue_addr,
                                         uint32_t exe_start,
                                         uint32_t& setup_start_out) {
    constexpr uint32_t max_setup_insns = 6;
    bool found = false;
    uint32_t best_start = prologue_addr;

    for (uint32_t count = 1; count <= max_setup_insns; count++) {
        uint32_t bytes = count * 4u;
        if (prologue_addr < exe_start + bytes) break;
        uint32_t candidate = prologue_addr - bytes;
        if (preprologue_window_is_valid(exe, candidate, prologue_addr, exe_start)) {
            best_start = candidate;
            found = true;
        }
    }

    if (!found) return false;
    setup_start_out = best_start;
    return true;
}

static bool pointer_target_has_near_prologue(const PS1Executable& exe,
                                             uint32_t target,
                                             uint32_t exe_start,
                                             uint32_t exe_end,
                                             uint32_t& prologue_addr_out) {
    constexpr uint32_t max_prelude_bytes = 32;
    for (uint32_t addr = target;
         addr < exe_end && addr <= target + max_prelude_bytes;
         addr += 4) {
        auto word_opt = exe.read_word(addr);
        if (!word_opt.has_value()) break;

        int32_t stack_size = 0;
        if (!FunctionAnalyzer::is_prologue(*word_opt, stack_size)) continue;

        if (addr == target) {
            prologue_addr_out = addr;
            return true;
        }

        uint32_t setup_start = 0;
        if (find_preprologue_setup_start(exe, addr, exe_start, setup_start) &&
            setup_start == target) {
            prologue_addr_out = addr;
            return true;
        }
    }
    return false;
}

uint32_t FunctionAnalyzer::find_function_start(uint32_t return_addr) {
    // Scan backward from jr $ra to find function start
    // Heuristic: Look for prologue or function alignment (16-byte boundary after prev function)

    uint32_t search_addr = return_addr;
    const uint32_t max_search = 4096; // Search up to 4096 instructions backward (16 KB)

    for (uint32_t i = 0; i < max_search; i++) {
        search_addr -= 4;

        if (search_addr < exe_.header.load_address) {
            // Reached beginning of code
            return exe_.header.load_address;
        }

        auto word_opt = exe_.read_word(search_addr);
        if (!word_opt.has_value()) {
            return return_addr; // Can't read, assume current position
        }

        uint32_t instr = *word_opt;
        int32_t stack_size;

        // Check if this is a prologue
        if (is_prologue(instr, stack_size)) {
            // Verify this isn't a delay slot of a branch/jump instruction.
            // MIPS compilers often place stack allocation in the delay slot of
            // the function's first conditional branch, e.g.:
            //   beq v0, zero, skip
            //   addiu sp, sp, -N   <- delay slot, looks like prologue but isn't a function start
            // If the preceding instruction is a branch/jump, skip this candidate.
            if (search_addr >= exe_.header.load_address + 4) {
                auto prev_opt = exe_.read_word(search_addr - 4);
                if (prev_opt.has_value() && is_branch_or_jump(*prev_opt)) {
                    continue;  // delay slot, not a real prologue — keep scanning
                }
            }
            {
                uint32_t setup_start = 0;
                if (find_preprologue_setup_start(exe_, search_addr,
                                                 exe_.header.load_address,
                                                 setup_start)) {
                    return setup_start;
                }
            }
            return search_addr;
        }

        // Check if we hit another function's return
        if (is_jr_ra(instr)) {
            // We've gone too far backward and hit another function
            // Return the address after this jr $ra (+ 8 for delay slot), then
            // skip alignment padding. Explicit JAL/forced entries still keep
            // their exact target addresses; this only normalizes starts inferred
            // from the previous function's trailing gap.
            return skip_leading_padding_nops(exe_, search_addr + 8, return_addr);
        }

        // Packed Psy-Q BIOS thunks tail-jump through A0/B0/C0 and therefore do
        // not contain `jr $ra`.  Treat their JR delay slot as a real boundary
        // while scanning backward, otherwise the following frameless routine
        // is swallowed into the whole preceding thunk run.
        if (search_addr >= exe_.header.load_address + 4u) {
            uint32_t thunk_jr = 0;
            if (is_bios_dispatch_thunk(search_addr - 4u, thunk_jr) &&
                thunk_jr == search_addr) {
                return skip_leading_padding_nops(exe_, search_addr + 8u,
                                                 return_addr);
            }
        }
    }

    // Couldn't find clear start, assume max search distance
    return return_addr - (max_search * 4);
}

bool FunctionAnalyzer::is_likely_data_section(uint32_t start_addr, uint32_t end_addr) const {
    uint32_t size = end_addr - start_addr;
    if (size < 100) return false;  // Minimum check size

    uint32_t total_words = size / 4;
    uint32_t invalid_jal_count = 0;
    uint32_t undefined_opcode_count = 0;

    // Valid PS1 (MIPS R3000) primary opcodes
    static const bool valid_opcode[64] = {
        true,  true,  true,  true,  true,  true,  true,  true,   // 0x00-0x07
        true,  true,  true,  true,  true,  true,  true,  true,   // 0x08-0x0F
        true,  false, true,  false, false, false, false, false,   // 0x10-0x17 (COP0=0x10, COP2=0x12)
        false, false, false, false, false, false, false, false,   // 0x18-0x1F
        true,  true,  true,  true,  true,  true,  true,  false,  // 0x20-0x27
        true,  true,  true,  true,  false, false, true,  false,  // 0x28-0x2F
        true,  false, true,  false, false, false, false, false,  // 0x30-0x37 (LWC0=0x30, LWC2=0x32)
        true,  false, true,  false, false, false, false, false,  // 0x38-0x3F (SWC0=0x38, SWC2=0x3A)
    };

    for (uint32_t addr = start_addr; addr < end_addr; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;

        uint32_t opcode = (instr >> 26) & 0x3F;

        // Check for JAL with invalid target
        if (opcode == 3) {  // JAL opcode
            // PS1 JAL target: upper 4 bits from PC region (0x80000000), low 28 bits from instr
            uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;
            if (target > 0x801FFFFFu) {
                invalid_jal_count++;
            }
        }

        // Check for undefined opcode
        if (!valid_opcode[opcode]) {
            undefined_opcode_count++;
        }
    }

    // Size-graduated JAL threshold: higher ratio needed for smaller functions
    uint32_t jal_ratio_x100 = (total_words > 0) ? (invalid_jal_count * 100u / total_words) : 0u;
    if (size >= 10000 && jal_ratio_x100 > 5u)  return true;  // Large: >5% invalid JALs
    if (size >= 1000  && jal_ratio_x100 > 30u) return true;  // Medium: >30% invalid JALs
    if (size >= 100   && jal_ratio_x100 > 60u) return true;  // Small: >60% invalid JALs

    // Undefined opcode check:
    // Real PS1 code consistently uses 0% undefined opcodes (calibrated on Tomba!).
    // Data sections masquerading as functions have ~9-27% undefined opcodes.
    // A safe threshold of 7% catches all observed data sections with no false positives.
    uint32_t undef_ratio_x100 = (total_words > 0) ? (undefined_opcode_count * 100u / total_words) : 0u;
    if (size >= 1000 && undef_ratio_x100 > 7u) return true;   // Large: >7% undefined opcodes
    if (size >= 400  && undef_ratio_x100 > 50u) return true;  // Small: >50% undefined opcodes (conservative)

    return false;
}

namespace {

enum class ExactCfKind {
    Normal,
    Branch,
    BranchNever,
    BranchNeverLikely,
    Jump,
    Jal,
    JrRa,
    JrOther,
    Jalr,
};

struct ExactCf {
    ExactCfKind kind = ExactCfKind::Normal;
    uint32_t target = 0;
};

struct ExactWalkResult {
    std::set<uint32_t> visited;
    std::set<uint32_t> direct_jal_targets;
    // target -> reachable (source PC, was resolved jalr/jr) evidence
    std::map<uint32_t, std::set<std::pair<uint32_t, bool>>> transfer_sources;
    std::set<uint32_t> jump_table_targets;
    uint32_t jr_ra_count = 0;
};

static ExactCf exact_classify_cf(uint32_t pc, uint32_t instr);

static bool exact_is_jr_ra_word(uint32_t instr) {
    return instr == 0x03E00008u;
}

static bool exact_is_addiu_sp_neg(uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3Fu;
    uint32_t rs = (instr >> 21) & 0x1Fu;
    uint32_t rt = (instr >> 16) & 0x1Fu;
    int16_t imm = static_cast<int16_t>(instr & 0xFFFFu);
    return opcode == 0x09u && rs == 29u && rt == 29u && imm < 0;
}

static bool exact_is_valid_mips_word(uint32_t instr) {
    return FunctionAnalyzer::is_valid_mips_word(instr);
}

static uint32_t exact_branch_target(uint32_t pc, uint32_t instr) {
    int16_t imm = static_cast<int16_t>(instr & 0xFFFFu);
    return pc + 4u + (static_cast<int32_t>(imm) << 2);
}

static uint32_t exact_jump_target(uint32_t pc, uint32_t instr) {
    return ((pc + 4u) & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2);
}

static bool exact_instruction_writes_gpr(uint32_t instr, uint32_t reg) {
    if (reg == 0u) return false;

    uint32_t opcode = (instr >> 26) & 0x3Fu;
    uint32_t rs = (instr >> 21) & 0x1Fu;
    uint32_t rt = (instr >> 16) & 0x1Fu;
    uint32_t rd = (instr >> 11) & 0x1Fu;
    uint32_t funct = instr & 0x3Fu;

    if (opcode == 0x00u) {
        // SPECIAL instructions with a GPR destination use rd.  JR, stores to
        // HI/LO, multiply/divide, syscall, and break have no GPR result.
        switch (funct) {
        case 0x08u: case 0x0Cu: case 0x0Du:
        case 0x11u: case 0x13u:
        case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu:
            return false;
        default:
            return rd == reg;
        }
    }
    if (opcode == 0x03u) return reg == 31u; // JAL
    if (opcode == 0x01u && (rt == 0x10u || rt == 0x11u)) return reg == 31u;

    // Immediate ALU ops, LUI, and loads write rt.  Branches and stores do not.
    if ((opcode >= 0x08u && opcode <= 0x0Fu) ||
        (opcode >= 0x20u && opcode <= 0x26u) ||
        opcode == 0x30u || opcode == 0x32u) {
        return rt == reg;
    }
    // MFC/CFC from COP0/COP2 write rt; MTC/CTC do not.
    if ((opcode == 0x10u || opcode == 0x12u) && (rs == 0u || rs == 2u)) {
        return rt == reg;
    }
    return false;
}

// Resolve the common absolute indirect-transfer idiom within one basic block:
//
//   lui    rN, hi(target)
//   addiu/ori rN, rN, lo(target)
//   jr/jalr rN
//
// The backward scan stops at control flow or any intervening write to rN, so a
// stale constant from another path cannot be mistaken for the transfer target.
static bool exact_resolve_constant_transfer(const PS1Executable& exe,
                                            uint32_t entry,
                                            uint32_t transfer_pc,
                                            uint32_t target_reg,
                                            uint32_t& target_out) {
    bool have_low = false;
    bool low_is_addiu = false;
    uint16_t low = 0;

    for (uint32_t count = 1; count <= 16u; count++) {
        uint32_t bytes = count * 4u;
        if (transfer_pc < entry + bytes) break;
        uint32_t pc = transfer_pc - bytes;
        auto word_opt = exe.read_word(pc);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;
        uint32_t opcode = (instr >> 26) & 0x3Fu;
        uint32_t rs = (instr >> 21) & 0x1Fu;
        uint32_t rt = (instr >> 16) & 0x1Fu;

        if (!have_low && (opcode == 0x09u || opcode == 0x0Du) &&
            rs == target_reg && rt == target_reg) {
            have_low = true;
            low_is_addiu = opcode == 0x09u;
            low = static_cast<uint16_t>(instr & 0xFFFFu);
            continue;
        }
        if (opcode == 0x0Fu && rt == target_reg) {
            // A definition in a control-transfer delay slot is not a local
            // reaching definition for the linear suffix.  A call can clobber
            // the register before returning at pc+4; a branch/jump can enter a
            // different path entirely.  Without path/interprocedural proof,
            // reject every such candidate rather than combine raw words.
            if (pc >= entry + 4u) {
                auto predecessor = exe.read_word(pc - 4u);
                if (predecessor.has_value()) {
                    uint32_t predecessor_op = (*predecessor >> 26) & 0x3Fu;
                    if (FunctionAnalyzer::is_branch_or_jump(*predecessor) ||
                        (predecessor_op >= 0x14u && predecessor_op <= 0x17u)) {
                        return false;
                    }
                }
            }

            // A linear backward scan is only a reaching-definition proof when
            // no direct control-flow edge can enter the suffix after this LUI.
            // For example, `j low; lui rN,hi; low: addiu rN,rN,lo; jalr rN`
            // skips the LUI at runtime.  Without this boundary check the raw
            // words still look like a constant pair and manufacture a target.
            bool crossed_inbound_boundary = false;
            for (uint32_t source = entry; source < transfer_pc; source += 4u) {
                auto source_word = exe.read_word(source);
                if (!source_word.has_value()) continue;
                ExactCf source_cf = exact_classify_cf(source, *source_word);
                bool has_direct_target =
                    source_cf.kind == ExactCfKind::Branch ||
                    source_cf.kind == ExactCfKind::Jump ||
                    source_cf.kind == ExactCfKind::Jal;
                if (has_direct_target && source_cf.target > pc &&
                    source_cf.target <= transfer_pc) {
                    crossed_inbound_boundary = true;
                    break;
                }
            }
            if (crossed_inbound_boundary) return false;

            uint32_t upper = (instr & 0xFFFFu) << 16;
            if (!have_low) {
                target_out = upper;
            } else if (low_is_addiu) {
                target_out = upper + static_cast<uint32_t>(
                    static_cast<int32_t>(static_cast<int16_t>(low)));
            } else {
                target_out = upper | low;
            }
            return true;
        }
        if (exact_instruction_writes_gpr(instr, target_reg)) break;
        if (FunctionAnalyzer::is_branch_or_jump(instr)) break;
    }
    return false;
}

static ExactCf exact_classify_cf(uint32_t pc, uint32_t instr) {
    ExactCf cf;
    uint32_t opcode = (instr >> 26) & 0x3Fu;
    uint32_t funct = instr & 0x3Fu;
    uint32_t rs = (instr >> 21) & 0x1Fu;

    if (opcode == 0x00u && funct == 0x08u) {
        cf.kind = (rs == 31u) ? ExactCfKind::JrRa : ExactCfKind::JrOther;
        return cf;
    }
    if (opcode == 0x00u && funct == 0x09u) {
        cf.kind = ExactCfKind::Jalr;
        return cf;
    }
    if (opcode == 0x02u) {
        cf.kind = ExactCfKind::Jump;
        cf.target = exact_jump_target(pc, instr);
        return cf;
    }
    if (opcode == 0x03u) {
        cf.kind = ExactCfKind::Jal;
        cf.target = exact_jump_target(pc, instr);
        return cf;
    }
    uint32_t rt = (instr >> 16) & 0x1Fu;

    // `beq rN,rN,target` is an unconditional branch. Treating its
    // syntactic fallthrough as reachable lets raw data after the delay slot
    // manufacture call/ownership evidence.
    if ((opcode == 0x04u || opcode == 0x14u) && rs == rt) {
        cf.kind = ExactCfKind::Jump;
        cf.target = exact_branch_target(pc, instr);
        return cf;
    }
    // Conversely, `bne rN,rN,target` can never take its encoded target.  Keep
    // its delay slot and PC+8 path, but never walk target-shaped data.
    if (opcode == 0x05u && rs == rt) {
        cf.kind = ExactCfKind::BranchNever;
        return cf;
    }
    // Branch-likely annuls its delay slot when the condition is false.
    if (opcode == 0x15u && rs == rt) {
        cf.kind = ExactCfKind::BranchNeverLikely;
        return cf;
    }
    // Conditions against the architectural zero register are also exact.
    if (opcode == 0x06u && rs == 0u) { // blez $zero
        cf.kind = ExactCfKind::Jump;
        cf.target = exact_branch_target(pc, instr);
        return cf;
    }
    if (opcode == 0x07u && rs == 0u) { // bgtz $zero
        cf.kind = ExactCfKind::BranchNever;
        return cf;
    }
    if (opcode == 0x16u && rs == 0u) { // blezl $zero
        cf.kind = ExactCfKind::Jump;
        cf.target = exact_branch_target(pc, instr);
        return cf;
    }
    if (opcode == 0x17u && rs == 0u) { // bgtzl $zero
        cf.kind = ExactCfKind::BranchNeverLikely;
        return cf;
    }
    if (opcode == 0x01u && rs == 0u) {
        // BLTZ/BLTZAL (and likely forms) are false for zero; BGEZ forms are
        // true.  The link-true forms are BAL/BALL aliases, so model their
        // target and return continuation like a call rather than a tail jump.
        if (rt == 0x00u || rt == 0x10u) {
            cf.kind = ExactCfKind::BranchNever;
            return cf;
        }
        if (rt == 0x02u || rt == 0x12u) {
            cf.kind = ExactCfKind::BranchNeverLikely;
            return cf;
        }
        if (rt == 0x01u || rt == 0x03u) {
            cf.kind = ExactCfKind::Jump;
            cf.target = exact_branch_target(pc, instr);
            return cf;
        }
        if (rt == 0x11u || rt == 0x13u) { // bal / ball aliases
            cf.kind = ExactCfKind::Jal;
            cf.target = exact_branch_target(pc, instr);
            return cf;
        }
    }
    if (opcode == 0x01u || (opcode >= 0x04u && opcode <= 0x07u) ||
        (opcode >= 0x14u && opcode <= 0x17u)) {
        cf.kind = ExactCfKind::Branch;
        cf.target = exact_branch_target(pc, instr);
        return cf;
    }
    // COP1/COP2 condition branches are ordinary conditional branches.  COP1
    // is not present on PS1, but classifying both keeps raw control-flow and
    // delay-slot boundaries conservative for composite inputs.
    if ((opcode == 0x11u || opcode == 0x12u) && rs == 0x08u) {
        cf.kind = ExactCfKind::Branch;
        cf.target = exact_branch_target(pc, instr);
        return cf;
    }
    return cf;
}

} // namespace

bool resolve_exact_bounded_jump_table(
    const PS1Executable& exe,
    uint32_t entry,
    uint32_t hard_cap,
    uint32_t jr_pc,
    uint32_t jr_rs,
    ExactJumpTable& table,
    ExactAddressMapper runtime_to_image,
    uint32_t producer_lo,
    uint32_t producer_hi) {
    table = {};
    if (entry >= hard_cap || jr_pc < entry || jr_pc >= hard_cap ||
        (entry & 3u) != 0u || (hard_cap & 3u) != 0u ||
        (jr_pc & 3u) != 0u || jr_rs == 0u || jr_rs == 31u) {
        return false;
    }

    auto read = [&](uint32_t pc) { return exe.read_word(pc); };
    auto mapped = [&](uint32_t addr) {
        return runtime_to_image ? runtime_to_image(addr, exe) : addr;
    };
    if (producer_lo == 0u && producer_hi == 0u) {
        producer_lo = exe.header.load_address;
        producer_hi = exe.end_address();
    }
    if (producer_lo >= producer_hi || (producer_lo & 3u) != 0u ||
        (producer_hi & 3u) != 0u) {
        return false;
    }
    auto jr_word = read(jr_pc);
    if (!jr_word.has_value() ||
        *jr_word != ((jr_rs & 0x1Fu) << 21u | 0x08u)) {
        return false;
    }
    auto is_control = [&](uint32_t pc, uint32_t instr) {
        (void)pc;
        return exact_classify_cf(pc, instr).kind != ExactCfKind::Normal;
    };
    auto in_delay_slot = [&](uint32_t pc) {
        if (pc < entry + 4u) return false;
        auto prev = read(pc - 4u);
        return prev.has_value() && is_control(pc - 4u, *prev);
    };
    auto has_call_between = [&](uint32_t lo, uint32_t hi) {
        for (uint32_t pc = lo; pc < hi; pc += 4u) {
            auto word = read(pc);
            if (!word.has_value()) return true;
            ExactCf cf = exact_classify_cf(pc, *word);
            if (cf.kind == ExactCfKind::Jal ||
                cf.kind == ExactCfKind::Jalr) {
                return true;
            }
        }
        return false;
    };
    auto writes_between = [&](uint32_t lo, uint32_t hi, uint32_t reg) {
        for (uint32_t pc = lo; pc < hi; pc += 4u) {
            auto word = read(pc);
            if (!word.has_value() || exact_instruction_writes_gpr(*word, reg))
                return true;
        }
        return false;
    };
    // Reject a definition that a direct edge from before it can skip. Edges
    // originating after the definition are harmless: that path has already
    // executed the definition (Ape's self-looping out-of-range guard does this).
    auto inbound_skips = [&](uint32_t def_pc, uint32_t use_pc) {
        for (uint32_t source = entry; source < def_pc; source += 4u) {
            auto word = read(source);
            if (!word.has_value()) return true;
            ExactCf cf = exact_classify_cf(source, *word);
            bool direct = cf.kind == ExactCfKind::Branch ||
                          cf.kind == ExactCfKind::Jump ||
                          cf.kind == ExactCfKind::Jal;
            if (direct && cf.target > def_pc && cf.target <= use_pc)
                return true;
        }
        return false;
    };

    // Keep this in parity with compile_overlays.py: the canonical scheduling
    // form permits zero or one NOP between LW and JR, never a raw window.
    uint32_t lw_pc = jr_pc - 4u, lw_base = 0;
    int32_t lw_offset = 0;
    if (lw_pc < entry) return false;
    auto lw_word = read(lw_pc);
    if (!lw_word.has_value()) return false;
    if (*lw_word == 0u) {
        if (lw_pc < entry + 4u) return false;
        lw_pc -= 4u;
        lw_word = read(lw_pc);
    }
    if (!lw_word.has_value()) return false;
    uint32_t lw_op = (*lw_word >> 26) & 0x3Fu;
    lw_base = (*lw_word >> 21) & 0x1Fu;
    uint32_t lw_rt = (*lw_word >> 16) & 0x1Fu;
    if (lw_op != 0x23u || lw_rt != jr_rs || lw_base == 0u ||
        lw_pc < entry + 8u || in_delay_slot(lw_pc)) {
        return false;
    }
    lw_offset = static_cast<int32_t>(
        static_cast<int16_t>(*lw_word & 0xFFFFu));

    // The address add and index scale must be the two instructions immediately
    // before the load. This deliberately excludes raw-window coincidences.
    uint32_t addu_pc = lw_pc - 4u;
    uint32_t sll_pc = addu_pc - 4u;
    auto addu_word = read(addu_pc);
    auto sll_word = read(sll_pc);
    if (!addu_word.has_value() || !sll_word.has_value()) return false;
    uint32_t addu_op = (*addu_word >> 26) & 0x3Fu;
    uint32_t addu_rs = (*addu_word >> 21) & 0x1Fu;
    uint32_t addu_rt = (*addu_word >> 16) & 0x1Fu;
    uint32_t addu_rd = (*addu_word >> 11) & 0x1Fu;
    uint32_t addu_shamt = (*addu_word >> 6) & 0x1Fu;
    uint32_t addu_fn = *addu_word & 0x3Fu;
    uint32_t sll_op = (*sll_word >> 26) & 0x3Fu;
    uint32_t sll_rs = (*sll_word >> 21) & 0x1Fu;
    uint32_t index_reg = (*sll_word >> 16) & 0x1Fu;
    uint32_t scaled_reg = (*sll_word >> 11) & 0x1Fu;
    uint32_t sll_shamt = (*sll_word >> 6) & 0x1Fu;
    uint32_t sll_fn = *sll_word & 0x3Fu;
    if (addu_op != 0u || addu_fn != 0x21u || addu_shamt != 0u ||
        addu_rd != lw_base ||
        sll_op != 0u || sll_fn != 0u || sll_rs != 0u || sll_shamt != 2u ||
        index_reg == 0u || scaled_reg == 0u ||
        (scaled_reg != addu_rs && scaled_reg != addu_rt)) {
        return false;
    }
    uint32_t base_reg = scaled_reg == addu_rs ? addu_rt : addu_rs;
    if (base_reg == 0u || base_reg == scaled_reg ||
        in_delay_slot(addu_pc)) {
        return false;
    }

    // Exact canonical guard: sltiu; beq; nop; sll. This is the same accepted
    // suffix as the Python capture verifier and excludes unrelated bounds.
    if (sll_pc < entry + 12u) return false;
    uint32_t guard_pc = sll_pc - 8u;
    uint32_t bound_pc = sll_pc - 12u;
    auto guard_word_opt = read(guard_pc);
    auto bound_word = read(bound_pc);
    auto guard_delay = read(sll_pc - 4u);
    if (!guard_word_opt.has_value() || !bound_word.has_value() ||
        !guard_delay.has_value() || *guard_delay != 0u) {
        return false;
    }
    uint32_t guard_word = *guard_word_opt;
    uint32_t guard_op = (guard_word >> 26) & 0x3Fu;
    uint32_t guard_rs = (guard_word >> 21) & 0x1Fu;
    uint32_t guard_rt = (guard_word >> 16) & 0x1Fu;
    uint32_t bound_reg = guard_rs == 0u ? guard_rt : guard_rs;
    if (guard_op != 0x04u || bound_reg == 0u ||
        (guard_rs != 0u && guard_rt != 0u)) {
        return false;
    }
    uint32_t bound_op = (*bound_word >> 26) & 0x3Fu;
    uint32_t bound_rs = (*bound_word >> 21) & 0x1Fu;
    uint32_t bound_rt = (*bound_word >> 16) & 0x1Fu;
    uint32_t count = *bound_word & 0xFFFFu;
    if (bound_op != 0x0Bu || bound_rs != index_reg ||
        bound_rt != bound_reg || count == 0u || count >= 512u ||
        in_delay_slot(bound_pc)) {
        return false;
    }
    uint32_t guard_target = exact_branch_target(guard_pc, guard_word);
    if ((guard_target & 3u) != 0u || guard_target < entry ||
        guard_target >= hard_cap ||
        (guard_target >= sll_pc && guard_target < jr_pc + 8u)) {
        return false;
    }

    // Resolve the table-base reaching definition. Cross-register constants
    // (`lui rA; addiu rB,rA,lo`) are valid, but both definitions must be local,
    // unskippable, outside delay slots, and unclobbered before use.
    uint32_t low_pc = 0, source_reg = base_reg;
    int16_t low = 0;
    uint32_t lui_pc = 0, upper = 0;
    for (uint32_t back = 1; back <= 32u; back++) {
        if (addu_pc < entry + back * 4u) break;
        uint32_t pc = addu_pc - back * 4u;
        auto word = read(pc);
        if (!word.has_value()) return false;
        if (!exact_instruction_writes_gpr(*word, base_reg)) continue;
        uint32_t op = (*word >> 26) & 0x3Fu;
        uint32_t rs = (*word >> 21) & 0x1Fu;
        uint32_t rt = (*word >> 16) & 0x1Fu;
        if (op == 0x09u && rt == base_reg && rs != 0u) {
            low_pc = pc;
            source_reg = rs;
            low = static_cast<int16_t>(*word & 0xFFFFu);
        } else if (op == 0x0Fu && rs == 0u && rt == base_reg) {
            lui_pc = pc;
            upper = (*word & 0xFFFFu) << 16;
        } else {
            return false;
        }
        break;
    }
    if (low_pc != 0u) {
        for (uint32_t back = 1; back <= 32u; back++) {
            if (low_pc < entry + back * 4u) break;
            uint32_t pc = low_pc - back * 4u;
            auto word = read(pc);
            if (!word.has_value()) return false;
            if (!exact_instruction_writes_gpr(*word, source_reg)) continue;
            uint32_t op = (*word >> 26) & 0x3Fu;
            uint32_t rs = (*word >> 21) & 0x1Fu;
            uint32_t rt = (*word >> 16) & 0x1Fu;
            if (op != 0x0Fu || rs != 0u || rt != source_reg) return false;
            lui_pc = pc;
            upper = (*word & 0xFFFFu) << 16;
            break;
        }
    }
    if (lui_pc == 0u || in_delay_slot(lui_pc) ||
        (low_pc != 0u && in_delay_slot(low_pc)) ||
        inbound_skips(lui_pc, addu_pc) ||
        (low_pc != 0u && inbound_skips(low_pc, addu_pc)) ||
        has_call_between(lui_pc, addu_pc)) {
        return false;
    }
    // No other control transfer may intervene between the proven constant and
    // the canonical suffix. The bounds BEQ is the sole exception.
    for (uint32_t pc = lui_pc + 4u; pc < sll_pc; pc += 4u) {
        auto word = read(pc);
        if (!word.has_value() ||
            (pc != guard_pc && is_control(pc, *word))) {
            return false;
        }
    }
    uint32_t base_def_end = low_pc != 0u ? low_pc + 4u : lui_pc + 4u;
    if (writes_between(base_def_end, addu_pc, base_reg) ||
        (low_pc != 0u &&
         writes_between(lui_pc + 4u, low_pc, source_reg))) {
        return false;
    }

    int64_t base64 = static_cast<int64_t>(upper);
    if (low_pc != 0u) base64 += static_cast<int32_t>(low);
    base64 += lw_offset;
    if (base64 < 0 || base64 > 0xFFFFFFFFll ||
        (static_cast<uint32_t>(base64) & 3u) != 0u) {
        return false;
    }
    uint32_t runtime_base = static_cast<uint32_t>(base64);
    uint64_t runtime_end = static_cast<uint64_t>(runtime_base) +
                           static_cast<uint64_t>(count) * 4u;
    if (runtime_end > 0x100000000ull) return false;

    ExactJumpTable resolved;
    resolved.table_base = runtime_base;
    resolved.table_count = count;
    resolved.targets.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t runtime_slot = runtime_base + i * 4u;
        uint32_t image_slot = mapped(runtime_slot);
        if (image_slot < producer_lo || image_slot >= producer_hi ||
            producer_hi - image_slot < 4u) {
            return false;
        }
        auto target_word = read(image_slot);
        if (!target_word.has_value() || (*target_word & 3u) != 0u)
            return false;
        uint32_t runtime_target = *target_word;
        uint32_t image_target = mapped(runtime_target);
        if (image_target < entry || image_target >= hard_cap ||
            image_target < producer_lo || image_target >= producer_hi ||
            (image_target & 3u) != 0u) {
            return false;
        }
        auto target_first = read(image_target);
        if (!target_first.has_value() ||
            !exact_is_valid_mips_word(*target_first)) {
            return false;
        }
        resolved.targets.emplace_back(runtime_target, image_target);
    }

    // Later case blocks may legitimately loop to the dispatch suffix (Ape
    // Escape does), but arbitrary later bytes must not create an inbound edge
    // that skips the reaching definitions. Prove the exemption by walking
    // only from the table's validated case targets.
    std::set<uint32_t> case_reachable;
    std::queue<uint32_t> case_work;
    for (const auto& target_pair : resolved.targets)
        case_work.push(target_pair.second);
    while (!case_work.empty() && case_reachable.size() < 2048u) {
        uint32_t pc = case_work.front();
        case_work.pop();
        if (case_reachable.count(pc) || pc < entry || pc >= hard_cap ||
            pc < producer_lo || pc >= producer_hi) {
            continue;
        }
        auto word = read(pc);
        if (!word.has_value() || !exact_is_valid_mips_word(*word)) continue;
        case_reachable.insert(pc);
        ExactCf cf = exact_classify_cf(pc, *word);
        uint32_t delay = pc + 4u;
        switch (cf.kind) {
        case ExactCfKind::Normal:
            case_work.push(pc + 4u);
            break;
        case ExactCfKind::Branch:
            case_reachable.insert(delay);
            case_work.push(pc + 8u);
            case_work.push(cf.target);
            break;
        case ExactCfKind::BranchNever:
            case_reachable.insert(delay);
            case_work.push(pc + 8u);
            break;
        case ExactCfKind::BranchNeverLikely:
            case_work.push(pc + 8u);
            break;
        case ExactCfKind::Jump:
            case_reachable.insert(delay);
            case_work.push(cf.target);
            break;
        case ExactCfKind::Jal:
        case ExactCfKind::Jalr:
            case_reachable.insert(delay);
            case_work.push(pc + 8u);
            break;
        case ExactCfKind::JrRa:
        case ExactCfKind::JrOther:
            case_reachable.insert(delay);
            break;
        }
    }
    for (uint32_t source = entry; source < hard_cap; source += 4u) {
        auto word = read(source);
        if (!word.has_value()) return false;
        ExactCf cf = exact_classify_cf(source, *word);
        bool direct = cf.kind == ExactCfKind::Branch ||
                      cf.kind == ExactCfKind::Jump ||
                      cf.kind == ExactCfKind::Jal;
        if (direct && cf.target > lui_pc && cf.target <= jr_pc &&
            (source < lui_pc || source > jr_pc) &&
            !case_reachable.count(source)) {
            return false;
        }
    }
    table = std::move(resolved);
    return true;
}

FunctionAnalysisResult FunctionAnalyzer::analyze_exact_entries(
    const std::vector<uint32_t>& entries,
    const std::vector<std::pair<uint32_t, uint32_t>>& producer_ranges,
    const std::set<uint32_t>& cross_call_allow) {
    FunctionAnalysisResult result;
    result.total_instructions = 0;
    result.jr_ra_count = 0;
    result.prologue_count = 0;
    result.call_discovered_count = 0;
    result.state_continuation_count = 0;

    fmt::print("\n=== Exact-Entry Function Analysis ===\n\n");

    auto in_exe = [&](uint32_t addr) {
        return addr >= exe_.header.load_address && addr < exe_.end_address() && (addr & 3u) == 0;
    };

    auto producer_for = [&](uint32_t addr) -> int {
        for (size_t i = 0; i < producer_ranges.size(); i++) {
            if (addr >= producer_ranges[i].first && addr < producer_ranges[i].second)
                return static_cast<int>(i);
        }
        return -1;
    };

    auto callable_direct_jal_target = [&](uint32_t source, uint32_t addr) {
        auto word_opt = exe_.read_word(addr);
        // A JAL encountered by the reachable CFG walk is direct call evidence.
        // Requiring a stack prologue here discarded legitimate frameless leaf
        // functions and tiny library wrappers.  The target still has to decode
        // as a valid PS1 instruction and lie inside the verified image bound.
        //
        // Three adjacent aligned words that all point back into this image are
        // stronger evidence for a position-fixed pointer table than the first
        // word's accidental MIPS decode.  Do not follow a JAL-shaped word into
        // that data.  The caller remains valid and dispatches the unresolved
        // target at runtime, where the ordinary live-byte guard can select a
        // separately proven implementation or fall back to the interpreter.
        if (!word_opt.has_value() || !exact_is_valid_mips_word(*word_opt))
            return false;
        bool dense_local_pointer_table = true;
        for (uint32_t i = 0; i < 3u; i++) {
            auto value = exe_.read_word(addr + i * 4u);
            if (!value.has_value() || (*value & 3u) != 0u ||
                !in_exe(*value)) {
                dense_local_pointer_table = false;
                break;
            }
        }
        if (dense_local_pointer_table) return false;
        if (producer_ranges.empty()) return true;
        int source_producer = producer_for(source);
        int target_producer = producer_for(addr);
        // A composite's uncovered padding is not a producer.  In particular,
        // two gap addresses both map to -1 and must not be treated as sharing
        // ownership, nor may an allow-list bless a target with no producer.
        if (source_producer < 0 || target_producer < 0) return false;
        if (source_producer == target_producer)
            return true;
        return cross_call_allow.count(addr) != 0;
    };

    auto walk = [&](uint32_t entry, uint32_t hard_cap) {
        ExactWalkResult wr;
        std::queue<uint32_t> work;
        work.push(entry);
        const int entry_producer = producer_for(entry);

        auto in_function = [&](uint32_t addr) {
            return in_exe(addr) && addr >= entry && addr < hard_cap &&
                   (producer_ranges.empty() ||
                    (entry_producer >= 0 &&
                     producer_for(addr) == entry_producer));
        };

        while (!work.empty()) {
            uint32_t pc = work.front();
            work.pop();

            if (wr.visited.count(pc) || !in_function(pc)) continue;
            auto word_opt = exe_.read_word(pc);
            if (!word_opt.has_value()) continue;

            uint32_t instr = *word_opt;
            wr.visited.insert(pc);
            ExactCf cf = exact_classify_cf(pc, instr);
            uint32_t delay = pc + 4u;

            switch (cf.kind) {
            case ExactCfKind::Normal:
                if (in_function(pc + 4u)) work.push(pc + 4u);
                break;
            case ExactCfKind::Branch:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(pc + 8u)) work.push(pc + 8u);
                if (in_function(cf.target)) work.push(cf.target);
                break;
            case ExactCfKind::BranchNever:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(pc + 8u)) work.push(pc + 8u);
                break;
            case ExactCfKind::BranchNeverLikely:
                if (in_function(pc + 8u)) work.push(pc + 8u);
                break;
            case ExactCfKind::Jump:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(cf.target)) work.push(cf.target);
                break;
            case ExactCfKind::Jal:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(pc + 8u)) work.push(pc + 8u);
                if (in_exe(cf.target) &&
                    callable_direct_jal_target(pc, cf.target)) {
                    wr.direct_jal_targets.insert(cf.target);
                    wr.transfer_sources[cf.target].insert(
                        std::make_pair(pc, false));
                }
                break;
            case ExactCfKind::Jalr:
                if (in_function(delay)) wr.visited.insert(delay);
                if (in_function(pc + 8u)) work.push(pc + 8u);
                {
                    uint32_t target = 0;
                    uint32_t target_reg = (instr >> 21) & 0x1Fu;
                    if (exact_resolve_constant_transfer(exe_, entry, pc,
                                                        target_reg, target) &&
                        in_exe(target) &&
                        callable_direct_jal_target(pc, target)) {
                        wr.direct_jal_targets.insert(target);
                        wr.transfer_sources[target].insert(
                            std::make_pair(pc, true));
                    }
                }
                break;
            case ExactCfKind::JrRa:
                if (in_function(delay)) wr.visited.insert(delay);
                wr.jr_ra_count++;
                break;
            case ExactCfKind::JrOther:
                if (in_function(delay)) wr.visited.insert(delay);
                {
                    uint32_t jr_rs = (instr >> 21) & 0x1Fu;
                    uint32_t target = 0;
                    if (exact_resolve_constant_transfer(exe_, entry, pc, jr_rs,
                                                        target)) {
                        if (in_function(target)) {
                            wr.jump_table_targets.insert(target);
                            work.push(target);
                        } else if (in_exe(target) &&
                                   callable_direct_jal_target(pc, target)) {
                            // An absolute JR out of the current function is a
                            // statically proven tail-call entry.
                            wr.direct_jal_targets.insert(target);
                            wr.transfer_sources[target].insert(
                                std::make_pair(pc, true));
                        }
                    } else {
                        ExactJumpTable table;
                        uint32_t producer_lo = exe_.header.load_address;
                        uint32_t producer_hi = exe_.end_address();
                        if (entry_producer >= 0) {
                            producer_lo = producer_ranges[entry_producer].first;
                            producer_hi = producer_ranges[entry_producer].second;
                        }
                        if (resolve_exact_bounded_jump_table(
                                exe_, entry, hard_cap, pc, jr_rs, table,
                                nullptr, producer_lo, producer_hi)) {
                            bool all_owned = std::all_of(
                                table.targets.begin(), table.targets.end(),
                                [&](const auto& target_pair) {
                                    return in_function(target_pair.second);
                                });
                            if (all_owned) {
                                for (const auto& target_pair : table.targets) {
                                    uint32_t target_pc = target_pair.second;
                                    wr.jump_table_targets.insert(target_pc);
                                    work.push(target_pc);
                                }
                            }
                        }
                        // Otherwise fail closed. Unresolved JR cases remain
                        // interpreted until the complete dependency chain and
                        // every table entry are proven.
                    }
                }
                break;
            }
        }
        return wr;
    };

    std::set<uint32_t> known_entries;
    for (uint32_t entry : entries) {
        if (!in_exe(entry)) continue;
        if (!producer_ranges.empty() && producer_for(entry) < 0) continue;
        known_entries.insert(entry);
    }

    size_t explicit_count = known_entries.size();
    const std::set<uint32_t> explicit_entries = known_entries;
    std::set<uint32_t> derived_entries;
    std::map<uint32_t, std::set<std::pair<uint32_t, bool>>> derived_evidence;
    fmt::print("Explicit entries: {}\n", explicit_count);

    // Discover callees in rounds against one stable entry partition. Processing
    // roots sequentially let an early caller insert a JAL target that actually
    // lay inside a later explicit root. That new entry hard-capped the later
    // root before it was walked (Tomba X01's jump-table host), splitting one
    // function and losing every case after the accidental cap. If a candidate
    // is already owned by any walk in the final partition, leave it absorbed;
    // the analyzer exports that proven host mapping for main_psx to materialize.
    uint32_t discovery_round = 0;
    std::set<std::pair<std::set<uint32_t>, std::set<uint32_t>>> seen_states;
    while (true) {
        auto state = std::make_pair(known_entries, derived_entries);
        if (++discovery_round > 64u || !seen_states.insert(state).second) {
            fmt::print("WARNING: exact-entry ownership did not converge; "
                       "falling back to explicit roots only\n");
            known_entries = explicit_entries;
            derived_entries.clear();
            derived_evidence.clear();
            break;
        }
        const std::set<uint32_t> previous_entries = known_entries;
        const std::set<uint32_t> previous_derived = derived_entries;
        std::vector<uint32_t> round_entries(
            known_entries.begin(), known_entries.end());
        std::set<uint32_t> candidate_targets;
        std::map<uint32_t, std::set<std::pair<uint32_t, bool>>> candidate_evidence;
        for (size_t i = 0; i < round_entries.size(); i++) {
            uint32_t hard_cap = (i + 1 < round_entries.size())
                ? round_entries[i + 1] : exe_.end_address();
            ExactWalkResult wr = walk(round_entries[i], hard_cap);
            candidate_targets.insert(wr.direct_jal_targets.begin(),
                                     wr.direct_jal_targets.end());
            for (const auto& [target, evidence] : wr.transfer_sources)
                candidate_evidence[target].insert(
                    evidence.begin(), evidence.end());
        }

        derived_entries.clear();
        derived_evidence.clear();
        for (uint32_t target : candidate_targets)
            if (!explicit_entries.count(target)) {
                derived_entries.insert(target);
                auto evidence = candidate_evidence.find(target);
                if (evidence != candidate_evidence.end())
                    derived_evidence.emplace(target, evidence->second);
            }

        // Rebuild ownership FROM SCRATCH whenever the candidate universe grows.
        // A target B absorbed by A in one round may need to become a root after
        // a later target X lands between A and B and caps A. A monotonic
        // "absorbed forever" set therefore loses reachable code.
        known_entries = explicit_entries;
        known_entries.insert(derived_entries.begin(), derived_entries.end());
        for (uint32_t target : derived_entries) {
            auto target_it = known_entries.find(target);
            if (target_it == known_entries.end() ||
                explicit_entries.count(target) ||
                target_it == known_entries.begin()) {
                continue;
            }
            auto owner_it = std::prev(target_it);
            auto next_it = std::next(target_it);
            uint32_t hard_cap = next_it != known_entries.end()
                ? *next_it : exe_.end_address();
            ExactWalkResult owner_walk = walk(*owner_it, hard_cap);
            if (owner_walk.visited.count(target)) {
                known_entries.erase(target_it);
            }
        }

        if (known_entries == previous_entries &&
            derived_entries == previous_derived) break;
    }

    result.call_discovered_count = static_cast<int>(known_entries.size() - explicit_count);
    fmt::print("Direct-JAL entries: {}\n", result.call_discovered_count);
    fmt::print("Total exact entries: {}\n\n", known_entries.size());

    std::vector<uint32_t> starts_vec(known_entries.begin(), known_entries.end());
    std::sort(starts_vec.begin(), starts_vec.end());

    for (size_t i = 0; i < starts_vec.size(); i++) {
        uint32_t entry = starts_vec[i];
        uint32_t hard_cap = (i + 1 < starts_vec.size()) ? starts_vec[i + 1] : exe_.end_address();
        ExactWalkResult wr = walk(entry, hard_cap);
        if (wr.visited.empty()) continue;

        Function func;
        func.start_addr = entry;
        func.end_addr = *wr.visited.rbegin() + 4u;
        if (func.end_addr > hard_cap) func.end_addr = hard_cap;
        if (func.end_addr <= func.start_addr) func.end_addr = func.start_addr + 4u;
        func.size = func.end_addr - func.start_addr;
        func.name = fmt::format("func_{:08X}", func.start_addr);
        if (producer_for(entry) >= 0) {
            const auto& producer = producer_ranges[producer_for(entry)];
            func.producer_lo = producer.first;
            func.producer_hi = producer.second;
        }

        auto first_instr = exe_.read_word(func.start_addr);
        if (first_instr.has_value()) {
            int32_t stack_size;
            func.has_prologue = is_prologue(*first_instr, stack_size);
            func.stack_frame_size = func.has_prologue ? stack_size : 0;
            if (func.has_prologue) result.prologue_count++;
        } else {
            func.has_prologue = false;
            func.stack_frame_size = 0;
        }

        func.has_epilogue = wr.jr_ra_count > 0;
        func.is_data_section = false;

        result.jr_ra_count += static_cast<int>(wr.jr_ra_count);
        result.total_instructions += static_cast<int>(wr.visited.size());
        result.exact_reachable_pcs.insert(wr.visited.begin(), wr.visited.end());
        result.functions.push_back(func);
    }

    // Export only analyzer-proven aliases with a host in the FINAL partition.
    // Re-walking final roots makes stale ownership impossible: if a later root
    // now caps the old host before B, B is either a real root or absent here.
    std::set<uint32_t> final_starts(known_entries.begin(), known_entries.end());
    std::vector<std::pair<Function, ExactWalkResult>> final_walks;
    for (size_t i = 0; i < starts_vec.size(); i++) {
        uint32_t host_start = starts_vec[i];
        uint32_t hard_cap = (i + 1 < starts_vec.size())
            ? starts_vec[i + 1] : exe_.end_address();
        ExactWalkResult wr = walk(host_start, hard_cap);
        if (wr.visited.empty()) continue;
        uint32_t host_end = *wr.visited.rbegin() + 4u;
        Function host{};
        host.start_addr = host_start;
        host.end_addr = host_end;
        final_walks.push_back({host, std::move(wr)});
    }
    for (uint32_t target : derived_entries) {
        if (final_starts.count(target)) continue;
        for (const auto& [host, wr] : final_walks) {
            if (!wr.visited.count(target)) continue;
            auto evidence = derived_evidence.find(target);
            if (evidence == derived_evidence.end()) break;
            auto source = std::find_if(
                evidence->second.begin(), evidence->second.end(),
                [&](const auto& item) { return wr.visited.count(item.first); });
            if (source == evidence->second.end()) break;
            result.absorbed_entries.push_back(
                {target, host.start_addr, host.end_addr,
                 source->first, source->second});
            break;
        }
    }

    return result;
}


FunctionAnalysisResult FunctionAnalyzer::analyze() {
    FunctionAnalysisResult result;
    result.total_instructions = 0;
    result.jr_ra_count = 0;
    result.prologue_count = 0;
    result.call_discovered_count = 0;
    result.strong_prologue_count = 0;
    result.bios_thunk_count = 0;
    result.state_continuation_count = 0;

    fmt::print("\n=== Function Boundary Detection ===\n\n");

    // Pass 1: Find all jr $ra instructions
    std::vector<uint32_t> return_addresses;

    uint32_t current_addr = exe_.header.load_address;
    uint32_t end_addr = exe_.end_address();

    fmt::print("Scanning {} KB of code for function returns...\n",
               (end_addr - current_addr) / 1024);

    while (current_addr < end_addr) {
        auto word_opt = exe_.read_word(current_addr);
        if (!word_opt.has_value()) {
            break;
        }

        uint32_t instr = *word_opt;
        result.total_instructions++;

        if (is_jr_ra(instr)) {
            return_addresses.push_back(current_addr);
            result.jr_ra_count++;
        }

        int32_t stack_size;
        if (is_prologue(instr, stack_size)) {
            result.prologue_count++;
        }

        current_addr += 4;
    }

    fmt::print("✓ Found {} jr $ra instructions\n", result.jr_ra_count);
    fmt::print("✓ Found {} function prologues\n", result.prologue_count);
    fmt::print("✓ Scanned {} total instructions\n\n", result.total_instructions);

    // Pass 2: For each jr $ra, find function boundaries
    fmt::print("Analyzing function boundaries...\n");

    std::set<uint32_t> function_starts; // Use set to avoid duplicates
    std::map<uint32_t, uint32_t> function_last_return;

    for (uint32_t return_addr : return_addresses) {
        uint32_t func_start = find_function_start(return_addr);
        function_starts.insert(func_start);
        auto it = function_last_return.find(func_start);
        if (it == function_last_return.end() || return_addr > it->second) {
            function_last_return[func_start] = return_addr;
        }
    }

    size_t jr_ra_discovered = function_starts.size();
    fmt::print("✓ Identified {} unique functions from jr $ra scan\n", jr_ra_discovered);

    // Pass 2.5: Follow JAL call targets to discover additional functions
    // This finds functions that don't have standard jr $ra prologues
    fmt::print("Following JAL call targets to discover additional functions...\n");

    uint32_t exe_start = exe_.header.load_address;
    uint32_t exe_end   = exe_.end_address();

    for (uint32_t addr = exe_start; addr < exe_end; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;

        uint32_t opcode = (instr >> 26) & 0x3F;
        if (opcode == 3) {  // JAL
            // PS1 JAL target: upper 4 bits from KSEG0 region (0x80000000)
            uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;

            // Only add if target is within EXE range and word-aligned
            if (target >= exe_start && target < exe_end && (target & 3) == 0) {
                auto target_word = exe_.read_word(target);
                int32_t target_stack_size = 0;
                if (target_word.has_value() &&
                    is_prologue(*target_word, target_stack_size)) {
                    uint32_t setup_start = 0;
                    if (find_preprologue_setup_start(exe_, target, exe_start,
                                                     setup_start)) {
                        target = setup_start;
                    }
                }
                function_starts.insert(target);
            }
        }
    }

    result.call_discovered_count = static_cast<int>(function_starts.size() - jr_ra_discovered);
    fmt::print("✓ Identified {} unique functions ({} call-discovered)\n\n",
               function_starts.size(), result.call_discovered_count);

    // Pass 2.55: promote strong standalone prologues.
    //
    // Return-scanning alone misses scheduler/task entry points that never
    // return normally, and direct JAL following misses callback entries whose
    // addresses are derived at runtime. A standard stack-frame prologue that
    // quickly saves $ra is a strong function-entry signal, provided the ADDIU
    // is not itself a branch delay slot.
    fmt::print("Finding strong standalone prologue entries...\n");
    int strong_prologues = 0;
    for (uint32_t addr = exe_start; addr < exe_end; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;

        int32_t stack_size = 0;
        if (!is_prologue(*word_opt, stack_size)) continue;

        if (addr >= exe_start + 4) {
            auto prev_opt = exe_.read_word(addr - 4);
            if (prev_opt.has_value() && is_branch_or_jump(*prev_opt)) {
                continue;
            }
        }

        uint32_t entry_addr = addr;
        uint32_t setup_start = 0;
        if (find_preprologue_setup_start(exe_, addr, exe_start, setup_start)) {
            entry_addr = setup_start;
        }

        auto existing_it = function_starts.upper_bound(entry_addr);
        if (existing_it != function_starts.begin()) {
            --existing_it;
            auto return_it = function_last_return.find(*existing_it);
            if (return_it != function_last_return.end() &&
                return_it->second + 8u > entry_addr) {
                continue;
            }
        }

        bool saves_ra = false;
        for (uint32_t look = addr + 4; look < addr + 64 && look < exe_end; look += 4) {
            auto next_opt = exe_.read_word(look);
            if (!next_opt.has_value()) break;
            if (is_sw_reg_sp(*next_opt, 31)) {
                saves_ra = true;
                break;
            }
            if (is_branch_or_jump(*next_opt)) {
                break;
            }
        }

        if (saves_ra && function_starts.insert(entry_addr).second) {
            if (entry_addr != addr) {
                function_starts.erase(addr);
            }
            strong_prologues++;
        }
    }
    result.strong_prologue_count = strong_prologues;
    fmt::print("Identified {} strong prologue entries\n\n",
               result.strong_prologue_count);

    // Pass 2.56: promote packed PSY-Q BIOS dispatch thunks.
    //
    // PSY-Q libraries often pack tiny A0/B0/C0 syscall thunks back-to-back:
    //   addiu rN, $zero, 0xA0/0xB0/0xC0
    //   jr    rN
    //   addiu $t1, $zero, index
    //
    // They do not contain jr $ra and may sit directly before data tables, so
    // return/prologue scanning either misses them or lets the following data
    // make the region look like a data section. Treat each thunk as a real
    // function and cap it after the JR delay slot.
    fmt::print("Finding packed BIOS dispatch thunk entries...\n");
    int bios_thunks = 0;
    int bios_thunks_skipped = 0;
    for (uint32_t addr = exe_start; addr + 12u <= exe_end; addr += 4) {
        uint32_t jr_addr = 0;
        if (!is_bios_dispatch_thunk(addr, jr_addr)) continue;

        // A li/jr BIOS-dispatch sequence is only a STANDALONE thunk when it
        // sits in a gap that earlier discovery (prologue scan, jr-$ra scan,
        // call-target following) missed. A real function may legitimately
        // *begin* with this sequence (a tail call into the BIOS) yet continue
        // with reachable code — e.g. a libcard routine that calls the BIOS and
        // then polls card status. Treating such a function as a thunk and
        // capping function_last_return at its first JR silently truncates the
        // rest of its body. That is exactly what gutted Tomba's card-poll
        // routine func_8005CDB8 (104B -> 12B) and stalled LOAD GAME.
        //
        // Mirror segagenesisrecomp's boundary-split discipline: a pattern
        // heuristic must never shrink or split an already-discovered function.
        // Only promote+cap thunks that are NOT already a known entry and do
        // NOT fall inside an existing function's discovered extent.
        if (function_starts.count(addr)) {
            // Already discovered as a real function start — leave its extent
            // alone; do not re-cap it to the first JR.
            bios_thunks_skipped++;
            continue;
        }
        auto owner_it = function_starts.upper_bound(addr); // first start > addr
        if (owner_it != function_starts.begin()) {
            --owner_it;                                     // greatest start <= addr
            uint32_t owner = *owner_it;
            auto ret_it = function_last_return.find(owner);
            if (owner < addr && ret_it != function_last_return.end() &&
                addr <= ret_it->second) {
                // Inside an existing function's body — promoting here would
                // split that function. Leave it whole.
                bios_thunks_skipped++;
                continue;
            }
        }

        if (function_starts.insert(addr).second) {
            bios_thunks++;
        }
        auto it = function_last_return.find(addr);
        if (it == function_last_return.end() || jr_addr > it->second) {
            function_last_return[addr] = jr_addr;
        }
    }
    result.bios_thunk_count = bios_thunks;
    fmt::print("Identified {} packed BIOS dispatch thunk entries "
               "({} candidates skipped: already discovered / inside a function)\n\n",
               result.bios_thunk_count, bios_thunks_skipped);

    // Pass 2.6: discover continuations saved by setjmp/SaveState-style helpers.
    //
    // Some games save the caller's $ra into an application context object and
    // later restore it from an exception/VSync callback. The restored PC is the
    // instruction after the original JAL delay slot, not a normal prologue, so
    // it must be available as a split dispatch entry.
    fmt::print("Finding SaveState-style continuation entries...\n");
    int state_continuations = 0;
    for (uint32_t addr = exe_start; addr < exe_end; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;
        uint32_t opcode = (instr >> 26) & 0x3F;
        if (opcode != 3) continue;  // JAL

        uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;
        uint32_t cont = addr + 8;
        if (target < exe_start || target >= exe_end || (target & 3) != 0) continue;
        if (cont < exe_start || cont >= exe_end || (cont & 3) != 0) continue;

        auto target_first = exe_.read_word(target);
        if (!target_first.has_value()) continue;

        // sw $ra, imm($a0) at callee entry is the narrow signature for these
        // context-save helpers. It avoids splitting ordinary calls.
        if (is_sw_reg_base(*target_first, 4, 31)) {
            bool inserted = function_starts.insert(cont).second;
            if (inserted) state_continuations++;
        }
    }
    result.state_continuation_count = state_continuations;
    fmt::print("Identified {} SaveState continuation entries\n\n",
               result.state_continuation_count);

    // Pass 2.65: promote executable pointer-table entries.
    //
    // Many PS1 libraries store callback vectors as raw function addresses in
    // the loaded EXE image. Some callback entries perform a few global loads
    // before allocating their stack frame, so prologue-only discovery emits
    // the later ADDIU $sp address while the game calls the earlier table
    // address. Treat aligned image words that point at a near-prologue entry
    // as first-class function starts, and collapse the later prologue-only
    // start when it belongs to that prelude.
    fmt::print("Finding executable pointer-table function entries...\n");
    std::map<uint32_t, uint32_t> pointer_refs;
    for (uint32_t addr = exe_start; addr < exe_end; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t target = *word_opt;
        if ((target & 3u) != 0) continue;
        if (target < exe_start || target >= exe_end) continue;

        uint32_t prologue_addr = 0;
        if (!pointer_target_has_near_prologue(exe_, target, exe_start, exe_end,
                                              prologue_addr)) {
            continue;
        }

        pointer_refs[target]++;

        if (prologue_addr != target) {
            auto prologue_it = function_starts.find(prologue_addr);
            if (prologue_it != function_starts.end()) {
                auto ret_it = function_last_return.find(prologue_addr);
                if (ret_it != function_last_return.end()) {
                    auto dst_it = function_last_return.find(target);
                    if (dst_it == function_last_return.end() ||
                        ret_it->second > dst_it->second) {
                        function_last_return[target] = ret_it->second;
                    }
                    function_last_return.erase(ret_it);
                }
                function_starts.erase(prologue_it);
            }
        }
    }

    int pointer_entries = 0;
    for (const auto& [target, refs] : pointer_refs) {
        (void)refs;
        if (function_starts.insert(target).second) {
            pointer_entries++;
        }
    }
    result.pointer_table_entry_count = pointer_entries;
    fmt::print("Identified {} pointer-table function entries\n\n",
               result.pointer_table_entry_count);

    // Pass 2.7: Add forced entry points
    // These are function starts that do not have a standard prologue (e.g. the
    // PS1 EXE entry point, which is launched directly by the BIOS without a
    // JAL and starts with a BSS-zeroing loop rather than ADDIU $sp, $sp, -N).
    if (!forced_entry_points_.empty()) {
        fmt::print("Adding {} forced entry point(s)...\n", forced_entry_points_.size());
        for (uint32_t addr : forced_entry_points_) {
            bool inserted = function_starts.insert(addr).second;
            if (inserted) {
                fmt::print("  Forced entry 0x{:08X} added\n", addr);
            } else {
                fmt::print("  Forced entry 0x{:08X} already present (skipped)\n", addr);
            }
        }
        fmt::print("\n");
    }

    // Pass 3: Build function table with details
    std::vector<uint32_t> starts_vec(function_starts.begin(), function_starts.end());
    std::sort(starts_vec.begin(), starts_vec.end());

    // ── Control-flow-aware function extent + code/data classification ──
    //
    // The legacy estimate set end_addr to the NEXT discovered function start
    // and then ran is_likely_data_section over that whole span. In Psy-Q /
    // Capcom titles (e.g. Mega Man X4) a real function is immediately followed
    // by an INLINE data / jump table (build-signature strings, dispatch
    // tables). That trailing data inflated the function's measured extent, and
    // is_likely_data_section's undefined-opcode density then misclassified the
    // real function as a data section — emitting a psx_unknown_dispatch stub
    // for real, directly-called code (the entire 0x800EE3A4.. text tail,
    // ~261 KB / 7,373 entries, became stubs, and the boot jal into it hit the
    // stub → fail-fast).
    //
    // Fix (mirrors the proven analyze_exact_entries overlay walk): walk each
    // function's control flow from its entry to its real terminus (the jr $ra /
    // tail-jump reachable from the entry). If the reachable body is provably
    // clean code — reaches a return/tail exit, decodes with zero invalid
    // primary opcodes, and every indirect jump was resolved to in-function
    // targets — bound end_addr to that terminus (so trailing inline data is
    // excluded and never linearly disassembled as code) and mark it code. Only
    // functions that are NOT provably clean code fall back to the legacy
    // boundary + density heuristic, which still correctly rejects genuine data
    // tables minted as function starts (those never walk as clean code:
    // ~44% of random data words carry an invalid primary opcode).
    auto in_exe_p3 = [&](uint32_t a) {
        return a >= exe_.header.load_address && a < exe_.end_address() && (a & 3u) == 0;
    };
    // Same primary-opcode validity table as is_likely_data_section so the two
    // agree on what "clean code" means (COP2/GTE, LWC2/SWC2 are valid PS1 ops).
    auto valid_primary_p3 = [](uint32_t instr) -> bool {
        static const bool valid[64] = {
            true,  true,  true,  true,  true,  true,  true,  true,   // 0x00-0x07
            true,  true,  true,  true,  true,  true,  true,  true,   // 0x08-0x0F
            true,  false, true,  false, false, false, false, false,  // 0x10-0x17
            false, false, false, false, false, false, false, false,  // 0x18-0x1F
            true,  true,  true,  true,  true,  true,  true,  false,  // 0x20-0x27
            true,  true,  true,  true,  false, false, true,  false,  // 0x28-0x2F
            true,  false, true,  false, false, false, false, false,  // 0x30-0x37
            true,  false, true,  false, false, false, false, false,  // 0x38-0x3F
        };
        return valid[(instr >> 26) & 0x3Fu];
    };
    // Mirror exact-entry discovery's strict bounded-switch recognizer so the
    // final extent cannot disagree about which JR targets are code.
    auto resolve_jt_p3 = [&](uint32_t entry, uint32_t hard_cap, uint32_t jr_pc,
                             uint32_t jr_rs, std::vector<uint32_t>& targets) -> bool {
        ExactJumpTable table;
        if (!resolve_exact_bounded_jump_table(
                exe_, entry, hard_cap, jr_pc, jr_rs, table)) {
            return false;
        }
        for (const auto& target_pair : table.targets)
            targets.push_back(target_pair.second);
        return true;
    };
    struct ExtentP3 { uint32_t terminus; bool clean_code; bool hit_invalid; bool resolved_jt; bool unresolved_indirect; };
    auto compute_extent_p3 = [&](uint32_t entry, uint32_t hard_cap) -> ExtentP3 {
        std::set<uint32_t> visited;
        std::queue<uint32_t> work;
        work.push(entry);
        bool hit_invalid = false, reached_exit = false, unresolved_indirect = false;
        bool resolved_jt = false;
        auto in_fn = [&](uint32_t a) { return in_exe_p3(a) && a >= entry && a < hard_cap; };
        while (!work.empty()) {
            uint32_t pc = work.front(); work.pop();
            if (visited.count(pc) || !in_fn(pc)) continue;
            auto word_opt = exe_.read_word(pc);
            if (!word_opt.has_value()) continue;
            uint32_t instr = *word_opt;
            visited.insert(pc);
            if (!valid_primary_p3(instr)) { hit_invalid = true; continue; }
            ExactCf cf = exact_classify_cf(pc, instr);
            uint32_t delay = pc + 4u;
            switch (cf.kind) {
            case ExactCfKind::Normal:
                if (in_fn(pc + 4u)) work.push(pc + 4u);
                break;
            case ExactCfKind::Branch:
                if (in_fn(delay)) visited.insert(delay);
                if (in_fn(pc + 8u)) work.push(pc + 8u);
                if (in_fn(cf.target)) work.push(cf.target);
                break;
            case ExactCfKind::BranchNever:
                if (in_fn(delay)) visited.insert(delay);
                if (in_fn(pc + 8u)) work.push(pc + 8u);
                break;
            case ExactCfKind::BranchNeverLikely:
                if (in_fn(pc + 8u)) work.push(pc + 8u);
                break;
            case ExactCfKind::Jump:
                if (in_fn(delay)) visited.insert(delay);
                if (in_fn(cf.target)) work.push(cf.target);
                else reached_exit = true;  // tail call / jump out of function
                break;
            case ExactCfKind::Jal:
            case ExactCfKind::Jalr:
                if (in_fn(delay)) visited.insert(delay);
                if (in_fn(pc + 8u)) work.push(pc + 8u);
                break;
            case ExactCfKind::JrRa:
                if (in_fn(delay)) visited.insert(delay);
                reached_exit = true;
                break;
            case ExactCfKind::JrOther: {
                if (in_fn(delay)) visited.insert(delay);
                uint32_t jr_rs = (instr >> 21) & 0x1Fu;
                uint32_t constant_target = 0;
                if (exact_resolve_constant_transfer(exe_, entry, pc, jr_rs,
                                                    constant_target)) {
                    resolved_jt = true;
                    if (in_fn(constant_target)) work.push(constant_target);
                    else reached_exit = true;
                } else {
                    std::vector<uint32_t> jt;
                    if (resolve_jt_p3(entry, hard_cap, pc, jr_rs, jt)) {
                        resolved_jt = true;
                        for (uint32_t t : jt) if (in_fn(t)) work.push(t);
                    } else {
                        unresolved_indirect = true;  // computed jump we can't bound
                    }
                }
                break;
            }
            }
        }
        ExtentP3 info;
        info.terminus = visited.empty() ? entry : (*visited.rbegin() + 4u);
        // Density guard: a real function's basic blocks densely cover
        // [entry, terminus]. A walk that follows a data-decoded branch/jump into
        // an inline data region can coincidentally reach a far jr $ra (the word
        // 0x03E00008 appears in data) and look "clean" while spanning tens of KB
        // it barely visited — that over-extended range then hosts many aliases
        // and emits the whole data region as code (huge bloat). Require the
        // visited instructions to cover a meaningful fraction (~25%) of the span
        // so sparse data-following walks are rejected and fall back to the
        // density heuristic. Real functions (incl. those with small inline jump
        // tables) stay well above this floor.
        uint32_t span = info.terminus - entry;
        bool dense = (span == 0) || (static_cast<uint64_t>(visited.size()) * 16u >= span);
        info.clean_code = reached_exit && !hit_invalid && !unresolved_indirect &&
                          !visited.empty() && dense;
        info.hit_invalid = hit_invalid;
        info.resolved_jt = resolved_jt;
        info.unresolved_indirect = unresolved_indirect;
        return info;
    };

    for (size_t i = 0; i < starts_vec.size(); i++) {
        Function func;
        func.start_addr = starts_vec[i];
        uint32_t next_start = (i + 1 < starts_vec.size()) ? starts_vec[i + 1] : end_addr;

        ExtentP3 ext = compute_extent_p3(func.start_addr, next_start);
        if (ext.clean_code) {
            // Real function: bound to its control-flow terminus so trailing
            // inline data / jump tables are excluded from the emitted extent.
            func.end_addr = ext.terminus;
            if (func.end_addr <= func.start_addr) func.end_addr = func.start_addr + 4u;
            if (func.end_addr > next_start) func.end_addr = next_start;
            func.is_data_section = false;
        } else if (ext.hit_invalid && !ext.resolved_jt) {
            // The reachable body (reached via straight/branch flow, no resolved
            // jump table) decodes with an invalid primary opcode — real PS1 code
            // never executes an invalid opcode, so this "function start" is
            // genuinely data (e.g. a filename/string or pointer-table slot minted
            // as a start by pointer-table promotion). Classify as data even when
            // the dense-minted-start span is below is_likely_data_section's size
            // floor. Guarded by !resolved_jt so a mis-resolved jump table can
            // never force a real function to a stub.
            func.end_addr = next_start;
            func.is_data_section = true;
        } else {
            // Legacy boundary estimate + density heuristic, but bound end_addr to
            // the control-flow walk terminus (a valid upper bound on code
            // reachable from the entry). Without this, a function-start on a
            // data-heavy tail with no intervening discovered entry gets
            // end_addr = next_start spanning tens of KB of inline data; that
            // over-extended range then hosts many pointer-table aliases and emits
            // the whole data region as code (massive bloat). The terminus bound
            // is skipped only when an unresolved indirect jump means real code may
            // exist beyond the walk that we cannot safely bound.
            func.end_addr = next_start;
            auto ret_it = function_last_return.find(func.start_addr);
            if (ret_it != function_last_return.end()) {
                uint32_t return_end = ret_it->second + 8u;  /* include jr delay slot */
                if (return_end > func.start_addr && return_end < func.end_addr) {
                    func.end_addr = return_end;
                }
            }
            if (!ext.unresolved_indirect && ext.terminus > func.start_addr &&
                ext.terminus < func.end_addr) {
                func.end_addr = ext.terminus;
            }
            func.is_data_section = is_likely_data_section(func.start_addr, func.end_addr);
        }

        func.size = func.end_addr - func.start_addr;
        func.name = fmt::format("func_{:08X}", func.start_addr);

        // Check for prologue at start
        auto first_instr = exe_.read_word(func.start_addr);
        if (first_instr.has_value()) {
            int32_t stack_size;
            func.has_prologue = is_prologue(*first_instr, stack_size);
            func.stack_frame_size = func.has_prologue ? stack_size : 0;
        } else {
            func.has_prologue = false;
            func.stack_frame_size = 0;
        }

        // Check for jr $ra before end
        func.has_epilogue = false;
        uint32_t search_end = func.end_addr - 4;
        for (uint32_t addr = func.start_addr; addr <= search_end && addr < func.end_addr; addr += 4) {
            auto instr_opt = exe_.read_word(addr);
            if (instr_opt.has_value() && is_jr_ra(*instr_opt)) {
                func.has_epilogue = true;
                break;
            }
        }

        result.functions.push_back(func);
    }

    return result;
}

} // namespace PSXRecomp
