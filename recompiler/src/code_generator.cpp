#include "code_generator.h"
#include "pgxp_hook_emitter.h"
#include "control_flow.h"
#include "gte_register_classification.h"
#include "../src/bios_address_model.h"
// Shared widescreen backdrop-window detector (single source of truth across the
// recompiler and the interpreter). Self-contained C header;
// included via relative path to avoid an include-dir collision (recompiler and
// runtime both ship a gte.h).
#include "../../runtime/include/ws_backdrop_detect.h"
#include "../../runtime/include/ws_cull_detect.h"
#include "../../runtime/include/psx_instr_cost.h"  /* single-source CPU cycle cost (shared with interp) */
#include <fmt/format.h>
#include <algorithm>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace PSXRecomp {

/* Reserved MIPS-II branch-likely words often appear when discovery sweeps
 * ASCII/data as code. Emit the RI raise every time (faithfulness), but do not
 * spam one WARNING per revisit of the same PC — product Generate was flooding
 * the setup wizard with hundreds of identical lines. Set
 * PSXRECOMP_VERBOSE_RI_WARNINGS=1 to print every hit (dev / tests). */
static bool should_print_reserved_opcode_warning(uint32_t addr) {
    const char* e = std::getenv("PSXRECOMP_VERBOSE_RI_WARNINGS");
    if (e != nullptr && e[0] != '\0' && e[0] != '0')
        return true;
    static std::unordered_set<uint32_t> seen;
    return seen.insert(addr).second;
}

static bool codegen_cycle_per_insn() {
    // DEFAULT ON for the faithful-timing (cycle-audit) branch: each instruction
    // charges its cost at its own site, so the running cycle count is correct
    // mid-block. This is REQUIRED for stateful timing — mult/div completion-stall
    // (mflo/mfhi wait for muldiv_ts_done) and later GTE stalls — to absorb
    // correctly, exactly like Beetle's per-instruction timestamp. Block-up-front
    // charging loses the in-between timing and can only charge full latency.
    // Set PSX_CODEGEN_CYCLE_PER_INSN=0 to force the old block-up-front mode.
    const char* e = std::getenv("PSX_CODEGEN_CYCLE_PER_INSN");
    if (e != nullptr && e[0] != '\0') return e[0] != '0';
    return true;
}

/* The BIOS address model of the profile this game is built against
 * (main_psx.cpp resolves [recompiler] bios_config, default the SCPH1001
 * profile, and sets it before generation). A game artifact depends on a
 * BIOS property here — jump tables inside relocated BIOS code windows —
 * so the window comes from the profile, never from a constant. */
static const ::PSXRecompV4::BiosAddressModel* g_game_bios_model = nullptr;

void CodeGenerator::set_bios_address_model(const PSXRecompV4::BiosAddressModel* m) {
    g_game_bios_model = m;
}

/*
 * Translate a runtime RAM address to an address that exe_.read_word() can
 * resolve.
 *
 * Game EXEs are read directly at their load address.  The BIOS copies code
 * into RAM at boot (SCPH1001: the shell to 0x80030000); jump tables within
 * such code store RAM-space target addresses, but at recompile time we can
 * only read the ROM.  This helper maps:
 *
 *   active EXE load range   ->  active EXE load range
 *   BIOS-copy RAM windows   ->  ROM kseg1 (per the BIOS profile's model)
 *
 * Other addresses pass through unchanged.
 */
static uint32_t ram_to_rom(uint32_t addr, const PS1Executable& exe) {
    uint32_t phys = addr & 0x1FFFFFFFu;

    /*
     * Game EXEs can legitimately occupy the BIOS-copy RAM windows (the
     * shell window especially), so prefer the active EXE's load range
     * before applying any BIOS remap.
     */
    uint32_t exe_phys = exe.load_address() & 0x1FFFFFFFu;
    uint32_t exe_size = exe.code_size();
    if (exe_size != 0 && phys >= exe_phys && phys < exe_phys + exe_size) {
        return exe.load_address() + (phys - exe_phys);
    }

    if (!g_game_bios_model) {
        throw std::runtime_error(
            "CodeGenerator: no BIOS address model set "
            "(CodeGenerator::set_bios_address_model must run before generation)");
    }
    return g_game_bios_model->ram_alias_to_rom(addr);
}

CodeGenerator::CodeGenerator(const PS1Executable& exe, const CodeGenConfig& config)
    : exe_(exe), config_(config) {
    // RECURSION_BUG.md §25 — continuation-passing call/return. Gen-time opt-in
    // via PSX_CPS so legacy codegen stays byte-identical when unset.
    // CPS is the DEFAULT (RECURSION_BUG.md §25). Opt out (legacy) with PSX_CPS=0.
    { const char* e = std::getenv("PSX_CPS"); cps_enabled_ = (e == nullptr || e[0] != '0'); }
}

uint32_t CodeGenerator::partial_block_cycle_count(uint32_t addr,
                                                  const ControlFlowGraph& cfg) const {
    if (cfg.blocks.count(addr)) {
        return 0;
    }

    for (const auto& [block_addr, block] : cfg.blocks) {
        (void)block_addr;
        if (addr <= block.start_addr || addr > block.end_addr) {
            continue;
        }
        if (((addr - block.start_addr) & 3u) != 0) {
            continue;
        }
        return ((block.end_addr - addr) / 4u) + 1u;
    }

    return 0;
}

std::string CodeGenerator::emit_mid_block_cycle_charge(uint32_t addr,
                                                       const ControlFlowGraph& cfg,
                                                       const std::string& indent) const {
    if (codegen_cycle_per_insn()) {
        return "";
    }

    uint32_t cycles = partial_block_cycle_count(addr, cfg);
    if (cycles == 0) {
        return "";
    }

    std::stringstream ss;
    ss << "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
    ss << indent << fmt::format("psx_advance_cycles({}u);\n", cycles);
    ss << "#endif\n";
    return ss.str();
}

/* Both check emitters honor an exception REDIRECT: when the delivery's
 * epilogue publishes a resume PC different from this site's static
 * continuation (guest RFE to a non-EPC target — longjmp-style handlers,
 * SetCustomExitFromException, thread switch), the compiled code must
 * surface it to the trampoline instead of overwriting it with the static
 * transfer. This is the compiled analogue of the dirty interpreter's
 * "Handler resumed elsewhere — surface to dispatch" contract; without it a
 * baked/compiled leader silently drops the guest's continuation (WipEout 3
 * static bake: deterministic top-level "execution completed, PC=0" ~10 s
 * into boot, root-caused to redirects dropped at compiled leaders). A
 * published pc equal to the static continuation falls through (same-thread
 * restore publishes 0; the depth-guard rescue publishes the site PC). */
std::string CodeGenerator::emit_interrupt_check(uint32_t resume_pc,
                                                const std::string& indent) const {
    return std::string("#ifdef PSX_ENABLE_BLOCK_CYCLES\n") + indent +
           "psx_cyc_bb_defer_flush();\n#endif\n" + indent +
           fmt::format("psx_check_interrupts_at(cpu, 0x{:08X}u);\n", resume_pc) + indent +
           fmt::format("if (cpu->pc != 0u) {{ if ((cpu->pc ^ 0x{:08X}u) & 0x1FFFFFFFu) return; cpu->pc = 0u; }}  /* IRQ redirect / consume same-site resume */\n",
                       resume_pc);
}

std::string CodeGenerator::emit_interrupt_check_expr(const std::string& resume_pc_expr,
                                                     const std::string& indent) const {
    return std::string("#ifdef PSX_ENABLE_BLOCK_CYCLES\n") + indent +
           "psx_cyc_bb_defer_flush();\n#endif\n" + indent +
           fmt::format("psx_check_interrupts_at(cpu, {});\n", resume_pc_expr) + indent +
           fmt::format("if (cpu->pc != 0u) {{ if ((cpu->pc ^ ({})) & 0x1FFFFFFFu) return; cpu->pc = 0u; }}  /* IRQ redirect / consume same-site resume */\n",
                       resume_pc_expr);
}

std::string CodeGenerator::reg_name(int reg_num) {
    if (reg_num >= 0 && reg_num < 32) {
        return fmt::format("cpu->gpr[{}]", reg_num);
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
    std::string addr = (offset == 0) ? reg_name(rs)
                                     : fmt::format("{} + {}", reg_name(rs), offset);
    uint32_t mask = 1u << rs;   /* GPR_DEP rs (load: dest rt armed via LDWhich) */

    if (config_.optimize_zero_reg && rt == 0) {
        /* load to $zero: no GPR write, but the data access + R3000A interlock still run */
        return fmt::format("(void)psx_cyc_load_word(cpu, {}, 0, 0x{:X}u);", addr, mask);
    }
    return fmt::format("{} = psx_cyc_load_word(cpu, {}, {}, 0x{:X}u);",
                       reg_name(rt), addr, rt, mask);
}

std::string CodeGenerator::translate_sw(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (offset == 0) {
        return fmt::format("psx_store_cycle_barrier(); cpu->write_word({}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("psx_store_cycle_barrier(); cpu->write_word({} + {}, {});",
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
    std::string addr = (offset == 0) ? reg_name(rs)
                                     : fmt::format("{} + {}", reg_name(rs), offset);
    uint32_t mask = 1u << rs;

    if (config_.optimize_zero_reg && rt == 0) {
        return fmt::format("(void)psx_cyc_load_byte(cpu, {}, 0, 0x{:X}u);", addr, mask);
    }
    return fmt::format("{} = (int32_t)(int8_t)psx_cyc_load_byte(cpu, {}, {}, 0x{:X}u);",
                       reg_name(rt), addr, rt, mask);
}

std::string CodeGenerator::translate_lbu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);
    std::string addr = (offset == 0) ? reg_name(rs)
                                     : fmt::format("{} + {}", reg_name(rs), offset);
    uint32_t mask = 1u << rs;

    if (config_.optimize_zero_reg && rt == 0) {
        return fmt::format("(void)psx_cyc_load_byte(cpu, {}, 0, 0x{:X}u);", addr, mask);
    }
    return fmt::format("{} = psx_cyc_load_byte(cpu, {}, {}, 0x{:X}u);",
                       reg_name(rt), addr, rt, mask);
}

std::string CodeGenerator::translate_lh(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);
    std::string addr = (offset == 0) ? reg_name(rs)
                                     : fmt::format("{} + {}", reg_name(rs), offset);
    uint32_t mask = 1u << rs;

    if (config_.optimize_zero_reg && rt == 0) {
        return fmt::format("(void)psx_cyc_load_half(cpu, {}, 0, 0x{:X}u);", addr, mask);
    }
    return fmt::format("{} = (int32_t)(int16_t)psx_cyc_load_half(cpu, {}, {}, 0x{:X}u);",
                       reg_name(rt), addr, rt, mask);
}

std::string CodeGenerator::translate_lhu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);
    std::string addr = (offset == 0) ? reg_name(rs)
                                     : fmt::format("{} + {}", reg_name(rs), offset);
    uint32_t mask = 1u << rs;

    if (config_.optimize_zero_reg && rt == 0) {
        return fmt::format("(void)psx_cyc_load_half(cpu, {}, 0, 0x{:X}u);", addr, mask);
    }
    return fmt::format("{} = psx_cyc_load_half(cpu, {}, {}, 0x{:X}u);",
                       reg_name(rt), addr, rt, mask);
}

std::string CodeGenerator::translate_lwl(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    // LWL: Load Word Left - merges high bytes from the aligned word into rt.
    // psx_lwl runs the full R3000A load interlock on the aligned address (GPR_DEP rs,
    // arm LDWhich=rt) and returns the merged value.
    std::string addr = (offset == 0) ? reg_name(rs)
                                     : fmt::format("{} + {}", reg_name(rs), (int32_t)offset);
    uint32_t mask = 1u << rs;
    if (config_.optimize_zero_reg && rt == 0) {
        return fmt::format("(void)psx_lwl(cpu, {}, {}, 0, 0x{:X}u);", addr, reg_name(rt), mask);
    }
    return fmt::format("{} = psx_lwl(cpu, {}, {}, {}, 0x{:X}u);",
                       reg_name(rt), addr, lwlr_merge_operand(rt), rt, mask);
}

std::string CodeGenerator::translate_lwr(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    // LWR: Load Word Right - merges low bytes from the aligned word into rt.
    std::string addr = (offset == 0) ? reg_name(rs)
                                     : fmt::format("{} + {}", reg_name(rs), (int32_t)offset);
    uint32_t mask = 1u << rs;
    if (config_.optimize_zero_reg && rt == 0) {
        return fmt::format("(void)psx_lwr(cpu, {}, {}, 0, 0x{:X}u);", addr, reg_name(rt), mask);
    }
    return fmt::format("{} = psx_lwr(cpu, {}, {}, {}, 0x{:X}u);",
                       reg_name(rt), addr, lwlr_merge_operand(rt), rt, mask);
}

std::string CodeGenerator::translate_swl(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    // SWL: Store Word Left - stores rt into memory using addr, MSB side
    // Generate a helper call: psx_swl(cpu, addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("psx_swl(cpu, {}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("psx_swl(cpu, {} + {}, {});",
                          reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_swr(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    // SWR: Store Word Right - stores rt into memory using addr, LSB side
    // Generate a helper call: psx_swr(cpu, addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("psx_swr(cpu, {}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("psx_swr(cpu, {} + {}, {});",
                          reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_sb(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (offset == 0) {
        return fmt::format("psx_store_cycle_barrier(); cpu->write_byte({}, (uint8_t){});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("psx_store_cycle_barrier(); cpu->write_byte({} + {}, (uint8_t){});",
                          reg_name(rs), offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_sh(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (offset == 0) {
        return fmt::format("psx_store_cycle_barrier(); cpu->write_half({}, (uint16_t){});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("psx_store_cycle_barrier(); cpu->write_half({} + {}, (uint16_t){});",
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

    return fmt::format("{{ int64_t result = (int64_t)(int32_t){} * (int64_t)(int32_t){}; cpu->lo = (uint32_t)result; cpu->hi = (uint32_t)(result >> 32); }}"
                      "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_set(cpu, psx_mult_latency_s({}));\n#endif",
                      reg_name(rs), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_multu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    return fmt::format("{{ uint64_t result = (uint64_t){} * (uint64_t){}; cpu->lo = (uint32_t)result; cpu->hi = (uint32_t)(result >> 32); }}"
                      "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_set(cpu, psx_mult_latency_u({}));\n#endif",
                      reg_name(rs), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_div(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    // MIPS division: quotient in LO, remainder in HI
    // Handle division by zero (MIPS behavior: result is unpredictable but doesn't trap)
    return fmt::format("if ({} != 0) {{ cpu->lo = (uint32_t)((int32_t){} / (int32_t){}); cpu->hi = (uint32_t)((int32_t){} % (int32_t){}); }} else {{ cpu->lo = ((int32_t){} >= 0) ? 0xFFFFFFFF : 1; cpu->hi = (uint32_t){}; }}"
                      "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_set(cpu, 37u);\n#endif",
                      reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rs));
}

std::string CodeGenerator::translate_divu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    // MIPS division: quotient in LO, remainder in HI
    // Handle division by zero (MIPS behavior: result is unpredictable but doesn't trap)
    return fmt::format("if ({} != 0) {{ cpu->lo = {} / {}; cpu->hi = {} % {}; }} else {{ cpu->lo = 0xFFFFFFFF; cpu->hi = {}; }}"
                      "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_set(cpu, 37u);\n#endif",
                      reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_mfhi(uint32_t instr) {
    uint32_t rd = get_rd(instr);

    // Stall until the mult/div completion deadline (faithful R3000A) — happens
    // even when rd==$zero (the read still stalls on HW).
    const char* stall = "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_stall(cpu);\n#endif";
    if (config_.optimize_zero_reg && rd == 0) {
        return fmt::format("/* nop: write to $zero */{}", stall);
    }

    return fmt::format("{} = cpu->hi;{}", reg_name(rd), stall);
}

std::string CodeGenerator::translate_mflo(uint32_t instr) {
    uint32_t rd = get_rd(instr);

    const char* stall = "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_stall(cpu);\n#endif";
    if (config_.optimize_zero_reg && rd == 0) {
        return fmt::format("/* nop: write to $zero */{}", stall);
    }

    return fmt::format("{} = cpu->lo;{}", reg_name(rd), stall);
}

std::string CodeGenerator::translate_mthi(uint32_t instr) {
    uint32_t rs = get_rs(instr);

    return fmt::format("cpu->hi = {};", reg_name(rs));
}

std::string CodeGenerator::translate_mtlo(uint32_t instr) {
    uint32_t rs = get_rs(instr);

    return fmt::format("cpu->lo = {};", reg_name(rs));
}

std::string CodeGenerator::generate_branch_condition(uint32_t instr, uint32_t addr) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    auto keep_branch_if_wide = [&](std::string cond) {
        if (!config_.ws_cull_branch_keep_sites.count(addr))
            return cond;
        return fmt::format("psx_ws_x_margin() > 0 ? 0 : ({}) /* ws branch keep */",
                           cond);
    };

    // REGIMM branches. R3000A hardware decodes EVERY rt value here, not just
    // the four assembler mnemonics: the branch sense is rt bit 0 (0 = bltz,
    // 1 = bgez) and the link register is written iff (rt & 0x1E) == 0x10 —
    // Beetle cpu.cpp op_BCOND: result = (int32)(rs ^ (rt<<31)) < 0,
    // link = ((rt & 0x1E) == 0x10). Undefined rt values appear when discovery
    // sweeps data-as-code into a function; emitting the hardware decode keeps
    // the regen alive AND matches the oracle if the word is ever executed.
    if (opcode == 0x01) {
        uint32_t regimm_op = (instr >> 16) & 0x1F;
        if ((regimm_op & 0x01u) == 0x00u) { // bltz family (incl. bltzal + undefined mirrors)
            // LEFT-edge funnel bltz: reject only past the revealed margin.
            // Identity at 4:3 (margin 0). Two sources: (a) auto_screen_x's
            // detect_cull_bltz_sites classification (ws_cull_bltz_pcs_), and
            // (b) explicit [widescreen.cull] bltz_sites — the left-edge
            // counterpart to slti_sites, for X-only funnels auto_screen_x can't
            // qualify (whose bltz would otherwise never be widened).
            if (regimm_op == 0x00 &&
                (ws_cull_bltz_pcs_.count(addr) || config_.ws_cull_bltz_sites.count(addr)))
                return fmt::format("psx_ws_cull_bltz({}) /* ws cull (left edge) */",
                                   reg_name(rs));
            if (regimm_op == 0x00 && config_.ws_cull_nclip_keep_sites.count(addr))
                return fmt::format("psx_ws_x_margin() > 0 ? 0 : ((int32_t){} < 0) /* ws nclip keep */",
                                   reg_name(rs));
            return keep_branch_if_wide(
                fmt::format("(int32_t){} < 0", reg_name(rs)));
        } else {                            // bgez family (incl. bgezal + undefined mirrors)
            return keep_branch_if_wide(
                fmt::format("(int32_t){} >= 0", reg_name(rs)));
        }
    }

    // Standard branches
    switch (opcode) {
        case 0x04: // beq
        case 0x14: // beql
            return keep_branch_if_wide(
                fmt::format("{} == {}", reg_name(rs), reg_name(rt)));

        case 0x05: // bne
        case 0x15: // bnel
            return keep_branch_if_wide(
                fmt::format("{} != {}", reg_name(rs), reg_name(rt)));

        case 0x06: // blez
        case 0x16: // blezl
            return keep_branch_if_wide(
                fmt::format("(int32_t){} <= 0", reg_name(rs)));

        case 0x07: // bgtz
        case 0x17: // bgtzl
            return keep_branch_if_wide(
                fmt::format("(int32_t){} > 0", reg_name(rs)));
    }

    return keep_branch_if_wide("0 /* unknown branch condition: defaults to not-taken */");
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

    // Full-word-guarded terrain-frustum half-angle constants. Scaling the
    // tangent is the geometric counterpart of widening the horizontal
    // projection; adding screen pixels directly to 12-bit angle units is not.
    for (const auto& site : config_.ws_cull_angle_sites) {
        if ((site.address & 0x1FFFFFFFu) !=
            (addr & 0x1FFFFFFFu)) continue;
        if (instr != site.expected) {
            if (config_.overlay_mode) continue;
            fmt::print(stderr,
                       "ERROR: cull angle expected 0x{:08X} at 0x{:08X}, "
                       "found 0x{:08X}\n",
                       site.expected, addr, instr);
            std::exit(1);
        }
        if ((opcode != 0x08u && opcode != 0x09u) ||
            get_rs(instr) != 0u) {
            fmt::print(stderr,
                       "ERROR: cull angle site 0x{:08X} is not "
                       "ADDI/ADDIU rt,zero,imm (0x{:08X})\n",
                       addr, instr);
            std::exit(1);
        }
        const uint32_t dst = get_rt(instr);
        const int32_t vanilla = (int16_t)(instr & 0xFFFFu);
        return fmt::format(
            "{} = psx_ws_angle_widen({}u);"
            "  /* aspect-scaled terrain frustum half-angle */{}",
            reg_name(dst), (uint32_t)vanilla, comment);
    }

    for (const auto& site : config_.ws_signed_x_bound_sites) {
        if ((site.address & 0x1FFFFFFFu) != (addr & 0x1FFFFFFFu)) continue;
        if (instr != site.expected) {
            if (config_.overlay_mode) continue;
            fmt::print(stderr,
                       "ERROR: signed_x_bound expected 0x{:08X} at 0x{:08X}, found 0x{:08X}\n",
                       site.expected, addr, instr);
            std::exit(1);
        }
        const int32_t vanilla = (int32_t)((instr & 0xFFFFu) << 16);
        return fmt::format("{} = (uint32_t)psx_ws_player_x_bound((int32_t)0x{:08X});"
                           "  /* typed native-wide signed X bound */{}",
                           reg_name(get_rt(instr)), (uint32_t)vanilla, comment);
    }

    // Full-word-guarded, camera-horizontal model-participation cones. The
    // runtime helper preserves the original compare at 4:3 and every vanilla
    // keep verdict, adding only the aspect-derived horizontal fringe.
    for (const auto& site : config_.ws_aspect_cone.sites) {
        if ((site.address & 0x1FFFFFFFu) != (addr & 0x1FFFFFFFu)) continue;
        if (instr != site.expected) {
            if (config_.overlay_mode) continue;
            fmt::print(stderr,
                       "ERROR: aspect cone expected 0x{:08X} at 0x{:08X}, "
                       "found 0x{:08X}\n",
                       site.expected, addr, instr);
            std::exit(1);
        }
        const bool is_slti = opcode == 0x0A;
        const bool is_slt =
            opcode == 0x00 && (instr & 0x3Fu) == 0x2Au &&
            ((instr >> 6) & 0x1Fu) == 0u;
        if (!is_slti && !is_slt) {
            fmt::print(stderr,
                       "ERROR: aspect cone site 0x{:08X} is not signed "
                       "SLTI/SLT "
                       "(0x{:08X})\n", addr, instr);
            std::exit(1);
        }
        const uint32_t dst = is_slti ? get_rt(instr) : get_rd(instr);
        const std::string vanilla = is_slti
            ? fmt::format("((int32_t){} < (int32_t){}) ? 1u : 0u",
                          reg_name(get_rs(instr)),
                          (int32_t)(int16_t)(instr & 0xFFFFu))
            : fmt::format("((int32_t){} < (int32_t){}) ? 1u : 0u",
                          reg_name(get_rs(instr)), reg_name(get_rt(instr)));
        const auto effective_reg = [](uint32_t site_reg,
                                      uint32_t default_reg) {
            return site_reg == 0xFFFFFFFFu ? default_reg : site_reg;
        };
        return fmt::format(
            "{} = psx_ws_aspect_cone_result(0x{:08X}u, ({}), {}, "
            "(int32_t)(int16_t){}, (int32_t)(int16_t){}, "
            "(int32_t)(int16_t){});"
            "  /* aspect-aware horizontal model participation */{}",
            reg_name(dst), addr, vanilla,
            reg_name(effective_reg(
                site.object_reg, config_.ws_aspect_cone.object_reg)),
            reg_name(effective_reg(
                site.x_reg, config_.ws_aspect_cone.x_reg)),
            reg_name(effective_reg(
                site.z_reg, config_.ws_aspect_cone.z_reg)),
            reg_name(effective_reg(
                site.y_reg, config_.ws_aspect_cone.y_reg)), comment);
    }

    // Full-word-guarded object/model participation compares. At 4:3 the
    // helper returns the comparison's vanilla value; when widescreen reveals
    // extra world it returns the configured keep verdict. Overlay variants
    // with another instruction at the same VA are deliberately untouched.
    for (const auto& site : config_.ws_cull_keep_sites) {
        if ((site.address & 0x1FFFFFFFu) != (addr & 0x1FFFFFFFu)) continue;
        if (instr != site.expected) {
            if (config_.overlay_mode) continue;
            fmt::print(stderr,
                       "ERROR: cull keep expected 0x{:08X} at 0x{:08X}, found 0x{:08X}\n",
                       site.expected, addr, instr);
            std::exit(1);
        }
        uint32_t dst = 0;
        std::string vanilla;
        if (opcode == 0x0A) { // SLTI
            dst = get_rt(instr);
            vanilla = fmt::format("((int32_t){} < {}) ? 1u : 0u",
                                  reg_name(get_rs(instr)), (int)get_imm16(instr));
        } else if (opcode == 0x0B) { // SLTIU (sign-extended immediate)
            dst = get_rt(instr);
            vanilla = fmt::format("({} < (uint32_t){}) ? 1u : 0u",
                                  reg_name(get_rs(instr)), (int)get_imm16(instr));
        } else if (opcode == 0x00 && funct == 0x2A) { // SLT
            dst = get_rd(instr);
            vanilla = fmt::format("((int32_t){} < (int32_t){}) ? 1u : 0u",
                                  reg_name(get_rs(instr)), reg_name(get_rt(instr)));
        } else if (opcode == 0x00 && funct == 0x2B) { // SLTU
            dst = get_rd(instr);
            vanilla = fmt::format("({} < {}) ? 1u : 0u",
                                  reg_name(get_rs(instr)), reg_name(get_rt(instr)));
        } else {
            fmt::print(stderr,
                       "ERROR: cull keep site 0x{:08X} is not a compare (0x{:08X})\n",
                       addr, instr);
            std::exit(1);
        }
        return fmt::format(
            "{} = psx_ws_cull_keep_result(({}), {}u);"
            "  /* ws maximal object/model participation */{}",
            reg_name(dst), vanilla, site.result, comment);
    }

    // Screen-edge compares whose BOUND follows the live reveal margin. Unlike a
    // keep site this never pins the verdict, so a clip-code packer keeps
    // classifying honestly and the clipper subdivides at the revealed edge
    // instead of emitting a primitive the GPU will discard. Identity at 4:3.
    for (const auto& site : config_.ws_cull_widen_sites) {
        if ((site.address & 0x1FFFFFFFu) != (addr & 0x1FFFFFFFu)) continue;
        if (instr != site.expected) {
            if (config_.overlay_mode) continue;
            fmt::print(stderr,
                       "ERROR: cull widen expected 0x{:08X} at 0x{:08X}, found 0x{:08X}\n",
                       site.expected, addr, instr);
            std::exit(1);
        }
        const bool imm_mode = site.mode == PSXRecompV4::WsCullWidenMode::ImmUpper ||
                              site.mode == PSXRecompV4::WsCullWidenMode::ImmLower;
        uint32_t dst = 0;
        std::string call;
        if (imm_mode) {
            if (!(opcode == 0x0A || opcode == 0x0B)) {
                fmt::print(stderr,
                           "ERROR: cull widen site 0x{:08X} imm mode on non-SLTI (0x{:08X})\n",
                           addr, instr);
                std::exit(1);
            }
            dst = get_rt(instr);
            call = fmt::format(
                "{}({}, {}u)",
                site.mode == PSXRecompV4::WsCullWidenMode::ImmUpper ? "psx_ws_cull_slti"
                                                       : "psx_ws_cull_slti_lower",
                reg_name(get_rs(instr)), get_imm16(instr));
        } else {
            if (!(opcode == 0x00 && (funct == 0x2A || funct == 0x2B))) {
                fmt::print(stderr,
                           "ERROR: cull widen site 0x{:08X} bound mode on non-SLT (0x{:08X})\n",
                           addr, instr);
                std::exit(1);
            }
            dst = get_rd(instr);
            call = fmt::format(
                "psx_ws_cull_slt_widen({}, {}, {})",
                reg_name(get_rs(instr)), reg_name(get_rt(instr)),
                site.mode == PSXRecompV4::WsCullWidenMode::BoundRt ? 1 : 0);
        }
        return fmt::format(
            "{} = (uint32_t){};"
            "  /* ws cull bound follows reveal margin */{}",
            reg_name(dst), call, comment);
    }

    // Widescreen automatic far-backdrop column PRELOAD ([widescreen.cull]
    // auto_backdrop). At a detected window's START/END finalize, route the value
    // through psx_ws_backdrop_value(orig, is_end): identity at 4:3, but in
    // native-wide it forces START->0 / END->0x100, so the generator's own low/
    // high clamps preload the whole finite tile row (filling the revealed 16:9
    // margin). ws_backdrop_sites_ is populated per function by
    // detect_backdrop_windows() only when config_.ws_auto_backdrop_preload is
    // set — inert and byte-identical when the feature is off. The instruction
    // shape is verified; a mismatch is a loud build error (detector/codegen drift).
    if (!ws_backdrop_sites_.empty()) {
        auto bd = ws_backdrop_sites_.find(addr);
        if (bd != ws_backdrop_sites_.end()) {
            int is_end = (bd->second.first == WS_BD_END) ? 1 : 0;
            int wcols  = bd->second.second;
            const char* tag = is_end ? "END" : "START";
            if (opcode == 0x00 && (funct == 0x21 || funct == 0x25)) {  // addu/or (move)
                uint32_t rd = get_rd(instr), rs = get_rs(instr), rt = get_rt(instr);
                uint32_t src = (rt == 0) ? rs : rt;
                return fmt::format("{} = psx_ws_backdrop_value({}, {}, {});"
                                   "  /* ws backdrop preload {} */{}",
                                   reg_name(rd), reg_name(src), is_end, wcols, tag, comment);
            }
            if (opcode == 0x09 || opcode == 0x08) {  // addiu / addi
                uint32_t rs = get_rs(instr), rt = get_rt(instr);
                int16_t imm = get_imm16(instr);
                std::string orig = (rs == 0)
                    ? fmt::format("(uint32_t){}", (int)imm)
                    : fmt::format("(uint32_t)({} + {})", reg_name(rs), (int)imm);
                return fmt::format("{} = psx_ws_backdrop_value({}, {}, {});"
                                   "  /* ws backdrop preload {} */{}",
                                   reg_name(rt), orig, is_end, wcols, tag, comment);
            }
            fmt::print(stderr, "ERROR: [widescreen.cull] auto_backdrop site 0x{:08X} "
                       "is not addu/or/addiu (opcode 0x{:02X} funct 0x{:02X})\n",
                       addr, opcode, funct);
            std::exit(1);
        }
    }

    // Widescreen cull-margin widening ([widescreen.cull] sites). Emit the
    // window immediate with a runtime margin term psx_ws_x_margin() — 0 at
    // 4:3/boot/menu/FMV, ~half-the-extra-width when stretching — so the world-
    // space draw cull tracks the aspect and one build serves both. Each site's
    // instruction type is verified; a mismatch is a loud build error in main-EXE
    // mode (a bad address would silently mis-emit otherwise). In OVERLAY mode the
    // same address holds different code across scene variants, so a mismatch is
    // expected — apply the transform only where the bytes match, else fall
    // through to the vanilla translation (see CodeGenConfig::overlay_mode).
    if (config_.ws_cull_bias_sites.count(addr)) {
        if (opcode == 0x08 || opcode == 0x09) {  // addi / addiu
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t imm = get_imm16(instr);
            const std::string margin =
                config_.ws_cull_activation_guard_pixels > 0
                    ? fmt::format(
                          "(psx_ws_x_margin() > 0 ? psx_ws_x_margin() + {} : 0)",
                          config_.ws_cull_activation_guard_pixels)
                    : "psx_ws_x_margin()";
            return fmt::format("{} = {} + ((int32_t){} + {});{}",
                               reg_name(rt), reg_name(rs), imm, margin,
                               comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] bias site 0x{:08X} is not "
                       "addi/addiu (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // overlay variant: addr is different code here — fall through to vanilla.
    }
    if (config_.ws_cull_vxrange_sites.count(addr)) {
        if (opcode == 0x0B) {  // sltiu
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            uint16_t imm = get_imm16_u(instr);
            return fmt::format("{} = psx_ws_cull_vxrange({}, {});"
                               "  /* ws cull masked-u16 X window */{}",
                               reg_name(rt), reg_name(rs), (unsigned)imm, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] vxrange site 0x{:08X} is not "
                       "sltiu (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // Overlay variant at the same address: leave nonmatching code unchanged.
    }
    if (config_.ws_cull_depth_sites.count(addr)) {
        if (opcode == 0x0A) {  // slti
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t imm = get_imm16(instr);
            return fmt::format("{} = ((int32_t){} < psx_ws_depth_bound({})) ? 1 : 0;"
                               "  /* ws cull depth */{}",
                               reg_name(rt), reg_name(rs), (int)imm, comment);
        }
        if (opcode == 0x0B) {  // sltiu: MIPS sign-extends its immediate
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t imm = get_imm16(instr);
            return fmt::format("{} = ((uint32_t){} < (uint32_t)psx_ws_depth_bound({})) ? 1 : 0;"
                               "  /* ws cull depth unsigned */{}",
                               reg_name(rt), reg_name(rs), (int)imm, comment);
        }
        if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] depth site 0x{:08X} is not "
                       "slti/sltiu (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // Overlay variant at the same address: leave nonmatching code unchanged.
    }
    // Side frustum-plane normal-X load: `lw rt,off(rs)` reads the nx of a
    // plane sign-tested per corner (dot = nx*px + nz*pz). The helper scales
    // nx by the inverse aspect factor (4*den)/(3*num) while revealed, which
    // widens the cone by exactly atan((3*num)/(4*den)*tan(theta)); identity
    // at 4:3.
    if (config_.ws_cull_plane_nx_sites.count(addr)) {
        if (opcode == 0x23) {  // lw
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t offset = get_imm16(instr);
            std::string laddr = (offset == 0)
                ? reg_name(rs)
                : fmt::format("{} + {}", reg_name(rs), (int)offset);
            uint32_t mask = 1u << rs;
            return fmt::format("{} = (uint32_t)psx_ws_plane_nx((int32_t)psx_cyc_load_word(cpu, {}, {}, 0x{:X}u));"
                               "  /* ws cull plane nx load */{}",
                               reg_name(rt), laddr, rt, mask, comment);
        }
        if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] plane_nx site 0x{:08X} is not "
                       "lw (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // Overlay variant at the same address: leave nonmatching code unchanged.
    }

    // Per-primitive X-reject bound load: `lw rt,off(rs)` reads the bound a
    // masked-u16 screen X is sltu-compared against. While the margins are
    // revealed the helper yields INT32_MAX (reject disabled; the wide-surface
    // scissor clips the overflow); identity at 4:3.
    if (config_.ws_cull_xclip_load_sites.count(addr)) {
        if (opcode == 0x23) {  // lw
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t offset = get_imm16(instr);
            std::string laddr = (offset == 0)
                ? reg_name(rs)
                : fmt::format("{} + {}", reg_name(rs), (int)offset);
            uint32_t mask = 1u << rs;
            return fmt::format("{} = psx_ws_xclip_bound(psx_cyc_load_word(cpu, {}, {}, 0x{:X}u));"
                               "  /* ws cull xclip bound load */{}",
                               reg_name(rt), laddr, rt, mask, comment);
        }
        if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] xclip_load site 0x{:08X} is not "
                       "lw (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // Overlay variant at the same address: leave nonmatching code unchanged.
    }
    if (config_.ws_cull_range_sites.count(addr)) {
        if (opcode == 0x0B) {  // sltiu
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t imm = get_imm16(instr);
            const std::string margin =
                config_.ws_cull_activation_guard_pixels > 0
                    ? fmt::format(
                          "(psx_ws_x_margin() > 0 ? psx_ws_x_margin() + {} : 0)",
                          config_.ws_cull_activation_guard_pixels)
                    : "psx_ws_x_margin()";
            return fmt::format(
                "{} = ({} < (uint32_t)((int32_t){} + 2*{})) ? 1 : 0;{}",
                reg_name(rt), reg_name(rs), imm, margin, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] range site 0x{:08X} is not "
                       "sltiu (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // overlay variant: addr is different code here — fall through to vanilla.
    }
    if (config_.ws_cull_a1_sites.count(addr)) {
        if (instr == 0x00000000u) {  // must be a nop we can safely repurpose
            // a1 = $5: widen the caller-supplied margin (param-margin classifiers).
            return fmt::format("cpu->gpr[5] = cpu->gpr[5] + psx_ws_x_margin();"
                               "  /* ws cull a1 bias */{}", comment);
        } else if (opcode == 0x00 && get_funct(instr) == 0x21) {
            // move rD,a1 (addu rD,a1,zero or addu rD,zero,a1): some Capcom
            // classifiers copy the caller-supplied horizontal margin
            // immediately, leaving no delay-slot nop to repurpose. Widen the
            // copied value; a1 itself and the vertical margin stay unchanged.
            uint32_t rs = get_rs(instr), rt = get_rt(instr), rd = get_rd(instr);
            if ((rs == 5 && rt == 0) || (rs == 0 && rt == 5))
                return fmt::format("{} = cpu->gpr[5] + psx_ws_x_margin();"
                                   "  /* ws cull copied a1 bias */{}",
                                   reg_name(rd), comment);
            if (!config_.overlay_mode) {
                fmt::print(stderr, "ERROR: [widescreen.cull] a1 site 0x{:08X} "
                           "is addu but not move rD,a1 (0x{:08X})\n", addr, instr);
                std::exit(1);
            }
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] a1 site 0x{:08X} is not a "
                       "nop or move rD,a1 (0x{:08X})\n", addr, instr);
            std::exit(1);
        }
        // overlay variant: addr is different code here — fall through to vanilla.
    }
    if (config_.ws_cull_screen_x_sites.count(addr)) {
        if (opcode == 0x0B) {  // sltiu rt,rs,imm
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            uint16_t imm = get_imm16_u(instr);
            return fmt::format("{} = psx_ws_cull_sltiu({}, {});"
                               " /* ws explicit screen-x cull */{}",
                               reg_name(rt), reg_name(rs), imm, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] screen_x site 0x{:08X} "
                       "is not sltiu (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // Overlay variant: the configured main-EXE PC holds different code.
    }
    // Widescreen automatic horizontal-FOV cull widening ([widescreen.cull]
    // auto_screen_x). ws_auto_cull_func_ is set when this function carries the
    // GTE screen-extent reject signature, so a per-vertex width compare here is
    // `sltiu rt, SX, 0x140` (or inclusive 0x141). Emit it widened by
    // +2*psx_ws_x_margin() (0 at 4:3 ⇒ byte-identical; = the wide-surface extra
    // when 16:9), so geometry out to the wide frame edge is submitted rather
    // than culled at the 320 boundary. Same shape as an explicit range_site but
    // applied by signature — no per-address list. (Reached only when the addr is
    // not already an explicit cull site, which returns above.)
    if (ws_auto_cull_func_ && (opcode == 0x0B || opcode == 0x0A)) {  // sltiu / slti
        uint16_t uimm = get_imm16_u(instr);
        if (psx_ws_cull_imm_in(uimm, config_.ws_cull_w_imms.data(),
                               (int)config_.ws_cull_w_imms.size())) {
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            if (opcode == 0x0B) {
                // Route through the shared runtime helper psx_ws_cull_sltiu so the
                // gcc emit and the interpreter both widen identically.
                // It sign-extends SX and shifts by +margin (wide window
                // -margin <= SX < imm+margin, both edges); at margin=0 it reduces
                // bit-for-bit to the vanilla `SX <u imm` verdict (4:3 byte-identical).
                return fmt::format("{} = psx_ws_cull_sltiu({}, {});"
                                   "  /* ws auto screen-x cull (both edges) */{}",
                                   reg_name(rt), reg_name(rs), (int)uimm, comment);
            }
            // Signed min/max funnel: `slti v, minSX, W` is the RIGHT edge only
            // (the paired left edge is the classified bltz — see
            // generate_branch_condition); widen by +margin.
            return fmt::format("{} = psx_ws_cull_slti({}, {});"
                               "  /* ws auto screen-x cull (right edge) */{}",
                               reg_name(rt), reg_name(rs), (int)uimm, comment);
        }
    }
    // Explicit signed lower-bound widen (`slti rt, sx, -W`). Unlike the
    // min/max right-edge helper below, this moves the immediate left by one
    // live reveal margin. The helper sign-extends the 16-bit immediate.
    if (config_.ws_cull_slti_lower_sites.count(addr)) {
        if (opcode == 0x0A) {  // slti
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            uint16_t uimm = get_imm16_u(instr);
            return fmt::format("{} = psx_ws_cull_slti_lower({}, {});"
                               "  /* ws cull slti lower site */{}",
                               reg_name(rt), reg_name(rs), (int)uimm, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] slti_lower site "
                       "0x{:08X} is not slti (opcode 0x{:02X})\n",
                       addr, opcode);
            std::exit(1);
        }
    }
    // Explicit signed right-edge widen site ([widescreen.cull] slti_sites) for
    // funnel functions the auto-detector cannot qualify (X-only, no H compare).
    if (config_.ws_cull_slti_sites.count(addr)) {
        if (opcode == 0x0A) {  // slti
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            uint16_t uimm = get_imm16_u(instr);
            return fmt::format("{} = psx_ws_cull_slti({}, {});"
                               "  /* ws cull slti site (right edge) */{}",
                               reg_name(rt), reg_name(rs), (int)uimm, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] slti site 0x{:08X} is not "
                       "slti (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // overlay variant: addr is different code here — fall through to vanilla.
    }
    // Explicit horizontal low-edge widen for negate-form classifiers:
    // `subu rd, zero, rt` computes -bound; subtracting the horizontal margin
    // moves that bound left. Identity at 4:3.
    if (config_.ws_cull_negsub_sites.count(addr)) {
        if (opcode == 0x00 && funct == 0x23 && get_rs(instr) == 0) {
            uint32_t rt = get_rt(instr), rd = get_rd(instr);
            return fmt::format("{} = 0u - {} - (uint32_t)psx_ws_x_margin();"
                               "  /* ws cull negsub */{}",
                               reg_name(rd), reg_name(rt), comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.cull] negsub site 0x{:08X} is not "
                       "subu rD,zero,rT (0x{:08X})\n", addr, instr);
            std::exit(1);
        }
        // Overlay variant at the same address: leave nonmatching code unchanged.
    }
    // Widescreen backdrop screenX squash ([widescreen.backdrop] x_sites). The
    // site is the `sh rt,off(base)` storing a parallax 2D backdrop layer's
    // final screen-X; squash the stored value around the screen centre so the
    // un-GTE'd backdrop tracks the 16:9 FOV (psx_ws_backdrop_x is identity at
    // 4:3). Instruction type is verified; a mismatch is a loud build error.
    if (config_.ws_backdrop_x_sites.count(addr)) {
        if (opcode == 0x29) {  // sh
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t offset = get_imm16(instr);
            std::string store_pc = fmt::format("g_debug_last_store_pc = 0x{:08X}u; ", addr);
            if (offset == 0)
                return store_pc + fmt::format("psx_store_cycle_barrier(); cpu->write_half({}, (uint16_t)psx_ws_backdrop_x((int16_t){}));{}",
                                   reg_name(rs), reg_name(rt), comment);
            return store_pc + fmt::format("psx_store_cycle_barrier(); cpu->write_half({} + {}, (uint16_t)psx_ws_backdrop_x((int16_t){}));{}",
                               reg_name(rs), offset, reg_name(rt), comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.backdrop] x site 0x{:08X} is not "
                       "sh (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
        // overlay variant: addr is different code here — fall through to vanilla.
    }

    // Widescreen pure-2D background tile-loop widen ([widescreen.bg2d]). The
    // per-layer BG renderer draws `count` 16px tile columns from a start column /
    // start screen-x derived from the camera scroll. Rewrite those three values
    // (via the gpu.c psx_ws_bg2d_* helpers — identity at 4:3 / 512 hi-res) so
    // the loop draws the 16:9 reveal columns on both sides. Each site's opcode is
    // verified; a mismatch is a loud build error (main-EXE addresses).
    if (config_.ws_bg2d_count_site && addr == config_.ws_bg2d_count_site) {
        if (opcode == 0x09 || opcode == 0x0D) {  // addiu / ori  (li rt,imm)
            uint32_t rt = get_rt(instr);
            uint16_t imm = get_imm16_u(instr);
            return fmt::format("{} = (uint32_t)psx_ws_bg2d_cols({});{}",
                               reg_name(rt), (int)imm, comment);
        } else if (opcode == 0x0A || opcode == 0x0B) {  // slti/sltiu rt,rs,imm (loop bound compare)
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            uint16_t imm = get_imm16_u(instr);
            if (opcode == 0x0A)  // signed variant
                return fmt::format("{} = ((int32_t){} < psx_ws_bg2d_cols({})) ? 1u : 0u;{}",
                                   reg_name(rt), reg_name(rs), (int)imm, comment);
            return fmt::format("{} = ({} < (uint32_t)psx_ws_bg2d_cols({})) ? 1u : 0u;{}",
                               reg_name(rt), reg_name(rs), (int)imm, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.bg2d] count_site 0x{:08X} is not "
                       "addiu/ori/slti/sltiu (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
    }
    if (config_.ws_bg2d_startcol_site && addr == config_.ws_bg2d_startcol_site) {
        if (opcode == 0x0C) {  // andi rt,rs,imm  (start tile-column mask)
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            uint16_t imm = get_imm16_u(instr);
            return fmt::format("{} = (uint32_t)psx_ws_bg2d_startcol((int)({} & 0x{:X}u), 0x{:X}u);{}",
                               reg_name(rt), reg_name(rs), imm, imm, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.bg2d] startcol_site 0x{:08X} is not "
                       "andi (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
    }
    if (config_.ws_bg2d_startx_site && addr == config_.ws_bg2d_startx_site) {
        if (opcode == 0x00 && (instr & 0x3F) == 0x03) {  // sra rd,rt,sa  (start screen-x)
            uint32_t rt = get_rt(instr), rd = get_rd(instr), sh = get_shamt(instr);
            return fmt::format("{} = (uint32_t)psx_ws_bg2d_startx((int32_t){} >> {});{}",
                               reg_name(rd), reg_name(rt), sh, comment);
        } else if (opcode == 0x00 &&
                   (instr & 0x3F) == 0x23 && get_rs(instr) == 0) {
            // subu rd,zero,rt (MMX4's start screen-x = -(scrollX & 15))
            uint32_t rt = get_rt(instr), rd = get_rd(instr);
            return fmt::format("{} = (uint32_t)psx_ws_bg2d_startx(-(int32_t){});{}",
                               reg_name(rd), reg_name(rt), comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.bg2d] startx_site 0x{:08X} is not "
                       "sra or subu-zero (instr 0x{:08X})\n", addr, instr);
            std::exit(1);
        }
    }
    // Tile-ring streamer leading-edge widen: two addiu computing the left
    // (scrollX-16) and right (scrollX+16) stream world-X; push them out by
    // LEFT*16 so the ring is populated across the widened column window.
    if (config_.ws_bg2d_stream_left_site && addr == config_.ws_bg2d_stream_left_site) {
        if (opcode == 0x08 || opcode == 0x09) {  // addi / addiu
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t imm = get_imm16(instr);
            return fmt::format("{} = (uint32_t)psx_ws_bg2d_stream_left((int32_t){} + ({}));{}",
                               reg_name(rt), reg_name(rs), imm, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.bg2d] stream_left_site 0x{:08X} is not "
                       "addi/addiu (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
    }
    if (config_.ws_bg2d_stream_right_site && addr == config_.ws_bg2d_stream_right_site) {
        if (opcode == 0x08 || opcode == 0x09) {  // addi / addiu
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            int16_t imm = get_imm16(instr);
            return fmt::format("{} = (uint32_t)psx_ws_bg2d_stream_right((int32_t){} + ({}));{}",
                               reg_name(rt), reg_name(rs), imm, comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.bg2d] stream_right_site 0x{:08X} is not "
                       "addi/addiu (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
    }
    // BG packet-buffer RELOCATION (cure for the 1024-slot/1000-cap overflow that the
    // widen causes in dense stages). bufbase_site = the driver addu computing the buffer
    // address (base + bufidx*0x4000); cap_site = the renderer's per-frame tile-cap slti.
    // Helpers relocate to a larger free buffer + raise the cap when the widen is active,
    // identity otherwise (4:3 / other games byte-identical).
    if (config_.ws_bg2d_bufbase_site && addr == config_.ws_bg2d_bufbase_site) {
        if (opcode == 0x00 && (instr & 0x3F) == 0x21) {  // addu rd,rs,rt  (BG buffer address)
            uint32_t rs = get_rs(instr), rt = get_rt(instr), rd = get_rd(instr);
            return fmt::format("{} = (uint32_t)psx_ws_mmx6_bg_bufbase((int)({} + {}));{}",
                               reg_name(rd), reg_name(rs), reg_name(rt), comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.bg2d] bufbase_site 0x{:08X} is not "
                       "addu (instr 0x{:08X})\n", addr, instr);
            std::exit(1);
        }
    }
    if (config_.ws_bg2d_cap_site && addr == config_.ws_bg2d_cap_site) {
        if (opcode == 0x0A) {  // slti rt,rs,imm  (BG per-frame tile cap compare)
            uint32_t rs = get_rs(instr), rt = get_rt(instr);
            return fmt::format("{} = (uint32_t)psx_ws_bg2d_undercap((int32_t){}, {});{}",
                               reg_name(rt), reg_name(rs), (int16_t)(instr & 0xFFFFu), comment);
        } else if (!config_.overlay_mode) {
            fmt::print(stderr, "ERROR: [widescreen.bg2d] cap_site 0x{:08X} is not "
                       "slti (opcode 0x{:02X})\n", addr, opcode);
            std::exit(1);
        }
    }

    // Persistent game-option init store ([persist_options] in game_options.toml).
    // The site is the boot-init sb/sh that writes a config global's DEFAULT value
    // (Tomba MESSAGE / SOUND / VIBRATION / ADJUST SCREEN). Route the stored value
    // through psx_game_option_store(addr, val): it returns a value persisted from a
    // prior session for that address, else `val` unchanged — so the saved setting
    // overrides the default exactly once, at initialization, and a fresh install
    // (no saved file) is byte-identical. The in-OPTION write is a different
    // instruction (option overlay), untouched, so the player can still change it.
    // Verified sb/sh; a mismatch is a loud build error.
    if (config_.persist_init_store_sites.count(addr)) {
        uint32_t rs = get_rs(instr), rt = get_rt(instr);
        int16_t offset = get_imm16(instr);
        std::string aexpr = (offset != 0)
            ? fmt::format("({} + {})", reg_name(rs), offset)
            : std::string(reg_name(rs));
        std::string store_pc = fmt::format("g_debug_last_store_pc = 0x{:08X}u; ", addr);
        if (opcode == 0x28)  // sb
            return store_pc + fmt::format("psx_store_cycle_barrier(); cpu->write_byte({0}, (uint8_t)psx_game_option_store({0}, (int){1}));{2}",
                               aexpr, reg_name(rt), comment);
        if (opcode == 0x29)  // sh
            return store_pc + fmt::format("psx_store_cycle_barrier(); cpu->write_half({0}, (uint16_t)psx_game_option_store({0}, (int){1}));{2}",
                               aexpr, reg_name(rt), comment);
        fmt::print(stderr, "ERROR: [persist_options] init site 0x{:08X} is not sb/sh "
                   "(opcode 0x{:02X})\n", addr, opcode);
        std::exit(1);
    }

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
                    code = fmt::format("{{ uint32_t _jt_{0:08X} = {1};  {2} = 0x{3:08X};  "
                                       "cpu->pc = _jt_{0:08X}; }}  /* jalr */",
                                       addr, reg_name(rs), reg_name(rd), addr + 8);
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
                        // MFC0 is a delayed load (Beetle: LDAbsorb=0, LDWhich=rt) — no
                        // give-back cycles, but it sets ReadFudge=rt so a load in the next
                        // slot gets no fudge. §1+DO_LDS ran in the block's psx_cyc_step.
                        code = fmt::format(
                            "{} = cpu->cop0[{}];"
                            "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    cpu->ld_absorb = 0u; cpu->ld_which_t = {}u;\n#endif"
                            "  /* mfc0 (delayed load) */",
                            reg_name(rt), rd, rt);
                    } else if (cop_op == 0x04) { // MTC0 - move to COP0
                        code = fmt::format("cpu->cop0[{}] = {};  /* mtc0 */", rd, reg_name(rt));
                    } else if (cop_op == 0x10 && (instr & 0x3F) == 0x10) { // RFE
                        // Restore interrupt enable bits: shift bits 5:2 → 3:0.
                        // Fix B: arm the host exception-return escape if this RFE runs
                        // inside the synchronous exception handler (see psx_runtime.h).
                        code = "{ uint32_t sr = cpu->cop0[12]; "
                               "cpu->cop0[12] = (sr & 0xFFFFFFF0u) | ((sr >> 2) & 0x0Fu); } "
                               "psx_rfe_mark_escape();  /* rfe */";
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
                    // Faithful GTE: any COP2 register access stalls to the pending
                    // command completion deadline (gte_execute arms it). Emitted
                    // before the access; cost-free unless an op is still in flight.
                    const char* gte_stall =
                        "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_gte_stall(cpu);\n#endif\n    ";
                    // MFC2/CFC2 (GPR-dest reads): stall to the GTE deadline AND hand the
                    // stall amount to the next instruction(s) as a load-delay give-back
                    // (Beetle MFC2/CFC2: LDAbsorb=gte_ts_done-ts, LDWhich=rt). §1+DO_LDS
                    // ran in the block's psx_cyc_step (COP2 is non-load).
                    const std::string gte_read = fmt::format(
                        "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_gte_read(cpu, {});\n#endif\n    ", rt);
                    if (cop_op == 0x00) { // MFC2 - move from COP2 data
                        const std::string value =
                            PSXRecompGTERegisters::data_read_needs_helper(static_cast<uint8_t>(rd))
                                ? fmt::format("gte_read_data(cpu, {})", rd)
                                : fmt::format("cpu->gte_data[{}]", rd);
                        code = gte_read + (rt == 0
                            ? fmt::format("(void){};  /* mfc2 to $zero: read preserved, write discarded */", value)
                            : fmt::format("{} = {};  /* mfc2 */", reg_name(rt), value));
                    } else if (cop_op == 0x02) { // CFC2 - move from COP2 control
                        const std::string value =
                            PSXRecompGTERegisters::ctrl_read_needs_helper(static_cast<uint8_t>(rd))
                                ? fmt::format("gte_read_ctrl(cpu, {})", rd)
                                : fmt::format("cpu->gte_ctrl[{}]", rd);
                        code = gte_read + (rt == 0
                            ? fmt::format("(void){};  /* cfc2 to $zero: read preserved, write discarded */", value)
                            : fmt::format("{} = {};  /* cfc2 */", reg_name(rt), value));
                    } else if (cop_op == 0x04) { // MTC2 - move to COP2 data
                        if (PSXRecompGTERegisters::data_write_needs_helper(static_cast<uint8_t>(rd))) {
                            code = gte_stall + fmt::format("gte_write_data(cpu, {}, {});  /* mtc2 */", rd, reg_name(rt));
                        } else {
                            code = gte_stall + fmt::format("cpu->gte_data[{}] = {};  /* mtc2 */", rd, reg_name(rt));
                        }
                    } else if (cop_op == 0x06) { // CTC2 - move to COP2 control
                        if (PSXRecompGTERegisters::ctrl_write_needs_helper(static_cast<uint8_t>(rd))) {
                            code = gte_stall + fmt::format("gte_write_ctrl(cpu, {}, {});  /* ctc2 */", rd, reg_name(rt));
                        } else {
                            code = gte_stall + fmt::format("cpu->gte_ctrl[{}] = {};  /* ctc2 */", rd, reg_name(rt));
                        }
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
            // Stores: stamp the EXACT executing store PC before the write so
            // wtrace/readtrace producer attribution is correct for game code.
            // (The BIOS emitter does the same in strict_translator.cpp; the game
            // emitter previously left g_debug_last_store_pc holding a STALE pc —
            // the last BIOS store — which mis-attributed game writes to BIOS.)
            case 0x28: code = fmt::format("g_debug_last_store_pc = 0x{:08X}u; ", addr) + translate_sb(instr); break;     // sb
            case 0x29: code = fmt::format("g_debug_last_store_pc = 0x{:08X}u; ", addr) + translate_sh(instr); break;     // sh
            case 0x2A: code = fmt::format("g_debug_last_store_pc = 0x{:08X}u; ", addr) + translate_swl(instr); break;    // swl
            case 0x2B: code = fmt::format("g_debug_last_store_pc = 0x{:08X}u; ", addr) + translate_sw(instr); break;     // sw
            case 0x2E: code = fmt::format("g_debug_last_store_pc = 0x{:08X}u; ", addr) + translate_swr(instr); break;    // swr
            case 0x2F:                                         // CACHE - cache op (no-op for static recomp)
                code = "/* cache (no-op: static recompilation has no I/D cache) */";
                break;
            case 0x32:                                         // LWC2 - load word to COP2
                {
                    uint32_t rs = get_rs(instr);
                    uint32_t rt = get_rt(instr);
                    int16_t offset = get_imm16(instr);
                    // Faithful GTE: COP2 reg write stalls to the command deadline.
                    const char* gte_stall =
                        "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_gte_stall(cpu);\n#endif\n    ";
                    // LWC2 load timing: §1+DO_LDS via the block's psx_cyc_step(cpu,0)
                    // (op 0x32 is non-load), the GTE deadline stall via psx_gte_stall,
                    // then psx_cyc_lwc2_read does the ReadMemory timing (completion +1,
                    // no LDWhich arm — the dest is a GTE register).
                    std::string addr = (offset == 0)
                        ? reg_name(rs)
                        : fmt::format("{} + {}", reg_name(rs), offset);
                    bool special = PSXRecompGTERegisters::data_write_needs_helper(static_cast<uint8_t>(rt));
                    /* The raw loaded word is captured for the PGXP hook: the
                     * register may hold a MASKED value (write helper), and
                     * the shadow must validate against the word as loaded. */
                    if (special) {
                        code = gte_stall + fmt::format(
                            "{{ uint32_t _pgxa = {}; uint32_t _pgxv = psx_cyc_lwc2_read(cpu, _pgxa); "
                            "gte_write_data(cpu, {}, _pgxv); PGXP_COP2(0x{:08X}u, _pgxv, _pgxa); }}  /* lwc2 gte[{}] */",
                            addr, rt, instr, rt);
                    } else {
                        code = gte_stall + fmt::format(
                            "{{ uint32_t _pgxa = {}; uint32_t _pgxv = psx_cyc_lwc2_read(cpu, _pgxa); "
                            "cpu->gte_data[{}] = _pgxv; PGXP_COP2(0x{:08X}u, _pgxv, _pgxa); }}  /* lwc2 gte[{}], ({}) */",
                            addr, rt, instr, rt, addr);
                    }
                }
                break;
            case 0x3A:                                         // SWC2 - store word from COP2
                {
                    uint32_t rs = get_rs(instr);
                    uint32_t rt = get_rt(instr);
                    int16_t offset = get_imm16(instr);
                    // Faithful GTE: COP2 reg read stalls to the command deadline.
                    const char* gte_stall =
                        "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_gte_stall(cpu);\n#endif\n    ";
                    std::string value = PSXRecompGTERegisters::data_read_needs_helper(static_cast<uint8_t>(rt))
                        ? fmt::format("gte_read_data(cpu, {})", rt)
                        : fmt::format("cpu->gte_data[{}]", rt);
                    std::string swc2_store_pc = fmt::format("g_debug_last_store_pc = 0x{:08X}u; ", addr);
                    std::string swc2_addr = (offset == 0)
                        ? reg_name(rs)
                        : fmt::format("{} + {}", reg_name(rs), offset);
                    code = gte_stall + swc2_store_pc + fmt::format(
                        "{{ uint32_t _pgxa = {}; uint32_t _pgxv = {}; "
                        "psx_store_cycle_barrier(); cpu->write_word(_pgxa, _pgxv); "
                        "gte_precision_store_word(_pgxa, {}); "
                        "PGXP_COP2(0x{:08X}u, _pgxv, _pgxa); }}  /* swc2 gte[{}], {}({}) */",
                        swc2_addr, value, rt, instr, rt, offset, reg_name(rs));
                }
                break;
            default:
                code = fmt::format("/* TODO: opcode=0x{:02X} */", opcode);
        }
    }

    PSXRecomp::append_pgxp_hooks(instr, code);
    /* The trailing disassembly comment must not land ON a preprocessor
     * directive line -- it would be swallowed as extra tokens and dropped,
     * costing the annotation and emitting -Wendif-labels noise. Reachable
     * whenever the PGXP hook did NOT get appended after a block-cycles stall
     * block, e.g. `mfhi $zero` / `mflo $zero` (append_pgxp_hooks returns early
     * for rd==0, leaving the emission ending on a bare #endif). Same hazard
     * class as PR #171. */
    if (!comment.empty() &&
        PSXRecomp::emission_ends_on_preprocessor_directive(code))
        return config_.indent + code + "\n" + config_.indent + comment;
    return config_.indent + code + comment;
}

std::string CodeGenerator::translate_basic_block(
    const BasicBlock& block,
    const ControlFlowGraph& cfg) {

    std::stringstream ss;

    // Block label
    ss << fmt::format("block_{:08X}:\n", block.start_addr);

    // Per-block-leader cycle observe (cyc_watch ruler — universal with the BIOS
    // emitter). Sampled at the block leader BEFORE any cycle is charged, matching
    // Beetle's before-instruction sample and the cycle_compare.py anchor
    // semantics, so ANY block-leader PC is anchorable on both backends (not just
    // function entries). Debug-only: prod (PSX_NO_DEBUG_TOOLS) emits nothing.
    ss << "#ifndef PSX_NO_DEBUG_TOOLS\n";
    ss << config_.indent
       << fmt::format("debug_server_cyc_observe(0x{:08X}u);\n", block.start_addr);
    ss << "#endif\n";
    // First-divergence co-sim oracle (COSIM_ORACLE.md): lean per-block-leader hook,
    // present only in the clean PSX_COSIM build (independent of the debug tools).
    ss << "#ifdef PSX_COSIM\n";
    ss << config_.indent
       << fmt::format("cosim_block(0x{:08X}u);\n", block.start_addr);
    ss << "#endif\n";

    // Cycle-budgeted precise event slicing (PRECISE_IRQ_SLICE.md). At the block
    // leader — before any cycle is charged or the body runs — divert to the
    // per-instruction interpreter when an interrupt could be taken inside this
    // block, so the IRQ lands at its exact architectural instruction instead of
    // the coarse block edge. psx_slice_block returns nonzero iff it sliced (it
    // interpreted the block — and possibly more — and left cpu->pc at a
    // dispatchable resume point), in which case this function returns so its
    // compiled body does not re-execute the same instructions.
    // side_effects=1 marks blocks that change interrupt visibility on the CPU
    // (mtc0 Status/Cause, rfe), which can make a pending+masked IRQ deliverable
    // mid-block with no external event deadline.
    // Faithful cycle accounting (FAITHFUL_TIMING_PLAN.md P2): the exit branch/jump
    // ALWAYS executes its delay-slot instruction (emitted as a clone before the
    // transfer, below). When the delay slot lives INSIDE this block
    // (block.exit_instr.address < end_addr) it is already part of
    // instruction_count. But when the delay slot is a SEPARATE block leader (a
    // branch target, or a split-function edge) the branch sits AT end_addr and the
    // delay slot at end_addr+4 is outside [start,end]; instruction_count then
    // EXCLUDES it, yet the clone still executes — so the block undercharges by one
    // cycle vs the per-instruction interpreter on BOTH paths. (This is the measured
    // -8 drift in the Tomba 2 logo init subtree: ~8 such sites on the boot path,
    // native runs behind interp, the elapsed-Timer1 logo-delay loop exits early.)
    // Charge the always-executed clone here so every dynamically executed
    // instruction is charged exactly once. (R3000 is MIPS-I: no branch-likely, so
    // the delay slot is unconditional.) cycle_per_insn mode already charges the
    // clone per-instruction at its emit site, so this adjustment is block-mode only.
    const bool exit_delay_slot_outside =
        block.exit_instr.has_delay_slot &&
        block.exit_instr.type != ControlFlowType::None &&
        block.exit_instr.address == block.end_addr;
    // Block cycle charge = sum of the single-source per-instruction cost
    // (psx_instr_cost.h) over every instruction the block executes, INCLUDING
    // the always-executed delay-slot clone when the exit branch sits at end_addr
    // (delay slot outside [start,end]). The interpreter charges the same cost
    // per instruction through the same function, so the two backends cannot
    // disagree. Stage 1 cost is identity (1/insn) => this equals
    // instruction_count(+1 outside delay) exactly => byte-identical regen; Stage 2
    // edits ONLY psx_instr_base_cycles and both backends update together.
    uint32_t block_exec_cycles = 0;
    for (uint32_t a = block.start_addr; a <= block.end_addr; a += 4) {
        auto w = exe_.read_word(a);
        if (!w.has_value()) break;
        block_exec_cycles += psx_instr_base_cycles(*w);
    }
    if (exit_delay_slot_outside) {
        auto dw = exe_.read_word(block.end_addr + 4u);  // the delay-slot clone
        if (dw.has_value()) block_exec_cycles += psx_instr_base_cycles(*dw);
    }

    {
        uint32_t slice_bcyc = block_exec_cycles;
        if (slice_bcyc == 0) slice_bcyc = 1;
        int slice_side_effects = 0;
        for (uint32_t a = block.start_addr; a <= block.end_addr; a += 4) {
            auto w = exe_.read_word(a);
            if (!w.has_value()) break;
            uint32_t insn = *w;
            if ((insn >> 26) == 0x10u) {                 // COP0
                uint32_t rs = (insn >> 21) & 0x1Fu;
                uint32_t co = (insn >> 25) & 0x1u;
                uint32_t funct = insn & 0x3Fu;
                if (rs == 0x04u) slice_side_effects = 1;            // MTC0 (writes Status/Cause)
                if (co && funct == 0x10u) slice_side_effects = 1;   // RFE
            }
        }
        ss << "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
        ss << config_.indent
           << fmt::format("if (psx_slice_block(cpu, 0x{:08X}u, {}u, {})) return;\n",
                          block.start_addr, slice_bcyc, slice_side_effects);
        ss << "#endif\n";
    }

    const bool cycle_per_insn = codegen_cycle_per_insn();
    // Per-instruction R3000A load-delay interlock (cycle_per_insn mode): §1 base +
    // GPR_DEPRES + DO_LDS, emitted BEFORE the instruction body so §1 precedes any
    // muldiv/GTE deadline stall the body applies (Beetle order). CPU loads (op
    // 0x20-0x26) are SKIPPED here — psx_cyc_load_* runs their full interlock inside
    // the body (and arms LDWhich=rt). The dep/res mask is a gen-time literal. This
    // replaces the old flat per-instruction psx_advance_cycles(1u).
    auto emit_pre_timing = [&](uint32_t in, const std::string& indent) {
        uint32_t op = in >> 26;
        if (op >= 0x20u && op <= 0x26u) return;   // CPU load: interlock inside psx_cyc_load_*
        ss << "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
        ss << indent << fmt::format("psx_cyc_step(cpu, 0x{:X}u);\n",
                                    psx_cyc_dep_res_mask(in));
        ss << "#endif\n";
    };
    // I-cache FETCH cost (faithful R3000A), emitted BEFORE the per-instruction
    // interlock/load — exactly like Beetle ReadInstruction precedes the base, and so a
    // fetch MISS clears any pending load give-back before the next load arms one. Only
    // emitted at cache-line LEADERS: a block leader / mid-block jump-table target (any
    // address reachable other than by fall-through, i.e. a possibly-cold cache entry) OR
    // a 16-byte-line start (addr&0xC==0, a sequential line crossing). Intra-line
    // followers reached by fall-through are guaranteed hits — the leader's fetch
    // refilled the line to its end — so they need no call (+0). Extra fetch points are
    // harmless (a hit is +0); only UNDER-counting a cold entry would diverge, which the
    // leader set prevents. The game runs at its KSEG0 load address, so `insn_addr` is
    // already the runtime guest PC (matching the dirty-RAM interp's cpu->pc and Beetle).
    auto emit_pre_icache = [&](uint32_t insn_addr, const std::string& indent) {
        if (!(insn_addr == block.start_addr || (insn_addr & 0xCu) == 0 ||
              extra_labels_.count(insn_addr))) return;
        ss << "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
        ss << indent << fmt::format("psx_icache_fetch(cpu, 0x{:08X}u);\n", insn_addr);
        ss << "#endif\n";
    };
    auto emit_cosim_instr = [&](uint32_t insn_addr, const std::string& indent) {
        ss << "#ifdef PSX_COSIM\n";
        ss << indent << fmt::format("cosim_instr(0x{:08X}u);\n", insn_addr);
        ss << "#endif\n";
    };
    if (!cycle_per_insn && block_exec_cycles > 0) {
        ss << "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
        ss << config_.indent << fmt::format("psx_advance_cycles({}u);\n",
                                            block_exec_cycles);
        ss << "#endif\n";
    }

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

    /* MIPS-I load-delay value semantics for ordinary in-block pairs.  The
     * resident/full-function emitter already models these; overlay/game CFG
     * emission must do the same.  OpenBIOS's CD/card fasttrack deliberately
     * uses `lw k0,...; move at,k0`, where the move must observe OLD k0. */
    auto simple_load_dest = [](uint32_t w) -> int {
        uint32_t op = w >> 26;
        if (op == 0x20u || op == 0x21u || op == 0x23u ||
            op == 0x24u || op == 0x25u) {
            uint32_t rt = (w >> 16) & 31u;
            return rt ? static_cast<int>(rt) : -1;
        }
        return -1;
    };
    auto reads_gpr = [](uint32_t w, uint32_t r) -> bool {
        if (r == 0u || w == 0u) return false;
        uint32_t op = w >> 26, rs = (w >> 21) & 31u;
        uint32_t rt = (w >> 16) & 31u, fn = w & 63u;
        if (op == 0u) {
            if (fn == 0x08u || fn == 0x09u) return rs == r;
            if (fn == 0x0Cu || fn == 0x0Du || fn == 0x10u || fn == 0x12u)
                return false;
            return rs == r || rt == r;
        }
        if (op == 0x02u || op == 0x03u || op == 0x0Fu) return false;
        if (op == 0x01u || op == 0x06u || op == 0x07u) return rs == r;
        if (op == 0x04u || op == 0x05u) return rs == r || rt == r;
        if (op >= 0x08u && op <= 0x0Eu) return rs == r;
        if (op == 0x10u) return ((w >> 21) & 31u) == 0x04u && rt == r;
        if (op == 0x12u) {
            uint32_t f = (w >> 21) & 31u;
            return (f == 0x04u || f == 0x06u) && rt == r;
        }
        if (op == 0x22u || op == 0x26u ||
            (op >= 0x28u && op <= 0x2Eu))
            return rs == r || rt == r;
        return rs == r;
    };
    auto writes_gpr = [](uint32_t w, uint32_t r) -> bool {
        if (r == 0u || w == 0u) return false;
        uint32_t op = w >> 26, rt = (w >> 16) & 31u;
        uint32_t rd = (w >> 11) & 31u, fn = w & 63u;
        if (op == 0u) {
            if (fn == 0x08u || fn == 0x0Cu || fn == 0x0Du || fn == 0x0Fu ||
                fn == 0x11u || fn == 0x13u ||
                (fn >= 0x18u && fn <= 0x1Bu)) return false;
            return rd == r;
        }
        if (op == 0x03u) return r == 31u;
        if (op == 0x01u) return (rt == 0x10u || rt == 0x11u) && r == 31u;
        if (op == 0x02u || (op >= 0x04u && op <= 0x07u) ||
            (op >= 0x28u && op <= 0x2Eu) || op == 0x32u || op == 0x3Au)
            return false;
        if (op == 0x10u) return ((w >> 21) & 31u) == 0u && rt == r;
        if (op == 0x12u) {
            uint32_t f = (w >> 21) & 31u;
            return (f == 0u || f == 2u) && rt == r;
        }
        return rt == r;
    };
    uint32_t delayed_load_addr = 0u;
    uint32_t delayed_load_dest = 0u;
    bool delayed_load_active = false;

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
            if (cycle_per_insn) emit_pre_icache(addr, config_.indent);
            if (cycle_per_insn) emit_pre_timing(instr, config_.indent);
            // If this is the successor half of a deferred load pair AND it is an
            // LWL/LWR merging into that same register, hardware forwards the
            // pending load into the merge (see set_lwlr_merge_forward). Point
            // the merge operand at the deferred temporary before translating.
            const uint32_t succ_op = instr >> 26;
            const bool succ_is_lwlr = (succ_op == 0x22u || succ_op == 0x26u);
            const bool forward_to_lwlr =
                delayed_load_active && addr == delayed_load_addr + 4u &&
                succ_is_lwlr && get_rt(instr) == delayed_load_dest;
            if (forward_to_lwlr) {
                set_lwlr_merge_forward(
                    delayed_load_dest,
                    fmt::format("psx_ldd_{:08X}", delayed_load_addr));
            }
            std::string emitted = translate_instruction(addr, instr);
            if (forward_to_lwlr) clear_lwlr_merge_forward();
            int load_dest = simple_load_dest(instr);
            bool defer_load = false;
            if (!delayed_load_active && load_dest > 0 && addr + 4u <= block.end_addr &&
                !extra_labels_.count(addr + 4u)) {
                auto next = exe_.read_word(addr + 4u);
                defer_load = next.has_value() &&
                    !ControlFlowAnalyzer::is_control_flow(*next) &&
                    reads_gpr(*next, static_cast<uint32_t>(load_dest));
            }
            if (defer_load) {
                const std::string lhs = fmt::format("cpu->gpr[{}] =", load_dest);
                const size_t pos = emitted.find(lhs);
                if (pos != std::string::npos) {
                    const std::string temp = fmt::format("psx_ldd_{:08X}", addr);
                    // Declare the temp in the pair block, not inline at the
                    // load: with PGXP the load already sits in its own
                    // `{ uint32_t _pgxa = ...; <load> PGXP_LOAD(...); }`
                    // wrapper, so an inline declaration would go out of
                    // scope before the writeback below.
                    emitted.replace(pos, lhs.size(), temp + " =");
                    // Point the PGXP hook at the temp: the writeback is
                    // deferred, so the GPR still holds the pre-load value.
                    const std::string pgxp_tail =
                        fmt::format(", _pgxa, cpu->gpr[{}]);", load_dest);
                    const size_t hook = emitted.rfind(pgxp_tail);
                    if (hook != std::string::npos)
                        emitted.replace(hook, pgxp_tail.size(),
                                        fmt::format(", _pgxa, {});", temp));
                    ss << config_.indent << "{ /* MIPS-I load-delay pair */\n";
                    ss << config_.indent
                       << fmt::format("    uint32_t {} = 0;\n", temp);
                    delayed_load_addr = addr;
                    delayed_load_dest = static_cast<uint32_t>(load_dest);
                    delayed_load_active = true;
                }
            }
            ss << emitted << "\n";
            if (delayed_load_active && addr == delayed_load_addr + 4u) {
                if (forward_to_lwlr) {
                    ss << config_.indent << fmt::format(
                        "/* psx_ldd_{:08X} forwarded into the LWL/LWR merge above */\n",
                        delayed_load_addr);
                } else if (writes_gpr(instr, delayed_load_dest)) {
                    ss << config_.indent << fmt::format(
                        "(void)psx_ldd_{:08X};  /* successor write wins */\n",
                        delayed_load_addr);
                } else {
                    ss << config_.indent << fmt::format(
                        "cpu->gpr[{}] = psx_ldd_{:08X};  /* load-delay writeback */\n",
                        delayed_load_dest, delayed_load_addr);
                }
                ss << config_.indent << "}\n";
                delayed_load_active = false;
            }
            emit_cosim_instr(addr, config_.indent);
        } else {
            // Control flow is handled at block exit
            if (addr == exit_branch_addr) {
                std::string delay_saved_cond;    // branch condition captured before delay
                std::string delay_saved_target;  // JR/JALR target captured before delay

                const uint32_t branch_opcode =
                    (block.exit_instr.instruction >> 26) & 0x3Fu;
                if (branch_opcode >= 0x14u && branch_opcode <= 0x17u) {
                    /* Opcodes 0x14-0x17 (the MIPS-II branch-likely family) are
                     * RESERVED on the R3000A: real hardware raises the Reserved
                     * Instruction exception at this PC (Beetle cpu.cpp opcode
                     * table row 0x10: op_ILL). Discovery can sweep data-as-code
                     * into a function body; aborting the WHOLE regen here (the
                     * old throw) broke "every title regenerates at tip" over one
                     * bogus word, and emitting the word as a real branch would
                     * be unfaithful. Emit the architectural RI raise inline —
                     * ABI-neutral: the dispatch trampoline (and the CPS exit
                     * contract in overlay mode) re-dispatches cpu->pc after the
                     * return, exactly like any other guest control transfer. If
                     * the word is data (never executed) nothing happens; if the
                     * guest really reaches it, the kernel handler sees the same
                     * exception hardware would deliver. */
                    if (should_print_reserved_opcode_warning(addr)) {
                        fmt::print("  WARNING: reserved opcode 0x{:08X} at 0x{:08X} "
                                   "— emitting guest RI exception raise\n",
                                   block.exit_instr.instruction, addr);
                    }
                    ss << config_.indent
                       << fmt::format("{{ /* reserved opcode 0x{:08X}: architectural RI exception */\n",
                                      block.exit_instr.instruction);
                    ss << config_.indent << "  uint32_t psx_ri_sr = cpu->cop0[12];\n";
                    ss << config_.indent
                       << fmt::format("  cpu->cop0[14] = 0x{:08X}u;  /* EPC = faulting PC, BD=0 */\n", addr);
                    ss << config_.indent
                       << "  cpu->cop0[13] = (cpu->cop0[13] & ~0x8000007Cu) | (10u << 2);  /* Cause: RI */\n";
                    ss << config_.indent
                       << "  cpu->cop0[12] = (psx_ri_sr & ~0x3Fu) | ((psx_ri_sr & 0x0Fu) << 2);  /* SR push */\n";
                    ss << config_.indent
                       << "  cpu->pc = (psx_ri_sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;\n";
                    ss << config_.indent << "  return; }\n";
                    break;  /* block ends at the raise; nothing after is reachable */
                }

                // Per-instruction interlock ORDER (Beetle): the branch's §1+deps+DO_LDS
                // runs at the branch PC, THEN the delay slot's at PC+4. Emit the branch
                // step FIRST (it is pure timing — does not touch GPR values, so it is
                // safe before the branch-condition capture below).
                if (cycle_per_insn)
                    emit_pre_icache(exit_branch_addr, config_.indent);
                if (cycle_per_insn)
                    emit_pre_timing(block.exit_instr.instruction, config_.indent);

                // Register-indirect targets are resolved at the jump instruction.
                // The delay slot may overwrite the source register, so all JR/JALR
                // paths below consume this pre-delay snapshot.
                if (block.exit_instr.type == ControlFlowType::Return ||
                    block.exit_instr.type == ControlFlowType::JumpRegister ||
                    block.exit_instr.type == ControlFlowType::JumpLinkReg) {
                    const uint32_t target_rs = get_rs(block.exit_instr.instruction);
                    delay_saved_target = fmt::format("_jt_{:08X}", addr);
                    ss << config_.indent
                       << fmt::format("uint32_t {} = {};  /* latch indirect target before delay slot */\n",
                                      delay_saved_target, reg_name(target_rs));
                }

                if (block.exit_instr.type == ControlFlowType::JumpLink) {
                    ss << config_.indent
                       << fmt::format("cpu->gpr[31] = 0x{:08X}u;  /* jal link before delay slot */\n",
                                      addr + 8);
                } else if (block.exit_instr.type == ControlFlowType::JumpLinkReg) {
                    uint32_t rd = get_rd(block.exit_instr.instruction);
                    if (rd != 0) {
                        ss << config_.indent
                           << fmt::format("{} = 0x{:08X}u;  /* jalr link before delay slot */\n",
                                          reg_name(rd), addr + 8);
                    }
                } else if (block.exit_instr.type == ControlFlowType::Branch) {
                    uint32_t branch_instr = block.exit_instr.instruction;
                    uint32_t b_opcode = (branch_instr >> 26) & 0x3F;
                    uint32_t regimm_op = (branch_instr >> 16) & 0x1F;
                    if (b_opcode == 0x01 && (regimm_op == 0x10 || regimm_op == 0x11)) {
                        ss << config_.indent
                           << fmt::format("cpu->gpr[31] = 0x{:08X}u;  /* branch-and-link before delay slot */\n",
                                          addr + 8);
                    }
                }

                // MIPS delay slot handling: emit delay slot instruction BEFORE branch/jump
                if (block.exit_instr.has_delay_slot) {
                    uint32_t delay_slot_addr = addr + 4;
                    auto delay_instr_opt = exe_.read_word(delay_slot_addr);

                    /* A native control transfer without its architectural delay
                     * slot is never a legal partial shard.  In particular, a
                     * page-scoped overlay capture can end with the branch at
                     * ...FFC while the slot lives at ...000 in the next page.
                     * The old code silently skipped the unavailable word and
                     * still emitted the transfer, corrupting guest state while
                     * looking like a successful native call.  Fail generation
                     * closed; the runtime interpreter remains authoritative
                     * until a capture containing the guard word is available. */
                    if (!delay_instr_opt.has_value()) {
                        throw std::runtime_error(fmt::format(
                            "cannot emit control flow at 0x{:08X}: mandatory "
                            "delay slot 0x{:08X} is outside the input image",
                            addr, delay_slot_addr));
                    }
                    {
                        uint32_t delay_instr = *delay_instr_opt;

                        // For branch-likely variants, delay slot is conditional
                        if (block.exit_instr.is_likely) {
                            ss << config_.indent << "/* delay slot (likely) - conditional execution */\n";
                            ss << config_.indent << "if (" << generate_branch_condition(block.exit_instr.instruction, block.exit_instr.address) << ") {\n";
                            ss << config_.indent << translate_instruction(delay_slot_addr, delay_instr) << "\n";
                            emit_cosim_instr(delay_slot_addr, config_.indent + config_.indent);
                            ss << config_.indent << "}\n";
                        } else {
                            // Normal delay slot - always executes.
                            // In MIPS, the branch condition is evaluated at the branch
                            // instruction (before the delay slot). If the delay slot
                            // modifies a condition register we must save the result first.
                            if (block.exit_instr.type == ControlFlowType::Branch) {
                                std::string cond = generate_branch_condition(block.exit_instr.instruction, block.exit_instr.address);
                                delay_saved_cond = fmt::format("_bc_{:08X}", addr);
                                ss << config_.indent
                                   << fmt::format("int {} = ({});  /* save branch cond before delay slot */\n",
                                                  delay_saved_cond, cond);
                            }
                            ss << config_.indent << "/* delay slot (always executes) */\n";
                            // Delay slot's own fetch + §1+deps+DO_LDS, before its body (skipped if a load).
                            if (cycle_per_insn) emit_pre_icache(delay_slot_addr, config_.indent);
                            if (cycle_per_insn) emit_pre_timing(delay_instr, config_.indent);
                            ss << translate_instruction(delay_slot_addr, delay_instr) << "\n";
                            emit_cosim_instr(delay_slot_addr, config_.indent);
                        }
                    }
                }

                // Now emit the branch/jump
                if (block.exit_instr.type == ControlFlowType::Branch) {
                    // Conditional branch - use pre-captured condition if available
                    std::string condition = delay_saved_cond.empty()
                        ? generate_branch_condition(block.exit_instr.instruction, block.exit_instr.address)
                        : delay_saved_cond;
                    if (block.successors.size() == 2) {
                        ss << config_.indent << "if (" << condition << ") {\n";
                        ss << emit_interrupt_check(block.successors[0], config_.indent + config_.indent);
                        ss << config_.indent << config_.indent
                           << fmt::format("goto block_{:08X};  /* taken */\n", block.successors[0]);
                        ss << config_.indent << "} else {\n";
                        ss << emit_interrupt_check(block.successors[1], config_.indent + config_.indent);
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
                            ss << emit_interrupt_check(branch_target, config_.indent + config_.indent);
                            ss << config_.indent << config_.indent
                               << fmt::format("goto block_{:08X};  /* taken */\n", branch_target);
                        } else if (cps_enabled_) {
                            ss << emit_interrupt_check(branch_target, config_.indent + config_.indent);
                            ss << config_.indent << config_.indent
                               << fmt::format("cpu->pc = 0x{:08X}u; return;  /* CPS taken: split */\n", branch_target);
                        } else if (known_functions_.count(branch_target)) {
                            ss << emit_interrupt_check(branch_target, config_.indent + config_.indent);
                            ss << config_.indent << config_.indent
                               << fmt::format("func_{:08X}(cpu); return;  /* taken: split piece */\n", branch_target);
                        } else {
                            ss << emit_interrupt_check(branch_target, config_.indent + config_.indent);
                            ss << config_.indent << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* taken: split (mid-func) */\n", branch_target);
                        }
                        ss << config_.indent << "} else {\n";
                        if (fallthru_in) {
                            ss << emit_interrupt_check(fall_through_addr, config_.indent + config_.indent);
                            ss << config_.indent << config_.indent
                               << fmt::format("goto block_{:08X};  /* not taken */\n", fall_through_addr);
                        } else if (cps_enabled_) {
                            ss << emit_interrupt_check(fall_through_addr, config_.indent + config_.indent);
                            ss << config_.indent << config_.indent
                               << fmt::format("cpu->pc = 0x{:08X}u; return;  /* CPS not taken: split */\n", fall_through_addr);
                        } else if (known_functions_.count(fall_through_addr)) {
                            ss << emit_interrupt_check(fall_through_addr, config_.indent + config_.indent);
                            ss << config_.indent << config_.indent
                               << fmt::format("func_{:08X}(cpu); return;  /* not taken: split piece */\n", fall_through_addr);
                        } else {
                            ss << emit_interrupt_check(fall_through_addr, config_.indent + config_.indent);
                            ss << config_.indent << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* not taken: split (mid-func) */\n", fall_through_addr);
                        }
                        ss << config_.indent << "}\n";
                    }
                } else if (block.exit_instr.type == ControlFlowType::Jump) {
                    // Unconditional jump
                    if (!block.successors.empty()) {
                        ss << emit_interrupt_check(block.successors[0], config_.indent);
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* j */\n", block.successors[0]);
                    } else if (cps_enabled_ && block.exit_instr.target != 0) {
                        // CPS: out-of-function jump target — tail-transfer (the
                        // flat trampoline dispatches the split piece / mid-func).
                        ss << emit_interrupt_check(block.exit_instr.target, config_.indent);
                        ss << config_.indent
                           << fmt::format("cpu->pc = 0x{:08X}u; return;  /* CPS j: split */\n",
                                          block.exit_instr.target);
                    } else if (block.exit_instr.target != 0 && known_functions_.count(block.exit_instr.target)) {
                        // Jump target is out-of-function and is a known function start
                        ss << emit_interrupt_check(block.exit_instr.target, config_.indent);
                        ss << config_.indent
                           << fmt::format("func_{:08X}(cpu); return;  /* j to split piece */\n",
                                          block.exit_instr.target);
                    } else if (block.exit_instr.target != 0) {
                        // Jump target is a mid-function address — dispatch dynamically
                        ss << emit_interrupt_check(block.exit_instr.target, config_.indent);
                        ss << config_.indent
                           << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* j to split (mid-func) */\n",
                                          block.exit_instr.target);
                    }
                } else if (block.exit_instr.type == ControlFlowType::Return) {
                    /* Return instruction.
                     *
                     * Detect "longjmp-return" pattern: if $ra was loaded from a
                     * non-$sp source anywhere in this function (e.g. RestoreState
                     * loads $ra from a save buffer via $a0), the jr $ra must set
                     * cpu->pc so the dispatch loop tail-calls to the restored
                     * address — the C `return` alone would go back to the wrong
                     * caller.  Normal functions that save/restore $ra on the $sp
                     * stack get the plain `return;`. */
                    bool ra_loaded_from_non_sp = false;
                    for (const auto& [baddr, blk] : cfg.blocks) {
                        for (uint32_t ia = blk.start_addr; ia < blk.end_addr; ia += 4) {
                            auto wopt = exe_.read_word(ia);
                            if (!wopt) continue;
                            uint32_t w = *wopt;
                            uint32_t i_op = (w >> 26) & 0x3F;
                            uint32_t i_rt = (w >> 16) & 0x1F;
                            uint32_t i_rs = (w >> 21) & 0x1F;
                            if (i_op == 0x23 && i_rt == 31 && i_rs == 4) {
                                /* lw $ra, offset($a0) — RestoreState/longjmp pattern */
                                ra_loaded_from_non_sp = true;
                                break;
                            }
                        }
                        if (ra_loaded_from_non_sp) break;
                    }
                    if (config_.ws_backdrop_unsquash_funcs.count(cfg.function_start)) {
                        ss << config_.indent
                           << "gte_ws_set_suppress(0);  /* widescreen: end far-backdrop un-squash (8C) */\n";
                    }
                    if (config_.data_shard_funcs.count(cfg.function_start)) {
                        // Capture finalize: fires only when this is the armed
                        // function's own return (sp/ra checked runtime-side).
                        ss << config_.indent
                           << "psx_datashard_ret(cpu);  /* data-shard: finalize capture */\n";
                    }
                    if (ra_loaded_from_non_sp) {
                        ss << emit_interrupt_check_expr(delay_saved_target, config_.indent);
                        ss << config_.indent
                           << "cpu->pc = " << delay_saved_target << "; psx_restore_state_escape(); return;"
                           << "  /* jr $ra — longjmp-return (ra loaded from non-sp) */\n";
                    } else if (cps_enabled_) {
                        // CPS: publish $ra so the flat trampoline dispatches the
                        // caller's continuation (no host C-return to nest).
                        ss << emit_interrupt_check_expr(delay_saved_target, config_.indent);
                        ss << config_.indent
                           << "cpu->pc = " << delay_saved_target << "; return;  /* CPS: jr $ra */\n";
                    } else {
                        ss << emit_interrupt_check_expr(delay_saved_target, config_.indent);
                        ss << config_.indent << "return;  /* jr $ra */\n";
                    }
                } else if (block.exit_instr.type == ControlFlowType::JumpRegister) {
                    // jr $rs — may be: BIOS call (jr $t2, t2=0xA0/0xB0/0xC0)
                    //                  or: jump table (jr $v0, v0 loaded from LW+ADDU+LUI)
                    uint32_t jr_rs = get_rs(block.exit_instr.instruction);

                    // Detect MIPS jump-table pattern by scanning backwards from jr.
                    // Handles two common patterns:
                    //   P1: lui $R,hi; addiu $R,$R,lo; addu $D,$idx,$R; lw $D,0($D)
                    //   P2: lui $R,hi;                 addu $D,$R,$idx; lw $D,off($D)
                    // table_base = lui_val + addiu_val + lw_offset
                    ExactJumpTable exact_table;
                    bool have_exact_table = resolve_exact_bounded_jump_table(
                        exe_, cfg.function_start, cfg.function_end,
                        block.exit_instr.address, jr_rs, exact_table,
                        ram_to_rom, cfg.producer_lo, cfg.producer_hi);
                    uint32_t table_base = have_exact_table
                        ? exact_table.table_base : 0u;
                    uint32_t table_count = have_exact_table
                        ? exact_table.table_count : 0u;
                    // If we found a valid table, read entries and emit switch.
                    // table_base may be a RAM address (BIOS shell code copies
                    // ROM 0xBFC18000+ to RAM 0x80030000+).  Translate to ROM
                    // so exe_.read_word() can read the data, and translate each
                    // entry so it matches cfg.blocks / extra_labels_ (ROM space).
                    bool emitted_switch = false;
                    if (table_base != 0 && table_count > 0) {
                        uint32_t rom_table_base = ram_to_rom(table_base, exe_);
                        std::set<uint32_t>    seen;
                        std::vector<std::pair<uint32_t,uint32_t>> targets; // {runtime_addr, rom_addr}
                        for (const auto& [runtime_addr, rom_addr] :
                             exact_table.targets) {
                            if (seen.insert(rom_addr).second &&
                                (cfg.blocks.count(rom_addr) || extra_labels_.count(rom_addr))) {
                                targets.push_back({runtime_addr, rom_addr});
                            }
                        }
                        if (!targets.empty()) {
                            ss << config_.indent << fmt::format("/* jump table 0x{:08X} (rom 0x{:08X}), {} entries */\n",
                                                                table_base, rom_table_base, table_count);
                            ss << config_.indent << fmt::format("switch ({}) {{\n", delay_saved_target);
                            for (auto& [rt, rom] : targets) {
                                if (partial_block_cycle_count(rom, cfg) != 0) {
                                    ss << config_.indent << fmt::format("    case 0x{:08X}u:\n", rt);
                                    ss << emit_interrupt_check(rom, config_.indent + "        ");
                                    ss << emit_mid_block_cycle_charge(rom, cfg, config_.indent + "        ");
                                    ss << config_.indent << fmt::format("        goto block_{:08X};\n", rom);
                                } else {
                                    ss << config_.indent << fmt::format("    case 0x{:08X}u:\n", rt);
                                    ss << emit_interrupt_check(rom, config_.indent + "        ");
                                    ss << config_.indent << fmt::format("        goto block_{:08X};\n", rom);
                                }
                            }
                            if (cps_enabled_) {
                                ss << config_.indent << "    default:\n";
                                ss << emit_interrupt_check_expr(delay_saved_target, config_.indent + "        ");
                                ss << config_.indent << "        cpu->pc = " << delay_saved_target
                                   << "; return;  /* CPS: jr table miss — tail-transfer */\n";
                            } else {
                                ss << config_.indent << "    default:\n";
                                ss << emit_interrupt_check_expr(delay_saved_target, config_.indent + "        ");
                                ss << config_.indent << "        call_by_address(cpu, " << delay_saved_target << "); return;\n";
                            }
                            ss << config_.indent << "}\n";
                            emitted_switch = true;
                        }
                    }
                    if (!emitted_switch) {
                        if (cps_enabled_) {
                            // CPS: indirect jump / BIOS-call gate — tail-transfer
                            // to the target (the flat trampoline dispatches it).
                            ss << emit_interrupt_check_expr(delay_saved_target, config_.indent);
                            ss << config_.indent << fmt::format("cpu->pc = {}; return;  /* CPS: jr {} */\n",
                                                                delay_saved_target, reg_name(jr_rs));
                        } else {
                            // BIOS call or unrecognised indirect jump
                            ss << emit_interrupt_check_expr(delay_saved_target, config_.indent);
                            ss << config_.indent << fmt::format("call_by_address(cpu, {});  /* jr {} */\n",
                                                                delay_saved_target, reg_name(jr_rs));
                            ss << config_.indent << "return;\n";
                        }
                    }
                } else if (block.exit_instr.type == ControlFlowType::JumpLink) {
                    uint32_t target   = block.exit_instr.target;
                    uint32_t cont_addr = block.exit_instr.address + 8;
                    if (cps_enabled_) {
                        // CPS: tail-transfer to the callee. Set $ra to the
                        // return point and register it as a dispatchable
                        // continuation; the callee's jr $ra publishes cpu->pc =
                        // $ra and the flat trampoline dispatches back into this
                        // function's entry-switch. No host nesting.
                        // Only register a LOCAL switch case when the return PC
                        // lives in this piece (has a block label). When the
                        // delay/return was split out, successors is empty — the
                        // mid-function pre-pass promotes addr+8 to its own
                        // dispatch entry (do not emit goto block_<foreign>).
                        if (!block.successors.empty()) {
                            cps_cur_continuations_.push_back(cont_addr);
                        }
                        ss << emit_interrupt_check(target, config_.indent);
                        ss << config_.indent
                           << fmt::format("cpu->pc = 0x{:08X}u; return;  /* CPS jal -> 0x{:08X} */\n",
                                          target, target);
                    } else {
                    // Function call (jal).  The call contract (Bug D family)
                    // guards the continuation: it may only run if the guest
                    // actually returned here with the caller's $sp.
                    ss << config_.indent << "{ uint32_t _csp = cpu->gpr[29];\n";
                    ss << emit_interrupt_check(target, config_.indent);
                    if (known_functions_.count(target) > 0) {
                        ss << config_.indent << fmt::format("func_{:08X}(cpu);  /* jal */\n", target);
                        ss << config_.indent << fmt::format("if (psx_call_contract(cpu, 0x{:08X}u, _csp)) return; }}\n", addr + 8);
                    } else {
                        ss << config_.indent << fmt::format("call_by_address(cpu, 0x{:08X}u);  /* external jal */\n", target);
                        /* psx_dispatch_call validated the (ra, sp) contract;
                         * only propagate an active bail unwind here. */
                        ss << config_.indent << "if (g_psx_call_bail) return; (void)_csp; }\n";
                    }
                    if (!block.successors.empty()) {
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* continue after call */\n", block.successors[0]);
                    } else {
                        // Split-function: JAL continuation is outside this function piece.
                        // Tail-call to the continuation piece (at exit_addr + 8, past delay slot).
                        if (known_functions_.count(cont_addr)) {
                            ss << config_.indent
                               << fmt::format("func_{:08X}(cpu); return;  /* jal cont: split piece */\n", cont_addr);
                        } else {
                            ss << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* jal cont: split */\n", cont_addr);
                        }
                    }
                    }
                } else if (block.exit_instr.type == ControlFlowType::JumpLinkReg) {
                    // Register indirect call (jalr $rs, $rd) — call function at rs, return to rd
                    uint32_t rs = get_rs(block.exit_instr.instruction);
                    uint32_t cont_addr = block.exit_instr.address + 8;

                    if (cps_enabled_) {
                        // The target and link were committed before the delay
                        // slot above. Register a local continuation only when
                        // the return PC is in this piece (see jal).
                        if (!block.successors.empty()) {
                            cps_cur_continuations_.push_back(cont_addr);
                        }
                        ss << emit_interrupt_check_expr(delay_saved_target, config_.indent);
                        ss << config_.indent << "cpu->pc = " << delay_saved_target
                           << "; return;  /* CPS jalr */\n";
                    } else {
                    // Dispatch the pre-delay latched indirect target.
                    // Dispatch indirect call — target is a runtime register value
                    ss << emit_interrupt_check_expr(delay_saved_target, config_.indent);
                    ss << config_.indent << fmt::format("call_by_address(cpu, {});  /* jalr {} */\n",
                                                        delay_saved_target, reg_name(rs));
                    /* psx_dispatch_call validated the (ra, sp) contract;
                     * only propagate an active bail unwind here. */
                    ss << config_.indent << "if (g_psx_call_bail) return;\n";

                    // Continue to successor block (instruction at return address)
                    if (!block.successors.empty()) {
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* continue after jalr */\n",
                                          block.successors[0]);
                    } else {
                        // Split-function: JALR continuation is outside this function piece.
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
        }

        addr += 4;
    }

    // If no explicit control flow, fall through to the next block. When a
    // CFG split or mid-function seed split placed the next instruction behind
    // a C label/function boundary, preserve the physical fall-through without
    // consuming an interrupt/cooldown check at the artificial boundary.
    if (block.exit_instr.type == ControlFlowType::None && !block.successors.empty()) {
        ss << config_.indent << fmt::format("/* fall through to block_{:08X} */\n",
                                           block.successors[0]);
    } else if (block.exit_instr.type == ControlFlowType::None) {
        uint32_t next_addr = block.end_addr + 4;
        if (known_functions_.count(next_addr) > 0) {
            ss << config_.indent
               << fmt::format("func_{:08X}(cpu); return;  /* fallthrough to split piece */\n",
                              next_addr);
        } else if (cps_enabled_) {
            // Unit-edge fall-through with no known successor function (e.g. a
            // partial overlay capture). Falling off the body would leave
            // cpu->pc == 0 (the continuation-entry prologue cleared it), which
            // the trampoline reads as a normal guest exit — a silent shutdown.
            // Publish the PC so dispatch can route it instead.
            ss << emit_interrupt_check(next_addr, config_.indent);
            ss << config_.indent
               << fmt::format("cpu->pc = 0x{:08X}u; return;  /* CPS fallthrough past unit edge */\n",
                              next_addr);
        } else {
            ss << emit_interrupt_check(next_addr, config_.indent);
        }
    }

    return ss.str();
}

bool CodeGenerator::func_has_screen_extent_cull(const ControlFlowGraph& cfg) const {
    // The GTE trivial-reject pairs a width compare (slti/sltiu, immediate from
    // the per-game W set) with a height compare (immediate from the H set) in
    // the same function. Presence of both is the signature of a screen-extent
    // render funnel; a lone width compare elsewhere (rare) is left alone.
    // Shared scan (ws_cull_detect.h) so every backend derives one verdict.
    bool has_x = false, has_y = false;
    for (uint32_t block_addr : cfg.block_order) {
        const BasicBlock& block = cfg.blocks.at(block_addr);
        for (uint32_t a = block.start_addr; a <= block.end_addr; a += 4) {
            auto io = exe_.read_word(a);
            if (!io.has_value()) continue;
            uint32_t in = *io;
            uint32_t op = in >> 26;
            if (op != 0x0Au && op != 0x0Bu) continue;  // not slti/sltiu
            uint32_t im = in & 0xFFFFu;
            if (psx_ws_cull_imm_in(im, config_.ws_cull_w_imms.data(),
                                   (int)config_.ws_cull_w_imms.size())) has_x = true;
            else if (psx_ws_cull_imm_in(im, config_.ws_cull_h_imms.data(),
                                        (int)config_.ws_cull_h_imms.size())) has_y = true;
            if (has_x && has_y) return true;
        }
    }
    return false;
}

int CodeGenerator::detect_cull_bltz_sites(const ControlFlowGraph& cfg) {
    // Populate ws_cull_bltz_pcs_ with the function's LEFT-edge funnel bltz
    // addresses (ws_cull_detect.h idioms 2/3). Only meaningful when the
    // function already qualified (ws_auto_cull_func_). Build a contiguous word
    // image over the function range (same approach as detect_backdrop_windows)
    // so the structural classifier sees cross-block context.
    ws_cull_bltz_pcs_.clear();
    if (!ws_auto_cull_func_) return 0;
    uint32_t lo = cfg.function_start, hi = cfg.function_end;
    if (hi <= lo || (hi - lo) > 0x40000u) return 0;   // sanity bound
    std::vector<uint32_t> words;
    words.reserve((hi - lo) / 4u);
    for (uint32_t a = lo; a < hi; a += 4) {
        auto io = exe_.read_word(a);
        words.push_back(io.has_value() ? *io : 0u);
    }
    int n = (int)words.size();
    for (int i = 0; i < n; i++)
        if (psx_ws_cull_bltz_here(words.data(), n, i,
                                  config_.ws_cull_w_imms.data(),
                                  (int)config_.ws_cull_w_imms.size()))
            ws_cull_bltz_pcs_.insert(lo + (uint32_t)i * 4u);
    return (int)ws_cull_bltz_pcs_.size();
}

int CodeGenerator::detect_backdrop_windows(const ControlFlowGraph& cfg) {
    // Always clear so a stale map from the previous function can never leak.
    ws_backdrop_sites_.clear();
    if (!config_.ws_auto_backdrop_preload) return 0;

    // Build a contiguous word image of the whole function range so the shared
    // detector's absolute-PC math (base_pc + i*4) lands on real addresses; any
    // non-code gap (jump table / constant pool) simply never matches the
    // invariant and is harmless. base_pc = function_start.
    uint32_t lo = cfg.function_start, hi = cfg.function_end;
    if (hi <= lo || (hi - lo) > 0x40000u) return 0;   // sanity bound
    std::vector<uint32_t> words;
    words.reserve((hi - lo) / 4u);
    for (uint32_t a = lo; a < hi; a += 4) {
        auto io = exe_.read_word(a);
        words.push_back(io.has_value() ? *io : 0u);
    }

    WsBackdropSite sites[64];
    int ns = psx_ws_find_backdrop_windows(words.data(), (int)words.size(), lo,
                                          sites, 64);
    for (int i = 0; i < ns; i++)
        ws_backdrop_sites_[sites[i].pc] = { sites[i].kind, sites[i].window_cols };
    return ns;
}

GeneratedFunction CodeGenerator::generate_function(
    const Function& func,
    const ControlFlowGraph& cfg,
    const std::string& fallthrough_name) {

    GeneratedFunction result;
    result.function_name = func.name;
    {
        std::string sig;
        if (config_.hot_funcs.count(func.start_addr)) {
            sig += "#if defined(__GNUC__) || defined(__clang__)\n"
                   "__attribute__((hot))\n"
                   "#endif\n";
        }
        sig += fmt::format("void {}(CPUState* cpu)", func.name);
        result.signature = std::move(sig);
    }

    // Widescreen auto cull (gated): detect the screen-extent reject signature so
    // translate_instruction widens this function's width compares. Cleared for
    // every function so it never leaks across functions. The LEFT-edge funnel
    // bltz sites (signed idioms) are classified alongside.
    ws_auto_cull_func_ = config_.ws_auto_screen_x_cull &&
                         func_has_screen_extent_cull(cfg);
    detect_cull_bltz_sites(cfg);

    // Widescreen backdrop preload (gated): detect each scrolling-backdrop column
    // window so translate_instruction rewrites its START/END finalize. Cleared +
    // repopulated per function so sites never leak across functions.
    {
        int bd_windows = detect_backdrop_windows(cfg);
        if (bd_windows > 0)
            fmt::print("  [ws backdrop] {} site(s) in {} (0x{:08X})\n",
                       bd_windows, func.name, func.start_addr);
    }

    // Pre-scan: find jump table targets that land mid-block (not a block boundary).
    // These need inline labels so goto statements in the switch can reach them.
    // Must run BEFORE translating blocks (translate_basic_block reads extra_labels_).
    extra_labels_.clear();
    std::map<uint32_t, std::vector<uint32_t>> jr_table_edges;
    scan_jr_tables(cfg, jr_table_edges);

    // CPS (§25): collect this function's continuations (call return points) as
    // blocks are translated, so the entry-switch below can route a dispatched
    // continuation address into the right block.
    cps_cur_continuations_.clear();

    // Translate blocks into a temp buffer first: the CPS entry-switch is emitted
    // at the top (before the entry hooks) and must know every continuation.
    std::stringstream blocks_ss;
    for (uint32_t block_addr : cfg.block_order) {
        const BasicBlock& block = cfg.blocks.at(block_addr);
        blocks_ss << "\n" << translate_basic_block(block, cfg);
    }

    // FAITHFUL RE-ENTRY — every basic-block leader is a dispatchable continuation
    // (MMX6 boot-wedge class B, resume PC 0x8005B07C). psx_check_interrupts_at is
    // emitted at EVERY block leader, so ANY leader can be published as an async-RFE
    // / interp-handoff interrupt-resume PC. The compiled top-level trampoline can
    // only re-enter a function at a registered continuation; a leader that is only
    // an interrupt-resume point (not a CPS jal-return) was NOT registered, so a
    // mid-function resume PC missed dispatch and the top loop read pc=0 =
    // "execution completed" abnormal exit. Game functions keep ALL guest state
    // canonical in cpu->gpr[]/RAM (no cross-block C-locals), so entering at
    // block_LEADER from a fresh call reconstructs identical state to normal flow.
    // Register every leader (the jal/jalr call-returns pushed during translation
    // above are a subset, deduped by `seen`/the owner map). Entry block runs from
    // the top (cpu->pc==0) and is dispatched as a function entry, so skip it.
    if (cps_enabled_) {
        for (uint32_t block_addr : cfg.block_order) {
            if (block_addr == func.start_addr) continue;
            cps_cur_continuations_.push_back(block_addr);
        }
    }

    std::stringstream body_ss;
    body_ss << "{\n";
    body_ss << "#if defined(PSX_ENABLE_BLOCK_CYCLES) && "
               "(defined(__GNUC__) || defined(__clang__))\n"
            << config_.indent
            << "__attribute__((cleanup(psx_cyc_bb_defer_cleanup))) "
               "int _psx_cyc_bb_guard = 1;\n"
            << config_.indent << "psx_cyc_bb_defer_begin();\n";
    // Declare local-acc AFTER bb_guard so cleanup runs local_end first
    // (publish), then bb_defer_end (flush). Gated VLC leaves only.
    if (config_.load_charge_batch_funcs.count(func.start_addr)) {
        body_ss << config_.indent << "uint32_t _psx_lc_acc = 0;\n"
                << config_.indent
                << "__attribute__((cleanup(psx_cyc_local_cleanup))) "
                   "uint32_t *_psx_lc_guard = &_psx_lc_acc;\n"
                << config_.indent << "psx_cyc_local_begin(&_psx_lc_acc);\n";
    }
    body_ss << "#endif\n";

    // Cadence / RAM-prep mod hooks must run on CPS mid-function re-entry too:
    // AOT interior blocks still `lw` guest RAM (e.g. SimVblankGate skip). Other
    // entry hooks (debug log, widescreen) stay AFTER the switch so they only
    // fire on a true prologue entry (cpu->pc == 0).
    if (config_.mod_function_entry_funcs.count(func.start_addr)) {
        body_ss << config_.indent
                << fmt::format(
                       "psx_mod_function_entry(cpu, 0x{:08X}u);"
                       "  /* trusted opt-in game-mod hook (pre-CPS) */\n",
                       func.start_addr);
    }

    // CPS entry-switch: when the unified flat trampoline dispatches a
    // continuation address (a callee published cpu->pc = $ra back to us), route
    // into the owning block. Emitted before debug/widescreen entry hooks so a
    // continuation re-entry bypasses those (true-entry only). mod_function_entry
    // is intentionally above so cadence plugins still run.
    // Overlay mode must emit the cpu->pc guard even when the continuation set
    // is EMPTY: a range re-entry can still request an interior PC of a
    // single-block function, and without the guard the body runs from its top
    // regardless of the requested PC — the same corrupting-fall-through this
    // switch's fail-closed default exists to prevent.
    if (cps_enabled_ && (!cps_cur_continuations_.empty() || config_.overlay_mode)) {
        std::set<uint32_t> seen;
        body_ss << config_.indent << "if (cpu->pc != 0u) {\n";
        body_ss << config_.indent << "    uint32_t _cont = cpu->pc; cpu->pc = 0;\n";
        body_ss << config_.indent << "    switch (_cont) {\n";
        for (uint32_t c : cps_cur_continuations_) {
            if (!seen.insert(c).second) continue;
            if (partial_block_cycle_count(c, cfg) != 0) {
                body_ss << config_.indent << fmt::format("        case 0x{:08X}u:\n", c);
                body_ss << emit_mid_block_cycle_charge(c, cfg, config_.indent + "            ");
                body_ss << config_.indent << fmt::format("            goto block_{:08X};\n", c);
            } else {
                body_ss << config_.indent
                        << fmt::format("        case 0x{:08X}u: goto block_{:08X};\n", c, c);
            }
            cps_continuation_owner_[c] = func.start_addr;
        }
        if (config_.overlay_mode) {
            // Overlay functions: FAIL CLOSED on a foreign interior entry (the
            // Tomba/Tomba2 native-overlay "blue screen" wedge). Entry at the true
            // prologue (cpu->pc == start_addr) runs from the top. Any OTHER non-zero
            // _cont is a foreign interior entry (a range-ownership mismatch routed a
            // PC this function does not own into it). The old `default: break` ran
            // the function FROM ITS TOP -- corrupting shared CPU/RAM state and
            // wedging. Instead restore the requested PC and signal the overlay
            // dispatch to route it to the sanctioned dirty-RAM interpreter
            // (psx_native_bad_entry). NEVER run from the top.
            if (seen.insert(func.start_addr).second) {
                body_ss << config_.indent
                        << fmt::format("        case 0x{:08X}u: break;  /* entry at prologue */\n",
                                       func.start_addr);
            }
            body_ss << config_.indent
                    << fmt::format("        default: cpu->pc = _cont; "
                                   "psx_native_bad_entry(cpu, 0x{:08X}u, _cont); return;\n",
                                   func.start_addr);
        } else {
            // Static (BIOS/boot-EXE) functions keep the legacy fall-through: the
            // generated static trampoline does not consume the bad-entry signal,
            // and static ranges are discovered once with no overlay multi-variant
            // ownership ambiguity. (The fail-closed default is overlay-scoped.)
            body_ss << config_.indent << "        default: break;\n";
        }
        body_ss << config_.indent << "    }\n";
        body_ss << config_.indent << "}\n";
    }

    body_ss << config_.indent
            << fmt::format("debug_server_log_call_entry(0x{:08X}u);\n",
                          func.start_addr);
    {
        const auto vq = config_.vsync_query_hle_funcs.find(func.start_addr);
        if (vq != config_.vsync_query_hle_funcs.end()) {
            body_ss << config_.indent
                    << fmt::format(
                        "if (psx_vsync_query_hle_enter(cpu, 0x{:08X}u, 0x{:08X}u, "
                        "0x{:08X}u, 0x{:08X}u, 0x{:08X}u)) return;"
                        "  /* load accel: cycle-faithful VSync(-1) query */\n",
                        func.start_addr, vq->second[0], vq->second[1],
                        vq->second[2], vq->second[3]);
        }
    }
    if (config_.data_shard_funcs.count(func.start_addr)) {
        // Data-shard hook (docs/DATA_SHARDS.md): on a verified shard hit the
        // runtime applies the recorded effect, credits the recorded cycles,
        // publishes pc=$ra and we return without executing the body. Otherwise
        // it arms the capture recorder and the body runs natively.
        body_ss << config_.indent
                << fmt::format("if (psx_datashard_enter(cpu, 0x{:08X}u)) return;"
                               "  /* data-shard: replay or arm capture */\n",
                               func.start_addr);
    }
    if (config_.ws_sprite_tag_funcs.count(func.start_addr)) {
        body_ss << config_.indent
                << "psx_ws_sprite_tag(cpu);  /* widescreen: record prim ($a0) + anchor */\n";
    }
    if (config_.ws_bg2d_init_func == func.start_addr) {
        body_ss << config_.indent
                << "psx_ws_mmx6_bg_stage_init();  /* widescreen: invalidate stale reveal at stage BG init */\n";
    }
    if (config_.ws_backdrop_unsquash_funcs.count(func.start_addr)) {
        body_ss << config_.indent
                << "gte_ws_set_suppress(1);  /* widescreen: un-squash far backdrop (8C) */\n";
    }

    // Add function comment
    if (config_.emit_comments) {
        body_ss << config_.indent
                << fmt::format("/* Address: 0x{:08X}, Size: {} bytes, Blocks: {} */\n",
                              func.start_addr, func.size, cfg.blocks.size());
    }

    body_ss << blocks_ss.str();

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

void CodeGenerator::scan_jr_tables(
    const ControlFlowGraph& cfg,
    std::map<uint32_t, std::vector<uint32_t>>& out_edges) {

    for (const auto& [baddr, blk] : cfg.blocks) {
        if (blk.exit_instr.type != ControlFlowType::JumpRegister) continue;
        uint32_t jr_r = get_rs(blk.exit_instr.instruction);
        ExactJumpTable exact_table;
        if (!resolve_exact_bounded_jump_table(
                exe_, cfg.function_start, cfg.function_end,
                blk.exit_instr.address, jr_r, exact_table, ram_to_rom,
                cfg.producer_lo, cfg.producer_hi)) {
            continue;
        }
        uint32_t tb = exact_table.table_base;
        uint32_t tc = exact_table.table_count;
        if (tb==0 || tc==0) continue;
        for (const auto& target_pair : exact_table.targets) {
            uint32_t t = target_pair.second;
            if (t >= cfg.function_start && t < cfg.function_end) {
                out_edges[baddr].push_back(t);
                if (!cfg.blocks.count(t))
                    extra_labels_.insert(t);
            }
        }
    }
}

std::vector<GeneratedFunction> CodeGenerator::generate_alias_group(
    const std::vector<const Function*>& aliases,
    const ControlFlowGraph& cfg,
    const std::string& fallthrough_name) {

    std::vector<GeneratedFunction> results;
    if (aliases.empty()) return results;
    uint32_t host = aliases[0]->alias_walk_lo;

    // Widescreen auto cull (gated): set per-group so a stale value from the last
    // generate_function() call can't leak into this body. (See generate_function.)
    ws_auto_cull_func_ = config_.ws_auto_screen_x_cull &&
                         func_has_screen_extent_cull(cfg);
    detect_cull_bltz_sites(cfg);

    // Backdrop preload sites for this alias group's shared CFG (see
    // generate_function). Set per-group so a stale map can't leak in.
    {
        int bd_windows = detect_backdrop_windows(cfg);
        if (bd_windows > 0)
            fmt::print("  [ws backdrop] {} site(s) in alias host 0x{:08X}\n",
                       bd_windows, host);
    }

    // Jump-table edges + mid-block labels for this host's CFG.
    extra_labels_.clear();
    std::map<uint32_t, std::vector<uint32_t>> jr_table_edges;
    scan_jr_tables(cfg, jr_table_edges);

    // CPS (§25): collect this alias group's continuations during block
    // translation (also prevents a stale list from leaking into the next
    // generate_function call).
    cps_cur_continuations_.clear();

    // Union of blocks reachable from any alias entry. Edges are
    // BasicBlock::successors plus jump-table targets (mapped to their
    // containing block — table targets may be mid-block labels).
    auto containing_block = [&](uint32_t a) -> uint32_t {
        auto it = cfg.blocks.upper_bound(a);
        if (it == cfg.blocks.begin()) return 0;
        --it;
        return (a >= it->second.start_addr && a <= it->second.end_addr)
            ? it->first : 0;
    };
    std::set<uint32_t> live_blocks;
    std::vector<uint32_t> work;
    for (const Function* a : aliases) {
        uint32_t eb = containing_block(a->start_addr);
        if (eb != 0) work.push_back(eb);
    }
    while (!work.empty()) {
        uint32_t b = work.back();
        work.pop_back();
        if (!live_blocks.insert(b).second) continue;
        const BasicBlock& blk = cfg.blocks.at(b);
        for (uint32_t s : blk.successors) {
            if (cfg.blocks.count(s)) work.push_back(s);
        }
        auto je = jr_table_edges.find(b);
        if (je != jr_table_edges.end()) {
            for (uint32_t t : je->second) {
                uint32_t cb = containing_block(t);
                if (cb != 0) work.push_back(cb);
            }
        }
    }

    // Translate live blocks first (populates cps_cur_continuations_ for the CPS
    // continuation switch below).
    std::stringstream blocks_buf;
    for (uint32_t block_addr : cfg.block_order) {
        if (!live_blocks.count(block_addr)) continue;
        const BasicBlock& block = cfg.blocks.at(block_addr);
        blocks_buf << "\n" << translate_basic_block(block, cfg);
    }

    // FAITHFUL RE-ENTRY — every live block leader is a dispatchable continuation
    // (see generate_function for the full rationale: psx_check_interrupts_at is at
    // every leader, so any leader can be an async-RFE/interp-handoff resume PC and
    // must be re-enterable, not just CPS jal-returns). Alias-set blocks live behind
    // an entry switch; the leaders re-enter via the continuation switch below.
    // Owner = aliases[0]->start_addr (same as the call-return continuations). Skip
    // the alias entry PCs (they are dispatched as function entries with cpu->pc==0).
    if (cps_enabled_) {
        std::set<uint32_t> alias_entries;
        for (const Function* a : aliases) alias_entries.insert(a->start_addr);
        for (uint32_t block_addr : cfg.block_order) {
            if (!live_blocks.count(block_addr)) continue;
            if (alias_entries.count(block_addr)) continue;
            cps_cur_continuations_.push_back(block_addr);
        }
    }

    // Shared body: host-range blocks (live subset) behind an entry switch.
    std::stringstream body;
    body << fmt::format("/* Overlapping-alias body for host func_{:08X}: {} entries */\n",
                        host, aliases.size());
    body << fmt::format("void psx_alias_body_{:08X}(CPUState* cpu, uint32_t entry)\n{{\n",
                        host);
    alias_body_decls_.push_back(fmt::format("void psx_alias_body_{:08X}(CPUState* cpu, uint32_t entry)", host));
    // CPS (§25): a dispatched continuation (callee published cpu->pc = $ra) is
    // routed into its block here, BEFORE the entry switch — cpu->pc overrides
    // the `entry` arg. The dispatch routes a continuation by calling the first
    // alias wrapper with cpu->pc set (entry is then ignored). Owner =
    // aliases[0]->start_addr.
    if (cps_enabled_ && (!cps_cur_continuations_.empty() || config_.overlay_mode)) {
        std::set<uint32_t> seen;
        body << config_.indent << "if (cpu->pc != 0u) {\n";
        body << config_.indent << "    uint32_t _cont = cpu->pc; cpu->pc = 0;\n";
        body << config_.indent << "    switch (_cont) {\n";
        for (uint32_t c : cps_cur_continuations_) {
            if (!seen.insert(c).second) continue;
            body << config_.indent
                 << fmt::format("        case 0x{:08X}u: goto block_{:08X};\n", c, c);
            cps_continuation_owner_[c] = aliases[0]->start_addr;
        }
        if (config_.overlay_mode) {
            // Alias entry PCs are excluded from cps_cur_continuations_ (they
            // dispatch as function entries with cpu->pc == 0), but a RANGE
            // re-entry can still request one with cpu->pc set — route it to
            // its block, exactly as the entry switch below would.
            for (const Function* a : aliases) {
                if (!seen.insert(a->start_addr).second) continue;
                body << config_.indent
                     << fmt::format("        case 0x{:08X}u: goto block_{:08X};\n",
                                    a->start_addr, a->start_addr);
            }
            // FAIL CLOSED on any other interior PC (same contract as
            // generate_function's overlay entry switch, PR #46). The old
            // `default: break` fell through to `switch (entry)` and ran the
            // ALIAS'S OWN block for a PC it does not own — the Tomba 2
            // splash→title reload death: dispatch(0x80089788) resolved to the
            // alias func_80089770, which re-ran the crt0 shim tail (reload
            // $ra, call main-init) in an infinite loop, leaking 0x40 of guest
            // stack per iteration until the frames overwrote the freshly
            // loaded module code itself. Restore the requested PC and signal
            // the overlay dispatch to route it to the dirty-RAM interpreter.
            body << config_.indent
                 << fmt::format("        default: cpu->pc = _cont; "
                                "psx_native_bad_entry(cpu, 0x{:08X}u, _cont); return;\n",
                                aliases[0]->start_addr);
        } else {
            body << config_.indent << "        default: break;\n";
        }
        body << config_.indent << "    }\n";
        body << config_.indent << "}\n";
    }
    body << config_.indent << "switch (entry) {\n";
    for (const Function* a : aliases) {
        body << config_.indent
             << fmt::format("    case 0x{:08X}u: goto block_{:08X};\n",
                            a->start_addr, a->start_addr);
    }
    if (config_.overlay_mode) {
        // A dispatched entry outside the alias set must not silently no-op:
        // the loader would count it as executed. Fail closed to the interp.
        body << config_.indent
             << fmt::format("    default: cpu->pc = entry; "
                            "psx_native_bad_entry(cpu, 0x{:08X}u, entry); return;\n",
                            aliases[0]->start_addr);
    } else {
        body << config_.indent << "    default: return;\n";
    }
    body << config_.indent << "}\n";

    body << blocks_buf.str();

    // Host-end fallthrough (mirrors generate_function): only relevant when the
    // host's final block is live and ends without explicit control flow.
    if (!fallthrough_name.empty() && !cfg.block_order.empty() &&
        live_blocks.count(cfg.block_order.back())) {
        const BasicBlock& last_block = cfg.blocks.at(cfg.block_order.back());
        bool needs_fallthrough =
            (last_block.exit_instr.type == ControlFlowType::None) ||
            ((last_block.exit_instr.type == ControlFlowType::Branch ||
              last_block.exit_instr.type == ControlFlowType::Jump) &&
             last_block.successors.empty());
        if (needs_fallthrough) {
            body << fmt::format("    {}(cpu);  /* fallthrough to next function */\n",
                                fallthrough_name);
        }
    }
    body << "    ;  /* label compatibility */\n";
    body << "}\n";

    // One dispatchable wrapper per alias entry; the first carries the body.
    for (size_t i = 0; i < aliases.size(); ++i) {
        const Function* a = aliases[i];
        GeneratedFunction gf;
        gf.function_name = a->name;
        gf.signature = fmt::format("void {}(CPUState* cpu)", a->name);
        gf.body = fmt::format(
            "{{\n"
            "    debug_server_log_call_entry(0x{:08X}u);\n"
            "    psx_alias_body_{:08X}(cpu, 0x{:08X}u);  /* alias entry into host func_{:08X} */\n"
            "}}\n",
            a->start_addr, host, a->start_addr, host);
        gf.full_code = (i == 0 ? body.str() + "\n" : std::string()) +
                       gf.signature + "\n" + gf.body;
        gf.line_count = std::count(gf.full_code.begin(), gf.full_code.end(), '\n');
        results.push_back(std::move(gf));
    }
    return results;
}

std::vector<GeneratedFunction> CodeGenerator::generate_all_functions(
    const std::vector<Function>& functions,
    const std::map<uint32_t, ControlFlowGraph>& cfgs) {

    std::vector<GeneratedFunction> results;
    results.reserve(functions.size());

    // CPS (§25): rebuild the continuation->owner map for this full pass so the
    // game dispatch table (psx_dispatch_game_compiled) reflects exactly the
    // functions emitted here (the final generate_file pass is authoritative).
    cps_continuation_owner_.clear();
    alias_body_decls_.clear();

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

    // Group overlapping-alias entries by host: each group is emitted as one
    // shared static body plus per-entry wrappers (see generate_alias_group),
    // not as per-alias duplicates of the host's blocks.
    std::map<uint32_t, std::vector<const Function*>> alias_groups;
    for (const Function& f : functions) {
        if (f.alias_walk_lo != 0) alias_groups[f.alias_walk_lo].push_back(&f);
    }
    std::set<uint32_t> alias_groups_emitted;

    int total_lines = 0;

    for (const Function& func : functions) {
        if (func.alias_walk_lo != 0) {
            if (!alias_groups_emitted.insert(func.alias_walk_lo).second) continue;
            const auto& group = alias_groups.at(func.alias_walk_lo);
            if (cfgs.count(func.start_addr) == 0) continue;
            const ControlFlowGraph& cfg = cfgs.at(func.start_addr);

            // Host-end fallthrough target (all group members share end_addr).
            std::string fallthrough_name;
            auto ft_it = func_name_by_addr.find(func.end_addr);
            if (ft_it != func_name_by_addr.end()) fallthrough_name = ft_it->second;

            auto group_funcs = generate_alias_group(group, cfg, fallthrough_name);
            for (auto& gf : group_funcs) {
                total_lines += gf.line_count;
                results.push_back(std::move(gf));
            }
            continue;
        }
        if (func.is_data_section) {
            GeneratedFunction stub;
            stub.function_name = func.name;
            stub.signature = fmt::format("void {}(CPUState* cpu)", func.name);
            stub.body = fmt::format(
                "{{\n    psx_unknown_dispatch(cpu, 0x{:08X}u, 0x{:08X}u);\n}}\n",
                func.start_addr, func.start_addr & 0x1FFFFFFFu);
            stub.full_code = stub.signature + "\n" + stub.body;
            stub.line_count = 4;
            stub.dispatchable = false;
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

        // PSX_EMIT_PROD (DEFAULT ON): a "function" whose body is overwhelmingly
        // untranslatable (TODO-opcode) words is a data region that discovery's
        // primary-opcode-only validity check let slip through as code (valid
        // primary opcode, garbage sub-field). Re-emit it as the same compact,
        // fail-closed data stub used for is_data_section — if it is ever
        // dispatched (it should not be: real reachable code never hits a TODO)
        // psx_unknown_dispatch fatals LOUDLY rather than silently no-op'ing.
        // This is strictly more faithful than the baseline it replaces (which
        // emitted a SILENT no-op for the same words). Smoke-validated on Tomba,
        // Tomba2, Ape Escape, MMX6. Set PSX_EMIT_PROD=0 to restore the full-scan
        // (oracle) emission. Proper productionization moves this judgment UPSTREAM
        // into discovery's validity check (sub-field aware) in function_analysis.
        static int s_emit_prod = -1;
        if (s_emit_prod < 0) {
            const char* e = std::getenv("PSX_EMIT_PROD");
            s_emit_prod = (e && *e == '0') ? 0 : 1;  // default ON; =0 restores full-scan baseline
        }
        if (s_emit_prod) {
            size_t todo = 0, pos = 0;
            while ((pos = gen_func.body.find("TODO: opcode", pos)) != std::string::npos) { todo++; pos += 12; }
            pos = 0;
            while ((pos = gen_func.body.find("TODO: SPECIAL", pos)) != std::string::npos) { todo++; pos += 13; }
            uint32_t instrs = func.size / 4u;
            if (instrs > 0 && todo * 2u >= instrs) {   // >= 50% untranslatable => data
                GeneratedFunction data_stub;
                data_stub.function_name = func.name;
                data_stub.signature = fmt::format("void {}(CPUState* cpu)", func.name);
                data_stub.body = fmt::format(
                    "{{\n    psx_unknown_dispatch(cpu, 0x{:08X}u, 0x{:08X}u);\n}}\n",
                    func.start_addr, func.start_addr & 0x1FFFFFFFu);
                data_stub.full_code = data_stub.signature + "\n" + data_stub.body;
                data_stub.line_count = 4;
                data_stub.dispatchable = false;
                total_lines += data_stub.line_count;
                results.push_back(std::move(data_stub));
                continue;
            }
        }

        total_lines += gen_func.line_count;
        results.push_back(gen_func);
    }

    fmt::print("✓ Generated {} functions\n", results.size());
    fmt::print("✓ Total C code: {} lines\n\n", total_lines);

    return results;
}

void CodeGenerator::emit_runtime_externs(std::ostream& ss) const {
    /* Release inlines debug_server_log_call_entry via debug_server.h
     * (included from psx_runtime.h). Debug builds need the extern. */
    ss << "#ifndef PSX_NO_DEBUG_TOOLS\n";
    ss << "extern void debug_server_log_call_entry(uint32_t func_addr);\n";
    ss << "extern void debug_server_cyc_observe(uint32_t block_leader_phys);\n";
    ss << "#endif\n";
    ss << "#ifdef PSX_COSIM\n";
    ss << "extern void cosim_block(uint32_t block_leader_phys);\n";
    ss << "extern void cosim_instr(uint32_t pc);\n";
    ss << "#endif\n";
    ss << "extern int  psx_datashard_enter(CPUState* cpu, uint32_t key);  /* data-shard replay/capture (data_shards.c) */\n";
    ss << "extern void psx_mod_function_entry(CPUState* cpu, uint32_t address);  /* trusted opt-in game-mod hook */\n";
    ss << "extern void psx_datashard_ret(CPUState* cpu);                  /* data-shard capture finalize */\n";
    ss << "extern int  psx_vsync_query_hle_enter(CPUState* cpu, uint32_t func, uint32_t counter_addr, uint32_t gpustat_ptr_addr, uint32_t timer1_ptr_addr, uint32_t timer1_cache_addr);  /* load_accel.c */\n";
    ss << "extern void psx_ws_sprite_tag(CPUState* cpu);  /* widescreen prim tag (gpu.c) */\n";
    ss << "extern void psx_ws_mmx6_bg_stage_init(void);    /* ws 2D stage reveal invalidation (gpu.c) */\n";
    ss << "extern int  psx_ws_x_margin(void);  /* widescreen cull-margin term (gpu.c) */\n";
    ss << "extern int32_t psx_ws_player_x_bound(int32_t vanilla);  /* typed gameplay X bound */\n";
    ss << "extern int  psx_ws_cull_sltiu(uint32_t sx, uint32_t imm);  /* ws auto screen-x cull (gpu.c) */\n";
    ss << "extern int  psx_ws_cull_slti(uint32_t sx, uint32_t imm);   /* ws cull signed right edge (gpu.c) */\n";
    ss << "extern int  psx_ws_cull_slti_lower(uint32_t sx, uint32_t imm); /* ws cull signed lower edge (gpu.c) */\n";
    ss << "extern int  psx_ws_cull_slt_widen(uint32_t rs, uint32_t rt, int bound_is_rt); /* ws cull register-bound widen (gpu.c) */\n";
    ss << "extern int  psx_ws_cull_bltz(uint32_t v);                  /* ws cull signed left edge (gpu.c) */\n";
    ss << "extern int  psx_ws_cull_vxrange(uint32_t x, uint32_t imm); /* ws masked-u16 X window */\n";
    ss << "extern int32_t psx_ws_depth_bound(int32_t imm);            /* ws aspect-scaled far bound */\n";
    ss << "extern int32_t psx_ws_plane_nx(int32_t nx);                /* ws side-plane normal-X scale (gpu.c) */\n";
    ss << "extern uint32_t psx_ws_xclip_bound(uint32_t vanilla);      /* ws per-prim X reject bound load (gpu.c) */\n";
    ss << "extern uint32_t psx_ws_cull_keep_result(uint32_t vanilla, uint32_t forced); /* ws guarded keep compare (gpu.c) */\n";
    ss << "extern uint32_t psx_ws_aspect_cone_result(uint32_t site, uint32_t vanilla, uint32_t object, int32_t x, int32_t z, int32_t y); /* aspect-aware participation cone */\n";
    ss << "extern uint32_t psx_ws_angle_widen(uint32_t vanilla); /* aspect-scaled 12-bit terrain-frustum half-angle */\n";
    ss << "extern int  psx_ws_backdrop_x(int x);  /* widescreen backdrop screenX squash (gpu.c) */\n";
    ss << "extern int  psx_ws_bg2d_cols(int base);                    /* ws 2D bg tile-loop widen: col count (gpu.c) */\n";
    ss << "extern int  psx_ws_bg2d_startcol(int col, unsigned mask);  /* ws 2D bg tile-loop widen: start tile col (gpu.c) */\n";
    ss << "extern int  psx_ws_bg2d_startx(int x);                     /* ws 2D bg tile-loop widen: start screen-x (gpu.c) */\n";
    ss << "extern int  psx_ws_bg2d_stream_left(int x);                /* ws 2D bg tile-ring streamer: left edge (gpu.c) */\n";
    ss << "extern int  psx_ws_bg2d_stream_right(int x);               /* ws 2D bg tile-ring streamer: right edge (gpu.c) */\n";
    ss << "extern int  psx_ws_bg2d_undercap(int counter, int native_cap); /* ws 2D BG packet cap (gpu.c) */\n";
    ss << "extern int  psx_ws_mmx6_bg_bufbase(int addr);   /* ws 2D bg packet-buffer relocation (gpu.c) */\n";
    ss << "extern int  psx_ws_mmx6_bg_undercap(int counter);/* ws 2D bg per-frame tile cap (gpu.c) */\n";
    ss << "extern int  psx_game_option_store(uint32_t addr, int val);  /* persisted OPTION restore-at-init (game_options.c) */\n";
    ss << "extern uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols);  /* ws backdrop preload (gpu.c) */\n";
    ss << "extern void gte_ws_set_suppress(int on);  /* widescreen far-backdrop un-squash (gte.cpp) */\n";
    ss << "extern uint32_t g_debug_last_store_pc;  /* exact PC of the executing SW/SH/SB — wtrace/readtrace producer attribution (debug_server.c) */\n\n";
}

void CodeGenerator::emit_unaligned_helpers(std::ostream& ss, bool as_inline) const {
    const char* kw = as_inline ? "static inline " : "static ";

    // Emit reference implementations for unaligned memory helpers.
    // These implement the MIPS lwl/lwr/swl/swr semantics.
    // The runtime may override these by setting the corresponding function pointers
    // in CPUState, but these standalone functions provide correct reference behavior.
    ss << "/* --- Unaligned memory access helper implementations --- */\n";
    ss << "\n";
    ss << "/* LWL/LWR/SWL/SWR implement little-endian R3000A unaligned word\n";
    ss << " * semantics. Keep these switch tables in sync with psx_interpreter.c\n";
    ss << " * and dirty_ram_interp.c.\n";
    ss << " */\n";
    ss << "\n";
    ss << "/* LWL: Load Word Left - merges high bytes from aligned word into rt.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the current value of the destination register.\n";
    ss << " */\n";
    ss << kw << "uint32_t psx_lwl(CPUState* cpu, uint32_t addr, uint32_t rt_value, uint32_t rt, uint32_t reg_mask) {\n";
    ss << "    uint32_t word = psx_cyc_load_word(cpu, addr & ~3u, rt, reg_mask);  /* full load interlock */\n";
    ss << "    switch (addr & 3u) {\n";
    ss << "        case 0: return (rt_value & 0x00FFFFFFu) | (word << 24);\n";
    ss << "        case 1: return (rt_value & 0x0000FFFFu) | (word << 16);\n";
    ss << "        case 2: return (rt_value & 0x000000FFu) | (word << 8);\n";
    ss << "        default: return word;\n";
    ss << "    }\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* LWR: Load Word Right - merges low bytes from aligned word into rt.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the current value of the destination register.\n";
    ss << " */\n";
    ss << kw << "uint32_t psx_lwr(CPUState* cpu, uint32_t addr, uint32_t rt_value, uint32_t rt, uint32_t reg_mask) {\n";
    ss << "    uint32_t word = psx_cyc_load_word(cpu, addr & ~3u, rt, reg_mask);  /* full load interlock */\n";
    ss << "    switch (addr & 3u) {\n";
    ss << "        case 0: return word;\n";
    ss << "        case 1: return (rt_value & 0xFF000000u) | (word >> 8);\n";
    ss << "        case 2: return (rt_value & 0xFFFF0000u) | (word >> 16);\n";
    ss << "        default: return (rt_value & 0xFFFFFF00u) | (word >> 24);\n";
    ss << "    }\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* SWL: Store Word Left - stores the high bytes of rt_value into memory.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the value of the source register.\n";
    ss << " */\n";
    ss << kw << "void psx_swl(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    psx_store_cycle_barrier();\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t word = cpu->read_word(aligned_addr);\n";
    ss << "    switch (addr & 3u) {\n";
    ss << "        case 0: word = (word & 0xFFFFFF00u) | (rt_value >> 24); break;\n";
    ss << "        case 1: word = (word & 0xFFFF0000u) | (rt_value >> 16); break;\n";
    ss << "        case 2: word = (word & 0xFF000000u) | (rt_value >> 8); break;\n";
    ss << "        default: word = rt_value; break;\n";
    ss << "    }\n";
    ss << "    cpu->write_word(aligned_addr, word);\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* SWR: Store Word Right - stores the low bytes of rt_value into memory.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the value of the source register.\n";
    ss << " */\n";
    ss << kw << "void psx_swr(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    psx_store_cycle_barrier();\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t word = cpu->read_word(aligned_addr);\n";
    ss << "    switch (addr & 3u) {\n";
    ss << "        case 0: word = rt_value; break;\n";
    ss << "        case 1: word = (word & 0x000000FFu) | (rt_value << 8); break;\n";
    ss << "        case 2: word = (word & 0x0000FFFFu) | (rt_value << 16); break;\n";
    ss << "        default: word = (word & 0x00FFFFFFu) | (rt_value << 24); break;\n";
    ss << "    }\n";
    ss << "    cpu->write_word(aligned_addr, word);\n";
    ss << "}\n";
    ss << "\n";
}

std::string CodeGenerator::build_shared_decls_header(const std::vector<GeneratedFunction>& gen_funcs) const {
    std::stringstream h;
    h << "/* Generated by PSXRecomp - shared declarations for split full.c shards. DO NOT EDIT. */\n";
    h << "#pragma once\n";
    h << "#include \"psx_runtime.h\"\n\n";
    emit_runtime_externs(h);
    emit_unaligned_helpers(h, /*as_inline=*/true);
    if (!gen_funcs.empty()) {
        h << "/* Forward declarations for all recompiled functions */\n";
        for (const auto& gf : gen_funcs) h << gf.signature << ";\n";
        for (const auto& decl : alias_body_decls_) h << decl << ";\n";
        h << "\n";
    }
    return h.str();
}

std::string CodeGenerator::generate_file(
    const std::vector<Function>& functions,
    const std::map<uint32_t, ControlFlowGraph>& cfgs) {

    std::stringstream ss;

    // File header
    ss << "/* Generated by PSXRecomp - PlayStation 1 Static Recompiler */\n\n";

    // Include the generic PSX runtime header.
    // This provides CPUState, GTE/trap declarations, and call_by_address().
    ss << "#include \"psx_runtime.h\"\n\n";
    emit_runtime_externs(ss);
    emit_unaligned_helpers(ss, /*as_inline=*/false);

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
    // at discovered targets. Iterate until convergence (no new mid-targets
    // found), since splitting creates new pieces whose branches may target
    // other mid-piece addresses. Each subsequent pass only scans CFGs that
    // were rebuilt in the prior pass.
    //
    // Previously hard-capped at 3 iterations; Tomba's `func_800905DC` had
    // 2 mid-func targets (0x800905E4, 0x80090600) that needed a 4th pass.
    // The unconverged stragglers fell through to the
    // `call_by_address(cpu, 0xX); return;` emit path (code_generator.cpp
    // lines 970/981/998), which at runtime missed the dispatch table and
    // hit `psx_unknown_dispatch`. Surfaced by Phase B2 audit. The 16-pass
    // cap is a safety limit; in practice each pass strictly reduces the
    // remaining target set so convergence is fast.
    //
    // Overlay exact mode disables this: overlay branch targets are basic-block
    // labels, not callable function entries, so splitting them into standalone
    // functions produces broken dispatch — the mid-function-seed failure mode.
    if (config_.split_mid_function_targets) {
        std::set<uint32_t> cfgs_to_scan;  // Which CFGs to scan (empty = all)
        uint32_t exe_start = exe_.header.load_address;
        uint32_t exe_end = exe_.end_address();
        int total_new = 0;
        // Safety cap only — the loop must run to convergence. Unconverged
        // targets emit `call_by_address(mid-func); return;` which misses the
        // dispatch table at runtime. 16 proved too low once data-scan
        // promotions and alias entries widened the function set.
        const int MAX_PASSES = 256;
        bool converged = false;

        for (int pass = 0; pass < MAX_PASSES; pass++) {
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
                    } else if (block.exit_instr.type == ControlFlowType::JumpLink ||
                               block.exit_instr.type == ControlFlowType::JumpLinkReg) {
                        // CPS jal/jalr publishes $ra = addr+8. When the delay slot
                        // was split into another piece, that return PC is often a
                        // mid-block address (not a function start / block leader) and
                        // was never registered — MotK 0x8001E7F0 fell to dirty_ram.
                        check_target(block.exit_instr.address + 8);
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

            if (mid_targets.empty()) { converged = true; break; }

            fmt::print("Pre-pass {}: {} mid-function branch targets\n", pass + 1, mid_targets.size());

            // Sort functions for binary search. Alias entries overlap their
            // host's range and must never be treated as the containing
            // function of a mid-target (that would split/truncate the alias
            // instead of the host) — exclude them from containment.
            std::sort(functions_mut.begin(), functions_mut.end(),
                [](const Function& a, const Function& b) { return a.start_addr < b.start_addr; });
            std::vector<uint32_t> func_starts;
            for (const auto& f : functions_mut) {
                if (f.alias_walk_lo == 0) func_starts.push_back(f.start_addr);
            }

            // Group targets by containing function. Some real compiler layouts
            // branch past an early jr-ra return into the bytes before the next
            // discovered function. Those targets are not "mid-function" by the
            // return-scanner's range, but they are executable and must still be
            // dispatchable CPS pieces.
            std::map<uint32_t, std::vector<uint32_t>> splits_by_func;
            std::set<uint32_t> gap_targets;
            for (uint32_t target : mid_targets) {
                auto it = std::upper_bound(func_starts.begin(), func_starts.end(), target);
                if (it == func_starts.begin()) {
                    gap_targets.insert(target);
                    continue;
                }
                --it;
                auto fit = std::find_if(functions_mut.begin(), functions_mut.end(),
                    [&](const Function& f) { return f.start_addr == *it; });
                if (fit != functions_mut.end() && target > fit->start_addr && target < fit->end_addr) {
                    splits_by_func[*it].push_back(target);
                } else {
                    gap_targets.insert(target);
                }
            }

            // Split each affected function
            std::vector<Function> new_funcs;
            std::set<uint32_t> affected;

            for (uint32_t target : gap_targets) {
                auto next_it = std::upper_bound(func_starts.begin(), func_starts.end(), target);
                uint32_t gap_end = (next_it != func_starts.end()) ? *next_it : exe_end;
                if (target >= gap_end) continue;

                Function nf;
                nf.start_addr = target;
                nf.end_addr = gap_end;
                nf.size = nf.end_addr - nf.start_addr;
                nf.name = fmt::format("func_{:08X}", nf.start_addr);
                nf.has_prologue = false;
                nf.has_epilogue = false;
                nf.stack_frame_size = 0;
                nf.is_data_section = false;
                new_funcs.push_back(nf);
                known_addrs.insert(nf.start_addr);
                affected.insert(nf.start_addr);
            }

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
        if (!converged) {
            fmt::print("WARNING: mid-function split pre-pass did NOT converge after {} passes; "
                       "remaining targets will dispatch-miss at runtime\n", MAX_PASSES);
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
        for (const auto& decl : alias_body_decls_) ss << decl << ";\n";
        ss << "\n";
    }

    // Function definitions
    for (const auto& gen_func : gen_funcs) {
        ss << gen_func.full_code << "\n";
    }

    // Stash for the split-shard writer (main_psx); copy is intentional so the
    // caller can consume it after generate_file() returns the monolith text.
    last_gen_funcs_ = gen_funcs;

    // Preserve the exact post-split CFG inventory used by the emitted C. The
    // dispatch generator consumes this so synthesized fallthrough functions
    // receive the same byte-precise native-validity ranges as ordinary entries.
    last_ranges_manifest_ = generate_ranges_manifest(functions_mut, cfgs_mut);

    return ss.str();
}

std::string CodeGenerator::generate_ranges_manifest(
    const std::vector<Function>& functions,
    const std::map<uint32_t, ControlFlowGraph>& cfgs) {

    std::stringstream ss;
    ss << "# psxrecomp overlay code-range manifest v1\n";
    ss << "# F <entry>   one per function (virtual entry address)\n";
    ss << "# R <lo> <len>  one per coalesced code range (hex, virtual)\n";

    for (const auto& func : functions) {
        auto it = cfgs.find(func.start_addr);
        if (it == cfgs.end()) continue;
        const ControlFlowGraph& cfg = it->second;

        // Byte intervals [start_addr, end_addr+4) per block (end_addr is the
        // last instruction, inclusive).
        std::vector<std::pair<uint32_t, uint32_t>> iv;
        for (const auto& [baddr, blk] : cfg.blocks) {
            (void)baddr;
            uint32_t lo = blk.start_addr;
            uint32_t hi = blk.end_addr + 4u;
            /* translate_basic_block clones an out-of-block delay slot when the
             * control-flow instruction itself is the block's last word.  That
             * cloned instruction is compiled code too: include it in the exact
             * validity range so its bytes participate in the candidate CRC and
             * page-generation watch.  Otherwise a later write to the slot could
             * leave stale native semantics callable. */
            if (blk.exit_instr.has_delay_slot &&
                blk.exit_instr.type != ControlFlowType::None &&
                blk.exit_instr.address == blk.end_addr && hi <= UINT32_MAX - 4u) {
                hi += 4u;
            }
            if (hi > lo) iv.emplace_back(lo, hi);
        }
        if (iv.empty()) continue;

        // Merge overlapping/adjacent intervals. A gap > 0 between blocks is
        // non-code (e.g. a jump table) and is intentionally left out.
        std::sort(iv.begin(), iv.end());
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        for (const auto& r : iv) {
            if (!merged.empty() && r.first <= merged.back().second) {
                if (r.second > merged.back().second) merged.back().second = r.second;
            } else {
                merged.push_back(r);
            }
        }

        ss << fmt::format("F {:08X}\n", func.start_addr);
        for (const auto& r : merged)
            ss << fmt::format("R {:08X} {:X}\n", r.first, r.second - r.first);
    }
    return ss.str();
}

} // namespace PSXRecomp
