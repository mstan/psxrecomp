#include "code_generator.h"
#include "control_flow.h"
#include <fmt/format.h>
#include <algorithm>
#include <set>
#include <vector>

namespace PSXRecomp {

CodeGenerator::CodeGenerator(const PS1Executable& exe, const CodeGenConfig& config)
    : exe_(exe), config_(config) {}

std::string CodeGenerator::reg_name(int reg_num) {
    // MIPS register names
    static const char* reg_names[32] = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
    };

    if (reg_num >= 0 && reg_num < 32) {
        return std::string("cpu->") + reg_names[reg_num];
    }
    return fmt::format("cpu->gpr[{}]", reg_num);
}

std::string CodeGenerator::translate_addiu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t imm = get_imm16(instr);

    // Optimize: loading immediate into register (from $zero)
    if (rs == 0) {
        return fmt::format("{} = {};", reg_name(rt), imm);
    }

    // Optimize: don't assign to $zero
    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} + {};", reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_lui(uint32_t instr) {
    uint32_t rt = get_rt(instr);
    uint16_t imm = get_imm16_u(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = 0x{:04X} << 16;  /* 0x{:08X} */",
                       reg_name(rt), imm, ((uint32_t)imm) << 16);
}

std::string CodeGenerator::translate_lw(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = cpu->read_word({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = cpu->read_word({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_sw(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (offset == 0) {
        return fmt::format("cpu->write_word({}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->write_word({} + {}, {});",
                          reg_name(rs), offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_or(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    // Optimize: move instruction (or $rd, $rs, $zero)
    if (rt == 0) {
        return fmt::format("{} = {};  /* move */", reg_name(rd), reg_name(rs));
    }
    if (rs == 0) {
        return fmt::format("{} = {};  /* move */", reg_name(rd), reg_name(rt));
    }

    return fmt::format("{} = {} | {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_and(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} & {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_sll(uint32_t instr) {
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);
    uint32_t shamt = get_shamt(instr);

    // nop instruction (sll $zero, $zero, 0)
    if (rd == 0 && rt == 0 && shamt == 0) {
        return "/* nop */";
    }

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    if (shamt == 0) {
        return fmt::format("{} = {};  /* no shift */", reg_name(rd), reg_name(rt));
    }

    return fmt::format("{} = {} << {};", reg_name(rd), reg_name(rt), shamt);
}

std::string CodeGenerator::translate_srl(uint32_t instr) {
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);
    uint32_t shamt = get_shamt(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = (uint32_t){} >> {};", reg_name(rd), reg_name(rt), shamt);
}

std::string CodeGenerator::translate_slt(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = ((int32_t){} < (int32_t){}) ? 1 : 0;",
                      reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_sltu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = ({} < {}) ? 1 : 0;",
                      reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_addu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    // Optimize: move instruction (addu $rd, $rs, $zero or addu $rd, $zero, $rt)
    if (rt == 0) {
        return fmt::format("{} = {};  /* move */", reg_name(rd), reg_name(rs));
    }
    if (rs == 0) {
        return fmt::format("{} = {};  /* move */", reg_name(rd), reg_name(rt));
    }

    return fmt::format("{} = {} + {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_add(uint32_t instr) {
    // add and addu are functionally identical in practice (trap on overflow not used)
    return translate_addu(instr);
}

std::string CodeGenerator::translate_subu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} - {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_sub(uint32_t instr) {
    // sub and subu are functionally identical in practice (trap on overflow not used)
    return translate_subu(instr);
}

std::string CodeGenerator::translate_lb(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = (int32_t)(int8_t)cpu->read_byte({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = (int32_t)(int8_t)cpu->read_byte({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_lbu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = cpu->read_byte({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = cpu->read_byte({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_lh(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = (int32_t)(int16_t)cpu->read_half({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = (int32_t)(int16_t)cpu->read_half({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_lhu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = cpu->read_half({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = cpu->read_half({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_lwl(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    // LWL: Load Word Left - loads bytes into the most-significant portion of rt
    // addr = rs + offset; byte_pos = addr & 3
    // Result merges high bytes from aligned word with low bytes from current rt value
    // Generate a helper call: cpu->rt = cpu->lwl(addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("{} = cpu->lwl({}, {});", reg_name(rt), reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("{} = cpu->lwl({} + {}, {});",
                          reg_name(rt), reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_lwr(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    // LWR: Load Word Right - loads bytes into the least-significant portion of rt
    // addr = rs + offset; byte_pos = addr & 3
    // Result merges low bytes from aligned word with high bytes from current rt value
    // Generate a helper call: cpu->rt = cpu->lwr(addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("{} = cpu->lwr({}, {});", reg_name(rt), reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("{} = cpu->lwr({} + {}, {});",
                          reg_name(rt), reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_swl(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    // SWL: Store Word Left - stores rt into memory using addr, MSB side
    // Generate a helper call: cpu->swl(addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("cpu->swl({}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->swl({} + {}, {});",
                          reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_swr(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    // SWR: Store Word Right - stores rt into memory using addr, LSB side
    // Generate a helper call: cpu->swr(addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("cpu->swr({}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->swr({} + {}, {});",
                          reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_sb(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (offset == 0) {
        return fmt::format("cpu->write_byte({}, (uint8_t){});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->write_byte({} + {}, (uint8_t){});",
                          reg_name(rs), offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_sh(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (offset == 0) {
        return fmt::format("cpu->write_half({}, (uint16_t){});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->write_half({} + {}, (uint16_t){});",
                          reg_name(rs), offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_sra(uint32_t instr) {
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);
    uint32_t shamt = get_shamt(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = (int32_t){} >> {};", reg_name(rd), reg_name(rt), shamt);
}

std::string CodeGenerator::translate_andi(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint16_t imm = get_imm16_u(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} & 0x{:04X};", reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_ori(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint16_t imm = get_imm16_u(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    // Optimize: loading immediate into register (from $zero)
    if (rs == 0) {
        return fmt::format("{} = 0x{:04X};", reg_name(rt), imm);
    }

    return fmt::format("{} = {} | 0x{:04X};", reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_xori(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint16_t imm = get_imm16_u(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} ^ 0x{:04X};", reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_xor(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} ^ {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_nor(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    // Optimize: not instruction (nor $rd, $rs, $zero)
    if (rt == 0) {
        return fmt::format("{} = ~{};  /* not */", reg_name(rd), reg_name(rs));
    }
    if (rs == 0) {
        return fmt::format("{} = ~{};  /* not */", reg_name(rd), reg_name(rt));
    }

    return fmt::format("{} = ~({} | {});", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_sllv(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} << ({} & 0x1F);", reg_name(rd), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_srlv(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = (uint32_t){} >> ({} & 0x1F);", reg_name(rd), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_srav(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = (int32_t){} >> ({} & 0x1F);", reg_name(rd), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_slti(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t imm = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = ((int32_t){} < {}) ? 1 : 0;",
                      reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_sltiu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t imm = get_imm16(instr);  // Sign-extended for comparison

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = ({} < (uint32_t){}) ? 1 : 0;",
                      reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_mult(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    return fmt::format("{{ int64_t result = (int64_t)(int32_t){} * (int64_t)(int32_t){}; cpu->lo = (uint32_t)result; cpu->hi = (uint32_t)(result >> 32); }}",
                      reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_multu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    return fmt::format("{{ uint64_t result = (uint64_t){} * (uint64_t){}; cpu->lo = (uint32_t)result; cpu->hi = (uint32_t)(result >> 32); }}",
                      reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_div(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    // MIPS division: quotient in LO, remainder in HI
    // Handle division by zero (MIPS behavior: result is unpredictable but doesn't trap)
    return fmt::format("if ({} != 0) {{ cpu->lo = (uint32_t)((int32_t){} / (int32_t){}); cpu->hi = (uint32_t)((int32_t){} % (int32_t){}); }} else {{ cpu->lo = ((int32_t){} >= 0) ? 0xFFFFFFFF : 1; cpu->hi = (uint32_t){}; }}",
                      reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rs));
}

std::string CodeGenerator::translate_divu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    // MIPS division: quotient in LO, remainder in HI
    // Handle division by zero (MIPS behavior: result is unpredictable but doesn't trap)
    return fmt::format("if ({} != 0) {{ cpu->lo = {} / {}; cpu->hi = {} % {}; }} else {{ cpu->lo = 0xFFFFFFFF; cpu->hi = {}; }}",
                      reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_mfhi(uint32_t instr) {
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = cpu->hi;", reg_name(rd));
}

std::string CodeGenerator::translate_mflo(uint32_t instr) {
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = cpu->lo;", reg_name(rd));
}

std::string CodeGenerator::translate_mthi(uint32_t instr) {
    uint32_t rs = get_rs(instr);

    return fmt::format("cpu->hi = {};", reg_name(rs));
}

std::string CodeGenerator::translate_mtlo(uint32_t instr) {
    uint32_t rs = get_rs(instr);

    return fmt::format("cpu->lo = {};", reg_name(rs));
}

std::string CodeGenerator::generate_branch_condition(uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    // REGIMM branches (bltz, bgez, bltzal, bgezal)
    if (opcode == 0x01) {
        uint32_t regimm_op = (instr >> 16) & 0x1F;
        if (regimm_op == 0x00) { // bltz
            return fmt::format("(int32_t){} < 0", reg_name(rs));
        } else if (regimm_op == 0x01) { // bgez
            return fmt::format("(int32_t){} >= 0", reg_name(rs));
        } else if (regimm_op == 0x10) { // bltzal
            return fmt::format("(int32_t){} < 0", reg_name(rs));
        } else if (regimm_op == 0x11) { // bgezal
            return fmt::format("(int32_t){} >= 0", reg_name(rs));
        }
    }

    // Standard branches
    switch (opcode) {
        case 0x04: // beq
        case 0x14: // beql
            return fmt::format("{} == {}", reg_name(rs), reg_name(rt));

        case 0x05: // bne
        case 0x15: // bnel
            return fmt::format("{} != {}", reg_name(rs), reg_name(rt));

        case 0x06: // blez
        case 0x16: // blezl
            return fmt::format("(int32_t){} <= 0", reg_name(rs));

        case 0x07: // bgtz
        case 0x17: // bgtzl
            return fmt::format("(int32_t){} > 0", reg_name(rs));
    }

    return "0 /* unknown branch condition: defaults to not-taken */";
}

std::string CodeGenerator::translate_instruction(uint32_t addr, uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t funct = instr & 0x3F;

    // Instruction comment (if enabled)
    std::string comment;
    if (config_.emit_comments) {
        comment = fmt::format("  /* 0x{:08X}: 0x{:08X} */", addr, instr);
    }

    std::string code;

    // SPECIAL opcode (0x00)
    if (opcode == 0x00) {
        switch (funct) {
            case 0x00: code = translate_sll(instr); break;     // sll
            case 0x02: code = translate_srl(instr); break;     // srl
            case 0x03: code = translate_sra(instr); break;     // sra
            case 0x04: code = translate_sllv(instr); break;    // sllv
            case 0x06: code = translate_srlv(instr); break;    // srlv
            case 0x07: code = translate_srav(instr); break;    // srav
            case 0x08: code = "return;  /* jr $ra */"; break;  // jr (assume return)
            case 0x09:                                          // jalr
                {
                    uint32_t rs = get_rs(instr);
                    uint32_t rd = get_rd(instr);
                    code = fmt::format("{} = 0x{:08X};  cpu->pc = {};  /* jalr */",
                                      reg_name(rd), addr + 8, reg_name(rs));
                }
                break;
            case 0x0C:                                          // syscall
                {
                    uint32_t syscall_code = (instr >> 6) & 0xFFFFF;
                    code = fmt::format("psx_syscall(cpu, {});  /* syscall {} */", syscall_code, syscall_code);
                }
                break;
            case 0x0D:                                          // break
                {
                    uint32_t break_code = (instr >> 6) & 0xFFFFF;
                    code = fmt::format("/* break({}) — trap, no-op in recompiler */", break_code);
                }
                break;
            case 0x10: code = translate_mfhi(instr); break;    // mfhi
            case 0x11: code = translate_mthi(instr); break;    // mthi
            case 0x12: code = translate_mflo(instr); break;    // mflo
            case 0x13: code = translate_mtlo(instr); break;    // mtlo
            case 0x18: code = translate_mult(instr); break;    // mult
            case 0x19: code = translate_multu(instr); break;   // multu
            case 0x1A: code = translate_div(instr); break;     // div
            case 0x1B: code = translate_divu(instr); break;    // divu
            case 0x20: code = translate_add(instr); break;     // add
            case 0x21: code = translate_addu(instr); break;    // addu
            case 0x22: code = translate_sub(instr); break;     // sub
            case 0x23: code = translate_subu(instr); break;    // subu
            case 0x24: code = translate_and(instr); break;     // and
            case 0x25: code = translate_or(instr); break;      // or
            case 0x26: code = translate_xor(instr); break;     // xor
            case 0x27: code = translate_nor(instr); break;     // nor
            case 0x2A: code = translate_slt(instr); break;     // slt
            case 0x2B: code = translate_sltu(instr); break;    // sltu
            default:
                code = fmt::format("/* TODO: SPECIAL funct=0x{:02X} */", funct);
        }
    }
    // Immediate and load/store instructions
    else {
        switch (opcode) {
            case 0x08: code = translate_addiu(instr); break;  // addi (same as addiu)
            case 0x09: code = translate_addiu(instr); break;  // addiu
            case 0x0A: code = translate_slti(instr); break;   // slti
            case 0x0B: code = translate_sltiu(instr); break;  // sltiu
            case 0x0C: code = translate_andi(instr); break;   // andi
            case 0x0D: code = translate_ori(instr); break;    // ori
            case 0x0E: code = translate_xori(instr); break;   // xori
            case 0x0F: code = translate_lui(instr); break;    // lui
            case 0x10:                                         // COP0
                {
                    uint32_t cop_op = (instr >> 21) & 0x1F;
                    uint32_t rt = get_rt(instr);
                    uint32_t rd = get_rd(instr);
                    if (cop_op == 0x00) { // MFC0 - move from COP0
                        code = fmt::format("{} = cpu->cop0[{}];  /* mfc0 */", reg_name(rt), rd);
                    } else if (cop_op == 0x04) { // MTC0 - move to COP0
                        code = fmt::format("cpu->cop0[{}] = {};  /* mtc0 */", rd, reg_name(rt));
                    } else if (cop_op == 0x10 && (instr & 0x3F) == 0x10) { // RFE
                        // Restore interrupt enable bits: shift bits 5:2 → 3:0
                        code = "{ uint32_t sr = cpu->cop0[12]; "
                               "cpu->cop0[12] = (sr & 0xFFFFFFF0u) | ((sr >> 2) & 0x0Fu); }  /* rfe */";
                    } else {
                        code = fmt::format("/* cop0: 0x{:08X} */", instr);
                    }
                }
                break;
            case 0x12:                                         // COP2 (GTE)
                {
                    uint32_t cop_op = (instr >> 21) & 0x1F;
                    uint32_t rt = get_rt(instr);
                    uint32_t rd = get_rd(instr);
                    if (cop_op == 0x00) { // MFC2 - move from COP2 data
                        code = fmt::format("{} = cpu->gte_data[{}];  /* mfc2 */", reg_name(rt), rd);
                    } else if (cop_op == 0x02) { // CFC2 - move from COP2 control
                        code = fmt::format("{} = cpu->gte_ctrl[{}];  /* cfc2 */", reg_name(rt), rd);
                    } else if (cop_op == 0x04) { // MTC2 - move to COP2 data
                        code = fmt::format("cpu->gte_data[{}] = {};  /* mtc2 */", rd, reg_name(rt));
                    } else if (cop_op == 0x06) { // CTC2 - move to COP2 control
                        code = fmt::format("cpu->gte_ctrl[{}] = {};  /* ctc2 */", rd, reg_name(rt));
                    } else if ((cop_op & 0x10) != 0) { // GTE command (bit 25 set)
                        uint32_t gte_cmd = instr & 0x1FFFFFF;
                        // Route ALL GTE commands through gte_execute() for correct behavior
                        code = fmt::format("gte_execute(cpu, 0x{:07X});  /* gte cmd 0x{:02X} */", gte_cmd, gte_cmd & 0x3F);
                    } else {
                        code = fmt::format("/* cop2: 0x{:08X} */", instr);
                    }
                }
                break;
            case 0x20: code = translate_lb(instr); break;     // lb
            case 0x21: code = translate_lh(instr); break;     // lh
            case 0x22: code = translate_lwl(instr); break;    // lwl
            case 0x23: code = translate_lw(instr); break;     // lw
            case 0x24: code = translate_lbu(instr); break;    // lbu
            case 0x25: code = translate_lhu(instr); break;    // lhu
            case 0x26: code = translate_lwr(instr); break;    // lwr
            case 0x28: code = translate_sb(instr); break;     // sb
            case 0x29: code = translate_sh(instr); break;     // sh
            case 0x2A: code = translate_swl(instr); break;    // swl
            case 0x2B: code = translate_sw(instr); break;     // sw
            case 0x2E: code = translate_swr(instr); break;    // swr
            case 0x32:                                         // LWC2 - load word to COP2
                {
                    uint32_t rs = get_rs(instr);
                    uint32_t rt = get_rt(instr);
                    int16_t offset = get_imm16(instr);
                    if (offset == 0) {
                        code = fmt::format("cpu->gte_data[{}] = cpu->read_word({});  /* lwc2 gte[{}], ({}) */",
                                          rt, reg_name(rs), rt, reg_name(rs));
                    } else {
                        code = fmt::format("cpu->gte_data[{}] = cpu->read_word({} + {});  /* lwc2 gte[{}], {}({}) */",
                                          rt, reg_name(rs), offset, rt, offset, reg_name(rs));
                    }
                }
                break;
            case 0x3A:                                         // SWC2 - store word from COP2
                {
                    uint32_t rs = get_rs(instr);
                    uint32_t rt = get_rt(instr);
                    int16_t offset = get_imm16(instr);
                    if (offset == 0) {
                        code = fmt::format("cpu->write_word({}, cpu->gte_data[{}]);  /* swc2 gte[{}], ({}) */",
                                          reg_name(rs), rt, rt, reg_name(rs));
                    } else {
                        code = fmt::format("cpu->write_word({} + {}, cpu->gte_data[{}]);  /* swc2 gte[{}], {}({}) */",
                                          reg_name(rs), offset, rt, rt, offset, reg_name(rs));
                    }
                }
                break;
            default:
                code = fmt::format("/* TODO: opcode=0x{:02X} */", opcode);
        }
    }

    return config_.indent + code + comment;
}

std::string CodeGenerator::translate_basic_block(
    const BasicBlock& block,
    const ControlFlowGraph& cfg) {

    std::stringstream ss;

    // Block label
    ss << fmt::format("block_{:08X}:\n", block.start_addr);

    // Translate each instruction in the block
    uint32_t addr = block.start_addr;

    // Determine if the exit instruction uses a delay slot pattern.
    // Normal case: branch at end_addr-4, delay slot at end_addr.
    // Split-function edge case: exit instruction IS at end_addr (delay slot
    // is outside the function), so exit_branch_addr = end_addr not end_addr-4.
    bool exit_has_delay = (block.exit_instr.has_delay_slot &&
                           block.exit_instr.type != ControlFlowType::None);
    bool delay_slot_in_block = (exit_has_delay &&
                                block.exit_instr.address < block.end_addr);
    bool exit_uses_delay_slot = delay_slot_in_block;
    uint32_t exit_branch_addr;
    if (delay_slot_in_block) {
        exit_branch_addr = block.end_addr - 4;  // branch at end-4, delay slot at end
    } else if (exit_has_delay) {
        exit_branch_addr = block.end_addr;       // branch at end, delay slot outside
    } else {
        exit_branch_addr = block.end_addr;
    }

    while (addr <= block.end_addr) {
        auto instr_opt = exe_.read_word(addr);
        if (!instr_opt.has_value()) {
            break;
        }

        uint32_t instr = *instr_opt;

        // Skip the delay slot at end_addr - it's emitted as part of the branch handling below
        if (exit_uses_delay_slot && addr == block.end_addr) {
            addr += 4;
            break;
        }

        // Don't emit control flow instructions here (handled separately)
        bool is_cf = ControlFlowAnalyzer::is_control_flow(instr);

        // Emit inline label for mid-block jump table targets
        if (addr != block.start_addr && extra_labels_.count(addr)) {
            ss << fmt::format("block_{:08X}:;\n", addr);
        }

        // Instruction-level annotation (skipped at function start — already shown above signature)
        if (annotations_ && addr != cfg.function_start) {
            const std::string& inote = annotations_->lookup(addr);
            if (!inote.empty())
                ss << config_.indent << fmt::format("/* [NOTE] {} */\n", inote);
        }

        if (!is_cf) {
            ss << translate_instruction(addr, instr) << "\n";
        } else {
            // Control flow is handled at block exit
            if (addr == exit_branch_addr) {
                // MIPS delay slot handling: emit delay slot instruction BEFORE branch/jump
                std::string delay_saved_cond;  // non-empty if condition was pre-captured
                if (block.exit_instr.has_delay_slot) {
                    uint32_t delay_slot_addr = addr + 4;
                    auto delay_instr_opt = exe_.read_word(delay_slot_addr);

                    if (delay_instr_opt.has_value()) {
                        uint32_t delay_instr = *delay_instr_opt;

                        // For branch-likely variants, delay slot is conditional
                        if (block.exit_instr.is_likely) {
                            ss << config_.indent << "/* delay slot (likely) - conditional execution */\n";
                            ss << config_.indent << "if (" << generate_branch_condition(block.exit_instr.instruction) << ") {\n";
                            ss << config_.indent << translate_instruction(delay_slot_addr, delay_instr) << "\n";
                            ss << config_.indent << "}\n";
                        } else {
                            // Normal delay slot - always executes.
                            // In MIPS, the branch condition is evaluated at the branch
                            // instruction (before the delay slot). If the delay slot
                            // modifies a condition register we must save the result first.
                            if (block.exit_instr.type == ControlFlowType::Branch) {
                                std::string cond = generate_branch_condition(block.exit_instr.instruction);
                                delay_saved_cond = fmt::format("_bc_{:08X}", addr);
                                ss << config_.indent
                                   << fmt::format("int {} = ({});  /* save branch cond before delay slot */\n",
                                                  delay_saved_cond, cond);
                            }
                            ss << config_.indent << "/* delay slot (always executes) */\n";
                            ss << translate_instruction(delay_slot_addr, delay_instr) << "\n";
                        }
                    }
                }

                // Now emit the branch/jump
                if (block.exit_instr.type == ControlFlowType::Branch) {
                    // Check if this is a branch-and-link (bgezal/bltzal)
                    // For REGIMM (opcode=0x01), rt=0x10 (bltzal) or rt=0x11 (bgezal)
                    // The link always happens unconditionally (even if branch not taken)
                    {
                        uint32_t branch_instr = block.exit_instr.instruction;
                        uint32_t b_opcode = (branch_instr >> 26) & 0x3F;
                        if (b_opcode == 0x01) {
                            uint32_t regimm_op = (branch_instr >> 16) & 0x1F;
                            if (regimm_op == 0x10 || regimm_op == 0x11) {
                                // bltzal or bgezal: link register always set
                                ss << config_.indent
                                   << fmt::format("cpu->ra = 0x{:08X};  /* branch-and-link return addr */\n", addr + 8);
                            }
                        }
                    }
                    // Conditional branch - use pre-captured condition if available
                    std::string condition = delay_saved_cond.empty()
                        ? generate_branch_condition(block.exit_instr.instruction)
                        : delay_saved_cond;
                    if (block.successors.size() == 2) {
                        ss << config_.indent << "if (" << condition << ") {\n";
                        ss << config_.indent << config_.indent
                           << fmt::format("goto block_{:08X};  /* taken */\n", block.successors[0]);
                        ss << config_.indent << "} else {\n";
                        ss << config_.indent << config_.indent
                           << fmt::format("goto block_{:08X};  /* not taken */\n", block.successors[1]);
                        ss << config_.indent << "}\n";
                    } else {
                        // Split-function: one or both branch targets are outside
                        // this function piece. Emit function calls for out-of-function
                        // targets to reconnect the split pieces.
                        uint32_t branch_target = block.exit_instr.target;
                        uint32_t fall_through_addr = block.exit_instr.address + 8;
                        bool target_in = cfg.blocks.count(branch_target) > 0;
                        bool fallthru_in = cfg.blocks.count(fall_through_addr) > 0;

                        ss << config_.indent << "if (" << condition << ") {\n";
                        if (target_in) {
                            ss << config_.indent << config_.indent
                               << fmt::format("goto block_{:08X};  /* taken */\n", branch_target);
                        } else if (known_functions_.count(branch_target)) {
                            ss << config_.indent << config_.indent
                               << fmt::format("func_{:08X}(cpu); return;  /* taken: split piece */\n", branch_target);
                        } else {
                            ss << config_.indent << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* taken: split (mid-func) */\n", branch_target);
                        }
                        ss << config_.indent << "} else {\n";
                        if (fallthru_in) {
                            ss << config_.indent << config_.indent
                               << fmt::format("goto block_{:08X};  /* not taken */\n", fall_through_addr);
                        } else if (known_functions_.count(fall_through_addr)) {
                            ss << config_.indent << config_.indent
                               << fmt::format("func_{:08X}(cpu); return;  /* not taken: split piece */\n", fall_through_addr);
                        } else {
                            ss << config_.indent << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* not taken: split (mid-func) */\n", fall_through_addr);
                        }
                        ss << config_.indent << "}\n";
                    }
                } else if (block.exit_instr.type == ControlFlowType::Jump) {
                    // Unconditional jump
                    if (!block.successors.empty()) {
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* j */\n", block.successors[0]);
                    } else if (block.exit_instr.target != 0 && known_functions_.count(block.exit_instr.target)) {
                        // Jump target is out-of-function and is a known function start
                        ss << config_.indent
                           << fmt::format("func_{:08X}(cpu); return;  /* j to split piece */\n",
                                          block.exit_instr.target);
                    } else if (block.exit_instr.target != 0) {
                        // Jump target is a mid-function address — dispatch dynamically
                        ss << config_.indent
                           << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* j to split (mid-func) */\n",
                                          block.exit_instr.target);
                    }
                } else if (block.exit_instr.type == ControlFlowType::Return) {
                    // Return instruction
                    ss << config_.indent << "return;  /* jr $ra */\n";
                } else if (block.exit_instr.type == ControlFlowType::JumpRegister) {
                    // jr $rs — may be: BIOS call (jr $t2, t2=0xA0/0xB0/0xC0)
                    //                  or: jump table (jr $v0, v0 loaded from LW+ADDU+LUI)
                    uint32_t jr_rs = get_rs(block.exit_instr.instruction);

                    // Detect MIPS jump-table pattern by scanning backwards from jr.
                    // Handles two common patterns:
                    //   P1: lui $R,hi; addiu $R,$R,lo; addu $D,$idx,$R; lw $D,0($D)
                    //   P2: lui $R,hi;                 addu $D,$R,$idx; lw $D,off($D)
                    // table_base = lui_val + addiu_val + lw_offset
                    uint32_t table_base  = 0;
                    uint32_t table_count = 0;
                    {
                        uint32_t lw_base_reg   = 0xFF;   // base reg in the LW
                        int32_t  lw_offset     = 0;      // offset in the LW instruction
                        uint32_t addu_cand[2]  = {0xFF, 0xFF}; // rs, rt of ADDU
                        uint32_t table_reg     = 0xFF;   // candidate for table base reg
                        uint32_t lui_val       = 0;
                        int16_t  addiu_val[2]  = {0, 0}; // per-candidate ADDIU
                        bool     found_addiu[2]= {false, false};
                        bool     found_lui     = false;

                        for (int back = 1; back <= 40; back++) {
                            uint32_t sa = block.exit_instr.address - (uint32_t)(back * 4);
                            if (sa < cfg.function_start) break;
                            auto sopt = exe_.read_word(sa);
                            if (!sopt) break;
                            uint32_t si   = *sopt;
                            uint32_t s_op = (si >> 26) & 0x3F;
                            uint32_t s_rs = (si >> 21) & 0x1F;
                            uint32_t s_rt = (si >> 16) & 0x1F;
                            uint32_t s_rd = (si >> 11) & 0x1F;
                            uint32_t s_fn = si & 0x3F;

                            // LW $jr_reg, offset($base)
                            if (s_op == 0x23 && s_rt == jr_rs && lw_base_reg == 0xFF) {
                                lw_base_reg = s_rs;
                                lw_offset   = (int32_t)(int16_t)(si & 0xFFFF);
                                continue;
                            }
                            // ADDU $lw_base, $a, $b  — save both operands as candidates
                            if (s_op == 0x00 && s_fn == 0x21 &&
                                s_rd == lw_base_reg && lw_base_reg != 0xFF &&
                                addu_cand[0] == 0xFF) {
                                addu_cand[0] = s_rs;
                                addu_cand[1] = s_rt;
                                table_reg    = s_rt;  // initial guess; may flip on LUI match
                                continue;
                            }
                            // ADDIU $cand, $cand, imm  (scanning back → before LUI)
                            if (s_op == 0x09 && addu_cand[0] != 0xFF) {
                                for (int c = 0; c < 2; c++) {
                                    if (!found_addiu[c] && addu_cand[c] != 0xFF &&
                                        s_rs == addu_cand[c] && s_rt == addu_cand[c]) {
                                        addiu_val[c]   = (int16_t)(si & 0xFFFF);
                                        found_addiu[c] = true;
                                        break;
                                    }
                                }
                                continue;
                            }
                            // LUI $cand, hi  — determines which candidate is the table base
                            if (s_op == 0x0F && addu_cand[0] != 0xFF && !found_lui) {
                                for (int c = 0; c < 2; c++) {
                                    if (addu_cand[c] != 0xFF && s_rt == addu_cand[c]) {
                                        lui_val   = ((uint32_t)(si & 0xFFFF)) << 16;
                                        table_reg = addu_cand[c];
                                        // carry over the ADDIU for this candidate
                                        if (!found_addiu[c]) { addiu_val[c] = 0; }
                                        // store addiu in slot 0 for convenience
                                        addiu_val[0]   = addiu_val[c];
                                        found_addiu[0] = found_addiu[c];
                                        found_lui      = true;
                                        break;
                                    }
                                }
                                continue;
                            }
                            // SLTIU — table entry count
                            if (s_op == 0x0B && table_count == 0) {
                                table_count = si & 0xFFFF;
                                continue;
                            }
                            if (found_lui && table_count != 0) break;
                        }

                        if (found_lui && table_count > 0 && table_count < 512) {
                            uint32_t base = lui_val +
                                (found_addiu[0] ? (uint32_t)(int32_t)addiu_val[0] : 0u);
                            table_base = base + (uint32_t)lw_offset;
                        }
                    }

                    // If we found a valid table, read entries and emit switch
                    bool emitted_switch = false;
                    if (table_base != 0 && table_count > 0) {
                        std::set<uint32_t>    seen;
                        std::vector<uint32_t> targets;
                        for (uint32_t i = 0; i < table_count; i++) {
                            auto eopt = exe_.read_word(table_base + i * 4);
                            if (eopt && seen.insert(*eopt).second &&
                                (cfg.blocks.count(*eopt) || extra_labels_.count(*eopt))) {
                                targets.push_back(*eopt);
                            }
                        }
                        if (!targets.empty()) {
                            ss << config_.indent << fmt::format("/* jump table 0x{:08X}, {} entries */\n", table_base, table_count);
                            ss << config_.indent << fmt::format("switch ({}) {{\n", reg_name(jr_rs));
                            for (uint32_t t : targets) {
                                ss << config_.indent << fmt::format("    case 0x{:08X}u: goto block_{:08X};\n", t, t);
                            }
                            ss << config_.indent << "    default: call_by_address(cpu, " << reg_name(jr_rs) << "); return;\n";
                            ss << config_.indent << "}\n";
                            emitted_switch = true;
                        }
                    }
                    if (!emitted_switch) {
                        // BIOS call or unrecognised indirect jump
                        ss << config_.indent << fmt::format("call_by_address(cpu, {});  /* jr {} */\n",
                                                            reg_name(jr_rs), reg_name(jr_rs));
                        ss << config_.indent << "return;\n";
                    }
                } else if (block.exit_instr.type == ControlFlowType::JumpLink) {
                    // Function call (jal)
                    ss << config_.indent << "cpu->ra = " << fmt::format("0x{:08X};  /* return address */\n", addr + 8);
                    uint32_t target = block.exit_instr.target;
                    if (known_functions_.count(target) > 0) {
                        ss << config_.indent << fmt::format("func_{:08X}(cpu);  /* jal */\n", target);
                    } else {
                        ss << config_.indent << fmt::format("call_by_address(cpu, 0x{:08X}u);  /* external jal */\n", target);
                    }
                    if (!block.successors.empty()) {
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* continue after call */\n", block.successors[0]);
                    } else {
                        // Split-function: JAL continuation is outside this function piece.
                        // Tail-call to the continuation piece (at exit_addr + 8, past delay slot).
                        uint32_t cont_addr = block.exit_instr.address + 8;
                        if (known_functions_.count(cont_addr)) {
                            ss << config_.indent
                               << fmt::format("func_{:08X}(cpu); return;  /* jal cont: split piece */\n", cont_addr);
                        } else {
                            ss << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* jal cont: split */\n", cont_addr);
                        }
                    }
                } else if (block.exit_instr.type == ControlFlowType::JumpLinkReg) {
                    // Register indirect call (jalr $rs, $rd) — call function at rs, return to rd
                    uint32_t rs = get_rs(block.exit_instr.instruction);
                    uint32_t rd = get_rd(block.exit_instr.instruction);

                    // Set link register to return address (PC + 8, past delay slot)
                    ss << config_.indent << fmt::format("{} = 0x{:08X};  /* jalr return addr */\n",
                                                        reg_name(rd), addr + 8);

                    // Dispatch indirect call — target is a runtime register value
                    ss << config_.indent << fmt::format("call_by_address(cpu, {});  /* jalr {} */\n",
                                                        reg_name(rs), reg_name(rs));

                    // Continue to successor block (instruction at return address)
                    if (!block.successors.empty()) {
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* continue after jalr */\n",
                                          block.successors[0]);
                    } else {
                        // Split-function: JALR continuation is outside this function piece.
                        uint32_t cont_addr = block.exit_instr.address + 8;
                        if (known_functions_.count(cont_addr)) {
                            ss << config_.indent
                               << fmt::format("func_{:08X}(cpu); return;  /* jalr cont: split piece */\n", cont_addr);
                        } else {
                            ss << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* jalr cont: split */\n", cont_addr);
                        }
                    }
                }
            }
        }

        addr += 4;
    }

    // If no explicit control flow, fall through to next block
    if (block.exit_instr.type == ControlFlowType::None && !block.successors.empty()) {
        ss << config_.indent << fmt::format("/* fall through to block_{:08X} */\n",
                                           block.successors[0]);
    }

    return ss.str();
}

GeneratedFunction CodeGenerator::generate_function(
    const Function& func,
    const ControlFlowGraph& cfg,
    const std::string& fallthrough_name) {

    GeneratedFunction result;
    result.function_name = func.name;
    result.signature = fmt::format("void {}(CPUState* cpu)", func.name);

    std::stringstream body_ss;
    body_ss << "{\n";

    // Override preamble: allows external code to intercept this function
    // without patching the generated file. psx_override_dispatch() is
    // implemented in tomba_runtime.c and checks the registered override table.
    body_ss << fmt::format("    if (psx_override_dispatch(cpu, 0x{:08X}u)) return;\n",
                           func.start_addr);

    // Add function comment
    if (config_.emit_comments) {
        body_ss << config_.indent
                << fmt::format("/* Address: 0x{:08X}, Size: {} bytes, Blocks: {} */\n",
                              func.start_addr, func.size, cfg.blocks.size());
    }

    // Pre-scan: find jump table targets that land mid-block (not a block boundary).
    // These need inline labels so goto statements in the switch can reach them.
    extra_labels_.clear();
    for (const auto& [baddr, blk] : cfg.blocks) {
        if (blk.exit_instr.type != ControlFlowType::JumpRegister) continue;
        uint32_t jr_r = get_rs(blk.exit_instr.instruction);
        // Run same backward scan as translate_basic_block to find table_base/count
        uint32_t tb = 0, tc = 0;
        {
            uint32_t lw_base = 0xFF; int32_t lw_off = 0;
            uint32_t ac[2] = {0xFF,0xFF}; uint32_t lui_v = 0;
            int16_t av[2] = {0,0}; bool fa[2] = {false,false}, fl = false;
            for (int b = 1; b <= 40; b++) {
                uint32_t sa = blk.exit_instr.address - (uint32_t)(b*4);
                if (sa < cfg.function_start) break;
                auto so = exe_.read_word(sa); if (!so) break;
                uint32_t si=*so, s_op=(si>>26)&0x3F, s_rs=(si>>21)&0x1F,
                         s_rt=(si>>16)&0x1F, s_rd=(si>>11)&0x1F, s_fn=si&0x3F;
                if (s_op==0x23 && s_rt==jr_r && lw_base==0xFF) { lw_base=s_rs; lw_off=(int32_t)(int16_t)(si&0xFFFF); continue; }
                if (s_op==0x00 && s_fn==0x21 && s_rd==lw_base && lw_base!=0xFF && ac[0]==0xFF) { ac[0]=s_rs; ac[1]=s_rt; continue; }
                if (s_op==0x09 && ac[0]!=0xFF) { for(int c=0;c<2;c++){if(!fa[c]&&ac[c]!=0xFF&&s_rs==ac[c]&&s_rt==ac[c]){av[c]=(int16_t)(si&0xFFFF);fa[c]=true;break;}} continue; }
                if (s_op==0x0F && ac[0]!=0xFF && !fl) { for(int c=0;c<2;c++){if(ac[c]!=0xFF&&s_rt==ac[c]){lui_v=((uint32_t)(si&0xFFFF))<<16;if(!fa[c]){av[c]=0;}av[0]=av[c];fa[0]=fa[c];fl=true;break;}} continue; }
                if (s_op==0x0B && tc==0) { tc=si&0xFFFF; continue; }
                if (fl && tc!=0) break;
            }
            if (fl && tc>0 && tc<512) tb = (lui_v+(fa[0]?(uint32_t)(int32_t)av[0]:0u)) + (uint32_t)lw_off;
        }
        if (tb==0 || tc==0) continue;
        for (uint32_t i = 0; i < tc; i++) {
            auto eopt = exe_.read_word(tb + i*4);
            if (!eopt) continue;
            uint32_t t = *eopt;
            if (t >= cfg.function_start && t < cfg.function_end && !cfg.blocks.count(t))
                extra_labels_.insert(t);
        }
    }

    // Generate code for each basic block
    for (uint32_t block_addr : cfg.block_order) {
        const BasicBlock& block = cfg.blocks.at(block_addr);
        body_ss << "\n" << translate_basic_block(block, cfg);
    }

    // Emit fallthrough call if the function ends without explicit control flow.
    // A MIPS function truly falls through when:
    //   1. It has NO jr/jr $ra block anywhere (no explicit return or indirect jump)
    //   2. Its last basic block (by address) ends with no control flow (ControlFlowType::None)
    //      OR it ends with a branch/jump whose targets are all outside this function
    //      (indicating a split-function where the branch targets are in the next piece)
    // Functions that have dead code after a jr $ra (e.g., padding) must NOT get a
    // fallthrough call — condition 1 guards against that.
    if (!fallthrough_name.empty() && !cfg.block_order.empty()) {
        const BasicBlock& last_block = cfg.blocks.at(cfg.block_order.back());
        bool needs_fallthrough = false;

        if (last_block.exit_instr.type == ControlFlowType::None) {
            needs_fallthrough = true;
        } else if ((last_block.exit_instr.type == ControlFlowType::Branch ||
                    last_block.exit_instr.type == ControlFlowType::Jump) &&
                   last_block.successors.empty()) {
            // Branch/jump with no in-function successors — likely a split-function bug.
            // All targets are in the next function piece. Emit fallthrough as a safety net.
            fmt::print("  WARNING: func_{:08X} has out-of-function {} at block_{:08X}, "
                       "emitting fallthrough to {}\n",
                       func.start_addr,
                       last_block.exit_instr.type == ControlFlowType::Branch ? "branch" : "jump",
                       last_block.start_addr, fallthrough_name);
            needs_fallthrough = true;
        }

        if (needs_fallthrough) {
            // Emit fallthrough if the last block is reachable (has predecessors
            // or is the entry block). Dead code after a return (e.g., padding
            // nops) has no predecessors and should NOT get a fallthrough call.
            const BasicBlock& last = cfg.blocks.at(cfg.block_order.back());
            bool is_reachable = last.is_entry || !last.predecessors.empty();
            if (is_reachable) {
                body_ss << fmt::format("    {}(cpu);  /* fallthrough to next function */\n",
                                       fallthrough_name);
            }
        }
    }
    body_ss << "    ;  /* label compatibility: C requires a statement after the last label */\n";
    body_ss << "}\n";

    result.body = body_ss.str();

    // Prepend annotation comment if one exists for this function's address
    std::string annotation_prefix;
    if (annotations_) {
        const std::string& note = annotations_->lookup(func.start_addr);
        if (!note.empty())
            annotation_prefix = fmt::format("/* [NOTE] {} */\n", note);
    }

    result.full_code = annotation_prefix + result.signature + "\n" + result.body;
    result.line_count = std::count(result.full_code.begin(), result.full_code.end(), '\n');

    return result;
}

std::vector<GeneratedFunction> CodeGenerator::generate_all_functions(
    const std::vector<Function>& functions,
    const std::map<uint32_t, ControlFlowGraph>& cfgs) {

    std::vector<GeneratedFunction> results;
    results.reserve(functions.size());

    fmt::print("\n=== C Code Generation ===\n\n");
    fmt::print("Generating C code for {} functions...\n", functions.size());

    // Build address -> function name map for fallthrough detection.
    // When a function ends without explicit control flow (ControlFlowType::None)
    // and the very next address is another function's start, it's a MIPS
    // fall-through: the recompiler must emit a tail call to the continuation.
    std::map<uint32_t, std::string> func_name_by_addr;
    for (const Function& f : functions) {
        func_name_by_addr[f.start_addr] = f.name;
    }

    int total_lines = 0;

    for (const Function& func : functions) {
        if (func.is_data_section) {
            // Emit a stub for data sections - do not decode instructions
            GeneratedFunction stub;
            stub.function_name = func.name;
            stub.signature = fmt::format("void {}(CPUState* cpu)", func.name);
            stub.body = fmt::format(
                "{{\n    if (psx_override_dispatch(cpu, 0x{:08X}u)) return;\n    /* data section 0x{:08X}: {} bytes - not executable code */\n}}\n",
                func.start_addr, func.start_addr, func.size);
            stub.full_code = stub.signature + "\n" + stub.body;
            stub.line_count = 4;
            results.push_back(stub);
            continue;
        }

        if (cfgs.count(func.start_addr) == 0) {
            continue; // Skip if no CFG
        }

        const ControlFlowGraph& cfg = cfgs.at(func.start_addr);

        // Detect fallthrough: is there a function immediately after this one?
        std::string fallthrough_name;
        uint32_t next_addr = func.start_addr + func.size;
        auto ft_it = func_name_by_addr.find(next_addr);
        if (ft_it != func_name_by_addr.end()) {
            fallthrough_name = ft_it->second;
        }

        GeneratedFunction gen_func = generate_function(func, cfg, fallthrough_name);
        total_lines += gen_func.line_count;
        results.push_back(gen_func);
    }

    fmt::print("✓ Generated {} functions\n", results.size());
    fmt::print("✓ Total C code: {} lines\n\n", total_lines);

    return results;
}

std::string CodeGenerator::generate_file(
    const std::vector<Function>& functions,
    const std::map<uint32_t, ControlFlowGraph>& cfgs) {

    std::stringstream ss;

    // File header
    ss << "/* Generated by PSXRecomp - PlayStation 1 Static Recompiler */\n\n";

    // Include the generic PSX runtime header.
    // This provides: CPUState typedef, gte_execute, call_by_address,
    // psx_syscall, psx_override_dispatch, and all other runtime API.
    ss << "#include \"psx_runtime.h\"\n\n";

    // Emit reference implementations for unaligned memory helpers.
    // These implement the MIPS lwl/lwr/swl/swr semantics.
    // The runtime may override these by setting the corresponding function pointers
    // in CPUState, but these standalone functions provide correct reference behavior.
    ss << "/* --- Unaligned memory access helper implementations --- */\n";
    ss << "\n";
    ss << "/* LWL: Load Word Left - merges high bytes from aligned word into rt.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the current value of the destination register.\n";
    ss << " * MIPS big-endian convention: byte 0 is the most-significant byte.\n";
    ss << " * The PSX (little-endian) maps addr&3==0 -> load entire word,\n";
    ss << " *   addr&3==1 -> load 3 bytes into high 24 bits, etc.\n";
    ss << " * Formula:\n";
    ss << " *   aligned_addr = addr & ~3;\n";
    ss << " *   shift        = (addr & 3) * 8;\n";
    ss << " *   mask         = 0xFFFFFFFF << shift;\n";
    ss << " *   result       = (rt_value & ~mask) | (word << shift);\n";
    ss << " *   where word   = read_word(aligned_addr).\n";
    ss << " */\n";
    ss << "static uint32_t psx_lwl(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t word  = cpu->read_word(aligned_addr);\n";
    ss << "    uint32_t shift = (addr & 3u) * 8u;\n";
    ss << "    uint32_t mask  = 0xFFFFFFFFu << shift;\n";
    ss << "    return (rt_value & ~mask) | (word << shift);\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* LWR: Load Word Right - merges low bytes from aligned word into rt.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the current value of the destination register.\n";
    ss << " * Formula:\n";
    ss << " *   aligned_addr = addr & ~3;\n";
    ss << " *   shift        = (3 - (addr & 3)) * 8;\n";
    ss << " *   mask         = 0xFFFFFFFF >> shift;\n";
    ss << " *   result       = (rt_value & ~mask) | (word >> shift);\n";
    ss << " *   where word   = read_word(aligned_addr).\n";
    ss << " */\n";
    ss << "static uint32_t psx_lwr(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t word  = cpu->read_word(aligned_addr);\n";
    ss << "    uint32_t shift = (3u - (addr & 3u)) * 8u;\n";
    ss << "    uint32_t mask  = 0xFFFFFFFFu >> shift;\n";
    ss << "    return (rt_value & ~mask) | (word >> shift);\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* SWL: Store Word Left - stores the high bytes of rt_value into memory.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the value of the source register.\n";
    ss << " * Formula:\n";
    ss << " *   aligned_addr  = addr & ~3;\n";
    ss << " *   shift         = (addr & 3) * 8;\n";
    ss << " *   mem_mask      = 0xFFFFFFFF >> (32 - shift)  [i.e. low 'shift' bits preserved]\n";
    ss << " *   Actually:\n";
    ss << " *   mask          = 0xFFFFFFFF >> shift;  [bits to take from rt]\n";
    ss << " *   write_word(aligned_addr,\n";
    ss << " *       (read_word(aligned_addr) & ~mask) | (rt_value >> shift));\n";
    ss << " */\n";
    ss << "static void psx_swl(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t existing = cpu->read_word(aligned_addr);\n";
    ss << "    uint32_t shift = (addr & 3u) * 8u;\n";
    ss << "    uint32_t mask  = 0xFFFFFFFFu >> shift;\n";
    ss << "    cpu->write_word(aligned_addr, (existing & ~mask) | (rt_value >> shift));\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* SWR: Store Word Right - stores the low bytes of rt_value into memory.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the value of the source register.\n";
    ss << " * Formula:\n";
    ss << " *   aligned_addr  = addr & ~3;\n";
    ss << " *   shift         = (3 - (addr & 3)) * 8;\n";
    ss << " *   mask          = 0xFFFFFFFF << shift;\n";
    ss << " *   write_word(aligned_addr,\n";
    ss << " *       (read_word(aligned_addr) & ~mask) | (rt_value << shift));\n";
    ss << " */\n";
    ss << "static void psx_swr(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t existing = cpu->read_word(aligned_addr);\n";
    ss << "    uint32_t shift = (3u - (addr & 3u)) * 8u;\n";
    ss << "    uint32_t mask  = 0xFFFFFFFFu << shift;\n";
    ss << "    cpu->write_word(aligned_addr, (existing & ~mask) | (rt_value << shift));\n";
    ss << "}\n";
    ss << "\n";

    // Make mutable copies for potential function splitting
    std::vector<Function> functions_mut(functions);
    std::map<uint32_t, ControlFlowGraph> cfgs_mut(cfgs);

    // Build initial set of known functions
    std::set<uint32_t> known_addrs;
    for (const auto& func : functions_mut) {
        known_addrs.insert(func.start_addr);
    }

    // ---- Pre-pass: find mid-function branch/jump targets and split ----
    // Scan all CFGs for branch/jump targets that fall within a function's
    // range but are NOT at any function start. Split containing functions
    // at discovered targets. Iterate up to 3 times since splitting creates
    // new pieces whose branches may target other mid-piece addresses.
    // Each subsequent pass only scans CFGs that were rebuilt in the prior pass.
    {
        std::set<uint32_t> cfgs_to_scan;  // Which CFGs to scan (empty = all)
        uint32_t exe_start = exe_.header.load_address;
        uint32_t exe_end = exe_.end_address();
        int total_new = 0;

        for (int pass = 0; pass < 3; pass++) {
            std::set<uint32_t> mid_targets;

            // Scan either all CFGs (pass 0) or only rebuilt CFGs (pass 1+)
            auto scan_cfg = [&](const ControlFlowGraph& cfg) {
                for (const auto& [baddr, block] : cfg.blocks) {
                    auto check_target = [&](uint32_t target) {
                        if (target == 0) return;
                        if (cfg.blocks.count(target)) return;
                        if (known_addrs.count(target)) return;
                        if (target < exe_start || target >= exe_end) return;
                        if (target & 3) return;
                        mid_targets.insert(target);
                    };

                    if (block.exit_instr.type == ControlFlowType::Branch) {
                        check_target(block.exit_instr.target);
                        check_target(block.exit_instr.address + 8);
                    } else if (block.exit_instr.type == ControlFlowType::Jump) {
                        check_target(block.exit_instr.target);
                    }
                }
            };

            if (cfgs_to_scan.empty()) {
                for (const auto& [cfg_addr, cfg] : cfgs_mut) scan_cfg(cfg);
            } else {
                for (uint32_t addr : cfgs_to_scan) {
                    auto it = cfgs_mut.find(addr);
                    if (it != cfgs_mut.end()) scan_cfg(it->second);
                }
            }

            if (mid_targets.empty()) break;

            fmt::print("Pre-pass {}: {} mid-function branch targets\n", pass + 1, mid_targets.size());

            // Sort functions for binary search
            std::sort(functions_mut.begin(), functions_mut.end(),
                [](const Function& a, const Function& b) { return a.start_addr < b.start_addr; });
            std::vector<uint32_t> func_starts;
            for (const auto& f : functions_mut) func_starts.push_back(f.start_addr);

            // Group targets by containing function
            std::map<uint32_t, std::vector<uint32_t>> splits_by_func;
            for (uint32_t target : mid_targets) {
                auto it = std::upper_bound(func_starts.begin(), func_starts.end(), target);
                if (it == func_starts.begin()) continue;
                --it;
                splits_by_func[*it].push_back(target);
            }

            // Split each affected function
            std::vector<Function> new_funcs;
            std::set<uint32_t> affected;

            for (auto& [func_start, targets] : splits_by_func) {
                std::sort(targets.begin(), targets.end());

                for (auto& f : functions_mut) {
                    if (f.start_addr != func_start) continue;
                    if (targets.back() >= f.end_addr) break;

                    uint32_t orig_end = f.end_addr;
                    bool orig_data = f.is_data_section;

                    f.end_addr = targets[0];
                    f.size = f.end_addr - f.start_addr;
                    affected.insert(f.start_addr);

                    for (size_t ti = 0; ti < targets.size(); ti++) {
                        Function nf;
                        nf.start_addr = targets[ti];
                        nf.end_addr = (ti + 1 < targets.size()) ? targets[ti + 1] : orig_end;
                        nf.size = nf.end_addr - nf.start_addr;
                        nf.name = fmt::format("func_{:08X}", nf.start_addr);
                        nf.has_prologue = false;
                        nf.has_epilogue = false;
                        nf.stack_frame_size = 0;
                        nf.is_data_section = orig_data;
                        new_funcs.push_back(nf);
                        known_addrs.insert(nf.start_addr);
                        affected.insert(nf.start_addr);
                    }
                    break;
                }
            }

            functions_mut.insert(functions_mut.end(), new_funcs.begin(), new_funcs.end());
            std::sort(functions_mut.begin(), functions_mut.end(),
                [](const Function& a, const Function& b) { return a.start_addr < b.start_addr; });

            // Rebuild CFGs for affected functions
            ControlFlowAnalyzer cfg_analyzer(exe_);
            cfgs_to_scan.clear();
            for (auto& f : functions_mut) {
                if (affected.count(f.start_addr)) {
                    cfgs_mut[f.start_addr] = cfg_analyzer.analyze_function(f);
                    cfgs_to_scan.insert(f.start_addr);
                }
            }

            total_new += (int)new_funcs.size();
            fmt::print("  Created {} new entry points\n", new_funcs.size());
        }

        if (total_new > 0) {
            fmt::print("Pre-pass total: {} new entry points, {} functions now\n",
                       total_new, functions_mut.size());
        }
    }

    set_known_functions(known_addrs);

    // Generate all functions first (to collect names)
    auto gen_funcs = generate_all_functions(functions_mut, cfgs_mut);

    // Forward declarations for all functions
    if (!gen_funcs.empty()) {
        ss << "/* Forward declarations */\n";
        for (const auto& gen_func : gen_funcs) {
            ss << gen_func.signature << ";\n";
        }
        ss << "\n";
    }

    // Function definitions
    for (const auto& gen_func : gen_funcs) {
        ss << gen_func.full_code << "\n";
    }

    return ss.str();
}

} // namespace PSXRecomp
