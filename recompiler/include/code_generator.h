#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <set>
#include "ps1_exe_parser.h"
#include "function_analysis.h"
#include "control_flow.h"
#include "../src/annotations.hpp"

namespace PSXRecomp {

// CPU state structure (registers + memory access)
struct CPUState {
    // General purpose registers (32 total)
    uint32_t gpr[32];  // $0-$31
    uint32_t pc;       // Program counter
    uint32_t hi, lo;   // Multiply/divide results

    // Memory access functions (to be implemented in runtime)
    uint32_t (*read_word)(uint32_t addr);
    void (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void (*write_half)(uint32_t addr, uint16_t value);
    uint8_t (*read_byte)(uint32_t addr);
    void (*write_byte)(uint32_t addr, uint8_t value);
};

// Code generation configuration
struct CodeGenConfig {
    bool emit_comments;           // Include MIPS instruction comments
    bool emit_line_numbers;       // Include original address as comments
    bool optimize_zero_reg;       // Optimize away $zero assignments
    bool use_switch_for_blocks;   // Use switch instead of goto labels
    std::string indent;           // Indentation string (default: "    ")

    CodeGenConfig()
        : emit_comments(true)
        , emit_line_numbers(true)
        , optimize_zero_reg(true)
        , use_switch_for_blocks(false)
        , indent("    ") {}
};

// Result of code generation for a function
struct GeneratedFunction {
    std::string function_name;    // e.g., "func_80016940"
    std::string signature;        // e.g., "void func_80016940(CPUState* cpu)"
    std::string body;             // C code body
    std::string full_code;        // signature + body
    int line_count;               // Number of lines generated
};

class CodeGenerator {
public:
    explicit CodeGenerator(const PS1Executable& exe, const CodeGenConfig& config = CodeGenConfig());

    // Generate C code for a single function.
    // fallthrough_name: if non-empty and the last block has no explicit control
    // flow (ControlFlowType::None), emit a tail call to that function before '}'.
    // This handles MIPS functions that fall through into the next function without
    // an explicit jr/j instruction.
    GeneratedFunction generate_function(
        const Function& func,
        const ControlFlowGraph& cfg,
        const std::string& fallthrough_name = "");

    // Generate C code for all functions
    std::vector<GeneratedFunction> generate_all_functions(
        const std::vector<Function>& functions,
        const std::map<uint32_t, ControlFlowGraph>& cfgs);

    // Generate complete C file with all functions
    std::string generate_file(
        const std::vector<Function>& functions,
        const std::map<uint32_t, ControlFlowGraph>& cfgs);

    // Set known functions for this compilation unit (for linking)
    void set_known_functions(const std::set<uint32_t>& functions) {
        known_functions_ = functions;
    }

    // Set annotation table (optional — no-op if not called)
    void set_annotations(const AnnotationTable* at) { annotations_ = at; }

private:
    const PS1Executable& exe_;
    CodeGenConfig config_;
    std::set<uint32_t> known_functions_;  // Addresses of functions in this compilation unit
    std::set<uint32_t> extra_labels_;    // Mid-block addresses that need inline labels (jump table targets)
    const AnnotationTable* annotations_ = nullptr;

    // Instruction translation
    std::string translate_instruction(uint32_t addr, uint32_t instr);

    // Register name mapping
    static std::string reg_name(int reg_num);

    // Basic block translation
    std::string translate_basic_block(
        const BasicBlock& block,
        const ControlFlowGraph& cfg);

    // Control flow translation
    std::string generate_branch_condition(uint32_t instr);
    std::string translate_branch(const ControlFlowInstr& cf, uint32_t fall_through);
    std::string translate_jump(const ControlFlowInstr& cf);

    // Arithmetic instructions
    std::string translate_add(uint32_t instr);
    std::string translate_addu(uint32_t instr);
    std::string translate_sub(uint32_t instr);
    std::string translate_subu(uint32_t instr);
    std::string translate_mult(uint32_t instr);
    std::string translate_multu(uint32_t instr);
    std::string translate_div(uint32_t instr);
    std::string translate_divu(uint32_t instr);
    std::string translate_mfhi(uint32_t instr);
    std::string translate_mflo(uint32_t instr);
    std::string translate_mthi(uint32_t instr);
    std::string translate_mtlo(uint32_t instr);

    // Logical instructions
    std::string translate_and(uint32_t instr);
    std::string translate_or(uint32_t instr);
    std::string translate_xor(uint32_t instr);
    std::string translate_nor(uint32_t instr);

    // Shift instructions
    std::string translate_sll(uint32_t instr);
    std::string translate_srl(uint32_t instr);
    std::string translate_sra(uint32_t instr);
    std::string translate_sllv(uint32_t instr);
    std::string translate_srlv(uint32_t instr);
    std::string translate_srav(uint32_t instr);

    // Load/store instructions
    std::string translate_lw(uint32_t instr);
    std::string translate_sw(uint32_t instr);
    std::string translate_lb(uint32_t instr);
    std::string translate_lbu(uint32_t instr);
    std::string translate_lh(uint32_t instr);
    std::string translate_lhu(uint32_t instr);
    std::string translate_lwl(uint32_t instr);
    std::string translate_lwr(uint32_t instr);
    std::string translate_swl(uint32_t instr);
    std::string translate_swr(uint32_t instr);
    std::string translate_sb(uint32_t instr);
    std::string translate_sh(uint32_t instr);

    // Immediate instructions
    std::string translate_addiu(uint32_t instr);
    std::string translate_andi(uint32_t instr);
    std::string translate_ori(uint32_t instr);
    std::string translate_xori(uint32_t instr);
    std::string translate_lui(uint32_t instr);

    // Set instructions
    std::string translate_slt(uint32_t instr);
    std::string translate_sltu(uint32_t instr);
    std::string translate_slti(uint32_t instr);
    std::string translate_sltiu(uint32_t instr);

    // Helper: Extract register fields
    static uint32_t get_rs(uint32_t instr) { return (instr >> 21) & 0x1F; }
    static uint32_t get_rt(uint32_t instr) { return (instr >> 16) & 0x1F; }
    static uint32_t get_rd(uint32_t instr) { return (instr >> 11) & 0x1F; }
    static uint32_t get_shamt(uint32_t instr) { return (instr >> 6) & 0x1F; }
    static uint32_t get_funct(uint32_t instr) { return instr & 0x3F; }
    static int16_t get_imm16(uint32_t instr) { return (int16_t)(instr & 0xFFFF); }
    static uint16_t get_imm16_u(uint32_t instr) { return (uint16_t)(instr & 0xFFFF); }
};

} // namespace PSXRecomp
