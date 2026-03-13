#include "mips_decoder.h"

namespace PSXRecomp {

// Register names (MIPS O32 ABI)
static const char* kRegisterNames[32] = {
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0",   "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0",   "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8",   "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra"
};

const char* MipsDecoder::register_name(uint8_t reg) {
    if (reg >= 32) return "$??";
    return kRegisterNames[reg];
}

bool MipsDecoder::is_nop(uint32_t instr) {
    return instr == 0;
}

uint32_t MipsDecoder::compute_branch_target(uint32_t address, int16_t imm16) {
    // Branch target = PC + 4 + (sign_extended_offset << 2)
    return address + 4u + (uint32_t)((int32_t)imm16 << 2);
}

uint32_t MipsDecoder::compute_jump_target(uint32_t address, uint32_t target26) {
    // Jump target = ((PC+4) & 0xF0000000) | (target26 << 2)
    return ((address + 4u) & 0xF0000000u) | (target26 << 2u);
}

const char* MipsDecoder::get_mnemonic(const DecodedInstruction& instr) {
    return instr.mnemonic ? instr.mnemonic : "???";
}

// ---------------------------------------------------------------------------
// SPECIAL opcode (opcode == 0): funct field [5:0] determines operation
// ---------------------------------------------------------------------------
void MipsDecoder::decode_special(DecodedInstruction& out) {
    out.format = InstrFormat::SPECIAL;
    switch (out.funct) {
        case 0x00: out.mnemonic = "SLL";     break;
        case 0x02: out.mnemonic = "SRL";     break;
        case 0x03: out.mnemonic = "SRA";     break;
        case 0x04: out.mnemonic = "SLLV";    break;
        case 0x06: out.mnemonic = "SRLV";    break;
        case 0x07: out.mnemonic = "SRAV";    break;
        case 0x08: out.mnemonic = "JR";      break;
        case 0x09: out.mnemonic = "JALR";    break;
        case 0x0C: out.mnemonic = "SYSCALL"; break;
        case 0x0D: out.mnemonic = "BREAK";   break;
        case 0x10: out.mnemonic = "MFHI";    break;
        case 0x11: out.mnemonic = "MTHI";    break;
        case 0x12: out.mnemonic = "MFLO";    break;
        case 0x13: out.mnemonic = "MTLO";    break;
        case 0x18: out.mnemonic = "MULT";    break;
        case 0x19: out.mnemonic = "MULTU";   break;
        case 0x1A: out.mnemonic = "DIV";     break;
        case 0x1B: out.mnemonic = "DIVU";    break;
        case 0x20: out.mnemonic = "ADD";     break;
        case 0x21: out.mnemonic = "ADDU";    break;
        case 0x22: out.mnemonic = "SUB";     break;
        case 0x23: out.mnemonic = "SUBU";    break;
        case 0x24: out.mnemonic = "AND";     break;
        case 0x25: out.mnemonic = "OR";      break;
        case 0x26: out.mnemonic = "XOR";     break;
        case 0x27: out.mnemonic = "NOR";     break;
        case 0x2A: out.mnemonic = "SLT";     break;
        case 0x2B: out.mnemonic = "SLTU";    break;
        default:
            out.mnemonic = "SPECIAL?";
            out.format   = InstrFormat::UNKNOWN;
            break;
    }
}

// ---------------------------------------------------------------------------
// REGIMM opcode (opcode == 1): rt field [20:16] determines operation
// ---------------------------------------------------------------------------
void MipsDecoder::decode_regimm(DecodedInstruction& out) {
    out.format = InstrFormat::REGIMM;
    switch (out.rt) {
        case 0x00: out.mnemonic = "BLTZ";    break;
        case 0x01: out.mnemonic = "BGEZ";    break;
        case 0x10: out.mnemonic = "BLTZAL";  break;
        case 0x11: out.mnemonic = "BGEZAL";  break;
        default:
            out.mnemonic = "REGIMM?";
            out.format   = InstrFormat::UNKNOWN;
            break;
    }
}

// ---------------------------------------------------------------------------
// COP0 (opcode == 0x10): rs field [25:21] determines sub-operation
// ---------------------------------------------------------------------------
void MipsDecoder::decode_cop0(DecodedInstruction& out) {
    out.format = InstrFormat::COP0;
    // rs field holds the COP sub-opcode
    switch (out.rs) {
        case 0x00: out.mnemonic = "MFC0"; break;
        case 0x04: out.mnemonic = "MTC0"; break;
        case 0x10:
            // CO bit set — could be RFE (funct==0x10) or TLBR/TLBWI/etc.
            if (out.funct == 0x10) out.mnemonic = "RFE";
            else                   out.mnemonic = "COP0CO";
            break;
        default: out.mnemonic = "COP0"; break;
    }
}

// ---------------------------------------------------------------------------
// COP2 / GTE (opcode == 0x12): rs field determines sub-operation
// ---------------------------------------------------------------------------
void MipsDecoder::decode_cop2(DecodedInstruction& out) {
    out.format = InstrFormat::COP2;
    switch (out.rs) {
        case 0x00: out.mnemonic = "MFC2"; break;
        case 0x02: out.mnemonic = "CFC2"; break;
        case 0x04: out.mnemonic = "MTC2"; break;
        case 0x06: out.mnemonic = "CTC2"; break;
        default:
            // CO bit (bit 25) set => GTE command word
            if (out.rs & 0x10) out.mnemonic = "GTE";
            else               out.mnemonic = "COP2";
            break;
    }
}

// ---------------------------------------------------------------------------
// Classification flags (called after mnemonic/format are set)
// ---------------------------------------------------------------------------
void MipsDecoder::set_classification_flags(DecodedInstruction& out) {
    out.is_branch         = false;
    out.is_jump           = false;
    out.is_load           = false;
    out.is_store          = false;
    out.is_alu            = false;
    out.is_nop            = (out.raw == 0);
    out.is_syscall        = false;
    out.is_break          = false;
    out.is_jr_ra          = false;
    out.is_delay_slot_user = false;

    if (out.format == InstrFormat::UNKNOWN) return;
    if (out.is_nop) return;

    // SPECIAL (opcode == 0)
    if (out.opcode == 0x00) {
        switch (out.funct) {
            case 0x08: // JR
                out.is_jump           = true;
                out.is_delay_slot_user = true;
                if (out.rs == 31) out.is_jr_ra = true;
                break;
            case 0x09: // JALR
                out.is_jump           = true;
                out.is_delay_slot_user = true;
                break;
            case 0x0C: out.is_syscall = true; break;
            case 0x0D: out.is_break   = true; break;
            default:   out.is_alu     = true; break;
        }
        return;
    }

    // REGIMM (opcode == 1) — all are conditional branches
    if (out.opcode == 0x01) {
        out.is_branch         = true;
        out.is_delay_slot_user = true;
        return;
    }

    switch (out.opcode) {
        // Unconditional jumps
        case 0x02: // J
        case 0x03: // JAL
            out.is_jump           = true;
            out.is_delay_slot_user = true;
            break;

        // Conditional branches
        case 0x04: // BEQ
        case 0x05: // BNE
        case 0x06: // BLEZ
        case 0x07: // BGTZ
            out.is_branch         = true;
            out.is_delay_slot_user = true;
            break;

        // ALU (immediate variants)
        case 0x08: // ADDI
        case 0x09: // ADDIU
        case 0x0A: // SLTI
        case 0x0B: // SLTIU
        case 0x0C: // ANDI
        case 0x0D: // ORI
        case 0x0E: // XORI
        case 0x0F: // LUI
            out.is_alu = true;
            break;

        // Loads
        case 0x20: // LB
        case 0x21: // LH
        case 0x22: // LWL
        case 0x23: // LW
        case 0x24: // LBU
        case 0x25: // LHU
        case 0x26: // LWR
        case 0x30: // LWC2
            out.is_load = true;
            break;

        // Stores
        case 0x28: // SB
        case 0x29: // SH
        case 0x2A: // SWL
        case 0x2B: // SW
        case 0x2E: // SWR
        case 0x38: // SWC2
            out.is_store = true;
            break;

        // COP0, COP2: neither load/store/alu/branch/jump
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Main decode function
// ---------------------------------------------------------------------------
DecodedInstruction MipsDecoder::decode(uint32_t instr, uint32_t address) {
    DecodedInstruction out{};
    out.raw      = instr;
    out.address  = address;
    out.mnemonic = "???";
    out.format   = InstrFormat::UNKNOWN;

    // Extract all bit fields
    out.opcode   = (instr >> 26) & 0x3F;
    out.rs       = (instr >> 21) & 0x1F;
    out.rt       = (instr >> 16) & 0x1F;
    out.rd       = (instr >> 11) & 0x1F;
    out.shamt    = (instr >>  6) & 0x1F;
    out.funct    = (instr      ) & 0x3F;
    out.uimm16   = (uint16_t)(instr & 0xFFFF);
    out.imm16    = (int16_t)(instr & 0xFFFF);
    out.target26 = instr & 0x03FFFFFF;
    out.sign_ext_imm = (int32_t)out.imm16;

    // Dispatch to format-specific decoder
    switch (out.opcode) {
        case 0x00: decode_special(out); break;
        case 0x01: decode_regimm(out);  break;
        case 0x02: out.mnemonic = "J";     out.format = InstrFormat::J; break;
        case 0x03: out.mnemonic = "JAL";   out.format = InstrFormat::J; break;
        case 0x04: out.mnemonic = "BEQ";   out.format = InstrFormat::I; break;
        case 0x05: out.mnemonic = "BNE";   out.format = InstrFormat::I; break;
        case 0x06: out.mnemonic = "BLEZ";  out.format = InstrFormat::I; break;
        case 0x07: out.mnemonic = "BGTZ";  out.format = InstrFormat::I; break;
        case 0x08: out.mnemonic = "ADDI";  out.format = InstrFormat::I; break;
        case 0x09: out.mnemonic = "ADDIU"; out.format = InstrFormat::I; break;
        case 0x0A: out.mnemonic = "SLTI";  out.format = InstrFormat::I; break;
        case 0x0B: out.mnemonic = "SLTIU"; out.format = InstrFormat::I; break;
        case 0x0C: out.mnemonic = "ANDI";  out.format = InstrFormat::I; break;
        case 0x0D: out.mnemonic = "ORI";   out.format = InstrFormat::I; break;
        case 0x0E: out.mnemonic = "XORI";  out.format = InstrFormat::I; break;
        case 0x0F: out.mnemonic = "LUI";   out.format = InstrFormat::I; break;
        case 0x10: decode_cop0(out); break;
        case 0x12: decode_cop2(out); break;
        case 0x20: out.mnemonic = "LB";    out.format = InstrFormat::I; break;
        case 0x21: out.mnemonic = "LH";    out.format = InstrFormat::I; break;
        case 0x22: out.mnemonic = "LWL";   out.format = InstrFormat::I; break;
        case 0x23: out.mnemonic = "LW";    out.format = InstrFormat::I; break;
        case 0x24: out.mnemonic = "LBU";   out.format = InstrFormat::I; break;
        case 0x25: out.mnemonic = "LHU";   out.format = InstrFormat::I; break;
        case 0x26: out.mnemonic = "LWR";   out.format = InstrFormat::I; break;
        case 0x28: out.mnemonic = "SB";    out.format = InstrFormat::I; break;
        case 0x29: out.mnemonic = "SH";    out.format = InstrFormat::I; break;
        case 0x2A: out.mnemonic = "SWL";   out.format = InstrFormat::I; break;
        case 0x2B: out.mnemonic = "SW";    out.format = InstrFormat::I; break;
        case 0x2E: out.mnemonic = "SWR";   out.format = InstrFormat::I; break;
        case 0x30: out.mnemonic = "LWC2";  out.format = InstrFormat::I; break;
        case 0x38: out.mnemonic = "SWC2";  out.format = InstrFormat::I; break;
        default:
            out.mnemonic = "???";
            out.format   = InstrFormat::UNKNOWN;
            break;
    }

    // Compute branch/jump targets
    if (out.opcode == 0x02 || out.opcode == 0x03) {
        // J / JAL
        out.jump_target = compute_jump_target(address, out.target26);
    }
    if (out.opcode == 0x04 || out.opcode == 0x05 ||
        out.opcode == 0x06 || out.opcode == 0x07 ||
        out.opcode == 0x01) {
        // BEQ/BNE/BLEZ/BGTZ + REGIMM (BLTZ/BGEZ/etc.)
        out.branch_target = compute_branch_target(address, out.imm16);
    }

    // Set classification flags
    set_classification_flags(out);

    // Override mnemonic for NOP (0x00000000 = SLL $zero,$zero,0)
    if (out.raw == 0) {
        out.mnemonic = "NOP";
    }

    return out;
}

// ---------------------------------------------------------------------------
// Decode all instructions in a PS1 executable
// ---------------------------------------------------------------------------
std::vector<DecodedInstruction> MipsDecoder::decode_executable(
    const PS1Executable& exe)
{
    std::vector<DecodedInstruction> result;
    const uint32_t word_count = exe.code_size() / 4;
    result.reserve(word_count);

    for (uint32_t addr = exe.load_address(); addr < exe.end_address(); addr += 4) {
        auto word = exe.read_word(addr);
        if (!word.has_value()) break;
        result.push_back(decode(*word, addr));
    }

    return result;
}

} // namespace PSXRecomp
