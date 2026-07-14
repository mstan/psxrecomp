/*
 * L2 Structural Test — Codegen Pattern Verification
 *
 * For each MIPS instruction form, decodes the instruction, translates it
 * via StrictTranslator, and verifies the generated C code contains the
 * expected register references and operation patterns.
 *
 * This is the "snesrecomp-style" approach: structural pattern matching on
 * the generated C text. It catches wrong register indices, missing
 * operations, and translation gaps. It does NOT verify semantic
 * correctness (that requires L2-semantic with actual execution).
 *
 * Exit codes:
 *   0  all forms pass
 *   1  at least one form fails
 *   2  usage error
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

#include "mips_decoder.h"
#include "strict_translator.h"
#include "fmt/format.h"

using PSXRecomp::MipsDecoder;
using PSXRecomp::DecodedInstruction;
using PSXRecompV4::StrictTranslator;
using PSXRecompV4::TranslateResult;

// ── Helpers ─────────────────────────────────────────────────────────────

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// MIPS instruction encoding helpers
static constexpr uint32_t R_TYPE(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t fn) {
    return (0u << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16)
         | ((uint32_t)rd << 11) | ((uint32_t)sa << 6) | fn;
}
static constexpr uint32_t I_TYPE(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return ((uint32_t)op << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | imm;
}

// Standard test registers: v0(2), v1(3), a0(4)
static constexpr uint8_t V0 = 2, V1 = 3, A0 = 4;

// ── Form definition ─────────────────────────────────────────────────────

struct Form {
    uint32_t    word;
    const char* name;
    // Checker: given the TranslateResult, return empty string on pass
    // or an error message on failure.
    std::function<std::string(const TranslateResult&)> check;
};

// Shorthand: check that translation is supported and c_code contains all
// of the given substrings.
static auto must_contain(std::initializer_list<const char*> needles) {
    std::vector<std::string> v(needles.begin(), needles.end());
    return [v](const TranslateResult& r) -> std::string {
        if (!r.supported)
            return fmt::format("unsupported: {}", r.fail_reason);
        for (auto& n : v) {
            if (!contains(r.c_code, n))
                return fmt::format("missing '{}' in: {}", n, r.c_code);
        }
        return "";
    };
}

// Check supported + custom lambda on c_code
static auto check_code(std::function<std::string(const std::string&)> fn) {
    return [fn](const TranslateResult& r) -> std::string {
        if (!r.supported)
            return fmt::format("unsupported: {}", r.fail_reason);
        return fn(r.c_code);
    };
}

// ── Form table ──────────────────────────────────────────────────────────

static const Form kForms[] = {
    // === SPECIAL (R-type) ALU ===
    // SLL $v0, $a0, 1
    { R_TYPE(0, A0, V0, 1, 0x00), "sll_v0_a0_1",
      must_contain({"gpr[2]", "gpr[4]", "<< 1u"}) },

    // SRL $v0, $a0, 1
    { R_TYPE(0, A0, V0, 1, 0x02), "srl_v0_a0_1",
      must_contain({"gpr[2]", "gpr[4]", ">> 1u"}) },

    // SRA $v0, $a0, 1
    { R_TYPE(0, A0, V0, 1, 0x03), "sra_v0_a0_1",
      must_contain({"gpr[2]", "int32_t", "gpr[4]", ">> 1u"}) },

    // SLLV $v0, $a0, $v1
    { R_TYPE(V1, A0, V0, 0, 0x04), "sllv_v0_a0_v1",
      must_contain({"gpr[2]", "gpr[4]", "gpr[3]", "0x1Fu"}) },

    // SRLV $v0, $a0, $v1
    { R_TYPE(V1, A0, V0, 0, 0x06), "srlv_v0_a0_v1",
      must_contain({"gpr[2]", "gpr[4]", "gpr[3]", "0x1Fu"}) },

    // SRAV $v0, $a0, $v1
    { R_TYPE(V1, A0, V0, 0, 0x07), "srav_v0_a0_v1",
      must_contain({"gpr[2]", "int32_t", "gpr[4]", "gpr[3]", "0x1Fu"}) },

    // ADDU $v0, $v1, $a0
    { R_TYPE(V1, A0, V0, 0, 0x21), "addu_v0_v1_a0",
      must_contain({"gpr[2]", "gpr[3]", "gpr[4]", "+"}) },

    // SUBU $v0, $v1, $a0
    { R_TYPE(V1, A0, V0, 0, 0x23), "subu_v0_v1_a0",
      must_contain({"gpr[2]", "gpr[3]", "gpr[4]", "-"}) },

    // AND $v0, $v1, $a0
    { R_TYPE(V1, A0, V0, 0, 0x24), "and_v0_v1_a0",
      must_contain({"gpr[2]", "gpr[3]", "gpr[4]", "&"}) },

    // OR $v0, $v1, $a0
    { R_TYPE(V1, A0, V0, 0, 0x25), "or_v0_v1_a0",
      must_contain({"gpr[2]", "gpr[3]", "gpr[4]", "|"}) },

    // XOR $v0, $v1, $a0
    { R_TYPE(V1, A0, V0, 0, 0x26), "xor_v0_v1_a0",
      must_contain({"gpr[2]", "gpr[3]", "gpr[4]", "^"}) },

    // NOR $v0, $v1, $a0
    { R_TYPE(V1, A0, V0, 0, 0x27), "nor_v0_v1_a0",
      must_contain({"gpr[2]", "gpr[3]", "gpr[4]", "~"}) },

    // SLT $v0, $v1, $a0
    { R_TYPE(V1, A0, V0, 0, 0x2A), "slt_v0_v1_a0",
      must_contain({"gpr[2]", "int32_t", "gpr[3]", "gpr[4]"}) },

    // SLTU $v0, $v1, $a0
    { R_TYPE(V1, A0, V0, 0, 0x2B), "sltu_v0_v1_a0",
      must_contain({"gpr[2]", "gpr[3]", "gpr[4]", "<"}) },

    // JR $at: target must be snapshotted before its delay slot.
    { R_TYPE(1, 0, 0, 0, 0x08), "jr_at_latches_target",
      [](const TranslateResult& r) -> std::string {
          if (!r.supported) return fmt::format("unsupported: {}", r.fail_reason);
          if (!contains(r.pre_delay_code, "psx_jt_00000000 = cpu->gpr[1]"))
              return fmt::format("missing pre-delay target latch: {}", r.pre_delay_code);
          if (!contains(r.c_code, "cpu->pc = psx_jt_00000000"))
              return fmt::format("terminator does not use latched target: {}", r.c_code);
          if (contains(r.c_code, "cpu->pc = cpu->gpr[1]"))
              return fmt::format("terminator rereads target after delay: {}", r.c_code);
          return "";
      }},

    // JALR $at,$at: capture must precede link write (rd==rs alias-safe).
    { R_TYPE(1, 0, 1, 0, 0x09), "jalr_at_at_latches_before_link",
      [](const TranslateResult& r) -> std::string {
          if (!r.supported) return fmt::format("unsupported: {}", r.fail_reason);
          const auto latch = r.pre_delay_code.find("psx_jt_00000000 = cpu->gpr[1]");
          const auto link = r.pre_delay_code.find("cpu->gpr[1] = 0x00000008u");
          if (latch == std::string::npos || link == std::string::npos || latch > link)
              return fmt::format("target not latched before aliased link: {}", r.pre_delay_code);
          if (!contains(r.c_code, "cpu->pc = psx_jt_00000000"))
              return fmt::format("terminator does not use latched target: {}", r.c_code);
          return "";
      }},

    // ADD $v0, $v1, $a0  (with overflow trap)
    { R_TYPE(V1, A0, V0, 0, 0x20), "add_v0_v1_a0",
      must_contain({"gpr[2]", "gpr[3]", "gpr[4]", "psx_arith_overflow"}) },

    // MULTU $v1, $a0
    { R_TYPE(V1, A0, 0, 0, 0x19), "multu_v1_a0",
      must_contain({"cpu->hi", "cpu->lo", "gpr[3]", "gpr[4]", "uint64_t"}) },

    // DIV $v1, $a0
    { R_TYPE(V1, A0, 0, 0, 0x1A), "div_v1_a0",
      must_contain({"cpu->hi", "cpu->lo", "gpr[3]", "gpr[4]"}) },

    // DIVU $v1, $a0
    { R_TYPE(V1, A0, 0, 0, 0x1B), "divu_v1_a0",
      must_contain({"cpu->hi", "cpu->lo", "gpr[3]", "gpr[4]"}) },

    // MFHI $v0
    { R_TYPE(0, 0, V0, 0, 0x10), "mfhi_v0",
      must_contain({"gpr[2]", "cpu->hi"}) },

    // MFLO $v0
    { R_TYPE(0, 0, V0, 0, 0x12), "mflo_v0",
      must_contain({"gpr[2]", "cpu->lo"}) },

    // MTHI $v1
    { R_TYPE(V1, 0, 0, 0, 0x11), "mthi_v1",
      must_contain({"cpu->hi", "gpr[3]"}) },

    // MTLO $v1
    { R_TYPE(V1, 0, 0, 0, 0x13), "mtlo_v1",
      must_contain({"cpu->lo", "gpr[3]"}) },

    // === Immediate ALU ===
    // ADDIU $v0, $v1, 42
    { I_TYPE(0x09, V1, V0, 42), "addiu_v0_v1_42",
      must_contain({"gpr[2]", "gpr[3]", "42"}) },

    // ADDI $v0, $v1, 42  (with overflow trap)
    { I_TYPE(0x08, V1, V0, 42), "addi_v0_v1_42",
      must_contain({"gpr[2]", "gpr[3]", "42", "psx_arith_overflow"}) },

    // ANDI $v0, $v1, 0xFF
    { I_TYPE(0x0C, V1, V0, 0xFF), "andi_v0_v1_ff",
      must_contain({"gpr[2]", "gpr[3]", "0xFFu"}) },

    // ORI $v0, $v1, 0xFF
    { I_TYPE(0x0D, V1, V0, 0xFF), "ori_v0_v1_ff",
      must_contain({"gpr[2]", "gpr[3]", "0xFFu"}) },

    // XORI $v0, $v1, 0xFF
    { I_TYPE(0x0E, V1, V0, 0xFF), "xori_v0_v1_ff",
      must_contain({"gpr[2]", "gpr[3]", "0xFFu"}) },

    // LUI $v0, 0x8000
    { I_TYPE(0x0F, 0, V0, 0x8000), "lui_v0_8000",
      must_contain({"gpr[2]", "0x80000000u"}) },

    // SLTI $v0, $v1, 42
    { I_TYPE(0x0A, V1, V0, 42), "slti_v0_v1_42",
      must_contain({"gpr[2]", "int32_t", "gpr[3]", "42"}) },

    // SLTIU $v0, $v1, 42
    { I_TYPE(0x0B, V1, V0, 42), "sltiu_v0_v1_42",
      must_contain({"gpr[2]", "gpr[3]", "<"}) },

    // === Load/Store ===
    // LW $v0, 0($v1)
    { I_TYPE(0x23, V1, V0, 0), "lw_v0_0_v1",
      must_contain({"gpr[2]", "gpr[3]", "load_word"}) },

    // SW $a0, 0($v1)
    { I_TYPE(0x2B, V1, A0, 0), "sw_a0_0_v1",
      must_contain({"gpr[4]", "gpr[3]", "write_word"}) },

    // LB $v0, 0($v1)
    { I_TYPE(0x20, V1, V0, 0), "lb_v0_0_v1",
      must_contain({"gpr[2]", "gpr[3]", "load_byte", "int8_t"}) },

    // LBU $v0, 0($v1)
    { I_TYPE(0x24, V1, V0, 0), "lbu_v0_0_v1",
      must_contain({"gpr[2]", "gpr[3]", "load_byte"}) },

    // LH $v0, 0($v1)
    { I_TYPE(0x21, V1, V0, 0), "lh_v0_0_v1",
      must_contain({"gpr[2]", "gpr[3]", "load_half", "int16_t"}) },

    // LHU $v0, 0($v1)
    { I_TYPE(0x25, V1, V0, 0), "lhu_v0_0_v1",
      must_contain({"gpr[2]", "gpr[3]", "load_half"}) },

    // SB $a0, 0($v1)
    { I_TYPE(0x28, V1, A0, 0), "sb_a0_0_v1",
      must_contain({"gpr[4]", "gpr[3]", "write_byte"}) },

    // SH $a0, 0($v1)
    { I_TYPE(0x29, V1, A0, 0), "sh_a0_0_v1",
      must_contain({"gpr[4]", "gpr[3]", "write_half"}) },

    // === COP0 ===
    // MFC0 $v0, $12  (read SR)
    { I_TYPE(0x10, 0x00, V0, 12 << 11), "mfc0_v0_sr",
      check_code([](const std::string& c) -> std::string {
          if (!contains(c, "gpr[2]")) return "missing gpr[2]";
          if (!contains(c, "cop0[12]")) return "missing cop0[12]";
          return "";
      })},

    // MTC0 $v1, $12  (write SR)
    { I_TYPE(0x10, 0x04, V1, 12 << 11), "mtc0_v1_sr",
      check_code([](const std::string& c) -> std::string {
          if (!contains(c, "gpr[3]")) return "missing gpr[3]";
          if (!contains(c, "cop0[12]")) return "missing cop0[12]";
          return "";
      })},

    // === $zero writes should be discarded ===
    // ADDIU $zero, $v1, 1 → should be discarded/commented
    { I_TYPE(0x09, V1, 0, 1), "addiu_zero_v1_1",
      check_code([](const std::string& c) -> std::string {
          // Must not actually write to gpr[0] — should be a comment or no-op
          if (contains(c, "gpr[0] =") && !contains(c, "/*"))
              return "writes to $zero without discard";
          return "";
      })},

    // NOP (SLL $zero, $zero, 0 = 0x00000000)
    { 0x00000000, "nop",
      check_code([](const std::string& c) -> std::string {
          // NOP should produce empty or nop comment
          if (c.size() > 40 && !contains(c, "nop"))
              return fmt::format("suspicious NOP output: {}", c);
          return "";
      })},
};

// ── main ────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0)
            verbose = true;
    }

    int total = 0, pass = 0, fail = 0;

    for (const auto& form : kForms) {
        total++;
        DecodedInstruction d = MipsDecoder::decode(form.word, 0);
        TranslateResult r = StrictTranslator::translate(d);

        std::string err = form.check(r);

        if (err.empty()) {
            pass++;
            if (verbose)
                fmt::print("  PASS  {:<30s}  {}\n", form.name, r.c_code);
        } else {
            fail++;
            fmt::print("  FAIL  {:<30s}  {}\n", form.name, err);
            if (verbose)
                fmt::print("        c_code: {}\n", r.c_code);
        }
    }

    fmt::print("\nL2 structural: {}/{} ok", pass, total);
    if (fail > 0) fmt::print(" ({} failures)", fail);
    fmt::print("\n");

    return fail > 0 ? 1 : 0;
}
