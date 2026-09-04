#include "pgxp_hook_emitter.h"

#include <fmt/format.h>

namespace PSXRecomp {

namespace {
/* Same register naming both emitters use ("cpu->gpr[n]"); kept local so this
 * TU links into psxrecomp-bios without pulling the game CodeGenerator. */
inline std::string reg_name(uint32_t n) { return fmt::format("cpu->gpr[{}]", n); }
inline uint32_t get_rs(uint32_t i) { return (i >> 21) & 0x1F; }
inline uint32_t get_rt(uint32_t i) { return (i >> 16) & 0x1F; }
inline uint32_t get_rd(uint32_t i) { return (i >> 11) & 0x1F; }
inline int16_t get_imm16(uint32_t i) { return (int16_t)(i & 0xFFFF); }
}

/* PGXP dataflow-shadowing hook emission (docs/ENHANCEMENTS.md G1.10; pgxp_hooks.h).
 *
 * Wraps the translated statement with a PGXP_*() macro carrying the raw
 * instruction word plus the operand values the runtime engine needs. Source
 * operands that the statement may clobber (a load/ALU destination aliasing a
 * source, a load overwriting its own base register) are captured into locals
 * BEFORE the statement; everything else is read after it. In the base build
 * the macros preprocess to ((void)0) and the optimizer erases the dead
 * capture locals, so base objects are unchanged; only a -DPSX_PGXP=1 TU pays.
 *
 * Deliberately unhooked: AND/XOR/NOR/SLT-family and the exotic immediates —
 * they only ever DESTROY precision, and the engine's validate-on-read drops
 * their stale shadows without help. The configured widescreen special sites
 * return early above translate_instruction's main dispatch and are likewise
 * unhooked (they are cull compares, not vertex moves; validation covers). */
bool emission_ends_on_preprocessor_directive(const std::string& code) {
    /* Scan back to the start of the final line, then find its first
     * non-blank character. A directive is legal with leading whitespace, so
     * "    #endif" counts just as much as "#endif". */
    const std::size_t nl = code.find_last_of('\n');
    const std::size_t line_begin = (nl == std::string::npos) ? 0u : nl + 1u;
    for (std::size_t i = line_begin; i < code.size(); ++i) {
        const char c = code[i];
        if (c == ' ' || c == '\t' || c == '\r') continue;
        return c == '#';
    }
    return false;  /* blank final line -- appending is already safe */
}

void append_pgxp_hooks(uint32_t instr, std::string& code) {
    const uint32_t opcode = (instr >> 26) & 0x3F;
    const uint32_t rs = get_rs(instr);
    const uint32_t rt = get_rt(instr);
    const uint32_t rd = get_rd(instr);
    const int16_t offset = get_imm16(instr);
    const auto addr_expr = [&]() {
        return (offset == 0) ? reg_name(rs)
                             : fmt::format("{} + {}", reg_name(rs), offset);
    };

    switch (opcode) {
    case 0x00: {                               /* SPECIAL                     */
        const uint32_t funct = instr & 0x3F;
        switch (funct) {
        case 0x00: case 0x02: case 0x03:       /* SLL / SRL / SRA             */
            if (rd == 0 || instr == 0) return; /* skip plain nop              */
            code = fmt::format(
                "{{ uint32_t _pgx1 = {}; {}\n    PGXP_ALU(0x{:08X}u, {}, _pgx1, {}u); }}",
                reg_name(rt), code, instr, reg_name(rd), (instr >> 6) & 31u);
            return;
        case 0x04: case 0x06: case 0x07:       /* SLLV / SRLV / SRAV          */
            if (rd == 0) return;
            code = fmt::format(
                "{{ uint32_t _pgx1 = {}; uint32_t _pgx2 = {}; {}\n    "
                "PGXP_ALU(0x{:08X}u, {}, _pgx1, _pgx2); }}",
                reg_name(rt), reg_name(rs), code, instr, reg_name(rd));
            return;
        case 0x10: case 0x12:                  /* MFHI / MFLO                 */
            if (rd == 0) return;
            code = fmt::format("{}\n    PGXP_ALU(0x{:08X}u, {}, {}, 0u);",
                               code, instr, reg_name(rd),
                               funct == 0x10 ? "cpu->hi" : "cpu->lo");
            return;
        case 0x11: case 0x13:                  /* MTHI / MTLO                 */
            code = fmt::format("{}\n    PGXP_ALU(0x{:08X}u, {}, {}, 0u);",
                               code, instr,
                               funct == 0x11 ? "cpu->hi" : "cpu->lo",
                               funct == 0x11 ? "cpu->hi" : "cpu->lo");
            return;
        case 0x18: case 0x19: case 0x1A: case 0x1B:  /* MULT(U)/DIV(U)        */
            code = fmt::format(
                "{}\n    PGXP_MULDIV(0x{:08X}u, cpu->hi, cpu->lo, {}, {});",
                code, instr, reg_name(rs), reg_name(rt));
            return;
        case 0x20: case 0x21: case 0x22: case 0x23:  /* ADD(U)/SUB(U)         */
        case 0x25:                                   /* OR                    */
            if (rd == 0) return;
            code = fmt::format(
                "{{ uint32_t _pgx1 = {}; uint32_t _pgx2 = {}; {}\n    "
                "PGXP_ALU(0x{:08X}u, {}, _pgx1, _pgx2); }}",
                reg_name(rs), reg_name(rt), code, instr, reg_name(rd));
            return;
        default:
            return;
        }
    }
    case 0x08: case 0x09:                      /* ADDI / ADDIU                */
        if (rt == 0) return;
        code = fmt::format(
            "{{ uint32_t _pgx1 = {}; {}\n    PGXP_ALU(0x{:08X}u, {}, _pgx1, 0x{:08X}u); }}",
            reg_name(rs), code, instr, reg_name(rt),
            (uint32_t)(int32_t)offset);
        return;
    case 0x0D:                                 /* ORI                         */
        if (rt == 0) return;
        code = fmt::format(
            "{{ uint32_t _pgx1 = {}; {}\n    PGXP_ALU(0x{:08X}u, {}, _pgx1, 0x{:04X}u); }}",
            reg_name(rs), code, instr, reg_name(rt),
            (uint32_t)(uint16_t)offset);
        return;
    case 0x0F:                                 /* LUI                         */
        if (rt == 0) return;
        code = fmt::format("{}\n    PGXP_ALU(0x{:08X}u, {}, 0u, 0u);",
                           code, instr, reg_name(rt));
        return;
    case 0x12: {                               /* COP2 register transfers     */
        const uint32_t cop_op = (instr >> 21) & 0x1F;
        if ((cop_op == 0x00 && rt != 0) || cop_op == 0x04)  /* MFC2 / MTC2   */
            code = fmt::format("{}\n    PGXP_COP2(0x{:08X}u, {}, 0u);",
                               code, instr, reg_name(rt));
        return;
    }
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26:           /* loads                       */
        if (rt == 0) return;
        code = fmt::format(
            "{{ uint32_t _pgxa = {}; {}\n    PGXP_LOAD(0x{:08X}u, _pgxa, {}); }}",
            addr_expr(), code, instr, reg_name(rt));
        return;
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2E:                                 /* stores                      */
        code = fmt::format(
            "{{ uint32_t _pgxa = {}; {}\n    PGXP_STORE(0x{:08X}u, _pgxa, {}); }}",
            addr_expr(), code, instr, reg_name(rt));
        return;
    default:
        return;
    }
}


} // namespace PSXRecomp
