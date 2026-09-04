// strict_translator.h
// ----------------------------------------------------------------------------
// Phase 1a strict, fail-loud MIPS-to-C translator for the BIOS boot slice.
//
// Design contract:
//   - Translates ONLY the instructions Phase 1a's bounded slice actually
//     contains. Currently: LUI, ORI, ADDIU, SW, SLL (incl. NOP), J, JAL,
//     JR, JALR, RFE.
//   - Returns {supported=false, fail_reason=...} for ANY other opcode/funct.
//     Never emits a `/* TODO */` comment, never falls through silently.
//   - Does NOT call into the salvaged CodeGenerator. Phase 1a is a clean,
//     audited path. The salvaged generator will be revisited later.
//
// If you are tempted to add a fallthrough case here: don't. Add a real
// translation, or leave it unsupported and let the build fail loud. That
// is the entire purpose of this file existing as a separate translator.

#pragma once

#include <cstdint>
#include <string>

#include "mips_decoder.h"

namespace PSXRecompV4 {

struct TranslateResult {
    bool        supported = false;  // true = c_code is valid; false = fail_reason is valid
    std::string c_code;              // C statement(s) to emit, no trailing newline
    std::string comment;             // human-readable annotation (e.g. "lui $t0, 0x0013")
    std::string fail_reason;         // populated when supported == false
    bool        is_terminator = false;  // J/JAL/JR/JALR/RFE/branches — slice walker uses this
    const char* terminator_kind = nullptr;  // "j", "jal", "jr", "jalr", "rfe", "branch_*"
    uint32_t    terminator_target = 0;  // for J/JAL/branches: computed target; else unused

    // Optional C statement(s) to be emitted BEFORE the delay slot.
    // Used by conditional branches to capture pre-delay-slot snapshots of
    // their rs/rt operands into uniquely-named function-scope locals, so
    // that the branch condition (in c_code, which is emitted AFTER the
    // delay slot) reads the architecturally-correct values. Empty for
    // every other instruction. The slice walker's emit logic emits this
    // as an EmittedInstr right before the delay slot when non-empty.
    std::string pre_delay_code;

    // MIPS-I load-delay modeling (simple loads only: LB/LBU/LH/LHU/LW with
    // rt != 0). load_dest is the architectural destination register, and
    // c_code_deferred is an alternative emission that assigns the loaded
    // value into the function-scope temp `psx_ldd_<addr>` instead of
    // cpu->gpr[load_dest]. The emitter uses it when the NEXT instruction
    // reads load_dest (a dependent pair): on a real R3000A that successor
    // executes in the load's delay shadow and sees the register's OLD
    // value — OpenBIOS cardfasttrack.s depends on this deliberately
    // ("gotta break those bad emulators"). The emitter flushes
    // cpu->gpr[load_dest] = psx_ldd_<addr> after the successor's register
    // reads. -1 / empty for everything that is not a simple load.
    // LWL/LWR are excluded: their rt-merge plus the hardware's special
    // LWL/LWR chain bypass need different treatment (logged if dependent).
    int         load_dest = -1;
    std::string c_code_deferred;
};

class StrictTranslator {
public:
    // Translate a single decoded instruction. Pure function — no state.
    static TranslateResult translate(const PSXRecomp::DecodedInstruction& d);

private:
    // The per-opcode translation body; translate() wraps its c_code with the
    // shared PGXP hook grammar (CodeGenerator::append_pgxp_hooks).
    static TranslateResult translate_impl(const PSXRecomp::DecodedInstruction& d);

public:
};

} // namespace PSXRecompV4
