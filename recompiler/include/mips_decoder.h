#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "ps1_exe_parser.h"

namespace PSXRecomp {

// MIPS R3000 instruction format types
enum class InstrFormat {
    R,        // Register: op[31:26]=0 rs rt rd shamt funct
    I,        // Immediate: op rs rt imm16
    J,        // Jump: op target26
    SPECIAL,  // SPECIAL opcode (funct field determines op) - alias for R
    REGIMM,   // REGIMM opcode (rt field determines op)
    COP0,     // Coprocessor 0
    COP2,     // GTE coprocessor
    UNKNOWN   // Unrecognized/invalid
};

// Fully decoded MIPS R3000 instruction
struct DecodedInstruction {
    uint32_t raw;           // Original instruction word
    uint32_t address;       // PC address where this was fetched
    InstrFormat format;

    // Register fields (R-type and I-type)
    uint8_t rs;             // Source register [25:21]
    uint8_t rt;             // Target register [20:16]
    uint8_t rd;             // Destination register [15:11]
    uint8_t shamt;          // Shift amount [10:6]
    uint8_t funct;          // Function code [5:0]

    // Immediate fields (I-type)
    int16_t  imm16;         // Sign-extended immediate [15:0]
    uint16_t uimm16;        // Zero-extended immediate [15:0]

    // Jump field (J-type)
    uint32_t target26;      // 26-bit jump target [25:0]

    // Derived fields
    uint8_t  opcode;        // Top 6 bits [31:26]
    uint32_t jump_target;   // Full computed jump address (for J/JAL/JR/JALR)
    uint32_t branch_target; // Full computed branch target (for beq/bne/etc)
    int32_t  sign_ext_imm;  // Sign-extended 16-bit immediate as int32

    // Classification flags
    bool is_branch;         // BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ/BLTZAL/BGEZAL
    bool is_jump;           // J/JAL/JR/JALR
    bool is_load;           // LB/LH/LW/LBU/LHU/LWL/LWR/LWC2
    bool is_store;          // SB/SH/SW/SWL/SWR/SWC2
    bool is_alu;            // Arithmetic/logic (not load/store/branch/jump)
    bool is_nop;            // raw == 0 (SLL $zero,$zero,0)
    bool is_syscall;        // SYSCALL instruction
    bool is_break;          // BREAK instruction
    bool is_jr_ra;          // JR $ra specifically (function return)
    bool is_delay_slot_user; // This instruction is followed by a delay slot

    // Mnemonic for debugging/display
    const char* mnemonic;   // e.g. "ADDIU", "LW", "JAL"
};

class MipsDecoder {
public:
    // Decode a single instruction word at a given address
    // address is used to compute branch/jump targets
    static DecodedInstruction decode(uint32_t instr, uint32_t address = 0);

    // Decode all instructions in a PS1 executable sequentially
    static std::vector<DecodedInstruction> decode_executable(
        const PS1Executable& exe
    );

    // Compute branch target: address + 4 + (imm16 << 2)
    static uint32_t compute_branch_target(uint32_t address, int16_t imm16);

    // Compute jump target: (address & 0xF0000000) | (target26 << 2)
    static uint32_t compute_jump_target(uint32_t address, uint32_t target26);

    // Check if instruction is a NOP (0x00000000 = SLL $zero,$zero,0)
    static bool is_nop(uint32_t instr);

    // Get MIPS register name string ("$zero", "$at", "$v0", etc.)
    static const char* register_name(uint8_t reg);

    // Get mnemonic string for a decoded instruction
    static const char* get_mnemonic(const DecodedInstruction& instr);

private:
    static void decode_special(DecodedInstruction& out);
    static void decode_regimm(DecodedInstruction& out);
    static void decode_cop0(DecodedInstruction& out);
    static void decode_cop2(DecodedInstruction& out);
    static void set_classification_flags(DecodedInstruction& out);
};

} // namespace PSXRecomp
