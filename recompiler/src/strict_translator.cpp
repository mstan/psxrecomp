// strict_translator.cpp — see header for design contract.
//
// FIRST_MILESTONE.md spec note: the milestone document literally lists
// "ERET" as a termination condition. The PS1's R3000A has no ERET — it has
// RFE (COP0 funct 0x10, full word 0x42000010). Per session approval on
// 2026-04-06, RFE is treated as the equivalent terminator and the manifest
// records this substitution explicitly.

#include "strict_translator.h"

#include <cstdint>
#include <string>

#include "fmt/format.h"

namespace PSXRecompV4 {

namespace {

// Emit a register-write that respects $zero being hardwired to 0.
// LUI/ORI/ADDIU/SLL with rd/rt == 0 are silent NOPs on real hardware.
std::string emit_gpr_write(uint8_t dest_reg, const std::string& rhs_expr) {
    if (dest_reg == 0) {
        return fmt::format("/* discarded write to $zero ({}) */", rhs_expr);
    }
    return fmt::format("cpu->gpr[{}] = {};", static_cast<int>(dest_reg), rhs_expr);
}

const char* gpr_name(uint8_t r) {
    static const char* names[32] = {
        "$zero","$at","$v0","$v1","$a0","$a1","$a2","$a3",
        "$t0","$t1","$t2","$t3","$t4","$t5","$t6","$t7",
        "$s0","$s1","$s2","$s3","$s4","$s5","$s6","$s7",
        "$t8","$t9","$k0","$k1","$gp","$sp","$fp","$ra"
    };
    return names[r & 0x1F];
}

TranslateResult unsupported(const PSXRecomp::DecodedInstruction& d, const std::string& why) {
    TranslateResult r;
    r.supported = false;
    r.fail_reason = fmt::format(
        "addr=0x{:08X} raw=0x{:08X} opcode=0x{:02X}: {}",
        d.address, d.raw, d.opcode, why);
    return r;
}

} // namespace

TranslateResult StrictTranslator::translate(const PSXRecomp::DecodedInstruction& d) {
    TranslateResult r;

    // NOP (raw == 0, i.e. SLL $zero,$zero,0) — emit as a comment with no semantics.
    if (d.raw == 0) {
        r.supported = true;
        r.c_code = "/* nop */";
        r.comment = "nop";
        return r;
    }

    const uint32_t opcode = (d.raw >> 26) & 0x3F;

    // SPECIAL — funct field selects op
    if (opcode == 0x00) {
        const uint32_t funct = d.raw & 0x3F;
        const uint8_t  rs    = (d.raw >> 21) & 0x1F;
        const uint8_t  rt    = (d.raw >> 16) & 0x1F;
        const uint8_t  rd    = (d.raw >> 11) & 0x1F;
        const uint8_t  shamt = (d.raw >> 6) & 0x1F;

        switch (funct) {
            case 0x00: { // SLL rd, rt, shamt  (raw==0 caught above as NOP)
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] << {}u", static_cast<int>(rt), static_cast<int>(shamt)));
                r.comment = fmt::format("sll {}, {}, {}", gpr_name(rd), gpr_name(rt), shamt);
                return r;
            }

            // -----------------------------------------------------------------
            // Shifts (Phase 1b B(3)). The shamt field is already 5 bits in the
            // SPECIAL encoding so SRL/SRA emit a literal in [0,31] — that is
            // always a defined shift count for a 32-bit C type. SLLV/SRLV/SRAV
            // take the shift count from rs[4:0]; we mask explicitly with
            // `& 0x1Fu` both to match real R3000A behavior and to keep the C
            // operation out of undefined-behavior land. SRL/SRLV are logical
            // (zero-fill); SRA/SRAV are arithmetic (sign-fill) and are emitted
            // via a cast through int32_t — gcc and MSVC both implement signed
            // right shift as arithmetic, and the C standard permits this as
            // implementation-defined behavior.
            //
            // SRLV (funct 0x06) has zero inventory hits in this BIOS but is
            // implemented anyway for ISA completeness, per session approval
            // (B(3) checkpoint). It is the *only* opcode in the strict
            // translator that is intentionally implemented without a current
            // inventory hit — every other "no inventory hit, no handler" rule
            // still applies. If a future audit asks "why is this here", the
            // answer is in the B(3) checkpoint history: ISA-completeness
            // exception, deliberately scoped to SRLV alone.
            // -----------------------------------------------------------------

            case 0x02: { // SRL rd, rt, shamt -- logical right shift
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] >> {}u",
                                static_cast<int>(rt), static_cast<int>(shamt)));
                r.comment = fmt::format("srl {}, {}, {}", gpr_name(rd), gpr_name(rt), shamt);
                return r;
            }

            case 0x03: { // SRA rd, rt, shamt -- arithmetic right shift
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("(uint32_t)((int32_t)cpu->gpr[{}] >> {}u)",
                                static_cast<int>(rt), static_cast<int>(shamt)));
                r.comment = fmt::format("sra {}, {}, {}", gpr_name(rd), gpr_name(rt), shamt);
                return r;
            }

            case 0x04: { // SLLV rd, rt, rs -- variable left shift, rs[4:0]
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] << (cpu->gpr[{}] & 0x1Fu)",
                                static_cast<int>(rt), static_cast<int>(rs)));
                r.comment = fmt::format("sllv {}, {}, {}", gpr_name(rd), gpr_name(rt), gpr_name(rs));
                return r;
            }

            case 0x06: { // SRLV rd, rt, rs -- variable logical right shift, rs[4:0]
                // ISA-completeness exception: zero inventory hits, but
                // implemented to keep the SLLV/SRLV/SRAV trio symmetric.
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] >> (cpu->gpr[{}] & 0x1Fu)",
                                static_cast<int>(rt), static_cast<int>(rs)));
                r.comment = fmt::format("srlv {}, {}, {}", gpr_name(rd), gpr_name(rt), gpr_name(rs));
                return r;
            }

            case 0x07: { // SRAV rd, rt, rs -- variable arithmetic right, rs[4:0]
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("(uint32_t)((int32_t)cpu->gpr[{}] >> (cpu->gpr[{}] & 0x1Fu))",
                                static_cast<int>(rt), static_cast<int>(rs)));
                r.comment = fmt::format("srav {}, {}, {}", gpr_name(rd), gpr_name(rt), gpr_name(rs));
                return r;
            }

            // -----------------------------------------------------------------
            // HI/LO + multiply/divide (Phase 1b B(4)).
            //
            // Architectural result semantics only — no load-delay simulation,
            // no interlock modelling, no per-instruction cycle counts. Each
            // op writes/reads HI and LO directly via cpu->hi / cpu->lo.
            //
            // Divide-by-zero and INT32_MIN/-1 edge cases match documented
            // R3000A behavior (PSX-SPX / IDT R3000 manual). The C-level
            // guards exist because:
            //   - C `a / 0u` and `a / 0` are undefined behavior, so we must
            //     branch around them or the compiler is free to delete code
            //     that depends on the result.
            //   - C `INT32_MIN / -1` is undefined behavior on two's-complement
            //     hosts (the mathematical result is 2^31, unrepresentable in
            //     int32_t), so we must branch around it explicitly.
            //
            // MULT (signed) is NOT implemented: zero inventory hits, no
            // ISA-completeness exception requested.
            // -----------------------------------------------------------------

            // -----------------------------------------------------------------
            // Traps (Phase 1b B(10)). SYSCALL and BREAK both translate to a
            // fail-loud extern call followed by an explicit `return;`. They
            // are NON-terminators in the strict-translator sense — the slice
            // walker continues past them — because:
            //   1. Real R3000A SYSCALL/BREAK do NOT have a delay slot. The
            //      next sequential instruction is reached only after the
            //      kernel returns via RFE, not as a delay slot.
            //   2. Marking them as terminators would cause the walker to
            //      fetch the next 4 bytes as a "delay slot", which in
            //      practice is real post-trap code (often `jr $ra; nop`
            //      for syscall wrappers like the one at 0xbfc0d8c4). The
            //      walker explicitly fails loud on terminator-in-delay-
            //      slot, so the slice would refuse to translate.
            //   3. The runtime `return;` statement after the extern call
            //      halts the slice C function so the post-trap C code is
            //      dead at execution time. The walker still emits it as
            //      additional C, which is harmless.
            //
            // SYSCALL: per PS1 ABI the syscall number lives in $v0 (gpr[2]).
            // The 20-bit immediate field [25:6] of the SYSCALL instruction is
            // ignored by the BIOS and not passed to psx_syscall.
            //
            // BREAK: the 20-bit immediate field [25:6] is the trap code; the
            // BIOS uses specific codes like 0x1c00 for divide-by-zero.
            // psx_break receives the code AND the PC for diagnostic
            // purposes.
            //
            // No exception model. No control-flow emulation. No return to
            // execution. The Phase 2 runtime defines psx_syscall and
            // psx_break; until then, linking fails by design.
            // -----------------------------------------------------------------

            case 0x0C: { // SYSCALL
                r.supported = true;
                r.c_code = fmt::format(
                    "cpu->pc = 0x{:08X}u; psx_syscall(cpu, cpu->gpr[2]); return;",
                    d.address);
                r.comment = "syscall";
                return r;
            }

            case 0x0D: { // BREAK
                const uint32_t code = (d.raw >> 6) & 0xFFFFFu;
                r.supported = true;
                r.c_code = fmt::format(
                    "psx_break(cpu, 0x{:05X}u, 0x{:08X}u); return;",
                    code, d.address);
                r.comment = fmt::format("break 0x{:X}", code);
                return r;
            }

            case 0x10: { // MFHI rd
                r.supported = true;
                r.c_code = emit_gpr_write(rd, "cpu->hi");
                r.comment = fmt::format("mfhi {}", gpr_name(rd));
                return r;
            }

            case 0x11: { // MTHI rs
                r.supported = true;
                r.c_code = fmt::format("cpu->hi = cpu->gpr[{}];",
                                       static_cast<int>(rs));
                r.comment = fmt::format("mthi {}", gpr_name(rs));
                return r;
            }

            case 0x12: { // MFLO rd
                r.supported = true;
                r.c_code = emit_gpr_write(rd, "cpu->lo");
                r.comment = fmt::format("mflo {}", gpr_name(rd));
                return r;
            }

            case 0x13: { // MTLO rs
                r.supported = true;
                r.c_code = fmt::format("cpu->lo = cpu->gpr[{}];",
                                       static_cast<int>(rs));
                r.comment = fmt::format("mtlo {}", gpr_name(rs));
                return r;
            }

            case 0x19: { // MULTU rs, rt -- 32x32 -> 64 unsigned, HI:LO
                r.supported = true;
                r.c_code = fmt::format(
                    "{{ uint64_t psx_p = (uint64_t)cpu->gpr[{}] * (uint64_t)cpu->gpr[{}]; "
                    "cpu->lo = (uint32_t)(psx_p & 0xFFFFFFFFu); "
                    "cpu->hi = (uint32_t)(psx_p >> 32); }}",
                    static_cast<int>(rs), static_cast<int>(rt));
                r.comment = fmt::format("multu {}, {}", gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x1A: { // DIV rs, rt -- signed, with divide-by-zero and overflow guards
                r.supported = true;
                // R3000A divide-by-zero (signed):
                //   if numerator >= 0: LO = 0xFFFFFFFF (-1), HI = numerator
                //   if numerator <  0: LO = 0x00000001 ( 1), HI = numerator
                // R3000A INT32_MIN / -1 (would overflow int32_t):
                //   LO = 0x80000000, HI = 0
                // Normal path: compute remainder via subtraction
                //   (r = n - q*d) instead of the C `%` operator, so the
                //   result does not depend on whether the host C compiler
                //   implements `%` as truncated, floored, or
                //   implementation-defined for signed operands. Since
                //   |q*d| <= |n|, neither the multiply nor the subtract
                //   can overflow int32_t when n is representable.
                r.c_code = fmt::format(
                    "{{ int32_t psx_n = (int32_t)cpu->gpr[{}]; "
                    "int32_t psx_d = (int32_t)cpu->gpr[{}]; "
                    "if (psx_d == 0) {{ "
                    "cpu->lo = (psx_n >= 0) ? 0xFFFFFFFFu : 0x00000001u; "
                    "cpu->hi = (uint32_t)psx_n; "
                    "}} else if (psx_n == INT32_MIN && psx_d == -1) {{ "
                    "cpu->lo = 0x80000000u; "
                    "cpu->hi = 0u; "
                    "}} else {{ "
                    "int32_t psx_q = psx_n / psx_d; "
                    "int32_t psx_r = psx_n - (psx_q * psx_d); "
                    "cpu->lo = (uint32_t)psx_q; "
                    "cpu->hi = (uint32_t)psx_r; "
                    "}} }}",
                    static_cast<int>(rs), static_cast<int>(rt));
                r.comment = fmt::format("div {}, {}", gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x1B: { // DIVU rs, rt -- unsigned, with divide-by-zero guard
                r.supported = true;
                // R3000A divide-by-zero (unsigned):
                //   LO = 0xFFFFFFFF, HI = numerator
                // Normal path: compute remainder via subtraction so the
                // result is independent of host `%` behavior, matching
                // the DIV case above. Unsigned `*` wraps mod 2^32 in C
                // but |q*d| <= n so no wrap actually occurs here.
                r.c_code = fmt::format(
                    "{{ uint32_t psx_n = cpu->gpr[{}]; "
                    "uint32_t psx_d = cpu->gpr[{}]; "
                    "if (psx_d == 0) {{ "
                    "cpu->lo = 0xFFFFFFFFu; "
                    "cpu->hi = psx_n; "
                    "}} else {{ "
                    "uint32_t psx_q = psx_n / psx_d; "
                    "uint32_t psx_r = psx_n - (psx_q * psx_d); "
                    "cpu->lo = psx_q; "
                    "cpu->hi = psx_r; "
                    "}} }}",
                    static_cast<int>(rs), static_cast<int>(rt));
                r.comment = fmt::format("divu {}, {}", gpr_name(rs), gpr_name(rt));
                return r;
            }

            // MULT (signed) intentionally not implemented:
            // no inventory hit -> forbidden by project rules.
            // (See B(4) checkpoint and the SRLV ISA-completeness exception
            // memory: SRLV is the *only* opcode allowed without an inventory
            // hit, deliberately scoped to one carve-out.)

            // -----------------------------------------------------------------
            // ALU R-type batch (Phase 1b B(1)). All read rs/rt/rd from the
            // standard SPECIAL fields decoded above. None of these touch HI/LO,
            // none take a shift amount.
            // -----------------------------------------------------------------

            case 0x20: { // ADD rd, rs, rt -- signed; traps on overflow
                r.supported = true;
                // Even if rd == 0 the trap check must still execute, because
                // real R3000A raises overflow before discarding the result.
                // We keep the rs/rt fetch + overflow probe + (optional) write
                // in one statement block. The widened int64 path is portable
                // across mingw64 and MSVC; __builtin_add_overflow would also
                // work but is gcc-specific.
                std::string write_part;
                if (rd == 0) {
                    write_part = "/* discarded write to $zero */";
                } else {
                    write_part = fmt::format("cpu->gpr[{}] = (uint32_t)(int32_t)psx_r;",
                                             static_cast<int>(rd));
                }
                r.c_code = fmt::format(
                    "{{ int32_t psx_a = (int32_t)cpu->gpr[{}]; "
                    "int32_t psx_b = (int32_t)cpu->gpr[{}]; "
                    "int64_t psx_r = (int64_t)psx_a + (int64_t)psx_b; "
                    "if (psx_r < (int64_t)INT32_MIN || psx_r > (int64_t)INT32_MAX) "
                    "{{ psx_arith_overflow(cpu); return; }} "
                    "{} }}",
                    static_cast<int>(rs), static_cast<int>(rt), write_part);
                r.comment = fmt::format("add {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x21: { // ADDU rd, rs, rt -- unsigned wrap, no trap
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] + cpu->gpr[{}]",
                                static_cast<int>(rs), static_cast<int>(rt)));
                r.comment = fmt::format("addu {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x23: { // SUBU rd, rs, rt -- unsigned wrap, no trap
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] - cpu->gpr[{}]",
                                static_cast<int>(rs), static_cast<int>(rt)));
                r.comment = fmt::format("subu {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x24: { // AND rd, rs, rt
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] & cpu->gpr[{}]",
                                static_cast<int>(rs), static_cast<int>(rt)));
                r.comment = fmt::format("and {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x25: { // OR rd, rs, rt
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] | cpu->gpr[{}]",
                                static_cast<int>(rs), static_cast<int>(rt)));
                r.comment = fmt::format("or {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x26: { // XOR rd, rs, rt
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("cpu->gpr[{}] ^ cpu->gpr[{}]",
                                static_cast<int>(rs), static_cast<int>(rt)));
                r.comment = fmt::format("xor {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x27: { // NOR rd, rs, rt -- ~(rs | rt)
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("~(cpu->gpr[{}] | cpu->gpr[{}])",
                                static_cast<int>(rs), static_cast<int>(rt)));
                r.comment = fmt::format("nor {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x2A: { // SLT rd, rs, rt -- signed compare
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("(uint32_t)((int32_t)cpu->gpr[{}] < (int32_t)cpu->gpr[{}] ? 1 : 0)",
                                static_cast<int>(rs), static_cast<int>(rt)));
                r.comment = fmt::format("slt {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x2B: { // SLTU rd, rs, rt -- unsigned compare
                r.supported = true;
                r.c_code = emit_gpr_write(rd,
                    fmt::format("(uint32_t)(cpu->gpr[{}] < cpu->gpr[{}] ? 1 : 0)",
                                static_cast<int>(rs), static_cast<int>(rt)));
                r.comment = fmt::format("sltu {}, {}, {}", gpr_name(rd), gpr_name(rs), gpr_name(rt));
                return r;
            }

            case 0x08: { // JR rs  (slice terminator)
                r.supported = true;
                r.is_terminator = true;
                r.terminator_kind = "jr";
                r.c_code = fmt::format(
                    "cpu->pc = cpu->gpr[{}]; /* jr {} -- slice terminator (indirect) */ return;",
                    static_cast<int>(rs), gpr_name(rs));
                r.comment = fmt::format("jr {}", gpr_name(rs));
                return r;
            }
            case 0x09: { // JALR rd, rs  (slice terminator)
                r.supported = true;
                r.is_terminator = true;
                r.terminator_kind = "jalr";
                std::string link;
                if (rd != 0) {
                    link = fmt::format(
                        "cpu->gpr[{}] = 0x{:08X}u; /* link */ ",
                        static_cast<int>(rd), d.address + 8);
                }
                r.c_code = fmt::format(
                    "{}cpu->pc = cpu->gpr[{}]; /* jalr {}, {} -- slice terminator (indirect) */ return;",
                    link, static_cast<int>(rs), gpr_name(rd), gpr_name(rs));
                r.comment = fmt::format("jalr {}, {}", gpr_name(rd), gpr_name(rs));
                return r;
            }
            default:
                return unsupported(d,
                    fmt::format("SPECIAL funct 0x{:02X} not implemented in Phase 1a strict translator", funct));
        }
    }

    // -----------------------------------------------------------------
    // Conditional branches (Phase 1b B(5), corrected after hazard scan).
    //
    // Encoding:
    //   - BEQ  (op 0x04), BNE  (op 0x05): rs, rt, simm16
    //   - BLEZ (op 0x06), BGTZ (op 0x07): rs, simm16  (rt field == 0)
    //   - BLTZ (REGIMM op 0x01, rt 0x00), BGEZ (REGIMM op 0x01, rt 0x01): rs, simm16
    //
    // Target = (branch_pc + 4) + (sign_extend(simm16) * 4). Computed at
    // translate time so the emitted C contains a literal target address.
    //
    // Pre-delay snapshot model:
    //
    //   Real R3000A: the branch decision uses rs/rt values from BEFORE
    //   the delay-slot instruction executes. The slice walker's emit
    //   order is delay-slot-first then terminator, so any naive
    //   `cpu->gpr[rs]` read in the terminator c_code would see
    //   POST-delay state. The BIOS hits this case 441 times (per
    //   `tools/scan_branch_delay_hazards.py`) — it is not a rare edge.
    //
    //   Fix: TranslateResult.pre_delay_code captures snapshots of all
    //   branch operands into uniquely-named function-scope C locals,
    //   and the walker emits pre_delay_code BEFORE the delay slot.
    //   Each branch's c_code reads ONLY the snapshot variables, never
    //   cpu->gpr[rs/rt] directly. Snapshot variable names embed the
    //   branch's address (uppercase hex) so they are unique within the
    //   slice C function and never collide.
    //
    //   This makes the C output architecturally correct for ALL
    //   branches, regardless of what their delay slot does.
    //
    // BLTZAL / BGEZAL (REGIMM rt 0x10, 0x11) are NOT implemented: zero
    // inventory hits.
    // -----------------------------------------------------------------

    // BEQ rs, rt, simm16 (op 0x04)
    if (opcode == 0x04) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        const uint32_t target = d.address + 4 + static_cast<uint32_t>(simm * 4);
        const uint32_t fallthrough = d.address + 8;
        r.supported = true;
        r.is_terminator = true;
        r.terminator_kind = "branch_beq";
        r.terminator_target = target;
        r.pre_delay_code = fmt::format(
            "uint32_t psx_brA_{:08X} = cpu->gpr[{}]; "
            "uint32_t psx_brB_{:08X} = cpu->gpr[{}];",
            d.address, static_cast<int>(rs),
            d.address, static_cast<int>(rt));
        r.c_code = fmt::format(
            "if (psx_brA_{:08X} == psx_brB_{:08X}) {{ cpu->pc = 0x{:08X}u; return; }} "
            "cpu->pc = 0x{:08X}u; return;",
            d.address, d.address, target, fallthrough);
        r.comment = fmt::format("beq {}, {}, 0x{:08X}", gpr_name(rs), gpr_name(rt), target);
        return r;
    }

    // BNE rs, rt, simm16 (op 0x05)
    if (opcode == 0x05) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        const uint32_t target = d.address + 4 + static_cast<uint32_t>(simm * 4);
        const uint32_t fallthrough = d.address + 8;
        r.supported = true;
        r.is_terminator = true;
        r.terminator_kind = "branch_bne";
        r.terminator_target = target;
        r.pre_delay_code = fmt::format(
            "uint32_t psx_brA_{:08X} = cpu->gpr[{}]; "
            "uint32_t psx_brB_{:08X} = cpu->gpr[{}];",
            d.address, static_cast<int>(rs),
            d.address, static_cast<int>(rt));
        r.c_code = fmt::format(
            "if (psx_brA_{:08X} != psx_brB_{:08X}) {{ cpu->pc = 0x{:08X}u; return; }} "
            "cpu->pc = 0x{:08X}u; return;",
            d.address, d.address, target, fallthrough);
        r.comment = fmt::format("bne {}, {}, 0x{:08X}", gpr_name(rs), gpr_name(rt), target);
        return r;
    }

    // BLEZ rs, simm16 (op 0x06) -- branch if (int32)rs <= 0
    if (opcode == 0x06) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        const uint32_t target = d.address + 4 + static_cast<uint32_t>(simm * 4);
        const uint32_t fallthrough = d.address + 8;
        r.supported = true;
        r.is_terminator = true;
        r.terminator_kind = "branch_blez";
        r.terminator_target = target;
        r.pre_delay_code = fmt::format(
            "uint32_t psx_brA_{:08X} = cpu->gpr[{}];",
            d.address, static_cast<int>(rs));
        r.c_code = fmt::format(
            "if ((int32_t)psx_brA_{:08X} <= 0) {{ cpu->pc = 0x{:08X}u; return; }} "
            "cpu->pc = 0x{:08X}u; return;",
            d.address, target, fallthrough);
        r.comment = fmt::format("blez {}, 0x{:08X}", gpr_name(rs), target);
        return r;
    }

    // BGTZ rs, simm16 (op 0x07) -- branch if (int32)rs > 0
    if (opcode == 0x07) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        const uint32_t target = d.address + 4 + static_cast<uint32_t>(simm * 4);
        const uint32_t fallthrough = d.address + 8;
        r.supported = true;
        r.is_terminator = true;
        r.terminator_kind = "branch_bgtz";
        r.terminator_target = target;
        r.pre_delay_code = fmt::format(
            "uint32_t psx_brA_{:08X} = cpu->gpr[{}];",
            d.address, static_cast<int>(rs));
        r.c_code = fmt::format(
            "if ((int32_t)psx_brA_{:08X} > 0) {{ cpu->pc = 0x{:08X}u; return; }} "
            "cpu->pc = 0x{:08X}u; return;",
            d.address, target, fallthrough);
        r.comment = fmt::format("bgtz {}, 0x{:08X}", gpr_name(rs), target);
        return r;
    }

    // REGIMM (op 0x01): BLTZ (rt 0x00), BGEZ (rt 0x01).
    // BLTZAL / BGEZAL (rt 0x10, 0x11) intentionally not implemented:
    // no inventory hits.
    if (opcode == 0x01) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt_field = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        const uint32_t target = d.address + 4 + static_cast<uint32_t>(simm * 4);
        const uint32_t fallthrough = d.address + 8;
        if (rt_field == 0x00) { // BLTZ
            r.supported = true;
            r.is_terminator = true;
            r.terminator_kind = "branch_bltz";
            r.terminator_target = target;
            r.pre_delay_code = fmt::format(
                "uint32_t psx_brA_{:08X} = cpu->gpr[{}];",
                d.address, static_cast<int>(rs));
            r.c_code = fmt::format(
                "if ((int32_t)psx_brA_{:08X} < 0) {{ cpu->pc = 0x{:08X}u; return; }} "
                "cpu->pc = 0x{:08X}u; return;",
                d.address, target, fallthrough);
            r.comment = fmt::format("bltz {}, 0x{:08X}", gpr_name(rs), target);
            return r;
        }
        if (rt_field == 0x01) { // BGEZ
            r.supported = true;
            r.is_terminator = true;
            r.terminator_kind = "branch_bgez";
            r.terminator_target = target;
            r.pre_delay_code = fmt::format(
                "uint32_t psx_brA_{:08X} = cpu->gpr[{}];",
                d.address, static_cast<int>(rs));
            r.c_code = fmt::format(
                "if ((int32_t)psx_brA_{:08X} >= 0) {{ cpu->pc = 0x{:08X}u; return; }} "
                "cpu->pc = 0x{:08X}u; return;",
                d.address, target, fallthrough);
            r.comment = fmt::format("bgez {}, 0x{:08X}", gpr_name(rs), target);
            return r;
        }
        return unsupported(d,
            fmt::format("REGIMM rt 0x{:02X} not implemented (only BLTZ/BGEZ have inventory hits in this BIOS)", rt_field));
    }

    // J (0x02) — direct jump, slice terminator per Option A (2026-04-06).
    if (opcode == 0x02) {
        const uint32_t target = ((d.address + 4) & 0xF0000000u) | ((d.raw & 0x03FFFFFFu) << 2);
        r.supported = true;
        r.is_terminator = true;
        r.terminator_kind = "j";
        r.terminator_target = target;
        r.c_code = fmt::format(
            "cpu->pc = 0x{:08X}u; /* j 0x{:08X} -- slice terminator (direct) */ return;",
            target, target);
        r.comment = fmt::format("j 0x{:08X}", target);
        return r;
    }

    // JAL (0x03) — direct call, slice terminator.
    if (opcode == 0x03) {
        const uint32_t target = ((d.address + 4) & 0xF0000000u) | ((d.raw & 0x03FFFFFFu) << 2);
        r.supported = true;
        r.is_terminator = true;
        r.terminator_kind = "jal";
        r.terminator_target = target;
        r.c_code = fmt::format(
            "cpu->gpr[31] = 0x{:08X}u; /* link $ra */ "
            "cpu->pc = 0x{:08X}u; /* jal 0x{:08X} -- slice terminator (direct) */ return;",
            d.address + 8, target, target);
        r.comment = fmt::format("jal 0x{:08X}", target);
        return r;
    }

    // ADDI rt, rs, simm16 -- signed; traps on overflow
    if (opcode == 0x08) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        // Same overflow rule as ADD: the trap probe must run even when
        // rt == $zero, because real R3000A raises overflow before
        // discarding the result.
        std::string write_part;
        if (rt == 0) {
            write_part = "/* discarded write to $zero */";
        } else {
            write_part = fmt::format("cpu->gpr[{}] = (uint32_t)(int32_t)psx_r;",
                                     static_cast<int>(rt));
        }
        r.c_code = fmt::format(
            "{{ int32_t psx_a = (int32_t)cpu->gpr[{}]; "
            "int32_t psx_b = ({}); "
            "int64_t psx_r = (int64_t)psx_a + (int64_t)psx_b; "
            "if (psx_r < (int64_t)INT32_MIN || psx_r > (int64_t)INT32_MAX) "
            "{{ psx_arith_overflow(cpu); return; }} "
            "{} }}",
            static_cast<int>(rs), simm, write_part);
        r.comment = fmt::format("addi {}, {}, {}", gpr_name(rt), gpr_name(rs), simm);
        return r;
    }

    // ADDIU rt, rs, simm16
    if (opcode == 0x09) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        r.c_code = emit_gpr_write(rt,
            fmt::format("(uint32_t)((int32_t)cpu->gpr[{}] + ({}))",
                        static_cast<int>(rs), simm));
        r.comment = fmt::format("addiu {}, {}, {}", gpr_name(rt), gpr_name(rs), simm);
        return r;
    }

    // SLTI rt, rs, simm16 -- signed compare against sign-extended imm
    if (opcode == 0x0A) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        r.c_code = emit_gpr_write(rt,
            fmt::format("(uint32_t)((int32_t)cpu->gpr[{}] < ({}) ? 1 : 0)",
                        static_cast<int>(rs), simm));
        r.comment = fmt::format("slti {}, {}, {}", gpr_name(rt), gpr_name(rs), simm);
        return r;
    }

    // SLTIU rt, rs, simm16 -- unsigned compare against SIGN-extended imm
    // (the immediate is sign-extended to 32 bits FIRST, then both operands
    // are compared as unsigned. Standard MIPS quirk.)
    if (opcode == 0x0B) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        const uint32_t uimm_sext = static_cast<uint32_t>(simm);
        r.supported = true;
        r.c_code = emit_gpr_write(rt,
            fmt::format("(uint32_t)(cpu->gpr[{}] < 0x{:08X}u ? 1 : 0)",
                        static_cast<int>(rs), uimm_sext));
        r.comment = fmt::format("sltiu {}, {}, {}", gpr_name(rt), gpr_name(rs), simm);
        return r;
    }

    // ANDI rt, rs, uimm16 -- zero-extended immediate
    if (opcode == 0x0C) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const uint16_t uimm = static_cast<uint16_t>(d.raw & 0xFFFF);
        r.supported = true;
        r.c_code = emit_gpr_write(rt,
            fmt::format("cpu->gpr[{}] & 0x{:X}u", static_cast<int>(rs), uimm));
        r.comment = fmt::format("andi {}, {}, 0x{:X}", gpr_name(rt), gpr_name(rs), uimm);
        return r;
    }

    // ORI rt, rs, uimm16
    if (opcode == 0x0D) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const uint16_t uimm = static_cast<uint16_t>(d.raw & 0xFFFF);
        r.supported = true;
        r.c_code = emit_gpr_write(rt,
            fmt::format("cpu->gpr[{}] | 0x{:X}u", static_cast<int>(rs), uimm));
        r.comment = fmt::format("ori {}, {}, 0x{:X}", gpr_name(rt), gpr_name(rs), uimm);
        return r;
    }

    // XORI rt, rs, uimm16 -- zero-extended immediate
    if (opcode == 0x0E) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const uint16_t uimm = static_cast<uint16_t>(d.raw & 0xFFFF);
        r.supported = true;
        r.c_code = emit_gpr_write(rt,
            fmt::format("cpu->gpr[{}] ^ 0x{:X}u", static_cast<int>(rs), uimm));
        r.comment = fmt::format("xori {}, {}, 0x{:X}", gpr_name(rt), gpr_name(rs), uimm);
        return r;
    }

    // LUI rt, imm16  (rs field is unused)
    if (opcode == 0x0F) {
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const uint16_t imm = static_cast<uint16_t>(d.raw & 0xFFFF);
        r.supported = true;
        r.c_code = emit_gpr_write(rt,
            fmt::format("0x{:08X}u", static_cast<uint32_t>(imm) << 16));
        r.comment = fmt::format("lui {}, 0x{:X}", gpr_name(rt), imm);
        return r;
    }

    // -----------------------------------------------------------------
    // Phase 1 rule: unaligned access = fail loud (no exception model yet)
    // This will be replaced by proper AdEL/AdES handling in Phase 2 runtime
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Loads (Phase 1b B(6)). I-type: base in rs, dest in rt, signed
    // 16-bit byte offset.
    //
    //   addr = (int32_t)cpu->gpr[base] + sign_extend(simm16)
    //
    // Sign vs zero extension:
    //   LB  (0x20) -> sign-extend  8 -> 32 via (int32_t)(int8_t)
    //   LH  (0x21) -> sign-extend 16 -> 32 via (int32_t)(int16_t)
    //   LBU (0x24) -> zero-extend  8 -> 32 via (uint32_t)
    //   LHU (0x25) -> zero-extend 16 -> 32 via (uint32_t)
    //   LW  (0x23) -> 32-bit, no extension
    //
    // $zero handling: even when rt == $zero, the memory read still
    // executes — real R3000A reads memory regardless of the destination
    // register, and MMIO reads can have side effects (status registers
    // that auto-clear on read, FIFOs that pop, etc.). Therefore the
    // emit-to-$zero variant wraps the read in `(void)expr;` so the
    // side effect happens but the value is discarded. emit_gpr_write
    // CANNOT be used for loads because it drops its rhs into a comment.
    //
    // No load-delay-slot modeling: per project decision, the recompiler
    // writes the destination register at the load instruction's
    // position rather than one instruction later. This relies on the
    // BIOS already respecting the architectural load-delay rule (which
    // it does — assemblers/compilers schedule loads correctly).
    //
    // LWL (0x22) and LWR (0x26) are NOT implemented in this batch:
    // they are inventory hits but were not in the explicit B(6) list.
    // -----------------------------------------------------------------

    // LB rt, simm16(rs) -- sign-extended byte load
    if (opcode == 0x20) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        const std::string addr_expr = fmt::format(
            "(uint32_t)((int32_t)cpu->gpr[{}] + ({}))", static_cast<int>(rs), simm);
        if (rt == 0) {
            r.c_code = fmt::format("(void)cpu->read_byte({});", addr_expr);
        } else {
            r.c_code = fmt::format(
                "cpu->gpr[{}] = (uint32_t)(int32_t)(int8_t)cpu->read_byte({});",
                static_cast<int>(rt), addr_expr);
        }
        r.comment = fmt::format("lb {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // -----------------------------------------------------------------
    // LWL / LWR (Phase 1b B(9)) -- unaligned word loads.
    //
    // Little-endian R3000A semantics. The address may be ANY alignment;
    // these instructions are *defined* on unaligned addresses by
    // construction (BIOS uses them precisely when it needs to load an
    // unaligned 32-bit value via the standard `lwl rt, 3(rs); lwr rt,
    // 0(rs);` pair). NO alignment fault.
    //
    // Both ops read the aligned base word at `addr & ~3` and merge it
    // with the existing rt value:
    //
    //   LWL (op 0x22): byte_offset = addr & 3
    //                  shift_left  = (3 - byte_offset) * 8
    //                  keep_mask   = (1u << shift_left) - 1u   (low bits to KEEP from rt)
    //                  rt = (old_rt & keep_mask) | (word << shift_left)
    //
    //   LWR (op 0x26): byte_offset = addr & 3
    //                  shift_right = byte_offset * 8
    //                  keep_mask   = ~(0xFFFFFFFFu >> shift_right)   (high bits to KEEP from rt)
    //                  rt = (old_rt & keep_mask) | (word >> shift_right)
    //
    // Snapshot rule: the formula reads cpu->gpr[rt] in the keep-mask
    // path. C evaluates the RHS before the assignment, so a single
    // expression `cpu->gpr[rt] = (cpu->gpr[rt] & ...) | ...;` is well
    // defined, but to make the merge intent explicit and audit-friendly
    // we snapshot `psx_old_rt` first.
    //
    // $zero handling: when rt == 0, the memory read still executes (MMIO
    // side effects, FIFO pops, etc.) but the result is discarded — we
    // must NOT write to cpu->gpr[0]. The variable form for rt != 0
    // performs the merge; the rt == 0 form does just `(void)read`.
    //
    // No alignment checks. No speculative simplification. The keep_mask
    // formulas for shift_left == 0 (byte_offset == 3) and shift_right
    // == 0 (byte_offset == 0) both yield the "fully overwrite rt"
    // result naturally; the constants are computed at runtime, not
    // baked in.
    // -----------------------------------------------------------------

    // LWL rt, simm16(rs)
    if (opcode == 0x22) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        const std::string body = (rt == 0)
            ? "(void)cpu->read_word(psx_aligned);"
            : fmt::format(
                "uint32_t psx_byte_offset = psx_addr & 3u; "
                "uint32_t psx_shift_left  = (3u - psx_byte_offset) * 8u; "
                "uint32_t psx_keep_mask   = (1u << psx_shift_left) - 1u; "
                "uint32_t psx_word        = cpu->read_word(psx_aligned); "
                "uint32_t psx_old_rt      = cpu->gpr[{}]; "
                "cpu->gpr[{}] = (psx_old_rt & psx_keep_mask) | (psx_word << psx_shift_left);",
                static_cast<int>(rt), static_cast<int>(rt));
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "uint32_t psx_aligned = psx_addr & ~3u; "
            "{} }}",
            static_cast<int>(rs), simm, body);
        r.comment = fmt::format("lwl {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // LWR rt, simm16(rs)
    if (opcode == 0x26) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        const std::string body = (rt == 0)
            ? "(void)cpu->read_word(psx_aligned);"
            : fmt::format(
                "uint32_t psx_byte_offset = psx_addr & 3u; "
                "uint32_t psx_shift_right = psx_byte_offset * 8u; "
                "uint32_t psx_keep_mask   = ~(0xFFFFFFFFu >> psx_shift_right); "
                "uint32_t psx_word        = cpu->read_word(psx_aligned); "
                "uint32_t psx_old_rt      = cpu->gpr[{}]; "
                "cpu->gpr[{}] = (psx_old_rt & psx_keep_mask) | (psx_word >> psx_shift_right);",
                static_cast<int>(rt), static_cast<int>(rt));
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "uint32_t psx_aligned = psx_addr & ~3u; "
            "{} }}",
            static_cast<int>(rs), simm, body);
        r.comment = fmt::format("lwr {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // LH rt, simm16(rs) -- sign-extended halfword load (addr must be 2-aligned)
    if (opcode == 0x21) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        const std::string body = (rt == 0)
            ? "(void)cpu->read_half(psx_addr);"
            : fmt::format("cpu->gpr[{}] = (uint32_t)(int32_t)(int16_t)cpu->read_half(psx_addr);",
                          static_cast<int>(rt));
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "if (psx_addr & 1u) {{ psx_unaligned_access(cpu, psx_addr, 0x{:08X}u); return; }} "
            "{} }}",
            static_cast<int>(rs), simm, d.address, body);
        r.comment = fmt::format("lh {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // LW rt, simm16(rs) -- 32-bit word load (addr must be 4-aligned)
    if (opcode == 0x23) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        const std::string body = (rt == 0)
            ? "(void)cpu->read_word(psx_addr);"
            : fmt::format("cpu->gpr[{}] = cpu->read_word(psx_addr);", static_cast<int>(rt));
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "if (psx_addr & 3u) {{ psx_unaligned_access(cpu, psx_addr, 0x{:08X}u); return; }} "
            "{} }}",
            static_cast<int>(rs), simm, d.address, body);
        r.comment = fmt::format("lw {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // LBU rt, simm16(rs) -- zero-extended byte load
    if (opcode == 0x24) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        const std::string addr_expr = fmt::format(
            "(uint32_t)((int32_t)cpu->gpr[{}] + ({}))", static_cast<int>(rs), simm);
        if (rt == 0) {
            r.c_code = fmt::format("(void)cpu->read_byte({});", addr_expr);
        } else {
            r.c_code = fmt::format(
                "cpu->gpr[{}] = (uint32_t)cpu->read_byte({});",
                static_cast<int>(rt), addr_expr);
        }
        r.comment = fmt::format("lbu {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // LHU rt, simm16(rs) -- zero-extended halfword load (addr must be 2-aligned)
    if (opcode == 0x25) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        const std::string body = (rt == 0)
            ? "(void)cpu->read_half(psx_addr);"
            : fmt::format("cpu->gpr[{}] = (uint32_t)cpu->read_half(psx_addr);", static_cast<int>(rt));
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "if (psx_addr & 1u) {{ psx_unaligned_access(cpu, psx_addr, 0x{:08X}u); return; }} "
            "{} }}",
            static_cast<int>(rs), simm, d.address, body);
        r.comment = fmt::format("lhu {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // -----------------------------------------------------------------
    // Stores (Phase 1b B(7), and SW updated for alignment-check
    // consistency with B(6) loads).
    //
    //   addr = (uint32_t)((int32_t)cpu->gpr[base] + sign_extend(simm16))
    //
    // Alignment rules (R3000A AdES on violation; we fail loud via
    // psx_unaligned_access — not real exception delivery):
    //   SB  (0x28): no alignment requirement
    //   SH  (0x29): addr must be 2-aligned (addr & 1 == 0)
    //   SW  (0x2B): addr must be 4-aligned (addr & 3 == 0)
    //   SWL (0x2A): NO alignment check — SWL operates on the
    //     unaligned high half of a word and is *defined* on
    //     non-4-aligned addresses by construction. The aligned
    //     base word is `addr & ~3`. Implementation reads the
    //     existing memory word, replaces the high (4 - (addr&3))
    //     bytes with the high bytes of cpu->gpr[rt], writes back.
    //   SWR (0x2E): same — operates on the unaligned low half.
    //
    // No special-casing based on the value being stored. The store
    // always happens (modulo the alignment fault). Stores never write
    // a destination register so there is no $zero handling concern;
    // the value to store may come from $zero, in which case the
    // emitted C reads cpu->gpr[0] (which is always 0) and writes that
    // — exactly what real R3000A does.
    //
    // Block scoping: every store is wrapped in `{ uint32_t psx_addr =
    // ...; ... }` so the local doesn't collide with neighboring
    // stores in the same slice C function.
    // -----------------------------------------------------------------

    // SB rt, simm16(rs)
    if (opcode == 0x28) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        r.c_code = fmt::format(
            "cpu->write_byte((uint32_t)((int32_t)cpu->gpr[{}] + ({})), (uint8_t)(cpu->gpr[{}] & 0xFFu));",
            static_cast<int>(rs), simm, static_cast<int>(rt));
        r.comment = fmt::format("sb {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // SH rt, simm16(rs) -- 2-aligned
    if (opcode == 0x29) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "if (psx_addr & 1u) {{ psx_unaligned_access(cpu, psx_addr, 0x{:08X}u); return; }} "
            "cpu->write_half(psx_addr, (uint16_t)(cpu->gpr[{}] & 0xFFFFu)); }}",
            static_cast<int>(rs), simm, d.address, static_cast<int>(rt));
        r.comment = fmt::format("sh {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // SWL rt, simm16(rs) -- unaligned word store, high bytes
    if (opcode == 0x2A) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        // SWL semantics (little-endian R3000A, which is the PS1
        // configuration): given an unaligned addr, base = addr & ~3,
        // shift = (addr & 3). The store writes the (shift+1)
        // high-numbered bytes of the aligned word, taking them from
        // the LOW (shift+1) bytes of cpu->gpr[rt] (no, wait — this
        // depends on endianness; on little-endian it's the high
        // bytes of rt going to the high bytes of the aligned word).
        //
        // The clean formulation, working in 32-bit values with a
        // mask: keep the (3 - shift) low bytes of memory unchanged,
        // overwrite the (shift + 1) high bytes from rt's high bytes
        // shifted right by ((3 - shift) * 8). Equivalently:
        //
        //   shift_bytes = addr & 3
        //   shift_bits  = (3 - shift_bytes) * 8
        //   keep_mask   = 0x00FFFFFFu >> (shift_bytes * 8)   (lower bits to keep)
        //   new_word    = (mem & keep_mask) | (rt >> shift_bits)
        //
        // Verified against the IDT R3000 manual SWL pseudocode and
        // PSX-SPX SWL/SWR description. SWL has no alignment fault by
        // design — it is the instruction PS1 code uses precisely
        // when the address is not 4-aligned.
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr  = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "uint32_t psx_aligned = psx_addr & ~3u; "
            "uint32_t psx_shift_bytes = psx_addr & 3u; "
            "uint32_t psx_shift_bits  = (3u - psx_shift_bytes) * 8u; "
            "uint32_t psx_keep_mask   = 0x00FFFFFFu >> (psx_shift_bytes * 8u); "
            "uint32_t psx_mem = cpu->read_word(psx_aligned); "
            "uint32_t psx_new = (psx_mem & psx_keep_mask) | (cpu->gpr[{}] >> psx_shift_bits); "
            "cpu->write_word(psx_aligned, psx_new); }}",
            static_cast<int>(rs), simm, static_cast<int>(rt));
        r.comment = fmt::format("swl {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // SW rt, simm16(rs) -- 4-aligned
    if (opcode == 0x2B) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "if (psx_addr & 3u) {{ psx_unaligned_access(cpu, psx_addr, 0x{:08X}u); return; }} "
            "cpu->write_word(psx_addr, cpu->gpr[{}]); }}",
            static_cast<int>(rs), simm, d.address, static_cast<int>(rt));
        r.comment = fmt::format("sw {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // SWR rt, simm16(rs) -- unaligned word store, low bytes
    if (opcode == 0x2E) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(d.raw & 0xFFFF));
        r.supported = true;
        // SWR (little-endian R3000A): mirror of SWL. Given an
        // unaligned addr, base = addr & ~3, shift = (addr & 3). The
        // store writes the (4 - shift) low-numbered bytes of the
        // aligned word, taking them from the HIGH (4 - shift) bytes
        // of cpu->gpr[rt] (formulated below in shift terms):
        //
        //   shift_bytes = addr & 3
        //   shift_bits  = shift_bytes * 8
        //   keep_mask   = 0xFFFFFF00u << ((3 - shift_bytes) * 8)   (upper bits to keep)
        //   new_word    = (mem & keep_mask) | (rt << shift_bits)
        //
        // Note keep_mask for shift_bytes==0 is 0xFFFFFF00 << 24 = 0,
        // i.e., overwrite the entire word — that's correct because
        // SWR with addr & 3 == 0 stores the full word at addr.
        // Verified against IDT R3000 manual and PSX-SPX.
        r.c_code = fmt::format(
            "{{ uint32_t psx_addr  = (uint32_t)((int32_t)cpu->gpr[{}] + ({})); "
            "uint32_t psx_aligned = psx_addr & ~3u; "
            "uint32_t psx_shift_bytes = psx_addr & 3u; "
            "uint32_t psx_shift_bits  = psx_shift_bytes * 8u; "
            "uint32_t psx_keep_mask   = 0xFFFFFF00u << ((3u - psx_shift_bytes) * 8u); "
            "uint32_t psx_mem = cpu->read_word(psx_aligned); "
            "uint32_t psx_new = (psx_mem & psx_keep_mask) | (cpu->gpr[{}] << psx_shift_bits); "
            "cpu->write_word(psx_aligned, psx_new); }}",
            static_cast<int>(rs), simm, static_cast<int>(rt));
        r.comment = fmt::format("swr {}, {}({})", gpr_name(rt), simm, gpr_name(rs));
        return r;
    }

    // COP0 (0x10): MFC0 (rs=0x00), MTC0 (rs=0x04), RFE (full word 0x42000010).
    //
    // Phase 1b B(8) decision: COP0 registers live in CPUState.cop0[32]
    // and MFC0/MTC0 are direct register transfers with NO side effects
    // beyond moving 32 bits. No SR-bit-write hooks, no IRQ delivery on
    // SR write, no Cause-write side effects, no exception modeling.
    // The Phase 2 runtime will revisit this when it needs IRQ delivery
    // (PLAN.md HP1 Q1).
    //
    // Note: cop0[0] is the Index register, NOT a hardwired zero. Do not
    // apply $zero rules to cop0[]. Only the GPR side (rt for MFC0)
    // gets the $zero discard via emit_gpr_write.
    if (opcode == 0x10) {
        // RFE is the entire instruction word 0x42000010.
        if (d.raw == 0x42000010u) {
            r.supported = true;
            r.is_terminator = true;
            r.terminator_kind = "rfe";
            // RFE pops the IE/KU stack in SR (cop0[12]) by shifting bits [5:0] right by 2,
            // preserving the top bits.
            r.c_code =
                "{ uint32_t sr = cpu->cop0[12]; "
                "cpu->cop0[12] = (sr & 0xFFFFFFC0u) | ((sr >> 2) & 0x0Fu); } "
                "/* rfe -- slice terminator (per session approval, RFE substitutes for "
                "FIRST_MILESTONE.md \"ERET\" since R3000A has no ERET) */ return;";
            r.comment = "rfe";
            return r;
        }

        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const uint8_t rd = (d.raw >> 11) & 0x1F;

        if (rs == 0x00) { // MFC0 rt, rd  -- gpr[rt] = cop0[rd]
            r.supported = true;
            r.c_code = emit_gpr_write(rt,
                fmt::format("cpu->cop0[{}]", static_cast<int>(rd)));
            r.comment = fmt::format("mfc0 {}, cop0[{}]", gpr_name(rt), rd);
            return r;
        }
        if (rs == 0x04) { // MTC0 rt, rd  -- cop0[rd] = gpr[rt]
            r.supported = true;
            // cop0[rd] is NOT subject to $zero rules; even cop0[0] is a
            // real writable register (Index). Direct assignment.
            r.c_code = fmt::format(
                "cpu->cop0[{}] = cpu->gpr[{}];",
                static_cast<int>(rd), static_cast<int>(rt));
            r.comment = fmt::format("mtc0 {}, cop0[{}]", gpr_name(rt), rd);
            return r;
        }

        return unsupported(d,
            fmt::format("COP0 rs 0x{:02X} not implemented (only MFC0/MTC0/RFE in Phase 1b)", rs));
    }

    // COP2 / GTE (opcode 0x12)
    if (opcode == 0x12) {
        const uint8_t cop_op = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const uint8_t rd = (d.raw >> 11) & 0x1F;

        if (cop_op == 0x00) { // MFC2 — move from COP2 data register
            r.supported = true;
            r.c_code = emit_gpr_write(rt,
                fmt::format("cpu->gte_data[{}]", static_cast<int>(rd)));
            r.comment = fmt::format("mfc2 {}, gte_data[{}]", gpr_name(rt), rd);
            return r;
        }
        if (cop_op == 0x02) { // CFC2 — move from COP2 control register
            r.supported = true;
            r.c_code = emit_gpr_write(rt,
                fmt::format("cpu->gte_ctrl[{}]", static_cast<int>(rd)));
            r.comment = fmt::format("cfc2 {}, gte_ctrl[{}]", gpr_name(rt), rd);
            return r;
        }
        if (cop_op == 0x04) { // MTC2 — move to COP2 data register
            r.supported = true;
            r.c_code = fmt::format(
                "cpu->gte_data[{}] = cpu->gpr[{}];",
                static_cast<int>(rd), static_cast<int>(rt));
            r.comment = fmt::format("mtc2 {}, gte_data[{}]", gpr_name(rt), rd);
            return r;
        }
        if (cop_op == 0x06) { // CTC2 — move to COP2 control register
            r.supported = true;
            r.c_code = fmt::format(
                "cpu->gte_ctrl[{}] = cpu->gpr[{}];",
                static_cast<int>(rd), static_cast<int>(rt));
            r.comment = fmt::format("ctc2 {}, gte_ctrl[{}]", gpr_name(rt), rd);
            return r;
        }
        if (cop_op & 0x10) { // GTE command (bit 25 set)
            uint32_t gte_cmd = d.raw & 0x1FFFFFF;
            r.supported = true;
            r.c_code = fmt::format(
                "gte_execute(cpu, 0x{:07X});",
                gte_cmd);
            r.comment = fmt::format("gte cmd 0x{:02X}", gte_cmd & 0x3F);
            return r;
        }

        return unsupported(d,
            fmt::format("COP2 rs 0x{:02X} not implemented", cop_op));
    }

    // LWC2 (opcode 0x32) — load word to COP2 data register
    if (opcode == 0x32) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int16_t offset = static_cast<int16_t>(d.raw & 0xFFFF);
        r.supported = true;
        if (offset == 0) {
            r.c_code = fmt::format(
                "cpu->gte_data[{}] = cpu->read_word(cpu->gpr[{}]);",
                static_cast<int>(rt), static_cast<int>(rs));
        } else {
            r.c_code = fmt::format(
                "cpu->gte_data[{}] = cpu->read_word((uint32_t)((int32_t)cpu->gpr[{}] + ({})));",
                static_cast<int>(rt), static_cast<int>(rs), static_cast<int>(offset));
        }
        r.comment = fmt::format("lwc2 gte[{}], {}({})",
            rt, static_cast<int>(offset), gpr_name(rs));
        return r;
    }

    // SWC2 (opcode 0x3A) — store word from COP2 data register
    if (opcode == 0x3A) {
        const uint8_t rs = (d.raw >> 21) & 0x1F;
        const uint8_t rt = (d.raw >> 16) & 0x1F;
        const int16_t offset = static_cast<int16_t>(d.raw & 0xFFFF);
        r.supported = true;
        if (offset == 0) {
            r.c_code = fmt::format(
                "cpu->write_word(cpu->gpr[{}], cpu->gte_data[{}]);",
                static_cast<int>(rs), static_cast<int>(rt));
        } else {
            r.c_code = fmt::format(
                "cpu->write_word((uint32_t)((int32_t)cpu->gpr[{}] + ({})), cpu->gte_data[{}]);",
                static_cast<int>(rs), static_cast<int>(offset), static_cast<int>(rt));
        }
        r.comment = fmt::format("swc2 gte[{}], {}({})",
            rt, static_cast<int>(offset), gpr_name(rs));
        return r;
    }

    return unsupported(d, fmt::format("top-level opcode 0x{:02X} not implemented in Phase 1a", opcode));
}

} // namespace PSXRecompV4
