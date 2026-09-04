// full_function_emitter.cpp — Phase 2 full BIOS C emitter.
// See full_function_emitter.h for the design contract.

#include "full_function_emitter.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "fmt/format.h"
#include "mips_decoder.h"
#include "strict_translator.h"
#include "write_if_changed.h"
#include "../../runtime/include/psx_instr_cost.h"  /* single-source CPU cycle cost (shared with interp + game emitter) */

namespace PSXRecompV4 {

// Per-instruction cycle charging (mirrors code_generator.cpp). DEFAULT ON for
// the faithful-timing branch so the running cycle count is accurate mid-block —
// required for the mult/div completion-stall (mflo/mfhi wait for muldiv_ts_done)
// to absorb correctly. Set PSX_CODEGEN_CYCLE_PER_INSN=0 to force block-up-front.
static bool bios_cycle_per_insn() {
    const char* e = std::getenv("PSX_CODEGEN_CYCLE_PER_INSN");
    if (e != nullptr && e[0] != '\0') return e[0] != '0';
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* The active BIOS address model (profile-derived; bios_address_model.h) —
 * single source of truth for every relocation window below. No built-in
 * default: emitting without a model is a caller bug and aborts loudly. */
static const BiosAddressModel* g_addr_model = nullptr;

void FullFunctionEmitter::set_address_model(const BiosAddressModel* m) {
    g_addr_model = m;
}

/* The loaded BIOS profile (identity + runtime_exports) couriered into the
 * generated psx_bios_image block. Unlike the address model this is optional:
 * null just means every profile-keyed HLE anchor is emitted as 0. */
static const BiosConfig* g_bios_profile = nullptr;

void FullFunctionEmitter::set_bios_profile(const BiosConfig* cfg) {
    g_bios_profile = cfg;
}

/* Symbol namespace for this BIOS image, derived from the profile's out_stem
 * (e.g. "OpenBIOS_"). Every symbol this emitter DEFINES is prefixed with it so
 * two recompiled BIOSes can be linked into one binary and chosen at runtime.
 *
 * Scoped deliberately: only names defined here. Runtime-provided externs the
 * emitted code calls (psx_check_interrupts, psx_cyc_bb_defer_flush,
 * psx_unknown_dispatch, psx_dispatch_game_compiled, ...) keep their real names,
 * as does game codegen — code_generator.cpp emits the game's own func_ symbols
 * and must stay unprefixed, since one game image serves both BIOSes.
 *
 * Measured collision set without this: 16 func_ at coincident normalized
 * addresses plus 6 dispatch entry points. */
static std::string g_sym_prefix;

/* Name of a BIOS function body: <STEM>_func_1FC00000. */
static std::string fn_sym(uint32_t norm) {
    return fmt::format("{}func_{:08X}", g_sym_prefix, norm);
}

/* Name of a dispatch continuation inside a BIOS function. */
static std::string cont_sym(uint32_t parent_norm, uint32_t rom_addr) {
    return fmt::format("{}func_{:08X}_cont_{:08X}", g_sym_prefix, parent_norm,
                       rom_addr);
}

/* Name of a dispatch-layer entry point this emitter defines. Pass the bare
 * name, e.g. rt_sym("psx_dispatch"). */
static std::string rt_sym(const char* base) {
    return g_sym_prefix + base;
}

static const BiosAddressModel& addr_model() {
    if (!g_addr_model) {
        throw std::runtime_error(
            "FullFunctionEmitter: no BIOS address model set "
            "(FullFunctionEmitter::set_address_model must run before emit)");
    }
    return *g_addr_model;
}

uint32_t FullFunctionEmitter::normalize_address(uint32_t addr) {
    return addr_model().normalize(addr);
}

static uint32_t ram_alias_to_rom(uint32_t addr) {
    return addr_model().ram_alias_to_rom(addr);
}

/* Map a BIOS ROM PC to the address the PSX CPU actually executes from after the
 * BIOS copies kernel/shell code into RAM. Literal interrupt resume PCs must use
 * this runtime address; otherwise psx_check_interrupts rejects ROM-shell EPCs
 * and falls back to the legacy sentinel path. */
static uint32_t bios_runtime_pc(uint32_t rom_pc) {
    return addr_model().runtime_pc(rom_pc);
}

uint32_t FullFunctionEmitter::read_u32_le(const std::vector<uint8_t>& rom, uint32_t offset) {
    return  static_cast<uint32_t>(rom[offset + 0])
         | (static_cast<uint32_t>(rom[offset + 1]) << 8)
         | (static_cast<uint32_t>(rom[offset + 2]) << 16)
         | (static_cast<uint32_t>(rom[offset + 3]) << 24);
}

// Map terminator_kind to the goto condition expression.
// Returns empty string if the kind is not a conditional branch.
static std::string branch_condition(const char* kind, uint32_t branch_addr) {
    if (!kind) return "";
    std::string k = kind;
    if (k == "branch_beq")  return fmt::format("psx_brA_{:08X} == psx_brB_{:08X}", branch_addr, branch_addr);
    if (k == "branch_bne")  return fmt::format("psx_brA_{:08X} != psx_brB_{:08X}", branch_addr, branch_addr);
    if (k == "branch_blez") return fmt::format("(int32_t)psx_brA_{:08X} <= 0", branch_addr);
    if (k == "branch_bgtz") return fmt::format("(int32_t)psx_brA_{:08X} > 0", branch_addr);
    if (k == "branch_bltz") return fmt::format("(int32_t)psx_brA_{:08X} < 0", branch_addr);
    if (k == "branch_bgez") return fmt::format("(int32_t)psx_brA_{:08X} >= 0", branch_addr);
    return "";
}

static bool is_branch_kind(const char* kind) {
    if (!kind) return false;
    std::string k = kind;
    return k.substr(0, 7) == "branch_";
}

// ---------------------------------------------------------------------------
// emit_function: emit one C function
// ---------------------------------------------------------------------------

bool FullFunctionEmitter::emit_function(
    std::string&                out,
    const DiscoveredFunction&   func,
    const FunctionDiscovery::SingleFunctionResult& sfr,
    const std::set<uint32_t>&   all_function_entries_norm,
    const std::vector<uint8_t>& rom,
    uint32_t                    base_addr,
    uint32_t                    /* rom_end */,
    std::vector<ContinuationLabel>& out_continuations,
    const std::set<uint32_t>&   injected_cross_targets,
    std::vector<ContinuationLabel>& out_cross_targets,
    std::string*                out_interpreter_reason)
{
    const uint32_t norm = func.normalized_addr;
    if (out_interpreter_reason) out_interpreter_reason->clear();

    // RECURSION_BUG.md §25 — continuation-passing call/return (the universal fix
    // for the idle-freeze host-stack leak). When PSX_CPS is set at gen time,
    // guest calls (jal/jalr) are emitted as TAIL-TRANSFERS (set $ra, cpu->pc =
    // target, return) with the return address registered as a dispatchable
    // continuation, instead of a nested psx_dispatch(). The trampoline drives
    // the whole call/return FLAT via the guest's own $ra/$sp — like hardware —
    // so the host stack can never mirror unbounded guest re-entrancy. Perf cost:
    // every call becomes a dispatch (accepted: "right before fast").
    // CPS is the DEFAULT (RECURSION_BUG.md §25 — the validated leak fix). Opt out
    // (legacy nested-dispatch codegen) with PSX_CPS=0.
    static const bool cps = []() {
        const char* e = std::getenv("PSX_CPS");
        return e == nullptr || e[0] != '0';
    }();
    auto emit_irq_check = [](uint32_t resume_pc, const std::string& indent = "    ") {
        return std::string("#ifdef PSX_ENABLE_BLOCK_CYCLES\n") + indent +
               "psx_cyc_bb_defer_flush();\n#endif\n" + indent +
               fmt::format("psx_check_interrupts_at(cpu, 0x{:08X}u);\n",
                           bios_runtime_pc(resume_pc));
    };
    auto emit_irq_check_expr = [](const std::string& resume_pc_expr,
                                  const std::string& indent = "    ") {
        return std::string("#ifdef PSX_ENABLE_BLOCK_CYCLES\n") + indent +
               "psx_cyc_bb_defer_flush();\n#endif\n" + indent +
               fmt::format("psx_check_interrupts_at(cpu, {});\n", resume_pc_expr);
    };
    auto emit_cosim_instr = [](uint32_t pc, const std::string& indent = "    ") {
        return "#ifdef PSX_COSIM\n" + indent +
               fmt::format("cosim_instr(0x{:08X}u);\n", bios_runtime_pc(pc)) +
               "#endif\n";
    };

    // Build sorted instruction list and a set for O(1) membership test.
    // sfr.instructions is already sorted by address.
    std::map<uint32_t, uint32_t> addr_to_raw;
    for (const auto& p : sfr.instructions) {
        addr_to_raw[p.first] = p.second;
    }

    // BIOS RestoreState (A0:0x14) restores all callee-saved registers from
    // a save buffer and returns to the saved $ra.  In the native build,
    // `jr $ra` must set cpu->pc and call psx_restore_state_escape() so the
    // dispatch loop routes to the restored address instead of returning to
    // the C caller.
    //
    // Detected by: any instruction in the function is exactly `lw $ra, 0($a0)`
    // (encoding 0x8C9F0000).  This covers both:
    //   - Standalone RestoreState (func_1FC0227C): first insn is lw $ra,0($a0)
    //   - Kernel inline RestoreState in the exception handler (func_00000C80):
    //     lw $ra, 0($a0) appears at BFC10964 deep inside the chain walker.
    //
    // We match the exact encoding (offset 0 only) to avoid false positives
    // from functions that load $ra from $a0 at other offsets (e.g. GPU code
    // at BFC2A180 loads $ra from offset 16 of a structure, but restores the
    // real $ra from $sp before returning).
    bool ra_loaded_from_non_sp = false;
    for (const auto& [addr, raw] : addr_to_raw) {
        if (raw == 0x8C9F0000u) {
            // lw $ra, 0($a0) — RestoreState pattern
            ra_loaded_from_non_sp = true;
            break;
        }
    }

    // Build the set of basic block leaders from the function metadata.
    std::set<uint32_t> block_leaders(func.block_leaders.begin(), func.block_leaders.end());
    // The function entry is always a leader.
    block_leaders.insert(func.entry_addr);

    // Collect continuation labels for this function (populated during emit).
    std::vector<ContinuationLabel> local_continuations;

    // Inject cross-function targets BEFORE body emission. These are addresses
    // INSIDE this function that other functions branch/jump to. Without these:
    //   - block_leaders doesn't have them → no `label_<addr>:` is emitted
    //   - local_continuations doesn't have them → no `case 0x<addr>u: goto`
    // → dispatch table won't route mid-function tail-calls (root cause of
    //   card-read silent halt).
    // Both must be added together to keep label use/definition consistent.
    for (uint32_t cross : injected_cross_targets) {
        if (!addr_to_raw.count(cross)) continue;
        uint32_t cross_norm = normalize_address(cross);
        if (all_function_entries_norm.count(cross_norm)) continue;
        block_leaders.insert(cross);
        local_continuations.push_back({cross, cross_norm, norm});
    }

    // FAITHFUL_TIMING_PLAN P5 / game code_generator.cpp parity: every basic-block
    // leader must be a dispatchable CPS continuation. psx_check_interrupts_at
    // publishes ANY leader as an async-RFE resume PC; without a dispatch key the
    // trampoline falls into dirty_ram_interp. MotK measured ~1300 dirty blocks/frame
    // dominated by kernel poll-loop leaders 0x45FC/0x4614 inside func_00004498 —
    // both had labels/gotos but no dispatch entries (only jal+8 conts were registered).
    // Entry block is already a function dispatch key; skip it. Dedup later.
    for (uint32_t leader : block_leaders) {
        if (!addr_to_raw.count(leader)) continue;
        uint32_t leader_norm = normalize_address(leader);
        if (all_function_entries_norm.count(leader_norm)) continue;
        local_continuations.push_back({leader, leader_norm, norm});
    }

    // Helper: find which function (in all_function_entries_norm) contains a
    // given ROM target address.  Returns the largest function-entry norm that
    // is <= target_norm.  Returns 0 if no containing function found.
    auto find_containing_function = [&](uint32_t target_rom) -> uint32_t {
        uint32_t target_norm = normalize_address(target_rom);
        uint32_t best = 0;
        for (uint32_t fn : all_function_entries_norm) {
            if (fn <= target_norm && fn > best) best = fn;
        }
        return best;
    };

    // Helper: register a cross-function tail-call target as a continuation
    // in the function that contains it.  Without this, dispatching to a
    // mid-function address misses the dispatch table and silently halts —
    // root cause of card-read failure where state-11 chain handler's branch
    // target 0xBFC15548 (mid func_00005A44) wasn't dispatchable.
    auto register_cross_function_target = [&](uint32_t target_rom) {
        uint32_t target_norm = normalize_address(target_rom);
        // If target IS already a function entry, no continuation needed.
        if (all_function_entries_norm.count(target_norm)) return;
        // If target is inside the CURRENT function, no continuation needed
        // (handled by goto labels).
        if (addr_to_raw.count(target_rom)) return;
        // Find containing function.
        uint32_t parent_norm = find_containing_function(target_rom);
        if (parent_norm == 0) return;  // No containing function — nothing to register
        // Push to OUT_CROSS_TARGETS (NOT local_continuations). The top-level
        // emit pass aggregates these and re-injects them as the containing
        // function's continuations in PASS 2.
        out_cross_targets.push_back({target_rom, target_norm, parent_norm});
    };

    // Emit function body to a temporary buffer so we can prepend the
    // continuation entry-switch after we know which labels exist.
    std::string branch_decls;
    std::string body;

    // --- Emit instructions in address order ---
    // Delay slots are emitted at their natural address (NOT inline with
    // their terminator). Branch resolution happens AFTER the delay slot.
    // This correctly handles the case where another branch targets a
    // delay slot address.
    //
    // To handle the variable scoping issue for branch snapshots when a
    // delay slot is also a branch target: we wrap each terminator+delay
    // pair in a block scope so the snapshot variables are defined only
    // in that block, and use a flag for the branch resolution.

    // Track pending branch resolutions: after a terminator, the delay
    // slot at addr+4 must execute, then the branch resolves.
    struct PendingBranch {
        std::string kind;           // terminator kind
        uint32_t    terminator_addr;
        uint32_t    target;         // branch/jump target (0 if indirect)
        uint32_t    raw;            // raw instruction word of terminator
        bool        has_pre_delay;
    };
    std::map<uint32_t, PendingBranch> pending_at; // keyed by delay slot addr

    // For relocated code (ROM copied into RAM at boot), J/JAL targets depend
    // on the upper 4 bits of the PC. The code runs at RAM addresses, not ROM
    // addresses, so we must fix J/JAL targets in the branch resolution.
    auto relocate_j_target = [](uint32_t rom_addr, uint32_t target) -> uint32_t {
        return addr_model().relocate_j_target(rom_addr, target);
    };

    /* Relocate a return address ($ra) from ROM PC to the runtime address.
     * On real hardware, $ra is set by the CPU using the runtime PC.
     * Shell code runs at RAM 0x80030000+, kernel Part 2 at 0x500+.
     * Without this, handler/callback tables get ROM addresses instead of
     * RAM addresses, breaking pointer comparisons in the BIOS. */
    auto relocate_ra = [](uint32_t rom_ra) -> uint32_t {
        return bios_runtime_pc(rom_ra);
    };

    // First pass: identify terminators and their delay slots.
    for (const auto& [addr, raw] : addr_to_raw) {
        PSXRecomp::DecodedInstruction d = PSXRecomp::MipsDecoder::decode(raw, addr);
        TranslateResult tr = StrictTranslator::translate(d);
        if (tr.is_terminator) {
            PendingBranch pb;
            pb.kind = tr.terminator_kind ? tr.terminator_kind : "";
            pb.terminator_addr = addr;
            pb.target = tr.terminator_target;
            pb.raw = raw;
            pb.has_pre_delay = !tr.pre_delay_code.empty();
            // Fix J/JAL targets for relocated code.
            std::string k = pb.kind;
            if (k == "j" || k == "jal") {
                pb.target = relocate_j_target(addr, pb.target);
            }
            uint32_t ds_addr = addr + 4;
            pending_at[ds_addr] = pb;
        }
    }

    // Detect delay slots that fall outside this function's address range.
    // These will never be visited by the emit loop, so their PendingBranch
    // would silently vanish.  Collect them for immediate resolution.
    std::set<uint32_t> orphaned_delay_slots;
    for (const auto& [ds_addr, pb] : pending_at) {
        if (addr_to_raw.count(ds_addr) == 0) {
            orphaned_delay_slots.insert(pb.terminator_addr);
        }
    }

    // Phase 1.0e-d: precompute per-block cycle estimates. For each block
    // leader L, count instructions from L up to (but not including) the
    // next leader address (or end of function). Conservative 1 cycle per
    // instruction; precise R3000A timing (load delays, multiply/divide,
    // memory stalls) is a future refinement. Includes terminator and
    // delay slot in the block before the next leader. */
    std::map<uint32_t, uint32_t> block_cycles;
    for (auto it = addr_to_raw.begin(); it != addr_to_raw.end(); ++it) {
        uint32_t leader = it->first;
        if (!block_leaders.count(leader)) continue;
        uint32_t count = 0;
        auto walker = it;
        while (walker != addr_to_raw.end()) {
            /* Single-source per-instruction cost (psx_instr_cost.h), summed over
             * the block — identical for all backends. Identity (1/insn) today, so
             * this equals the old instruction count; Stage-2 real costs land in
             * that one function and every backend updates together. */
            count += psx_instr_base_cycles(walker->second);
            ++walker;
            if (walker == addr_to_raw.end()) break;
            if (block_leaders.count(walker->first)) break;
        }
        block_cycles[leader] = count;
    }

    /* Emit branch-predicate variable declarations at function entry,
     * each initialized to 0.  Subsequent terminator-emit assigns instead of
     * declares, so a `goto label_X` that lands on the delay slot of a
     * preceding beq does NOT execute stale delay-slot branch resolution.
     * Some BIOS blocks use the same address as both a branch delay slot and
     * a normal branch target; the DELETE modal hit BFC21860 that way and
     * reused the prior BFC2185C predicate, causing the BFC21800 loop. */
    /* jr/jalr target-clobber hazard (hardware latches the jump target BEFORE
     * the delay slot executes): when the delay-slot instruction WRITES the
     * jump register (hand-written asm reuses it — OpenBIOS fastMemset does
     * `jr $t1` with delay `andi $t1,$a2,0xff`), the resolution code below
     * would read the clobbered value. Snapshot the register at the
     * terminator into psx_jrt_<addr> and resolve from the snapshot. Gated on
     * the actual hazard so hazard-free functions emit byte-identical code.
     * (Conditional branches already snapshot operands via psx_brA_/psx_brB_;
     * the dirty-RAM interpreter captures the target pre-delay-slot too.) */
    auto insn_writes_gpr = [](uint32_t w, uint32_t r) -> bool {
        if (r == 0 || w == 0) return false;
        uint32_t op = w >> 26, rt = (w >> 16) & 31, rd = (w >> 11) & 31, fn = w & 63;
        switch (op) {
        case 0x00:
            if (fn == 0x08 || fn == 0x0C || fn == 0x0D || fn == 0x0F) return false;
            if (fn == 0x11 || fn == 0x13 || fn == 0x18 || fn == 0x19 ||
                fn == 0x1A || fn == 0x1B) return false;   /* mthi/mtlo/mult/div */
            return rd == r;                                /* incl. jalr rd */
        case 0x01: return (rt == 0x10 || rt == 0x11) && r == 31; /* bltzal/bgezal */
        case 0x02: return false;                           /* j */
        case 0x03: return r == 31;                         /* jal */
        case 0x04: case 0x05: case 0x06: case 0x07: return false;
        case 0x10: return ((w >> 21) & 31) == 0 && rt == r;      /* mfc0 */
        case 0x12: { uint32_t f = (w >> 21) & 31;
                     return (f == 0x00 || f == 0x02) && rt == r; } /* mfc2/cfc2 */
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x2E: case 0x32: case 0x3A: return false;     /* stores, lwc2, swc2 */
        default:   return rt == r;                         /* I-type ALU + loads */
        }
    };
    /* MIPS-I load-delay-slot modeling (dependent pairs only). On a real
     * R3000A the instruction after a load executes in the load's delay
     * shadow: if it READS the load's destination register it sees the OLD
     * value (the load writes back one instruction later). Compilers and
     * assemblers schedule around this, so the recompiler historically
     * skipped it — but OpenBIOS's cardfasttrack.s exploits it DELIBERATELY
     * ("move $at, $k0  / * gotta break those bad emulators * /" right after
     * `lw $k0, ...($k0)`): without modeling, the fasttrack's buffer-pointer
     * writeback lands at ptr+lo16 (a kernel-heap stomp at OpenBIOS 0x9960)
     * instead of the pointer cell, every streamed card byte hits buffer[0],
     * and the game reads a zeroed sector -> "Memory Card is not formatted".
     *
     * Model: at a simple load (LB/LBU/LH/LHU/LW, rt!=0) whose IMMEDIATE
     * successor in the same emission (not across a label, not a delay slot)
     * reads rt, emit the load into the function-scope temp psx_ldd_<addr>
     * (memory read stays at the load's position — MMIO order and cycle
     * interlock unchanged), emit the successor untouched (its reads of
     * gpr[rt] naturally see the old value), then flush
     * `cpu->gpr[rt] = psx_ldd_<addr>` after the successor's register reads.
     * If the successor architecturally writes rt (its writeback is later in
     * program order, so it wins on hardware), the flush is dropped — unless
     * the successor is itself a deferred load to rt (chain), whose own
     * writeback is deferred further and must not be pre-clobbered.
     * Unmodeled shapes (dependent pair split by a label, load in a branch
     * delay slot with a dependent successor, LWL/LWR pairs) are logged so
     * they can't fail silently. Non-dependent loads emit byte-identically
     * to before. Beetle/mednafen models the same semantics (LDWhich), so
     * cosim stays aligned. */
    auto insn_reads_gpr = [](uint32_t w, uint32_t r) -> bool {
        if (r == 0 || w == 0) return false;   /* $zero / nop */
        uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, fn = w & 63;
        switch (op) {
        case 0x00:
            switch (fn) {
            case 0x00: case 0x02: case 0x03: return rt == r;             /* sll/srl/sra (shamt) */
            case 0x04: case 0x06: case 0x07: return rs == r || rt == r;  /* sllv/srlv/srav */
            case 0x08: case 0x09: return rs == r;                        /* jr/jalr */
            case 0x0C: case 0x0D: return false;                          /* syscall/break */
            case 0x10: case 0x12: return false;                          /* mfhi/mflo */
            case 0x11: case 0x13: return rs == r;                        /* mthi/mtlo */
            default:   return rs == r || rt == r;                        /* muldiv + 3-op ALU */
            }
        case 0x01: return rs == r;                                       /* regimm branches */
        case 0x02: case 0x03: return false;                              /* j/jal */
        case 0x04: case 0x05: return rs == r || rt == r;                 /* beq/bne */
        case 0x06: case 0x07: return rs == r;                            /* blez/bgtz */
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: case 0x0E: return rs == r;                 /* addi..xori */
        case 0x0F: return false;                                         /* lui */
        case 0x10: return ((w >> 21) & 31) == 0x04 && rt == r;           /* mtc0 */
        case 0x12: { uint32_t f = (w >> 21) & 31;
                     return (f == 0x04 || f == 0x06) && rt == r; }       /* mtc2/ctc2 */
        case 0x22: case 0x26: return rs == r || rt == r;                 /* lwl/lwr: base + merge */
        case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
                   return rs == r;                                       /* simple loads: base */
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E:
                   return rs == r || rt == r;                            /* sb/sh/swl/sw/swr */
        case 0x32: case 0x3A: return rs == r;                            /* lwc2/swc2 base */
        default:   return rs == r || rt == r;                            /* conservative */
        }
    };
    auto simple_load_dest = [](uint32_t w) -> int {
        uint32_t op = w >> 26;
        if (op == 0x20 || op == 0x21 || op == 0x23 || op == 0x24 || op == 0x25) {
            uint32_t rt = (w >> 16) & 31;
            return rt ? static_cast<int>(rt) : -1;
        }
        return -1;
    };
    /* load addr -> {dest reg, flush writeback (vs discard)} */
    std::map<uint32_t, std::pair<int, bool>> ldd_sites;
    /* Load addrs whose dependent successor is an LWL/LWR merging into that
     * same rt. LWL/LWR read their destination late enough to receive the
     * forwarded result of an immediately preceding load, so hardware merges
     * into the value the load just fetched -- they are NOT subject to the
     * load delay. The merge base must therefore name psx_ldd_<addr>, and the
     * ordinary writeback is suppressed because the merge already consumed it.
     * Without this the load was deferred AND then discarded (the successor
     * writes rt), so the fetched word vanished and the merge combined the
     * stale pre-load register -- silently wrong code. Mirrors the CFG
     * emitter's set_lwlr_merge_forward path. */
    std::set<uint32_t> ldd_lwlr_forward;
    std::string unmodeled_load_delay;
    for (const auto& [la, lw_raw] : addr_to_raw) {
        int dest = simple_load_dest(lw_raw);
        uint32_t lop = lw_raw >> 26;
        bool is_lwlr = (lop == 0x22 || lop == 0x26);
        if (dest < 0 && !is_lwlr) continue;
        int dep_reg = is_lwlr ? static_cast<int>((lw_raw >> 16) & 31) : dest;
        if (dep_reg <= 0) continue;
        if (pending_at.count(la)) {
            /* Load in a branch delay slot: the shadow instruction is the
             * dynamic successor (target or fallthrough) — cross-block,
             * unmodeled. Log if either static successor depends. */
            const PendingBranch& pb = pending_at[la];
            bool dep = false;
            auto f = addr_to_raw.find(pb.terminator_addr + 8);
            if (f != addr_to_raw.end() && insn_reads_gpr(f->second, dep_reg)) dep = true;
            auto t = addr_to_raw.find(pb.target);
            if (pb.target && t != addr_to_raw.end() && insn_reads_gpr(t->second, dep_reg)) dep = true;
            if (dep) {
                unmodeled_load_delay = fmt::format(
                    "load 0x{:08X} in a branch delay slot has a dependent successor", la);
                if (out_interpreter_reason) {
                    std::fprintf(stderr,
                        "[load-delay] UNMODELED: load 0x%08X in delay slot with dependent successor (func 0x%08X)\n",
                        la, func.entry_addr);
                }
            }
            continue;
        }
        auto nx = addr_to_raw.find(la + 4);
        if (nx == addr_to_raw.end()) {
            /* Successor belongs to another function/fragment (code_ptr
             * confetti). Can't model across the boundary — but a dependent
             * pair here would fail SILENTLY, so peek the ROM and log it. */
            uint32_t base_phys = base_addr & 0x1FFFFFFFu;
            uint32_t nx_phys = (la + 4) & 0x1FFFFFFFu;
            if (nx_phys >= base_phys && nx_phys + 4 <= base_phys + rom.size()) {
                uint32_t nx_raw = read_u32_le(rom, nx_phys - base_phys);
                if (insn_reads_gpr(nx_raw, dep_reg)) {
                    unmodeled_load_delay = fmt::format(
                        "dependent load pair crosses a fragment boundary after 0x{:08X}", la);
                    if (out_interpreter_reason) {
                        std::fprintf(stderr,
                            "[load-delay] UNMODELED: dependent pair crosses fragment boundary after load 0x%08X rt=%d (func 0x%08X)\n",
                            la, dep_reg, func.entry_addr);
                    }
                }
            }
            continue;
        }
        if (!insn_reads_gpr(nx->second, dep_reg)) continue;
        if (is_lwlr) {
            /* LWL immediately followed by LWR to the same rt (or vice versa)
             * is the documented MIPS-I exception: the hardware forwards the
             * partial result to the pair's second half. Our immediate
             * writeback gives exactly that merge — correct, don't log. */
            uint32_t nop_ = nx->second >> 26, nrt = (nx->second >> 16) & 31;
            bool complementary = (nrt == (uint32_t)dep_reg) &&
                                 ((lop == 0x22 && nop_ == 0x26) || (lop == 0x26 && nop_ == 0x22));
            if (!complementary) {
                unmodeled_load_delay = fmt::format(
                    "LWL/LWR 0x{:08X} has a dependent non-complementary successor", la);
                if (out_interpreter_reason) {
                    std::fprintf(stderr,
                        "[load-delay] UNMODELED: LWL/LWR 0x%08X with dependent successor (func 0x%08X)\n",
                        la, func.entry_addr);
                }
            }
            continue;
        }
        if (block_leaders.count(la + 4)) {
            unmodeled_load_delay = fmt::format(
                "dependent load pair is split by a label at 0x{:08X}", la + 4);
            if (out_interpreter_reason) {
                std::fprintf(stderr,
                    "[load-delay] UNMODELED: dependent pair split by label at 0x%08X (func 0x%08X)\n",
                    la + 4, func.entry_addr);
            }
            continue;
        }
        {
            const uint32_t nop_ = nx->second >> 26;
            const uint32_t nrt_ = (nx->second >> 16) & 31u;
            if ((nop_ == 0x22u || nop_ == 0x26u) &&
                nrt_ == static_cast<uint32_t>(dep_reg)) {
                ldd_lwlr_forward.insert(la);
                ldd_sites[la] = {dest, false};
                continue;
            }
        }
        ldd_sites[la] = {dest, true};
    }
    if (!unmodeled_load_delay.empty()) {
        // dirty_ram_dispatch interprets live main-RAM bytes only. Relocated
        // kernel/shell functions satisfy that contract; a pure ROM function
        // does not. Never turn a correctness fallback into a later
        // unknown-dispatch abort: stop generation if there is no interpreter
        // backend capable of executing every instruction in this function.
        const bool ram_backed = std::all_of(
            addr_to_raw.begin(), addr_to_raw.end(),
            [](const auto& insn) {
                return (bios_runtime_pc(insn.first) & 0x1FFFFFFFu) <
                       (2u * 1024u * 1024u);
            });
        if (!ram_backed) {
            throw std::runtime_error(fmt::format(
                "cannot safely emit BIOS function 0x{:08X}: {}; "
                "the function is not RAM-backed, so interpreter fallback is unavailable",
                func.entry_addr, unmodeled_load_delay));
        }
        if (out_interpreter_reason) {
            *out_interpreter_reason = unmodeled_load_delay;
            std::fprintf(stderr,
                "[load-delay] FALLBACK: function 0x%08X excluded from native dispatch; using interpreter (%s)\n",
                func.entry_addr, unmodeled_load_delay.c_str());
        }
        return false;
    }

    // Shadow 'out' with a reference to 'body' so all existing out+= lines
    // write to the temporary buffer.  The real 'out' is assembled at the end.
    std::string& func_out = body;
    #define out func_out

    for (auto& [la, s] : ldd_sites) {
        if (ldd_lwlr_forward.count(la)) {
            /* The LWL/LWR merge consumes the temp; no writeback and no
               discard. Do NOT let the successor-writes-rt rule below turn
               this into a discard. */
            std::fprintf(stderr,
                "[load-delay] modeled pair: load 0x%08X rt=%d succ 0x%08X "
                "LWL/LWR-forward (func 0x%08X)\n",
                la, s.first, la + 4, func.entry_addr);
            continue;
        }
        uint32_t nw = addr_to_raw.at(la + 4);
        if (insn_writes_gpr(nw, static_cast<uint32_t>(s.first))) {
            auto nsite = ldd_sites.find(la + 4);
            bool chain = (nsite != ldd_sites.end() && nsite->second.first == s.first);
            s.second = chain;   /* successor's write wins unless it is itself deferred */
        }
        std::fprintf(stderr,
            "[load-delay] modeled pair: load 0x%08X rt=%d succ 0x%08X flush=%d (func 0x%08X)\n",
            la, s.first, la + 4, s.second ? 1 : 0, func.entry_addr);
    }
    auto jr_snapshot_needed = [&](uint32_t term_addr, uint32_t term_raw) -> bool {
        uint32_t rs = (term_raw >> 21) & 0x1F;
        /* jr/jalr in a load shadow (`lw $rs,..; jr $rs`): hardware reads the
         * PRE-load value. The psx_jrt_ latch (emitted before the deferred
         * writeback flush) captures exactly that — force the snapshot. */
        auto ls = ldd_sites.find(term_addr - 4);
        if (ls != ldd_sites.end() && static_cast<uint32_t>(ls->second.first) == rs) return true;
        auto ds = addr_to_raw.find(term_addr + 4);
        if (ds == addr_to_raw.end()) return false;  /* orphaned ds: resolved pre-delay */
        return insn_writes_gpr(ds->second, rs);
    };

    for (const auto& [ds_addr, pb] : pending_at) {
        branch_decls += fmt::format("    int psx_delay_{:08X} = 0;\n", pb.terminator_addr);
        if (is_branch_kind(pb.kind.c_str())) {
            branch_decls += fmt::format("    int psx_taken_{:08X} = 0;\n", pb.terminator_addr);
        }
        if ((pb.kind == "jr" || pb.kind == "jalr") &&
            jr_snapshot_needed(pb.terminator_addr, pb.raw)) {
            branch_decls += fmt::format("    uint32_t psx_jrt_{:08X} = 0;\n", pb.terminator_addr);
        }
    }
    for (const auto& [la, s] : ldd_sites) {
        branch_decls += fmt::format("    uint32_t psx_ldd_{:08X} = 0;  /* load-delay temp */\n", la);
    }

    auto should_probe_pc = [](uint32_t pc) -> bool {
        switch (pc) {
        case 0xBFC148DCu:
        case 0xBFC148F0u:
        case 0xBFC148F8u:
        case 0xBFC14900u:
        case 0xBFC14908u:
        case 0xBFC14934u:
        case 0xBFC14F80u:
        case 0xBFC15174u:
        case 0xBFC15178u:
        case 0xBFC15E80u:
            return true;
        default:
            return false;
        }
    };

    const bool per_insn_cycles = bios_cycle_per_insn();

    /* Flush a deferred load writeback after its dependent successor's
     * register reads (successor = instruction at succ_addr; the load sits at
     * succ_addr - 4). No-op when no site precedes succ_addr. */
    auto emit_ldd_flush = [&](uint32_t succ_addr) {
        auto it = ldd_sites.find(succ_addr - 4);
        if (it == ldd_sites.end()) return;
        if (ldd_lwlr_forward.count(it->first)) {
            out += fmt::format(
                "    /* psx_ldd_{:08X} forwarded into the LWL/LWR merge above */\n",
                it->first);
            return;
        }
        if (it->second.second) {
            out += fmt::format("    cpu->gpr[{}] = psx_ldd_{:08X};  /* load-delay writeback */\n",
                               it->second.first, it->first);
        } else {
            out += fmt::format("    (void)psx_ldd_{:08X};  /* load-delay: successor overwrites rt */\n",
                               it->first);
        }
    };

    // Per-instruction R3000A load-delay interlock (cycle_per_insn mode): §1 base +
    // GPR_DEPRES + DO_LDS for one instruction, emitted BEFORE its body so §1 precedes
    // any muldiv/GTE deadline stall (Beetle order). CPU loads (op 0x20-0x26) are
    // SKIPPED — psx_cyc_load_* runs their full interlock inside the body. The dep/res
    // mask is a gen-time literal. Replaces the old flat per-instruction +1.
    auto emit_insn_interlock = [&](uint32_t w) {
        if (!per_insn_cycles) return;
        uint32_t op = w >> 26;
        if (op >= 0x20u && op <= 0x26u) return;   // CPU load: interlock inside psx_cyc_load_*
        out += fmt::format("#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_cyc_step(cpu, 0x{:X}u);\n#endif\n",
                           psx_cyc_dep_res_mask(w));
    };

    // I-cache FETCH cost (faithful R3000A), emitted BEFORE the per-instruction
    // interlock/load — like Beetle ReadInstruction precedes the base, so a fetch MISS
    // clears any pending load give-back before the next load arms one. Only emitted at
    // cache-line LEADERS: a block leader (any branch/dispatch entry — a possibly-cold
    // cache entry; cross-function targets are inserted into block_leaders above) OR a
    // 16-byte-line start (addr&0xC==0, a sequential line crossing). Intra-line followers
    // reached by fall-through are guaranteed hits (the leader refilled the line to its
    // end) → no call (+0). `rom_addr` is the ROM/compile-time address; relocate_ra maps
    // it to the RUNTIME guest PC the CPU actually fetches from (BIOS main stays in-place
    // KSEG1 0xBFC..; relocated kernel Part 2 → 0x500+, shell → 0x80030000+), so the
    // shared I-cache evolves identically to the dirty-RAM interp (cpu->pc) and Beetle —
    // and the KSEG1 uncached test (>=0xA0000000) sees the true virtual address. The
    // relocation preserves bits[3:0], so the line-leader test is space-independent.
    auto emit_icache_fetch = [&](uint32_t rom_addr) {
        if (!per_insn_cycles) return;
        if (!(block_leaders.count(rom_addr) || (rom_addr & 0xCu) == 0)) return;
        out += fmt::format("#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_icache_fetch(cpu, 0x{:08X}u);\n#endif\n",
                           relocate_ra(rom_addr));
    };

    for (auto it = addr_to_raw.begin(); it != addr_to_raw.end(); ++it) {
        uint32_t addr = it->first;
        uint32_t raw = it->second;

        // Emit label if this is a block leader.
        // Include an interrupt check so tight loops (backward branches)
        // can service vblank and other hardware interrupts.
        if (block_leaders.count(addr)) {
            out += fmt::format("label_{:08X}:\n", addr);
            // Per-block-leader cycle observe (cyc_watch ruler). Sampled BEFORE
            // this block's cycle advance, so it reports cumulative cycles for
            // all PRIOR blocks — matching Beetle's before-instruction sample
            // and the cycle_compare.py anchor semantics. Debug-only: prod
            // (PSX_NO_DEBUG_TOOLS) emits nothing → zero overhead.
            out += "#ifndef PSX_NO_DEBUG_TOOLS\n";
            // Use the NORMALIZED (runtime) phys so anchors match the address
            // space the entry hook + cyc_watch use — relocated kernel funcs
            // (ROM 0x1FC10000+) run at RAM 0x500+, so the raw ROM addr would
            // never match a RAM anchor.
            out += fmt::format("    debug_server_cyc_observe(0x{:08X}u);\n",
                               normalize_address(addr));
            out += "#endif\n";
            // First-divergence co-sim oracle (COSIM_ORACLE.md): lean block-leader hook.
            out += "#ifdef PSX_COSIM\n";
            out += fmt::format("    cosim_block(0x{:08X}u);\n", normalize_address(addr));
            out += "#endif\n";
            // Phase 1.0e-d: advance guest cycles for this block. Macro-
            // gated; when off, generated code matches pre-1.0e-d output.
            // In per-instruction mode the charge is emitted per instruction
            // below (so muldiv/GTE stalls absorb correctly), NOT block-up-front.
            if (!per_insn_cycles) {
                uint32_t bcyc = 0;
                auto bcit = block_cycles.find(addr);
                if (bcit != block_cycles.end()) bcyc = bcit->second;
                if (bcyc > 0) {
                    out += "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
                    out += fmt::format("    psx_advance_cycles({}u);\n", bcyc);
                    out += "#endif\n";
                }
            }
            if (should_probe_pc(addr)) {
                out += fmt::format("    debug_server_log_probe(0x{:08X}u, cpu);\n", addr);
            }
        }

        // Per-instruction cycle charge (faithful-timing mode). Emitted for EVERY
        // in-function instruction (terminators, non-terminators, in-block delay
        // slots are all separate addr_to_raw entries) in execution order, so the
        // running cycle count is exact at MULT/DIV/MFLO/MFHI (and later GTE) for
        // the completion-stall to absorb correctly. Orphaned (out-of-function)
        // delay slots are inlined elsewhere and charged at those sites.
        emit_icache_fetch(addr);
        emit_insn_interlock(raw);

        // Decode and translate.
        PSXRecomp::DecodedInstruction d = PSXRecomp::MipsDecoder::decode(raw, addr);
        TranslateResult tr = StrictTranslator::translate(d);

        if (!tr.supported) {
            out += fmt::format("    /* UNSUPPORTED 0x{:08X}: {:08X} {} */\n",
                               addr, raw, tr.fail_reason);
            return false;
        }

        if (tr.is_terminator) {
            // Emit the terminator: comment + pre_delay_code (for branches).
            // The actual control transfer happens after the delay slot.
            out += fmt::format("    /* 0x{:08X}: {:08X}  {} */\n", addr, raw, tr.comment);

            const std::string kind = tr.terminator_kind ? tr.terminator_kind : "";
            if (kind == "jal") {
                out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;  /* jal link before delay slot */\n",
                                   relocate_ra(addr + 8));
            } else if (kind == "jalr") {
                uint8_t rd = (raw >> 11) & 0x1F;
                if (rd != 0) {
                    out += fmt::format("    cpu->gpr[{}] = 0x{:08X}u;  /* jalr link before delay slot */\n",
                                       static_cast<int>(rd), relocate_ra(addr + 8));
                }
            }

            if (is_branch_kind(tr.terminator_kind)) {
                // Emit pre-delay snapshot using a unique flag.
                if (!tr.pre_delay_code.empty()) {
                    out += fmt::format("    {}\n", tr.pre_delay_code);
                }
                std::string cond = branch_condition(tr.terminator_kind, addr);
                /* Assignment, not declaration — variable is declared at
                 * function entry initialized to 0 (see fix for chained
                 * branch + delay-slot label-placement bug). */
                out += fmt::format("    psx_taken_{:08X} = ({});\n", addr, cond);
            } else if (kind == "rfe") {
                // RFE: emit cop0 stack pop immediately (no delay slot).
                //
                // HOWEVER: RFE is commonly used as the delay slot of
                // `jr $k0` (exception return).  In MIPS the sequence is:
                //     jr $k0          <- terminator, pending at addr
                //     rfe             <- delay slot AND terminator
                // The jr records a PendingBranch at addr (this address).
                // If we just emit the RFE and `continue`, the jr's
                // `cpu->pc = cpu->gpr[26]` is never emitted and the
                // exception return address is lost.
                //
                // Fix: if this RFE address has a pending jr, emit the
                // RFE SR manipulation WITHOUT the return, then emit the
                // JR's pc-set + return instead.
                if (pending_at.count(addr)) {
                    const PendingBranch& pb = pending_at[addr];
                    if (pb.kind == "jr") {
                        // Emit RFE SR pop without return.
                        out += "    { uint32_t sr = cpu->cop0[12]; "
                               "cpu->cop0[12] = (sr & 0xFFFFFFC0u) | ((sr >> 2) & 0x0Fu); } /* rfe */\n";
                        // Fix B: arm the host exception-return escape (the jr below sets
                        // cpu->pc = real EPC; the trampoline's psx_rfe_escape_check then
                        // unwinds to psx_check_interrupts iff we're in the synchronous
                        // handler — a fiber/thread resume just dispatches the real EPC).
                        out += "    psx_rfe_mark_escape();\n";
                        // Emit JR resolution.
                        uint8_t rs = (pb.raw >> 21) & 0x1F;
                        if (rs == 31) {
                            if (ra_loaded_from_non_sp)
                                out += emit_irq_check_expr("cpu->gpr[31]") +
                                       "    cpu->pc = cpu->gpr[31]; psx_restore_state_escape(); return;  /* longjmp-return */\n";
                            else if (cps)
                                out += emit_irq_check_expr("cpu->gpr[31]") +
                                       "    cpu->pc = cpu->gpr[31]; return;  /* CPS: publish $ra */\n";
                            else
                                out += emit_irq_check_expr("cpu->gpr[31]") +
                                       "    return;\n";
                        } else {
                            out += emit_irq_check_expr(fmt::format("cpu->gpr[{}]", static_cast<int>(rs)));
                            out += fmt::format("    cpu->pc = cpu->gpr[{}]; return;\n",
                                               static_cast<int>(rs));
                        }
                    } else {
                        // Pending non-JR terminator (unexpected but safe).
                        out += fmt::format("    {}\n", tr.c_code);
                    }
                } else {
                    // Standalone RFE (no pending JR): emit as-is with return.
                    out += fmt::format("    {}\n", tr.c_code);
                }
            }
            if (kind != "rfe" && !orphaned_delay_slots.count(addr)) {
                out += fmt::format("    psx_delay_{:08X} = 1;\n", addr);
                /* Latch the jump target BEFORE the delay slot can clobber it
                 * (hardware semantics; see psx_jrt_ decl comment). */
                if ((kind == "jr" || kind == "jalr") && jr_snapshot_needed(addr, raw)) {
                    out += fmt::format("    psx_jrt_{:08X} = cpu->gpr[{}];\n",
                                       addr, (raw >> 21) & 0x1F);
                }
            }
            /* Deferred load writeback for a `load; branch/jump-reading-rt`
             * pair: the condition / psx_jrt_ latch above read the OLD value
             * (hardware: shadow read); flush before the delay slot runs. */
            emit_ldd_flush(addr);
            // For J/JAL/JALR/JR: normally nothing emitted at terminator
            // address — resolution happens after delay slot.  But if the
            // delay slot falls outside this function, resolve NOW (the
            // delay-slot side effect is in the adjacent function and will
            // execute when dispatch routes there).
            //
            // For BRANCHES with orphaned delay slots: also inline the
            // delay slot instruction here (read raw from rom) so its side
            // effect isn't lost on the branch-taken path.  Without this,
            // a function that ends on a `beq ..., target` whose delay slot
            // is the first instruction of the next function would fall off
            // its own end with cpu->pc unset, silently halting the chain
            // dispatcher (root cause of card-read truncation at byte 11).
            if (orphaned_delay_slots.count(addr)) {
                if (kind == "jr") {
                    uint8_t rs = (raw >> 21) & 0x1F;
                    /* Latch the jump target BEFORE the orphaned delay slot
                     * runs — hardware reads rs pre-slot (the in-block
                     * psx_jrt_ contract). */
                    out += fmt::format("    {{ uint32_t _t = cpu->gpr[{}];\n",
                                       static_cast<int>(rs));
                    /* Inline the orphaned delay slot from rom. A jr NEVER
                     * falls through to ds_addr, so — unlike a branch's
                     * not-taken path — no adjacent fragment can make up for
                     * a skipped slot: without this its side effect is simply
                     * LOST. Observed: OpenBIOS mcWriteHandler's case-return
                     * `jr $ra / addiu $sp,$sp,0x18` dropped the sp pop; the
                     * caller's epilogue then unwound a shifted frame,
                     * returned through a stale saved-ra slot with the
                     * callee's callee-saved registers live, and the
                     * exception handler's IntRP chain walk marched a garbage
                     * cursor into a wild verifier (FAIL-FAST 0x54000000 at
                     * the first memcard test write under OpenBIOS). */
                    {
                        uint32_t ds_addr = addr + 4;
                        uint32_t base_phys = base_addr & 0x1FFFFFFFu;
                        uint32_t ds_phys = ds_addr & 0x1FFFFFFFu;
                        if (ds_phys >= base_phys && ds_phys + 4 <= base_phys + rom.size()) {
                            uint32_t ds_offset = ds_phys - base_phys;
                            uint32_t ds_raw = read_u32_le(rom, ds_offset);
                            auto ds_d = PSXRecomp::MipsDecoder::decode(ds_raw, ds_addr);
                            auto ds_tr = StrictTranslator::translate(ds_d);
                            if (ds_tr.supported && !ds_tr.is_terminator) {
                                out += fmt::format("    /* DELAY (orphaned) 0x{:08X}: {:08X}  {} */\n",
                                                   ds_addr, ds_raw, ds_tr.comment);
                                emit_icache_fetch(ds_addr);
                                emit_insn_interlock(ds_raw);
                                out += fmt::format("    {}\n", ds_tr.c_code);
                            } else if (ds_raw != 0u) {
                                /* jr never falls through, so a slot we cannot
                                 * inline is a LOST side effect (the bug class
                                 * this path exists to fix) — never silent. */
                                std::fprintf(stderr,
                                    "[delay-slot] LOST: orphaned jr slot 0x%08X "
                                    "(%08X) not inlinable (unsupported/terminator)\n",
                                    ds_addr, ds_raw);
                            }
                        }
                    }
                    if (rs == 31) {
                        if (ra_loaded_from_non_sp)
                            out += emit_irq_check_expr("_t") +
                                   "    cpu->pc = _t; psx_restore_state_escape(); return; }  /* longjmp-return */\n";
                        else if (cps)
                            out += emit_irq_check_expr("_t") +
                                   "    cpu->pc = _t; return; }  /* CPS: publish $ra */\n";
                        else
                            out += emit_irq_check_expr("_t") +
                                   "    return; }\n";
                    } else {
                        out += emit_irq_check_expr("_t");
                        out += "    cpu->pc = _t; return; }\n";
                    }
                } else if (is_branch_kind(kind.c_str())) {
                    // Inline the orphaned delay slot from rom.
                    uint32_t ds_addr = addr + 4;
                    uint32_t base_phys = base_addr & 0x1FFFFFFFu;
                    uint32_t ds_phys = ds_addr & 0x1FFFFFFFu;
                    bool ds_inlined = false;
                    if (ds_phys >= base_phys && ds_phys + 4 <= base_phys + rom.size()) {
                        uint32_t ds_offset = ds_phys - base_phys;
                        uint32_t ds_raw = read_u32_le(rom, ds_offset);
                        auto ds_d = PSXRecomp::MipsDecoder::decode(ds_raw, ds_addr);
                        auto ds_tr = StrictTranslator::translate(ds_d);
                        if (ds_tr.supported && !ds_tr.is_terminator) {
                            out += fmt::format("    /* DELAY (orphaned) 0x{:08X}: {:08X}  {} */\n",
                                               ds_addr, ds_raw, ds_tr.comment);
                            // Orphaned delay slot is inlined here (not an addr_to_raw
                            // entry), so charge its interlock directly, BEFORE its body
                            // (block mode already counts it in the owning block_cycles).
                            emit_icache_fetch(ds_addr);
                            emit_insn_interlock(ds_raw);
                            out += fmt::format("    {}\n", ds_tr.c_code);
                            ds_inlined = true;
                        }
                    }
                    // Resolve branch.  Target was decoded as absolute virtual addr.
                    uint32_t target = tr.terminator_target;
                    out += fmt::format("    if (psx_taken_{:08X}) {{\n", addr);
                    if (addr_to_raw.count(target)) {
                        // In-function target: goto its label (branch targets are
                        // block leaders), same as the non-orphaned branch path.
                        // register_cross_function_target early-returns for
                        // in-function targets, so publishing here left the
                        // target with NO dispatch key (OpenBIOS exceptionHandler
                        // priority_loop: the published ROM alias 0xBFC208E8 has
                        // phys > 2MB, so even the dirty-RAM interp fallback was
                        // unreachable — FAIL-FAST).
                        out += emit_irq_check(target, "        ");
                        out += fmt::format("        goto label_{:08X};\n", target);
                    } else {
                        register_cross_function_target(target);
                        out += emit_irq_check(target, "        ");
                        // Publish the RUNTIME PC (relocated windows run at their
                        // RAM address; the ROM-space alias never executes and its
                        // phys can sit outside guest RAM entirely).
                        out += fmt::format("        cpu->pc = 0x{:08X}u; return;\n",
                                           relocate_ra(target));
                    }
                    out += "    }\n";
                    // Not-taken: the delay slot ALREADY RAN (inlined above), so
                    // resume at ds_addr+4 — hardware semantics (branch not-taken ->
                    // ds executes -> continue after it). The old "dispatch to
                    // ds_addr; the next function's prologue re-executes the delay
                    // slot — a duplicate side effect we accept" was only survivable
                    // for idempotent slots: OpenBIOS readPad's `sll $s0,$s0,1`
                    // (fifoBytes<<1, a read-modify-write) doubled TWICE, the pad
                    // halfword loop over-iterated, timed out on a phantom ack and
                    // padAbort'ed EVERY poll — dead pads under OpenBIOS.
                    // Only when the slot could NOT be inlined (unsupported /
                    // terminator) do we still dispatch ds_addr and let the next
                    // function run it.
                    if (ds_inlined) {
                        register_cross_function_target(ds_addr + 4);
                        out += emit_irq_check(ds_addr + 4);
                        out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n",
                                           relocate_ra(ds_addr + 4));
                    } else {
                        register_cross_function_target(ds_addr);
                        out += emit_irq_check(ds_addr);
                        out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n",
                                           relocate_ra(ds_addr));
                    }
                } else if (kind == "j") {
                    // Inline orphaned delay slot, then unconditional jump.
                    uint32_t ds_addr = addr + 4;
                    uint32_t base_phys = base_addr & 0x1FFFFFFFu;
                    uint32_t ds_phys = ds_addr & 0x1FFFFFFFu;
                    if (ds_phys >= base_phys && ds_phys + 4 <= base_phys + rom.size()) {
                        uint32_t ds_offset = ds_phys - base_phys;
                        uint32_t ds_raw = read_u32_le(rom, ds_offset);
                        auto ds_d = PSXRecomp::MipsDecoder::decode(ds_raw, ds_addr);
                        auto ds_tr = StrictTranslator::translate(ds_d);
                        if (ds_tr.supported && !ds_tr.is_terminator) {
                            out += fmt::format("    /* DELAY (orphaned) 0x{:08X}: {:08X}  {} */\n",
                                               ds_addr, ds_raw, ds_tr.comment);
                            // Orphaned delay slot is inlined here (not an addr_to_raw
                            // entry), so charge its interlock directly, BEFORE its body
                            // (block mode already counts it in the owning block_cycles).
                            emit_icache_fetch(ds_addr);
                            emit_insn_interlock(ds_raw);
                            out += fmt::format("    {}\n", ds_tr.c_code);
                        }
                    }
                    uint32_t target = relocate_j_target(addr, tr.terminator_target);
                    register_cross_function_target(target);
                    out += emit_irq_check(target);
                    out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", target);
                } else if (kind == "jal") {
                    // Set $ra (link happens BEFORE the delay slot on hardware —
                    // same order as the in-block path above), inline the
                    // orphaned delay slot, dispatch to target.
                    uint32_t return_addr = relocate_ra(addr + 8);
                    out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;  /* jal link before delay slot */\n",
                                       return_addr);
                    uint32_t ds_addr = addr + 4;
                    uint32_t base_phys = base_addr & 0x1FFFFFFFu;
                    uint32_t ds_phys = ds_addr & 0x1FFFFFFFu;
                    if (ds_phys >= base_phys && ds_phys + 4 <= base_phys + rom.size()) {
                        uint32_t ds_offset = ds_phys - base_phys;
                        uint32_t ds_raw = read_u32_le(rom, ds_offset);
                        auto ds_d = PSXRecomp::MipsDecoder::decode(ds_raw, ds_addr);
                        auto ds_tr = StrictTranslator::translate(ds_d);
                        if (ds_tr.supported && !ds_tr.is_terminator) {
                            out += fmt::format("    /* DELAY (orphaned) 0x{:08X}: {:08X}  {} */\n",
                                               ds_addr, ds_raw, ds_tr.comment);
                            // Orphaned delay slot is inlined here (not an addr_to_raw
                            // entry), so charge its interlock directly, BEFORE its body
                            // (block mode already counts it in the owning block_cycles).
                            emit_icache_fetch(ds_addr);
                            emit_insn_interlock(ds_raw);
                            out += fmt::format("    {}\n", ds_tr.c_code);
                        }
                    }
                    uint32_t target = relocate_j_target(addr, tr.terminator_target);
                    if (cps) {
                        if (addr_to_raw.count(addr + 8)) {
                            uint32_t cn = normalize_address(addr + 8);
                            if (!all_function_entries_norm.count(cn))
                                local_continuations.push_back({addr + 8, cn, norm});
                        } else {
                            register_cross_function_target(addr + 8);
                        }
                        out += emit_irq_check(target);
                        out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", target);
                    } else {
                        /* _csp captured after the slot (post-slot sp is the
                         * call contract, matching the interp's site_sp). */
                        out += "    { uint32_t _csp = cpu->gpr[29];\n";
                        out += emit_irq_check(target);
                        out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);\n", target);
                        out += fmt::format("    if (psx_call_contract(cpu, 0x{:08X}u, _csp)) return; }}\n",
                                           return_addr);
                        out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", return_addr);
                    }
                } else if (kind == "jalr") {
                    uint8_t rs = (raw >> 21) & 0x1F;
                    uint8_t rd = (raw >> 11) & 0x1F;
                    /* Latch the jump target BEFORE the orphaned delay slot
                     * runs — hardware reads rs pre-slot (the in-block
                     * psx_jrt_ contract; same class as the orphaned-jr fix
                     * above). The old order inlined the slot first, so a
                     * slot that writes rs redirected the call. */
                    out += fmt::format("    {{ uint32_t _t = cpu->gpr[{}];\n",
                                       static_cast<int>(rs));
                    uint32_t return_addr = relocate_ra(addr + 8);
                    if (rd != 0) {
                        /* Link BEFORE the delay slot (hardware order, same as
                         * the in-block "jalr link before delay slot"). */
                        out += fmt::format("    cpu->gpr[{}] = 0x{:08X}u;  /* jalr link before delay slot */\n",
                                           static_cast<int>(rd), return_addr);
                    }
                    uint32_t ds_addr = addr + 4;
                    uint32_t base_phys = base_addr & 0x1FFFFFFFu;
                    uint32_t ds_phys = ds_addr & 0x1FFFFFFFu;
                    if (ds_phys >= base_phys && ds_phys + 4 <= base_phys + rom.size()) {
                        uint32_t ds_offset = ds_phys - base_phys;
                        uint32_t ds_raw = read_u32_le(rom, ds_offset);
                        auto ds_d = PSXRecomp::MipsDecoder::decode(ds_raw, ds_addr);
                        auto ds_tr = StrictTranslator::translate(ds_d);
                        if (ds_tr.supported && !ds_tr.is_terminator) {
                            out += fmt::format("    /* DELAY (orphaned) 0x{:08X}: {:08X}  {} */\n",
                                               ds_addr, ds_raw, ds_tr.comment);
                            // Orphaned delay slot is inlined here (not an addr_to_raw
                            // entry), so charge its interlock directly, BEFORE its body
                            // (block mode already counts it in the owning block_cycles).
                            emit_icache_fetch(ds_addr);
                            emit_insn_interlock(ds_raw);
                            out += fmt::format("    {}\n", ds_tr.c_code);
                        }
                    }
                    if (cps) {
                        if (addr_to_raw.count(addr + 8)) {
                            uint32_t cn = normalize_address(addr + 8);
                            if (!all_function_entries_norm.count(cn))
                                local_continuations.push_back({addr + 8, cn, norm});
                        } else {
                            register_cross_function_target(addr + 8);
                        }
                        out += emit_irq_check_expr("_t");
                        out += "    cpu->pc = _t; return; }\n";
                    } else {
                        /* _csp is captured AFTER the delay slot on purpose:
                         * the call contract's sp is the post-slot value (the
                         * interp captures site_sp after exec_delay_slot). */
                        out += "    uint32_t _csp = cpu->gpr[29];\n";
                        out += emit_irq_check_expr("_t");
                        out += "    psx_dispatch(cpu, _t);\n";
                        if (rd == 31) {
                            out += fmt::format("    if (psx_call_contract(cpu, 0x{:08X}u, _csp)) return; }}\n",
                                               return_addr);
                        } else {
                            /* Return register isn't $ra: only propagate an active bail. */
                            out += "    if (g_psx_call_bail) return; (void)_csp; }\n";
                        }
                        out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", return_addr);
                    }
                }
            }
            continue;
        }

        // Install-slot hook (CLAUDE.md Rule 18 / docs/dynamic_handler_install.md).
        // The PS1 BIOS overwrites specific kernel-RAM addresses at runtime
        // with dispatch stubs (e.g. RAM 0xCF0 for the SIO data-byte handler).
        // The recompiler emitted NOPs from the ROM image; if we just run those
        // statically, the installed stub never executes.  At known install-slot
        // PCs, emit a runtime check: if the page is dirty (RAM was written-to),
        // dispatch into RAM to run the installed code.  Otherwise fall through
        // to the static NOP.
        //
        // Install slots come from the profile's [[recompiler.install_slots]]
        // (e.g. SCPH1001's SIO data-byte handler slot at RAM 0xCF0). See
        // docs/dynamic_handler_install.md for how to find new ones.
        uint32_t ram_pc = addr_model().rom_to_ram_phys(addr);
        bool is_install_slot = addr_model().is_install_slot(ram_pc);
        if (is_install_slot) {
            /* The installed stub is 4 instructions: lui, addiu, jalr, nop.
             * The jalr captures ra = stub_PC + 8 = install_slot + 0x10.  When
             * the stub's called function returns via jr ra, control flows back
             * to install_slot + 0x10.  Register that ROM address as both a
             * block leader (so label_<post_stub>: is emitted) and a local
             * continuation (so the entry-switch routes to it).  Without this,
             * external dispatch to the post-stub PC misses and falls into
             * interpretation, which doesn't have COP0 and crashes the
             * exception handler. */
            uint32_t post_stub_rom = addr + 0x10u;
            if (addr_to_raw.count(post_stub_rom)) {
                block_leaders.insert(post_stub_rom);
                uint32_t post_stub_norm = normalize_address(post_stub_rom);
                if (!all_function_entries_norm.count(post_stub_norm)) {
                    local_continuations.push_back({post_stub_rom, post_stub_norm, norm});
                }
            }
            /* Hook fires only when the FIRST instruction word at this PC
             * differs from the ROM-baked value (= 0x00000000 NOP for these
             * slots).  A page-level dirty bit is too coarse: the kernel
             * handler dirties page 0 on every exception by saving registers,
             * which would unconditionally redirect into the interpreter.
             * Word-level check is exact: the slot is "live" iff the BIOS
             * install function has actually overwritten it. */
            out += fmt::format(
                "    /* 0x{:08X}: install-slot hook (RAM 0x{:08X}) — if the BIOS\n"
                "     * has overwritten this slot with an install stub, dispatch\n"
                "     * into the stub.  Otherwise fall through to static NOP.\n"
                "     * After the stub's jalr returns, ra=RAM 0x{:08X} routes\n"
                "     * back here as a registered continuation target. */\n"
                "    if (cpu->read_word(0x{:08X}u) != 0u) {{\n"
                "        psx_check_interrupts_at(cpu, 0x{:08X}u);\n"
                "        cpu->pc = 0x{:08X}u; return;\n"
                "    }}\n",
                addr, ram_pc, ram_pc + 0x10u, ram_pc, ram_pc, ram_pc);
        }

        // Non-terminator: emit normally — unless this load's successor reads
        // its destination (MIPS-I load-delay pair): then defer the register
        // writeback into psx_ldd_<addr>, flushed after the successor.
        out += fmt::format("    /* 0x{:08X}: {:08X}  {} */\n", addr, raw, tr.comment);
        if (ldd_sites.count(addr) && !tr.c_code_deferred.empty()) {
            out += fmt::format("    /* load-delay pair: gpr[{}] writeback deferred past 0x{:08X} */\n",
                               ldd_sites[addr].first, addr + 4);
            out += fmt::format("    {}\n", tr.c_code_deferred);
        } else if (ldd_lwlr_forward.count(addr - 4u)) {
            /* This LWL/LWR consumes the pending load from addr-4: point its
             * merge base at the deferred temp. strict_translator emits that
             * base as a single psx_old_rt initializer reading the GPR, which
             * still holds the PRE-load value here. If that emitted shape ever
             * changes, fail the build rather than silently emit a stale merge. */
            const int fwd_rt = ldd_sites.at(addr - 4u).first;
            const std::string from =
                fmt::format("uint32_t psx_old_rt      = cpu->gpr[{}];", fwd_rt);
            const std::string to =
                fmt::format("uint32_t psx_old_rt      = psx_ldd_{:08X};", addr - 4u);
            std::string fwd = tr.c_code;
            const size_t at = fwd.find(from);
            if (at == std::string::npos) {
                throw std::runtime_error(fmt::format(
                    "cannot forward pending load 0x{:08X} into the LWL/LWR at "
                    "0x{:08X} (func 0x{:08X}): merge-base initializer not found",
                    addr - 4u, addr, func.entry_addr));
            }
            fwd.replace(at, from.size(), to);
            out += fmt::format(
                "    /* LWL/LWR merge takes the forwarded psx_ldd_{:08X} */\n",
                addr - 4u);
            out += fmt::format("    {}\n", fwd);
        } else {
            out += fmt::format("    {}\n", tr.c_code);
        }
        emit_ldd_flush(addr);
        out += emit_cosim_instr(addr);

        // Check if this instruction is a delay slot with pending resolution.
        if (pending_at.count(addr)) {
            const PendingBranch& pb = pending_at[addr];
            const std::string& kind = pb.kind;
            out += fmt::format("    if (psx_delay_{:08X}) {{\n", pb.terminator_addr);
            out += fmt::format("        psx_delay_{:08X} = 0;\n", pb.terminator_addr);

            if (is_branch_kind(kind.c_str())) {
                // Conditional branch resolution.
                uint32_t target = pb.target;
                uint32_t fallthrough = pb.terminator_addr + 8;
                bool target_in_function = addr_to_raw.count(target) != 0;
                if (target_in_function) {
                    out += fmt::format("    if (psx_taken_{:08X}) {{\n", pb.terminator_addr);
                    out += emit_irq_check(target, "        ");
                    out += fmt::format("        goto label_{:08X};\n", target);
                    out += "    }\n";
                } else {
                    // Tail call: set cpu->pc and return; dispatch loop re-dispatches.
                    // Register the cross-function target as a continuation so
                    // the dispatch table can route to it (otherwise mid-function
                    // targets miss the table — root cause of card-read failure).
                    // Publish the RUNTIME PC: relocated-window code runs at its
                    // RAM address; the ROM-space alias never executes and its
                    // phys can sit outside guest RAM (no interp fallback).
                    register_cross_function_target(target);
                    out += fmt::format("    if (psx_taken_{:08X}) {{\n", pb.terminator_addr);
                    out += emit_irq_check(target, "        ");
                    out += fmt::format("        cpu->pc = 0x{:08X}u; return;\n", relocate_ra(target));
                    out += "    }\n";
                }
                out += emit_irq_check(fallthrough);
            } else if (kind == "j") {
                uint32_t target = pb.target;
                bool target_in_function = addr_to_raw.count(target) != 0;
                if (target_in_function) {
                    out += emit_irq_check(target);
                    out += fmt::format("    goto label_{:08X};\n", target);
                } else {
                    // Tail call: set cpu->pc and return; dispatch loop re-dispatches.
                    register_cross_function_target(target);
                    out += emit_irq_check(target);
                    out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", target);
                }
            } else if (kind == "jal") {
                uint32_t target = pb.target;
                uint32_t return_addr = relocate_ra(pb.terminator_addr + 8);
                bool cont_in_func = addr_to_raw.count(pb.terminator_addr + 8) != 0;
                // Register the continuation (return point) as dispatchable: a
                // local continuation if it lands in this function, else a
                // cross-function target re-injected into its owner (§25/CPS) —
                // legacy also needs the in-func label for the callee's jr $ra.
                if (cont_in_func) {
                    uint32_t cont_rom = pb.terminator_addr + 8;
                    uint32_t cont_norm = normalize_address(cont_rom);
                    if (!all_function_entries_norm.count(cont_norm)) {
                        local_continuations.push_back({cont_rom, cont_norm, norm});
                    }
                } else if (cps) {
                    register_cross_function_target(pb.terminator_addr + 8);
                }
                if (cps) {
                    // Continuation-passing: tail-transfer to the callee. Its
                    // jr $ra sets cpu->pc = return_addr; the trampoline then
                    // dispatches the registered continuation. No host nesting.
                    if (target == 0x00006380u) {
                        out += fmt::format("    debug_server_log_probe(0x{:08X}u, cpu);\n",
                                           pb.terminator_addr);
                    }
                    out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;\n", return_addr);
                    out += emit_irq_check(target);
                    out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", target);
                } else {
                    out += "    { uint32_t _csp = cpu->gpr[29];\n";
                    out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;\n", return_addr);
                    // Regular call: always go through psx_dispatch (handles tail-call loop).
                    if (target == 0x00006380u) {
                        out += fmt::format("    debug_server_log_probe(0x{:08X}u, cpu);\n",
                                           pb.terminator_addr);
                    }
                    out += emit_irq_check(target);
                    out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);\n", target);
                    if (target == 0x00006380u) {
                        out += fmt::format("    debug_server_log_probe(0x{:08X}u, cpu);\n",
                                           pb.terminator_addr + 1);
                    }
                    out += fmt::format("    if (psx_call_contract(cpu, 0x{:08X}u, _csp)) return; }}\n",
                                       return_addr);
                    // Safety net: if continuation falls outside this function, tail-call to it.
                    if (!cont_in_func) {
                        out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);  /* jal cont: outside func */\n", return_addr);
                    }
                }
            } else if (kind == "jalr") {
                uint8_t rs = (pb.raw >> 21) & 0x1F;
                uint8_t rd = (pb.raw >> 11) & 0x1F;
                uint32_t return_addr = relocate_ra(pb.terminator_addr + 8);
                bool cont_in_func = addr_to_raw.count(pb.terminator_addr + 8) != 0;
                if (cont_in_func) {
                    uint32_t cont_rom = pb.terminator_addr + 8;
                    uint32_t cont_norm = normalize_address(cont_rom);
                    if (!all_function_entries_norm.count(cont_norm)) {
                        local_continuations.push_back({cont_rom, cont_norm, norm});
                    }
                } else if (cps) {
                    register_cross_function_target(pb.terminator_addr + 8);
                }
                /* Delay slot may have clobbered the target register — resolve
                 * from the terminator-latched snapshot then (psx_jrt_). */
                const bool jalr_snap = jr_snapshot_needed(pb.terminator_addr, pb.raw);
                const std::string jalr_tgt = jalr_snap
                    ? fmt::format("psx_jrt_{:08X}", pb.terminator_addr)
                    : fmt::format("cpu->gpr[{}]", static_cast<int>(rs));
                if (cps) {
                    // CPS jalr: capture the target register BEFORE writing the
                    // link reg (rd==rs alias-safe), then tail-transfer.
                    out += fmt::format("    {{ uint32_t _t = {};\n", jalr_tgt);
                    if (rd != 0) {
                        out += fmt::format("    cpu->gpr[{}] = 0x{:08X}u;\n",
                                           static_cast<int>(rd), return_addr);
                    }
                    out += emit_irq_check_expr("_t");
                    out += "    cpu->pc = _t; return; }\n";
                } else {
                    out += "    { uint32_t _csp = cpu->gpr[29];\n";
                    if (rd != 0) {
                        out += fmt::format("    cpu->gpr[{}] = 0x{:08X}u;\n",
                                           static_cast<int>(rd), return_addr);
                    }
                    out += emit_irq_check_expr(jalr_tgt);
                    out += fmt::format("    psx_dispatch(cpu, {});\n", jalr_tgt);
                    if (rd == 31) {
                        out += fmt::format("    if (psx_call_contract(cpu, 0x{:08X}u, _csp)) return; }}\n",
                                           return_addr);
                    } else {
                        /* Return register isn't $ra: only propagate an active bail. */
                        out += "    if (g_psx_call_bail) return; (void)_csp; }\n";
                    }
                    // Safety net: if continuation falls outside this function, tail-call to it.
                    if (!cont_in_func) {
                        out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);  /* jalr cont: outside func */\n", return_addr);
                    }
                }
            } else if (kind == "jr") {
                uint8_t rs = (pb.raw >> 21) & 0x1F;
                /* Delay slot may have clobbered the target register — resolve
                 * from the terminator-latched snapshot then (psx_jrt_). */
                const bool jr_snap = jr_snapshot_needed(pb.terminator_addr, pb.raw);
                const std::string jr_tgt = jr_snap
                    ? fmt::format("psx_jrt_{:08X}", pb.terminator_addr)
                    : fmt::format("cpu->gpr[{}]", static_cast<int>(rs));
                if (rs == 31) {
                    if (ra_loaded_from_non_sp)
                        out += emit_irq_check_expr(jr_tgt) +
                               fmt::format("    cpu->pc = {}; psx_restore_state_escape(); return;  /* longjmp-return */\n", jr_tgt);
                    else if (cps)
                        out += emit_irq_check_expr(jr_tgt) +
                               fmt::format("    cpu->pc = {}; return;  /* CPS: publish $ra for trampoline dispatch */\n", jr_tgt);
                    else
                        out += emit_irq_check_expr(jr_tgt) +
                               "    return;\n";
                } else {
                    // Attempt jump table resolution: scan backward from the
                    // jr instruction to find SLTIU/SLL/LUI/ADDU/LW pattern.
                    // If found, read the table from ROM, map entries from RAM
                    // to ROM addresses, and emit a switch with goto labels
                    // for targets that exist within this function.
                    bool emitted_switch = false;
                    {
                        uint32_t jr_addr = pb.terminator_addr;
                        uint32_t jr_rs = rs;
                        uint32_t lw_base = 0xFF; int32_t lw_off = 0;
                        uint32_t ac[2] = {0xFF, 0xFF}; uint32_t lui_v = 0;
                        int16_t  av[2] = {0, 0}; bool fa[2] = {false, false};
                        bool fl = false; uint32_t tc = 0;

                        for (int b = 1; b <= 40; b++) {
                            uint32_t sa = jr_addr - (uint32_t)(b * 4);
                            if (sa < func.entry_addr) break;
                            auto sa_it = addr_to_raw.find(sa);
                            if (sa_it == addr_to_raw.end()) break;
                            uint32_t si = sa_it->second;
                            uint32_t s_op=(si>>26)&0x3F, s_rs=(si>>21)&0x1F,
                                     s_rt=(si>>16)&0x1F, s_rd=(si>>11)&0x1F, s_fn=si&0x3F;

                            if (s_op==0x23 && s_rt==jr_rs && lw_base==0xFF) {
                                lw_base=s_rs; lw_off=(int32_t)(int16_t)(si&0xFFFF); continue; }
                            if (s_op==0x00 && s_fn==0x21 && s_rd==lw_base &&
                                lw_base!=0xFF && ac[0]==0xFF) {
                                ac[0]=s_rs; ac[1]=s_rt; continue; }
                            if (s_op==0x09 && ac[0]!=0xFF) {
                                for(int c=0;c<2;c++){
                                    if(!fa[c]&&ac[c]!=0xFF&&s_rs==ac[c]&&s_rt==ac[c]){
                                        av[c]=(int16_t)(si&0xFFFF);fa[c]=true;break;}}
                                continue; }
                            if (s_op==0x0F && ac[0]!=0xFF && !fl) {
                                for(int c=0;c<2;c++){
                                    if(ac[c]!=0xFF&&s_rt==ac[c]){
                                        lui_v=((uint32_t)(si&0xFFFF))<<16;
                                        if(!fa[c])av[c]=0; av[0]=av[c]; fa[0]=fa[c];
                                        fl=true; break;}}
                                continue; }
                            if (s_op==0x0B && tc==0) { tc=si&0xFFFF; continue; }
                            if (fl && tc!=0) break;
                        }

                        if (fl && tc > 0 && tc < 512) {
                            uint32_t tb = (lui_v + (fa[0] ? (uint32_t)(int32_t)av[0] : 0u))
                                        + (uint32_t)lw_off;
                            // Map RAM table address to ROM for reading.
                            uint32_t rom_tb = ram_alias_to_rom(tb);

                            // Read table entries and map to ROM labels.
                            // Deduplicate by runtime address to avoid duplicate
                            // case values in the switch. Register all targets
                            // with the cross-function continuation pass before
                            // filtering local goto labels; BIOS jump tables can
                            // legally branch into a different discovered
                            // function's body.
                            std::vector<std::pair<uint32_t,uint32_t>> targets; // {runtime, rom}
                            std::set<uint32_t> seen_runtime;
                            uint32_t rom_off = rom_tb - base_addr;
                            for (uint32_t i = 0; i < tc; i++) {
                                if (rom_off + i*4 + 3 >= rom.size()) break;
                                uint32_t rv = read_u32_le(rom, rom_off + i * 4);
                                if (!seen_runtime.insert(rv).second) continue;
                                uint32_t rom_target = ram_alias_to_rom(rv);
                                register_cross_function_target(rom_target);
                                if (addr_to_raw.count(rom_target) && block_leaders.count(rom_target))
                                    targets.push_back({rv, rom_target});
                            }
                            if (!targets.empty()) {
                                out += fmt::format("    /* jump table at 0x{:08X}, {} entries */\n",
                                                   tb, tc);
                                out += fmt::format("    switch ({}) {{\n", jr_tgt);
                                for (auto& [rt, rom_t] : targets) {
                                    out += fmt::format("        case 0x{:08X}u:\n", rt);
                                    out += emit_irq_check(rom_t, "            ");
                                    out += fmt::format("            goto label_{:08X};\n", rom_t);
                                }
                                out += "        default:\n";
                                out += emit_irq_check_expr(jr_tgt, "            ");
                                out += fmt::format("            cpu->pc = {}; return;\n", jr_tgt);
                                out += "    }\n";
                                emitted_switch = true;
                            }
                        }
                    }
                    if (!emitted_switch) {
                        // Tail call: set cpu->pc and return; dispatch loop re-dispatches.
                        out += emit_irq_check_expr(jr_tgt);
                        out += fmt::format("    cpu->pc = {}; return;\n", jr_tgt);
                    }
                }
            }
            // RFE: already emitted at terminator address, no delay slot.
            out += "    }\n";
        }
    }

    // Fallthrough detection: if the last instruction in this function is NOT
    // a terminator (no j/jr/branch), the MIPS code falls through into the
    // next function. Emit a tail call to that function.
    if (!addr_to_raw.empty()) {
        uint32_t last_addr = addr_to_raw.rbegin()->first;
        uint32_t last_raw  = addr_to_raw.rbegin()->second;
        PSXRecomp::DecodedInstruction last_d = PSXRecomp::MipsDecoder::decode(last_raw, last_addr);
        TranslateResult last_tr = StrictTranslator::translate(last_d);

        // If the last instruction is a delay slot with a pending branch,
        // check whether it fully handles control flow.  Unconditional control
        // flow (j / jr / jal / jalr) covers all paths — no fall-through needed.
        // Conditional branches (beq / bne / blez / bgtz / bltz / bgez / etc.)
        // only set cpu->pc for the "taken" path; the "not taken" path still
        // falls through to the next sequential address and needs a tail call.
        bool has_control_flow = last_tr.is_terminator;
        if (!has_control_flow && pending_at.count(last_addr)) {
            const PendingBranch& pb = pending_at.at(last_addr);
            if (pb.kind == "j" || pb.kind == "jr" || pb.kind == "jal" ||
                pb.kind == "jalr" || pb.kind == "rfe") {
                has_control_flow = true;
            }
            // Conditional branches: has_control_flow stays false → emit fall-through
        }
        if (!has_control_flow) {
            // Fallthrough tail call: set cpu->pc and return; dispatch loop re-dispatches.
            uint32_t next_addr = last_addr + 4;
            out += emit_irq_check(next_addr);
            out += fmt::format("    cpu->pc = 0x{:08X}u; return;  /* fallthrough */\n", next_addr);
        }
    }

    out += "}\n\n";
    #undef out

    // --- Assemble the final function output ---
    // Now 'body' has the full function body (labels, instructions, etc.)
    // 'local_continuations' has the continuation labels we found.

    /* (cross-function target injection moved above the body-emit loop) */

    // Deduplicate local continuations by ROM address (multiple jal/jalr
    // within the same function may share a return label).
    {
        std::set<uint32_t> seen;
        std::vector<ContinuationLabel> deduped;
        for (auto& cl : local_continuations) {
            if (seen.insert(cl.rom_addr).second)
                deduped.push_back(cl);
        }
        local_continuations = std::move(deduped);
    }

    // Copy local continuations to the output parameter.
    for (auto& cl : local_continuations) {
        out_continuations.push_back(cl);
    }

    // Emit function header.
    out += fmt::format("void {}(CPUState* cpu) {{\n", fn_sym(norm));
    out += "#if defined(PSX_ENABLE_BLOCK_CYCLES) && "
           "(defined(__GNUC__) || defined(__clang__))\n"
           "    __attribute__((cleanup(psx_cyc_bb_defer_cleanup))) "
           "int _psx_cyc_bb_guard = 1;\n"
           "    psx_cyc_bb_defer_begin();\n"
           "#endif\n";
    // Direct-call entry hook: captures into fn_entry ring (gated by
    // fn_filter at runtime).  Lets us see direct-jal call paths that
    // never go through psx_dispatch.
    out += fmt::format("    debug_server_log_call_entry(0x{:08X}u);\n", norm);
    // Branch predicate locals must be initialized before the continuation
    // switch. Continuation entry can jump directly to a merge block that reads
    // a predicate set only on a different inbound path.
    out += branch_decls;

    // If there are continuation labels, prepend an entry-switch so the
    // dispatch loop can route to internal labels when called from a
    // continuation wrapper.
    if (!local_continuations.empty()) {
        out += "    if (cpu->pc != 0) {\n";
        out += "        uint32_t _cont = cpu->pc;\n";
        out += "        cpu->pc = 0;\n";
        out += "        switch (_cont) {\n";
        for (const auto& cl : local_continuations) {
            out += fmt::format("            case 0x{:08X}u: goto label_{:08X};\n",
                               cl.rom_addr, cl.rom_addr);
        }
        out += "            default: break;\n";
        out += "        }\n";
        out += "    }\n";
    }

    // Append the body.
    out += body;
    return true;
}

// ---------------------------------------------------------------------------
// emit_dispatch: generate the dispatch table
// ---------------------------------------------------------------------------

void FullFunctionEmitter::emit_dispatch(
    std::string&                        out,
    const DiscoveryResult&              dr,
    const std::set<uint32_t>&           emitted_normalized,
    const std::map<uint32_t, ContinuationLabel>& continuations,
    const std::string&                  bios_sha256,
    const std::vector<uint8_t>&         rom,
    uint32_t                            base_addr,
    const std::vector<BiosVectorTable>& bios_vectors,
    const std::vector<BiosAlias>&       bios_aliases)
{
    // RECURSION_BUG.md §25 — continuation-passing. Under CPS the BIOS A0/B0/C0
    // vector dispatch must TAIL-TRANSFER to the handler (set cpu->pc, return)
    // rather than nest psx_dispatch_call. A nested psx_dispatch_call runs its
    // own trampoline with stop_addr = the caller's return; when the handler
    // returns there it zeroes cpu->pc and C-returns to the (flat) main
    // trampoline, which then sees pc==0 and exits at boot. In legacy this was
    // safe because the caller's `jal gate` was itself a nested dispatch whose
    // psx_call_contract resumed the continuation; in CPS the caller
    // tail-transferred, so the return must flow back to the SAME flat
    // trampoline — which only happens if the vector handler tail-transfers too.
    // CPS is the DEFAULT (RECURSION_BUG.md §25 — the validated leak fix). Opt out
    // (legacy nested-dispatch codegen) with PSX_CPS=0.
    static const bool cps = []() {
        const char* e = std::getenv("PSX_CPS");
        return e == nullptr || e[0] != '0';
    }();

    out += "/* AUTO-GENERATED by psxrecomp-bios --emit-full. DO NOT EDIT.\n";
    out += " *\n";
    out += fmt::format(" * BIOS SHA256: {}\n", bios_sha256);
    out += fmt::format(" * Dispatch entries: {}\n", emitted_normalized.size());
    out += " */\n\n";
    // Namespace the two externally-visible dispatch entry points to this
    // image. They are referenced from ~20 emit sites and from the sibling
    // generated file, so renaming them in the preamble is both smaller and
    // harder to get wrong than rewriting each reference. The runtime keeps
    // the unprefixed names as thin forwarders to the ACTIVE bios, which is
    // what the game's generated C calls. Everything else this file defines
    // is either static (file-local, cannot collide) or already stem-prefixed.
    out += fmt::format("#define psx_dispatch      {}psx_dispatch\n", g_sym_prefix);
    out += fmt::format("#define psx_dispatch_call {}psx_dispatch_call\n\n", g_sym_prefix);
    out += "#include \"cpu_state.h\"\n";
    // Layouts (PsxKernelBody / PsxBiosImageInfo / PsxNativeStub) and the
    // backend ABI come from the runtime headers rather than being
    // re-declared here, so they cannot drift from what the runtime reads.
    out += "#include \"psx_bios_backend.h\"\n";
    out += "#include <stdint.h>\n";
    out += "#include <stdio.h>\n";
    out += "#include <stdlib.h>\n\n";

    // Extern declarations for runtime-provided functions.
    out += "extern void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys);\n";
    out += "extern void psx_check_interrupts(CPUState* cpu);\n";
    out += "extern void psx_check_interrupts_at(CPUState* cpu, uint32_t resume_pc);\n";
    out += "extern void psx_restore_state_escape(void);\n";
    out += "extern void gte_execute(CPUState* cpu, uint32_t cmd);\n";
    out += "extern void gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);\n";
    out += "extern uint32_t gte_read_data(CPUState* cpu, uint8_t reg);\n";
    out += "extern uint32_t g_debug_current_func_addr;\n";
    out += "extern void debug_server_trace_dispatch(uint32_t func_addr);\n";
    out += "/* BIOS HLE tier (CLAUDE.md \xC2\xA7""0 amendment 2026-07-02): null-by-default\n";
    out += " * hook consulted at the top of every dispatch iteration, BEFORE any\n";
    out += " * backend (game image / dirty-RAM interp / static table) claims the\n";
    out += " * target. Installed by the runtime (bios_hle.c) when [runtime] bios_hle\n";
    out += " * is on; returns 1 when it fully serviced the target, and the guest\n";
    out += " * resumes at $ra exactly as if the kernel routine executed jr $ra.\n";
    out += " * NULL (the default) = pure LLE, dispatch identical to a build without\n";
    out += " * the tier. */\n";
    out += "extern int (*g_psx_bios_hle_hook)(CPUState* cpu, uint32_t phys);\n\n";
    out += "#ifdef PSX_HAS_GAME_DISPATCH\n";
    out += "extern int psx_game_address_in_text(uint32_t addr);\n";
    out += "#endif\n\n";

    // Forward declarations for all emitted functions.
    for (uint32_t norm : emitted_normalized) {
        if (continuations.count(norm)) {
            // Continuation wrapper: use the wrapper name.
            const auto& cl = continuations.at(norm);
            out += fmt::format("extern void {}(CPUState* cpu);\n",
                               cont_sym(cl.parent_func_norm, cl.rom_addr));
        } else {
            out += fmt::format("extern void {}(CPUState* cpu);\n", fn_sym(norm));
        }
    }
    out += "\n";

    // --- BIOS vector switch handlers ---
    // For each [[recompiler.bios_vectors]] entry: read the function pointer
    // table from the ROM binary, normalize each entry to a dispatch key, and
    // emit a static C switch function. These handlers are added to the dispatch
    // table so that 0xA0/0xB0/0xC0 are binary-search hits at runtime instead
    // of falling through to dirty_ram_interp.
    struct VecHandler { uint32_t ram_addr; std::string func_name; };
    std::vector<VecHandler> vec_handlers;

    if (!bios_vectors.empty() || !bios_aliases.empty()) {
        // psx_dispatch_call is defined later in this file; forward-declare it
        // so the vector handlers can call it for runtime-table fallbacks.
        out += "void psx_dispatch_call(CPUState* cpu, uint32_t addr, uint32_t return_addr);\n\n";
    }

    // --- BIOS fixed-target aliases ---
    // Simple one-liner wrappers for trampolines that always redirect to a
    // single known function (e.g. SIO handler at 0x0CF0 → func_0000641C).
    for (const auto& ba : bios_aliases) {
        if (emitted_normalized.count(ba.target_key) == 0) {
            out += fmt::format("/* bios_alias 0x{:08X} -> 0x{:08X}: "
                               "target not in dispatch table, skipped */\n",
                               ba.ram_addr, ba.target_key);
            continue;
        }
        out += fmt::format("/* BIOS fixed alias: 0x{:08X} -> {} */\n",
                           ba.ram_addr, fn_sym(ba.target_key));
        out += fmt::format("static void bios_alias_{:08X}(CPUState* cpu) "
                           "{{ {}(cpu); }}\n\n",
                           ba.ram_addr, fn_sym(ba.target_key));
        vec_handlers.push_back({ba.ram_addr,
                                 fmt::format("bios_alias_{:08X}", ba.ram_addr)});
    }

    {
        const uint32_t base_phys = base_addr & 0x1FFFFFFFu;
        for (const auto& bvt : bios_vectors) {
            const uint32_t table_phys = bvt.table_rom_addr & 0x1FFFFFFFu;
            if (table_phys < base_phys ||
                table_phys + bvt.table_count * 4u > base_phys + (uint32_t)rom.size()) {
                std::fprintf(stderr,
                    "emit_dispatch: bios_vector table 0x%08X out of ROM range, skipping\n",
                    bvt.table_rom_addr);
                continue;
            }
            const uint32_t file_off = table_phys - base_phys;
            const std::string hname = fmt::format("bios_vec_{:02X}", bvt.ram_addr);

            out += fmt::format("/* BIOS vector 0x{:02X}: static switch handler, "
                               "table ROM 0x{:08X} ({} entries) */\n",
                               bvt.ram_addr, bvt.table_rom_addr, bvt.table_count);
            out += fmt::format("static void {}(CPUState* cpu) {{\n", hname);
            out += fmt::format("    switch (cpu->gpr[{}]) {{\n", bvt.index_reg);

            for (uint32_t i = 0; i < bvt.table_count; i++) {
                if (file_off + i * 4u + 4u > (uint32_t)rom.size()) break;
                const uint32_t entry = read_u32_le(rom, file_off + i * 4u);
                if (entry == 0u) continue;
                const uint32_t key = normalize_address(entry);
                if (emitted_normalized.count(key) == 0) {
                    out += fmt::format("        /* case 0x{:02X}: {} — "
                                       "not in dispatch table, skipped */\n", i, fn_sym(key));
                    continue;
                }
                out += fmt::format("        case 0x{:02X}: {}(cpu); return;\n",
                                   i, fn_sym(key));
            }

            out += "    }\n";
            // Runtime fallback: for Shell-patched entries not in the ROM table,
            // read the live function pointer from RAM and dispatch.
            if (bvt.table_ram_addr != 0u) {
                if (cps) {
                    // CPS: tail-transfer to the handler. $ra already holds the
                    // caller's return (set by the guest's jal to the A0/B0/C0
                    // gate, preserved through the gate's jr and this vector).
                    // The flat trampoline dispatches the handler; its jr $ra
                    // then continues at the caller's return — no nesting.
                    out += fmt::format(
                        "    /* Runtime fallback (CPS tail-transfer) */\n"
                        "    {{\n"
                        "        uint32_t target = cpu->read_word(0x{:08X}u + (uint32_t)cpu->gpr[{}] * 4u);\n"
                        "        if (target) {{ cpu->pc = target; return; }}\n"
                        "    }}\n",
                        bvt.table_ram_addr, bvt.index_reg);
                } else {
                    out += fmt::format(
                        "    /* Runtime fallback for Shell-patched / out-of-range entries */\n"
                        "    {{\n"
                        "        uint32_t target = cpu->read_word(0x{:08X}u + (uint32_t)cpu->gpr[{}] * 4u);\n"
                        "        if (target) psx_dispatch_call(cpu, target, cpu->gpr[31]);\n"
                        "    }}\n",
                        bvt.table_ram_addr, bvt.index_reg);
                }
            }
            out += "}\n\n";
            vec_handlers.push_back({bvt.ram_addr, hname});
        }
    }

    // Dispatch table: sorted array.
    // Vector handlers (ram_addr 0xA0/0xB0/0xC0) sort before all static entries.
    out += "typedef void (*PsxRecompFunc)(CPUState*);\n\n";
    out += "typedef struct {\n";
    out += "    uint32_t addr;\n";
    out += "    PsxRecompFunc func;\n";
    out += "} DispatchEntry;\n\n";

    const size_t total_entries = emitted_normalized.size() + vec_handlers.size();
    out += fmt::format("static const DispatchEntry dispatch_table[{}] = {{\n",
                       total_entries);

    // Vector entries first (addresses 0xA0/0xB0/0xC0 < 0x500, always first).
    for (const auto& vh : vec_handlers) {
        out += fmt::format("    {{ 0x{:08X}u, {} }},\n", vh.ram_addr, vh.func_name);
    }

    for (uint32_t norm : emitted_normalized) {
        if (continuations.count(norm)) {
            const auto& cl = continuations.at(norm);
            out += fmt::format("    {{ 0x{:08X}u, {} }},\n",
                               norm, cont_sym(cl.parent_func_norm, cl.rom_addr));
        } else {
            out += fmt::format("    {{ 0x{:08X}u, {} }},\n", norm, fn_sym(norm));
        }
    }
    out += "};\n\n";

    // --- Kernel body-extent table (runtime kernel-image bless) ---
    // The BIOS copies Kernel Part 2 from ROM [0x1FC10000,0x1FC18000) to RAM
    // [0x500,0x8500) at boot; the functions above with keys in that window
    // were compiled FROM those ROM bytes. The runtime may execute a key's
    // static native function instead of interpreting the (dirty) kernel page
    // IFF the live RAM bytes of everything that function can execute still
    // byte-match the ROM source. This table gives the runtime that judgment
    // boundary: for each kernel-RAM dispatch key, the FULL RAM extent of the
    // compiled code reachable from it. A continuation wrapper re-enters its
    // PARENT (backward branches reachable), so continuations carry the
    // parent's whole extent. Synthetic vector/alias wrappers are excluded —
    // their stub bytes are runtime-installed and never ROM-matching.
    {
        // Function extents by normalized entry, from discovery (end inclusive).
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> extent_by_norm;
        for (const auto& fn : dr.functions) {
            uint32_t lo = normalize_address(fn.entry_addr);
            uint32_t hi = normalize_address(fn.end_addr) + 4u;
            extent_by_norm[normalize_address(fn.entry_addr)] = {lo, hi};
            (void)lo;
        }
        std::string kb;
        size_t kb_count = 0;
        for (uint32_t norm : emitted_normalized) {
            if (!addr_model().in_kbless(norm)) continue;
            uint32_t owner = norm;
            if (continuations.count(norm))
                owner = continuations.at(norm).parent_func_norm;
            auto it = extent_by_norm.find(owner);
            if (it == extent_by_norm.end()) continue;
            uint32_t lo = it->second.first, hi = it->second.second;
            // Only bodies that live entirely inside the relocated window are
            // verifiable against the ROM source; skip anything straddling it.
            if (lo < addr_model().kbless_ram_lo() ||
                hi > addr_model().kbless_ram_hi() || lo >= hi) continue;
            kb += fmt::format("    {{ 0x{:08X}u, 0x{:08X}u, 0x{:08X}u }},\n",
                              norm, lo, hi);
            kb_count++;
        }
        // Per-image table. static + stem-prefixed: a build links more than
        // one recompiled BIOS, and only the backend descriptor is exported.
        // The PsxKernelBody layout now comes from psx_bios_image.h rather
        // than being re-declared here, so it cannot drift from the runtime.
        out += fmt::format("static const PsxKernelBody {}psx_bios_kernel_bodies[{}] = {{\n",
                           g_sym_prefix, kb_count);
        out += kb;
        out += "};\n";
        // An enum is an integer constant expression in C. A const-qualified
        // object is not, so MSVC rejects it in the file-scope backend
        // initializer even though GCC and Clang accept it as an extension.
        out += fmt::format("enum {{ {}psx_bios_kernel_body_count = {}u }};\n\n",
                           g_sym_prefix, kb_count);
    }

    // --- Image self-description (runtime/include/psx_bios_image.h) ---
    // The runtime reads these instead of hardcoding per-image constants;
    // emitted next to the code they describe, so they cannot disagree with
    // the linked BIOS. Layout must match PsxBiosImageInfo in the header.
    {
        uint32_t crc = 0xFFFFFFFFu;               /* IEEE CRC-32 (zlib) */
        for (uint8_t b : rom) {
            crc ^= b;
            for (int k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        crc = ~crc;
        uint32_t wsum = 0;                        /* memory.c savestate checksum */
        for (size_t i = 0; i + 3 < rom.size(); i += 4)
            wsum += read_u32_le(rom, static_cast<uint32_t>(i));

        const std::string id  = g_bios_profile ? g_bios_profile->id : "";
        const uint32_t    sep = g_bios_profile ? g_bios_profile->shell_entry_phys : 0u;
        const uint32_t    der = g_bios_profile ? g_bios_profile->deliver_event_ret : 0u;
        const uint32_t    kb_lo  = addr_model().has_kbless() ? addr_model().kbless_ram_lo()  : 0u;
        const uint32_t    kb_hi  = addr_model().has_kbless() ? addr_model().kbless_ram_hi()  : 0u;
        const uint32_t    kb_off = addr_model().has_kbless() ? addr_model().kbless_rom_off() : 0u;

        const int bundled = (g_bios_profile && g_bios_profile->image_redistributable) ? 1 : 0;
        out += fmt::format(
            "static const PsxBiosImageInfo {}psx_bios_image = {{\n"
            "    0x{:08X}u, 0x{:08X}u, 0x{:08X}u,  /* kbless lo/hi/rom_off */\n"
            "    0x{:08X}u,                        /* shell_entry_phys */\n"
            "    0x{:08X}u,                        /* deliver_event_ret */\n"
            "    {}u, 0x{:08X}u, 0x{:08X}u,   /* size / crc32 / wordsum */\n"
            "    \"{}\",\n"
            "    \"{}\",\n"
            "    {},                               /* image_bundled */\n"
            "}};\n\n",
            g_sym_prefix, kb_lo, kb_hi, kb_off, sep, der,
            rom.size(), crc, wsum, bios_sha256, id, bundled);
    }
    // --- Runtime-installed BIOS call-vector stubs ---
    // Every retail PSX BIOS installs the A0/B0/C0 ABI gates as the same
    // four-instruction shape: lui/addiu $t0,target; jr $t0; nop. The target is
    // BIOS-specific, so decode it from live RAM and require the complete shape
    // before taking the native tail transfer. A game/BIOS patch that changes
    // even one word fails closed to dirty_ram_interp below.
    out += fmt::format("static const PsxNativeStub {}psx_bios_native_stubs[3] = {{\n",
                       g_sym_prefix);
    out += "    { 0x000000A0u, 0x000000A0u, 0x000000B0u },\n";
    out += "    { 0x000000B0u, 0x000000B0u, 0x000000C0u },\n";
    out += "    { 0x000000C0u, 0x000000C0u, 0x000000D0u },\n";
    out += "};\n";
    out += fmt::format("static const uint32_t {}psx_bios_native_stub_count = 3u;\n\n",
                       g_sym_prefix);
    out += "static int psx_bios_try_native_call_stub(CPUState* cpu, "
           "uint32_t addr) {\n";
    out += "    uint32_t phys = addr & 0x1FFFFFFFu;\n";
    out += "    if (phys != 0xA0u && phys != 0xB0u && phys != 0xC0u) return 0;\n";
    out += "    uint32_t w0 = cpu->read_word(phys + 0u);\n";
    out += "    uint32_t w1 = cpu->read_word(phys + 4u);\n";
    out += "    uint32_t w2 = cpu->read_word(phys + 8u);\n";
    out += "    uint32_t w3 = cpu->read_word(phys + 12u);\n";
    out += "    if ((w0 & 0xFFFF0000u) != 0x3C080000u ||\n";
    out += "        (w1 & 0xFFFF0000u) != 0x25080000u ||\n";
    out += "        w2 != 0x01000008u || w3 != 0u) return 0;\n";
    out += "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
    out += "    psx_icache_fetch(cpu, addr);\n";
    out += fmt::format("    psx_cyc_step(cpu, 0x{:X}u);\n",
                       psx_cyc_dep_res_mask(0x3C080000u));
    out += fmt::format("    psx_cyc_step(cpu, 0x{:X}u);\n",
                       psx_cyc_dep_res_mask(0x25080000u));
    out += fmt::format("    psx_cyc_step(cpu, 0x{:X}u);\n",
                       psx_cyc_dep_res_mask(0x01000008u));
    out += fmt::format("    psx_cyc_step(cpu, 0x{:X}u);\n",
                       psx_cyc_dep_res_mask(0x00000000u));
    out += "#endif\n";
    out += "    cpu->gpr[8] = ((w0 & 0xFFFFu) << 16) +\n";
    out += "                  (uint32_t)(int32_t)(int16_t)(w1 & 0xFFFFu);\n";
    out += "    cpu->pc = cpu->gpr[8];\n";
    out += "    return 1;\n";
    out += "}\n\n";

    // Dispatch function with binary search. normalize() is generated from
    // the SAME address-model table the C++ helpers above use, so the emitted
    // C can never drift from the analysis that produced it.
    out += addr_model().emit_normalize_c();

    out += "extern int dirty_ram_dispatch(CPUState* cpu, uint32_t addr, uint32_t stop_addr);\n";
    out += "extern int dirty_ram_is_dirty(uint32_t phys);\n";
    out += "extern int psx_kernel_bless_dispatchable(uint32_t phys);\n";
    out += "extern void fntrace_record(CPUState* cpu, uint32_t target);\n";
    out += "extern uint64_t g_dispatch_static_hits;\n";
    out += "\n";
    out += "extern int g_psx_dispatch_depth;  /* runtime-owned: shared dispatch state, not per-image */\n\n";
    out += "static void psx_dispatch_check_return_boundary(CPUState* cpu, uint32_t stop_addr) {\n";
    out += "    if (stop_addr != 0u) {\n";
    out += "        psx_check_interrupts_at(cpu, stop_addr);\n";
    out += "        if (((cpu->pc ^ stop_addr) & 0x1FFFFFFFu) == 0) cpu->pc = 0;\n";
    out += "    } else {\n";
    out += "        psx_check_interrupts(cpu);\n";
    out += "    }\n";
    out += "}\n\n";
    out += "static void psx_dispatch_impl(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {\n";
    out += "    /* Tail-call trampoline: functions signal tail calls by setting\n";
    out += "     * cpu->pc to the target and returning. We loop here to re-dispatch\n";
    out += "     * without growing the native stack. Interrupts are only checked when\n";
    out += "     * the outermost dispatch returns, so a nested callee cannot interrupt\n";
    out += "     * before its generated caller runs the post-call continuation. */\n";
    out += "    int outermost = (g_psx_dispatch_depth++ == 0);\n";
    out += "    /* Call contract (Bug D family): the guest $sp at the call.  A C\n";
    out += "     * continuation behind this dispatch may only run if the guest\n";
    out += "     * actually returns here ($ra == stop_addr) with this $sp. */\n";
    out += "    uint32_t sp_at_call = cpu->gpr[29];\n";
    out += "    for (;;) {\n";
    out += "        /* Always-on call ring: every iteration counts as a separate\n";
    out += "         * call (initial entry + each tail-call re-dispatch). a0..a3\n";
    out += "         * reflect the args being passed for THIS iteration. */\n";
    out += "        fntrace_record(cpu, addr);\n";
    out += "        cpu->pc = 0;\n";
    out += fmt::format("        int lo = 0, hi = {} - 1;\n", total_entries);
    out += "        int found = 0;\n";
    out += "        /* BIOS HLE tier: consult the hook FIRST, on the pre-normalize\n";
    out += "         * physical address. It must see (a) the A0/B0/C0 service vectors\n";
    out += "         * (game thunks jr there with the function number in $t1) and (b)\n";
    out += "         * the shell entry for HLE boot-skip \xE2\x80\x94 the latter lies inside the\n";
    out += "         * game-text/dirty-RAM window, so the check cannot sit after those\n";
    out += "         * backends. Handled (rc 1) \xE2\x87\x92 the service completed against guest\n";
    out += "         * state and the guest resumes at $ra via the trampoline's normal\n";
    out += "         * return/tail contract below. rc 0 \xE2\x87\x92 pure LLE fall-through. */\n";
        out += "        if (g_psx_bios_hle_hook &&\n";
        out += "            g_psx_bios_hle_hook(cpu, addr & 0x1FFFFFFFu)) {\n";
        out += "            cpu->pc = cpu->gpr[31];\n";
        out += "            found = 1;\n";
        out += "        }\n";
        out += "        /* Byte-guarded A0/B0/C0 call-vector tail stubs. */\n";
        out += "        if (!found && psx_bios_try_native_call_stub(cpu, addr))\n";
        out += "            found = 1;\n";
    out += "#ifdef PSX_HAS_GAME_DISPATCH\n";
    out += "        /* Game EXEs can overlap the BIOS shell copy window at\n";
    out += "         * physical 0x30000-0x5AFFF. If the target belongs to the\n";
    out += "         * active game text range, route it through the game/dirty-RAM\n";
    out += "         * path before normalizing it to shell ROM. */\n";
    out += "        uint32_t game_phys = addr & 0x1FFFFFFFu;\n";
    out += "        /* Class-A shell-window collision fix: post-game-start the BIOS shell\n";
    out += "         * copy at RAM 0x30000-0x5AFFF is DEAD (overwritten by the game EXE,\n";
    out += "         * its runtime-loaded overlays, or CD streaming), so normalize()->shell\n";
    out += "         * ROM for a shell-window address is stale and runs dead shell code\n";
    out += "         * (func_1FC42090 -> null jalr -> pc=0, the MMX6 boot wedge class A;\n";
    out += "         * func_1FC38B80 data-as-code self-loop starving the main thread, the\n";
    out += "         * Tomba2 Whoopee-logo wedge). Once the game has started, route the\n";
    out += "         * WHOLE shell-overlap window to the game/overlay/dirty-RAM path\n";
    out += "         * BEFORE normalize() can shadow it to the shell \xE2\x80\x94 real hardware\n";
    out += "         * executes the RAM bytes, whoever loaded them (boot EXE in text,\n";
    out += "         * runtime overlay above text-end, DMA stream). The old gate also\n";
    out += "         * required psx_game_address_in_text(), which silently exempted\n";
    out += "         * OVERLAY code above the boot-EXE text end (Tomba2 text ends\n";
    out += "         * 0x38800; its post-logo ISR-wait caller lives at 0x50B80) and sent\n";
    out += "         * it to the stale shell image. A genuine pre-start shell address\n";
    out += "         * (game not started) still falls through to normalize(). */\n";
    out += "        extern int fntrace_is_game_started(void);\n";
    out += "        extern int dirty_ram_text_native_ok(uint32_t phys);\n";
    if (addr_model().has_rom_keyed_ram_window()) {
        out += "        int game_shell_overlap = fntrace_is_game_started() &&\n";
        out += fmt::format(
            "            game_phys >= 0x{:08X}u && game_phys <= 0x{:08X}u;\n",
            addr_model().rom_keyed_ram_lo(), addr_model().rom_keyed_ram_hi_incl());
    } else {
        /* No ROM-keyed RAM copy window (a BIOS with no shell relocation):
         * nothing for a game to collide with. */
        out += "        int game_shell_overlap = 0;\n";
    }
    out += "        /* Shell-window addresses the loaded game EXE image covers with\n";
    out += "         * matching bytes are GAME code, not the BIOS shell \xE2\x80\x94 route them to\n";
    out += "         * the game path even when the game-start latch (a dispatch to\n";
    out += "         * entry_pc) never tripped. Fixes titles whose static text fills\n";
    out += "         * 0x30000-0x5AFFF: Crash Bash reaches its entry 0x2E7B0 via compiled\n";
    out += "         * internal flow, so `started` never latched and normalize() shadowed\n";
    out += "         * real text at 0x3358C to dead shell ROM (unknown-dispatch abort).\n";
    out += "         * The native-ok compare is ground truth: pre-game the RAM holds shell\n";
    out += "         * bytes (differ from the game image -> not routed here -> shell\n";
    out += "         * dispatches); post-load it holds the game EXE (matches -> game path). */\n";
    if (addr_model().has_rom_keyed_ram_window()) {
        out += "        int game_text_in_shell_window =\n";
        out += fmt::format(
            "            game_phys >= 0x{:08X}u && game_phys <= 0x{:08X}u &&\n",
            addr_model().rom_keyed_ram_lo(), addr_model().rom_keyed_ram_hi_incl());
        out += "            psx_game_address_in_text(addr) && dirty_ram_text_native_ok(game_phys);\n";
    } else {
        out += "        int game_text_in_shell_window = 0;\n";
    }
    out += "        if (!found && (game_shell_overlap || game_text_in_shell_window ||\n";
    out += "            (psx_game_address_in_text(addr) && dirty_ram_is_dirty(game_phys)))) {\n";
    out += "            found = dirty_ram_dispatch(cpu, addr, stop_addr);\n";
    out += "        }\n";
    out += "#endif\n";
    out += "        uint32_t phys = normalize(addr);\n";
    out += "        if (!found) {\n";
    out += "        while (lo <= hi) {\n";
    out += "            int mid = (lo + hi) / 2;\n";
    out += "            if (dispatch_table[mid].addr == phys) {\n";
    if (addr_model().has_kbless()) {
        out += fmt::format(
        "                /* Kernel-image bless guard (CLAUDE.md Rule 18). The keys in\n"
        "                 * the relocated kernel window [0x{:X},0x{:X}) were compiled\n",
            addr_model().kbless_ram_lo(), addr_model().kbless_ram_hi());
        out += "                 * from the ROM source of the BIOS's boot-time kernel copy —\n";
        out += "                 * but the BIOS (and games) PATCH kernel RAM at runtime (pad/\n";
        out += "                 * SIO installs land inside compiled bodies). A static hit\n";
        out += "                 * here may only run if the live RAM bytes of everything the\n";
        out += "                 * function can execute still byte-match the ROM image\n";
        out += "                 * (memory.c psx_kernel_bless_dispatchable, lazily verified,\n";
        out += "                 * invalidated on writes). Mismatched/unverifiable bodies\n";
        out += "                 * fall through to the faithful dirty-RAM interpreter below.\n";
        out += "                 * Non-kernel keys are unaffected. */\n";
        out += fmt::format(
        "                if (phys - 0x{:X}u < 0x{:X}u &&\n",
            addr_model().kbless_ram_lo(),
            addr_model().kbless_ram_hi() - addr_model().kbless_ram_lo());
        out += "                    !psx_kernel_bless_dispatchable(phys))\n";
        out += "                    break; /* found stays 0 -> dirty_ram_dispatch */\n";
    }
    out += "                g_debug_current_func_addr = phys;\n";
    out += "                debug_server_trace_dispatch(phys);\n";
    out += "                dispatch_table[mid].func(cpu);\n";
    out += "                g_dispatch_static_hits++;\n";
    out += "                found = 1;\n";
    out += "                break;\n";
    out += "            } else if (dispatch_table[mid].addr < phys) {\n";
    out += "                lo = mid + 1;\n";
    out += "            } else {\n";
    out += "                hi = mid - 1;\n";
    out += "            }\n";
    out += "        }\n";
    out += "        }\n";
    out += "        /* Static dispatch miss.  Self-modifying / install-at-runtime RAM\n";
    out += "         * (CLAUDE.md Rule 18): the BIOS writes dispatch stubs into kernel\n";
    out += "         * RAM at runtime.  If the target page has been written-to since\n";
    out += "         * boot, interpret the basic block on cpu state.  Falls back to\n";
    out += "         * psx_unknown_dispatch for genuinely unmapped PCs. */\n";
    out += "        if (!found) {\n";
    out += "            if (dirty_ram_dispatch(cpu, addr, stop_addr)) {\n";
    out += "                found = 1;\n";
    out += "            } else {\n";
    out += "                psx_unknown_dispatch(cpu, addr, phys);\n";
    out += "            }\n";
    out += "        }\n";
    out += "        /* Fix B: if the function just RETURNED via the recompiled exception\n";
    out += "         * return (jr $k0; rfe) inside the synchronous handler, cpu->pc now holds\n";
    out += "         * the real resume EPC and the RFE armed the escape — unwind to\n";
    out += "         * psx_check_interrupts here. A fiber/thread resume (in_exception==0) is a\n";
    out += "         * no-op, so the real EPC keeps dispatching. */\n";
    out += "        psx_rfe_escape_check(cpu);\n";
    out += "        if (g_psx_call_bail) {\n";
    out += "            /* A nested generated frame began a bail unwind; cpu->pc\n";
    out += "             * holds the guest's true target.  Resolve here iff the\n";
    out += "             * wild flow arrived exactly at this call's contract. */\n";
    out += "            if (stop_addr != 0 &&\n";
    out += "                ((cpu->pc ^ stop_addr) & 0x1FFFFFFFu) == 0 &&\n";
    out += "                cpu->gpr[29] == sp_at_call) {\n";
    out += "                g_psx_call_bail = 0;\n";
    out += "                g_psx_bail_resolved++;\n";
    out += "                cpu->pc = 0;\n";
    out += "                --g_psx_dispatch_depth;\n";
    out += "                if (outermost) {\n";
    out += "                    psx_dispatch_check_return_boundary(cpu, stop_addr);\n";
    out += "                }\n";
    out += "                return;\n";
    out += "            }\n";
    out += "            if (!outermost) {\n";
    out += "                --g_psx_dispatch_depth;\n";
    out += "                return;  /* propagate to the enclosing call site */\n";
    out += "            }\n";
    out += "            /* Outermost: flatten — host stack above is clean, keep\n";
    out += "             * executing the wild flow as a tail dispatch. */\n";
    out += "            g_psx_call_bail = 0;\n";
    out += "            g_psx_bail_flattened++;\n";
    out += "            addr = cpu->pc;\n";
    out += "            continue;\n";
    out += "        }\n";
    out += "        if (cpu->pc == 0) {\n";
    out += "            if (stop_addr != 0 &&\n";
    out += "                (cpu->gpr[29] != sp_at_call ||\n";
    out += "                 ((cpu->gpr[31] ^ stop_addr) & 0x1FFFFFFFu) != 0)) {\n";
    out += "                /* Callee C-returned but the guest did not return to\n";
    out += "                 * this call site ($ra holds the wild jr's target):\n";
    out += "                 * begin the bail unwind instead of resuming the\n";
    out += "                 * suspended C continuation. */\n";
    out += "                g_psx_call_bail = 1;\n";
    out += "                g_psx_bail_first++;\n";
    out += "                cpu->pc = cpu->gpr[31];\n";
    out += "                if (outermost) {\n";
    out += "                    g_psx_call_bail = 0;\n";
    out += "                    g_psx_bail_flattened++;\n";
    out += "                    addr = cpu->pc;\n";
    out += "                    continue;\n";
    out += "                }\n";
    out += "                --g_psx_dispatch_depth;\n";
    out += "                return;\n";
    out += "            }\n";
    out += "            --g_psx_dispatch_depth;\n";
    out += "            if (outermost) {\n";
    out += "                psx_dispatch_check_return_boundary(cpu, stop_addr);\n";
    out += "            }\n";
    out += "            return;\n";
    out += "        }\n";
    out += "        if (stop_addr != 0 && cpu->pc == stop_addr) {\n";
    out += "            /* An IRQ taken at a polled call's return boundary swaps sp\n";
    out += "             * to/from the kernel exception/scratchpad stack, so a\n";
    out += "             * LEGITIMATE return ($ra == stop_addr) can arrive with sp\n";
    out += "             * differing from sp_at_call in EITHER direction (born on the\n";
    out += "             * task stack / returning on the exception stack, and vice\n";
    out += "             * versa). The strict gate misread that as recursion and\n";
    out += "             * re-dispatched the same delivery forever until the depth\n";
    out += "             * guard tripped (MoH, MoH Underground, Driver 2 mission\n";
    out += "             * freezes). Recognize the straddle ONLY when $ra matches and\n";
    out += "             * exactly one side is on the exception stack; recursion\n";
    out += "             * (different $ra) and same-stack wild arrivals keep the\n";
    out += "             * strict gate (Tomba Bug D unaffected). */\n";
    out += "            uint32_t sp0_ = sp_at_call, sp1_ = cpu->gpr[29];\n";
    out += "            int sp0_exc_ = ((uint32_t)(sp0_ - 0x1F800000u) < 0x400u);\n";
    out += "            int sp1_exc_ = ((uint32_t)(sp1_ - 0x1F800000u) < 0x400u);\n";
    out += "            int exc_sp_straddle_ =\n";
    out += "                (((cpu->gpr[31] ^ stop_addr) & 0x1FFFFFFFu) == 0) &&\n";
    out += "                (sp0_exc_ != sp1_exc_);\n";
    out += "            if (cpu->gpr[29] != sp_at_call && !exc_sp_straddle_) {\n";
    out += "                /* Same address, different frame (recursion or a wild\n";
    out += "                 * arrival): not this call's return — keep executing\n";
    out += "                 * via tail dispatch (interior alias route). */\n";
    out += "                addr = cpu->pc;\n";
    out += "                continue;\n";
    out += "            }\n";
    out += "            cpu->pc = 0;\n";
    out += "            --g_psx_dispatch_depth;\n";
    out += "            if (outermost) {\n";
    out += "                psx_dispatch_check_return_boundary(cpu, stop_addr);\n";
    out += "            }\n";
    out += "            return;\n";
    out += "        }\n";
    out += "        addr = cpu->pc;  /* tail call: re-dispatch */\n";
    out += "    }\n";
    out += "}\n\n";
    out += "void psx_dispatch(CPUState* cpu, uint32_t addr) {\n";
    out += "    psx_dispatch_impl(cpu, addr, 0);\n";
    out += "}\n\n";
    out += "void psx_dispatch_call(CPUState* cpu, uint32_t addr, uint32_t return_addr) {\n";
    out += "    psx_dispatch_impl(cpu, addr, return_addr);\n";
    out += "}\n";

    if (cps) {
        // RECURSION_BUG.md §25 — mark CPS mode at startup for runtime code that
        // routes CPS continuations (overlay_loader.c).
        out += "\n/* CPS runtime-mode marker (the overlay loader reads g_psx_cps_mode). */\n";
        out += "static void psx_cps_mark_bios(void) {\n";
        out += "    extern int g_psx_cps_mode; g_psx_cps_mode = 1;\n";
        out += "}\n";
        // Run psx_cps_mark_bios before main(). __attribute__((constructor)) is
        // GCC/Clang-only; MSVC uses a static initializer pointer in .CRT$XCU.
        out += "#if defined(_MSC_VER)\n";
        out += "#pragma section(\".CRT$XCU\", read)\n";
        out += "__declspec(allocate(\".CRT$XCU\")) static void (*psx_cps_mark_bios_ctor)(void) = psx_cps_mark_bios;\n";
        out += "#else\n";
        out += "__attribute__((constructor)) static void psx_cps_mark_bios_ctor(void) { psx_cps_mark_bios(); }\n";
        out += "#endif\n";
    }

    // --- Backend descriptor (runtime/include/psx_bios_backend.h) ---
    // The ONE symbol this file exports. Everything else it defines is
    // static or stem-prefixed, so a second recompiled BIOS can be linked
    // alongside and chosen at runtime. The runtime routes psx_dispatch(),
    // psx_dispatch_call() and psx_bios_image through the selected backend,
    // which is why adding one changed no call sites.
    out += fmt::format(
        "\n/* Backend descriptor: the one exported symbol of this image. */\n"
        "const PsxBiosBackend {0}psx_bios_backend = {{" "\n"
        "    &{0}psx_bios_image," "\n"
        "    {0}psx_dispatch," "\n"
        "    {0}psx_dispatch_call," "\n"
        "    {0}psx_bios_kernel_bodies," "\n"
        "    {0}psx_bios_kernel_body_count," "\n"
        "}};" "\n",
        g_sym_prefix);
}

// ---------------------------------------------------------------------------
// emit: top-level entry point
// ---------------------------------------------------------------------------

EmitStats FullFunctionEmitter::emit(
    const std::vector<uint8_t>&       rom,
    uint32_t                          base_addr,
    uint32_t                          rom_end,
    const DiscoveryResult&            dr,
    const std::string&                bios_sha256,
    const std::string&                out_dir,
    const std::string&                out_stem,
    const std::vector<BiosVectorTable>& bios_vectors,
    const std::vector<BiosAlias>&       bios_aliases)
{
    EmitStats stats;

    // Setup / RetComM zips omit generated/; create it before ofstream.
    if (!out_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        if (ec) {
            throw std::runtime_error(
                fmt::format("cannot create out_dir {}: {}", out_dir, ec.message()));
        }
    }

    // Namespace every symbol this emitter defines under the image's stem, so
    // two recompiled BIOSes can coexist in one binary (see g_sym_prefix).
    g_sym_prefix = out_stem + "_";

    // Build the set of all known function entry addresses (normalized).
    std::set<uint32_t> all_function_entries_norm;
    for (const auto& fn : dr.functions) {
        all_function_entries_norm.insert(fn.normalized_addr);
    }

    // Build a map from entry_addr to DiscoveredFunction index.
    std::map<uint32_t, size_t> entry_to_idx;
    for (size_t i = 0; i < dr.functions.size(); ++i) {
        entry_to_idx[dr.functions[i].entry_addr] = i;
    }

    // Compute hard caps for each function (same logic as discovery pipeline).
    // A function's hard cap is the entry address of the next function at a
    // higher address within the same region.
    std::vector<uint32_t> sorted_entries;
    for (const auto& fn : dr.functions) {
        sorted_entries.push_back(fn.entry_addr);
    }
    std::sort(sorted_entries.begin(), sorted_entries.end());

    std::map<uint32_t, uint32_t> hard_caps;
    for (size_t i = 0; i < sorted_entries.size(); ++i) {
        if (i + 1 < sorted_entries.size()) {
            hard_caps[sorted_entries[i]] = sorted_entries[i + 1];
        } else {
            hard_caps[sorted_entries[i]] = rom_end + 1;
        }
    }

    // Emit <stem>_full.c
    std::string full_c;
    full_c += "/* AUTO-GENERATED by psxrecomp-bios --emit-full. DO NOT EDIT.\n";
    full_c += " *\n";
    full_c += fmt::format(" * BIOS SHA256: {}\n", bios_sha256);
    full_c += fmt::format(" * Functions: {}\n", dr.functions.size());
    full_c += " */\n\n";
    // Namespace the two externally-visible dispatch entry points to this
    // image. They are referenced from ~20 emit sites and from the sibling
    // generated file, so renaming them in the preamble is both smaller and
    // harder to get wrong than rewriting each reference. The runtime keeps
    // the unprefixed names as thin forwarders to the ACTIVE bios, which is
    // what the game's generated C calls. Everything else this file defines
    // is either static (file-local, cannot collide) or already stem-prefixed.
    full_c += fmt::format("#define psx_dispatch      {}psx_dispatch\n", g_sym_prefix);
    full_c += fmt::format("#define psx_dispatch_call {}psx_dispatch_call\n\n", g_sym_prefix);
    full_c += "#include \"cpu_state.h\"\n\n";

    // Forward declare psx_dispatch, psx_unknown_dispatch, and interrupt check.
    full_c += "extern void psx_dispatch(CPUState* cpu, uint32_t addr);\n";
    full_c += "extern void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys);\n";
    full_c += "extern void psx_check_interrupts(CPUState* cpu);\n";
    full_c += "extern void psx_check_interrupts_at(CPUState* cpu, uint32_t resume_pc);\n";
    full_c += "extern void psx_restore_state_escape(void);\n";
    full_c += "extern void gte_execute(CPUState* cpu, uint32_t cmd);\n";
    full_c += "extern void gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);\n";
    full_c += "extern uint32_t gte_read_data(CPUState* cpu, uint8_t reg);\n";
    full_c += "extern void debug_server_log_probe(uint32_t pc, CPUState *cpu);\n";
    full_c += "#ifdef PSX_COSIM\n";
    full_c += "extern void cosim_block(uint32_t block_leader_phys);\n";
    full_c += "extern void cosim_instr(uint32_t pc);\n";
    full_c += "#endif\n";
    full_c += "#ifndef PSX_NO_DEBUG_TOOLS\n";
    full_c += "extern void debug_server_log_call_entry(uint32_t func_addr);\n";
    full_c += "extern void debug_server_cyc_observe(uint32_t block_leader_phys);\n";
    full_c += "#endif\n";
    full_c += "extern uint32_t g_debug_last_store_pc;\n";
    full_c += "/* Phase 1.0e-d: per-block guest cycle accounting.\n";
    full_c += " * Compile generated code with -DPSX_ENABLE_BLOCK_CYCLES=1 to\n";
    full_c += " * activate cycle advancement at every block leader. */\n";
    full_c += "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
    full_c += "extern void psx_advance_cycles(uint32_t cycles);\n";
    full_c += "#endif\n\n";

    // Forward declare ALL functions so intra-file calls resolve.
    for (const auto& fn : dr.functions) {
        full_c += fmt::format("void {}(CPUState* cpu);\n", fn_sym(fn.normalized_addr));
    }
    full_c += "\n";

    std::set<uint32_t> emitted_normalized;
    std::vector<ContinuationLabel> all_continuations;

    // ---- PASS 1: dry run to collect cross-function tail-call targets ----
    std::map<uint32_t, std::set<uint32_t>> cross_targets_by_parent;
    {
        std::vector<ContinuationLabel> dry_run_continuations;
        std::vector<ContinuationLabel> dry_run_cross;
        /* Instruction-address -> owning function (normalized). Hard caps
         * partition the address space, so each walked instruction has exactly
         * one owner. Used below to route cross-function targets to the
         * function that actually CONTAINS them: the range-based parent guess
         * (largest entry <= target) is wrong when seeds split a function into
         * fragments and the preceding fragment ends before the target —
         * emit_function's injection then silently dropped the continuation
         * and the runtime FAIL-FASTed on dispatch (OpenBIOS exceptionHandler
         * priority_loop 0xBFC208E8, split by the patch-slot code_ptr seeds). */
        std::map<uint32_t, uint32_t> insn_owner;
        for (const auto& fn : dr.functions) {
            std::string lineage = fn.discovered_by;
            uint32_t cap = hard_caps.count(fn.entry_addr)
                               ? hard_caps[fn.entry_addr]
                               : rom_end + 1;
            FunctionDiscovery::SingleFunctionResult sfr =
                FunctionDiscovery::walk_function(rom, base_addr, rom_end, fn.entry_addr, cap, lineage);
            if (!sfr.unsupported.empty()) continue;
            for (const auto& [iaddr, iraw] : sfr.instructions) {
                insn_owner[iaddr] = fn.normalized_addr;
            }
            std::string tmp;
            std::set<uint32_t> empty;
            (void)emit_function(tmp, fn, sfr, all_function_entries_norm, rom, base_addr, rom_end,
                                dry_run_continuations, empty, dry_run_cross, nullptr);
        }
        for (const auto& cl : dry_run_cross) {
            auto ow = insn_owner.find(cl.rom_addr);
            if (ow != insn_owner.end()) {
                cross_targets_by_parent[ow->second].insert(cl.rom_addr);
            } else {
                /* No emitted function contains this published target: a
                 * runtime dispatch of it would FAIL-FAST. Discovery's
                 * branch-target closure covers direct branches; anything
                 * reaching here is a skipped-function target or a genuinely
                 * new hole — surface it at build time. */
                std::fprintf(stderr,
                    "psxrecomp-bios: WARNING: cross-function target 0x%08X "
                    "(parent guess 0x%08X) is in no emitted function — "
                    "dispatching it at runtime will fail\n",
                    cl.rom_addr, cl.parent_func_norm);
            }
        }
    }

    // ---- PASS 2: real emission with injected cross-targets ----
    for (const auto& fn : dr.functions) {
        // Re-walk the function to get raw instructions.
        std::string lineage = fn.discovered_by;
        uint32_t cap = hard_caps.count(fn.entry_addr)
                           ? hard_caps[fn.entry_addr]
                           : rom_end + 1;

        FunctionDiscovery::SingleFunctionResult sfr =
            FunctionDiscovery::walk_function(rom, base_addr, rom_end, fn.entry_addr, cap, lineage);

        // Check for unsupported instructions (FPU functions).
        if (!sfr.unsupported.empty()) {
            std::string reason = sfr.unsupported[0].reason;
            stats.skipped.emplace_back(fn.entry_addr, reason);
            stats.functions_skipped++;
            continue;
        }

        std::set<uint32_t> injected;
        if (cross_targets_by_parent.count(fn.normalized_addr)) {
            injected = cross_targets_by_parent[fn.normalized_addr];
        }

        std::vector<ContinuationLabel> discard_cross;  /* PASS 2 cross is unused */
        std::string interpreter_reason;
        bool ok = emit_function(full_c, fn, sfr, all_function_entries_norm, rom, base_addr, rom_end,
                                all_continuations, injected, discard_cross, &interpreter_reason);
        if (!ok) {
            if (!interpreter_reason.empty()) {
                stats.interpreted.emplace_back(fn.entry_addr, interpreter_reason);
                stats.functions_interpreted++;
                continue;
            }
            stats.skipped.emplace_back(fn.entry_addr, "emit_function failed");
            stats.functions_skipped++;
            continue;
        }

        emitted_normalized.insert(fn.normalized_addr);
        stats.functions_emitted++;
        stats.total_instructions += static_cast<uint32_t>(sfr.instructions.size());
    }

    stats.dispatch_entries = static_cast<uint32_t>(emitted_normalized.size());

    // Emit fatal stubs for skipped functions (e.g. FPU) so calls to them
    // link but abort at runtime with a diagnostic.
    for (const auto& [skip_addr, reason] : stats.skipped) {
        uint32_t skip_norm = normalize_address(skip_addr);
        full_c += fmt::format("void {}(CPUState* cpu) {{\n", fn_sym(skip_norm));
        full_c += fmt::format("    psx_unknown_dispatch(cpu, 0x{:08X}u, 0x{:08X}u);\n",
                              skip_addr, skip_norm);
        full_c += "}\n\n";
        emitted_normalized.insert(skip_norm);
        stats.dispatch_entries++;
    }

    // --- Emit continuation wrappers ---
    // Deduplicate continuations by norm_addr (same label from multiple callers).
    std::map<uint32_t, ContinuationLabel> unique_continuations;
    for (const auto& cl : all_continuations) {
        // Only add if not already a function entry (shouldn't be, but guard).
        if (!emitted_normalized.count(cl.norm_addr)) {
            unique_continuations[cl.norm_addr] = cl;
        }
    }

    if (!unique_continuations.empty()) {
        full_c += fmt::format("\n/* --- {} continuation wrappers for jal/jalr return routing --- */\n\n",
                              unique_continuations.size());
        for (const auto& [cnorm, cl] : unique_continuations) {
            // Wrapper: sets cpu->pc to the ROM label address so the parent's
            // entry-switch routes to the correct goto label.
            full_c += fmt::format("void {}(CPUState* cpu) {{\n",
                                  cont_sym(cl.parent_func_norm, cl.rom_addr));
            full_c += fmt::format("    cpu->pc = 0x{:08X}u;\n", cl.rom_addr);
            full_c += fmt::format("    {}(cpu);\n", fn_sym(cl.parent_func_norm));
            full_c += "}\n\n";
            emitted_normalized.insert(cnorm);
        }
        stats.continuation_entries = static_cast<uint32_t>(unique_continuations.size());
        stats.dispatch_entries += stats.continuation_entries;
    }

    // Write <stem>_full.c
    {
        std::string path = out_dir + "/" + out_stem + "_full.c";
        write_file_if_changed(path, full_c);
    }

    // Emit and write <stem>_dispatch.c
    {
        std::string dispatch_c;
        emit_dispatch(dispatch_c, dr, emitted_normalized, unique_continuations,
                      bios_sha256, rom, base_addr, bios_vectors, bios_aliases);
        std::string path = out_dir + "/" + out_stem + "_dispatch.c";
        write_file_if_changed(path, dispatch_c);
    }

    // Write <stem>_skipped_functions.json. Stemmed like the C outputs: two
    // BIOS images regenerated into the same out_dir must not clobber each
    // other's skip report.
    if (!stats.skipped.empty()) {
        std::string json = "[\n";
        for (size_t i = 0; i < stats.skipped.size(); ++i) {
            json += fmt::format("  {{\"address\": \"0x{:08X}\", \"reason\": \"{}\"}}",
                                stats.skipped[i].first, stats.skipped[i].second);
            if (i + 1 < stats.skipped.size()) json += ",";
            json += "\n";
        }
        json += "]\n";
        std::string path = out_dir + "/" + out_stem + "_skipped_functions.json";
        write_file_if_changed(path, json);
    }

    // Fail-closed functions are deliberately absent from native dispatch so
    // the existing dispatch miss path executes their live bytes through the
    // interpreter. Keep this separate from skipped_functions.json: skipped
    // functions receive fatal diagnostic stubs, while these remain runnable.
    if (!stats.interpreted.empty()) {
        std::string json = "[\n";
        for (size_t i = 0; i < stats.interpreted.size(); ++i) {
            json += fmt::format("  {{\"address\": \"0x{:08X}\", \"reason\": \"{}\"}}",
                                stats.interpreted[i].first, stats.interpreted[i].second);
            if (i + 1 < stats.interpreted.size()) json += ",";
            json += "\n";
        }
        json += "]\n";
        std::string path = out_dir + "/" + out_stem + "_interpreted_functions.json";
        write_file_if_changed(path, json);
    }

    return stats;
}

} // namespace PSXRecompV4
