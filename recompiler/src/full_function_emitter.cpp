// full_function_emitter.cpp — Phase 2 full BIOS C emitter.
// See full_function_emitter.h for the design contract.

#include "full_function_emitter.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "fmt/format.h"
#include "mips_decoder.h"
#include "strict_translator.h"
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

uint32_t FullFunctionEmitter::normalize_address(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    /* Kernel Part 2: ROM 0x1FC10000+ → RAM 0x500+ */
    if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
        phys = phys - 0x1FC10000u + 0x00000500u;
    }
    /* Shell: RAM 0x30000+ → ROM physical 0x1FC18000+ */
    if (phys >= 0x00030000u && phys <= 0x0005AFFFu) {
        phys = phys - 0x00030000u + 0x1FC18000u;
    }
    return phys;
}

static uint32_t ram_alias_to_rom(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x00000500u && phys < 0x00008500u) {
        return 0xBFC10000u + (phys - 0x00000500u);
    }
    if (phys >= 0x00030000u && phys <= 0x0005AFFFu) {
        return 0xBFC18000u + (phys - 0x00030000u);
    }
    return addr;
}

/* Map a BIOS ROM PC to the address the PSX CPU actually executes from after the
 * BIOS copies kernel/shell code into RAM. Literal interrupt resume PCs must use
 * this runtime address; otherwise psx_check_interrupts rejects ROM-shell EPCs
 * and falls back to the legacy sentinel path. */
static uint32_t bios_runtime_pc(uint32_t rom_pc) {
    uint32_t phys = rom_pc & 0x1FFFFFFFu;
    if (phys >= 0x1FC18000u && phys <= 0x1FC42FFFu) {
        return 0x80030000u + (phys - 0x1FC18000u);
    }
    if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
        return phys - 0x1FC10000u + 0x00000500u;
    }
    return rom_pc;
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
    std::vector<ContinuationLabel>& out_cross_targets)
{
    const uint32_t norm = func.normalized_addr;

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
        return indent + fmt::format("psx_check_interrupts_at(cpu, 0x{:08X}u);\n",
                                    bios_runtime_pc(resume_pc));
    };
    auto emit_irq_check_expr = [](const std::string& resume_pc_expr,
                                  const std::string& indent = "    ") {
        return indent + fmt::format("psx_check_interrupts_at(cpu, {});\n", resume_pc_expr);
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
    // Shadow 'out' with a reference to 'body' so all existing out+= lines
    // write to the temporary buffer.  The real 'out' is assembled at the end.
    std::string& func_out = body;
    #define out func_out

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

    // For relocated code (ROM 0xBFC10000+ → RAM 0x500+), J/JAL targets
    // depend on the upper 4 bits of the PC. The code runs at RAM addresses,
    // not ROM addresses, so we must fix J/JAL targets in the branch resolution.
    auto relocate_j_target = [](uint32_t rom_addr, uint32_t target) -> uint32_t {
        uint32_t phys = rom_addr & 0x1FFFFFFFu;
        /* Kernel Part 2: ROM runs at RAM 0x500+ */
        if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
            uint32_t runtime_addr = phys - 0x1FC10000u + 0x00000500u;
            uint32_t target26 = target & 0x0FFFFFFFu;
            return (runtime_addr & 0xF0000000u) | target26;
        }
        /* Shell: ROM runs at RAM 0x80030000+ */
        if (phys >= 0x1FC18000u && phys <= 0x1FC42FFFu) {
            uint32_t runtime_addr = 0x80030000u + (phys - 0x1FC18000u);
            uint32_t target26 = target & 0x0FFFFFFFu;
            return (runtime_addr & 0xF0000000u) | target26;
        }
        return target;
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
    for (const auto& [ds_addr, pb] : pending_at) {
        branch_decls += fmt::format("    int psx_delay_{:08X} = 0;\n", pb.terminator_addr);
        if (is_branch_kind(pb.kind.c_str())) {
            branch_decls += fmt::format("    int psx_taken_{:08X} = 0;\n", pb.terminator_addr);
        }
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
            }
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
                } else if (is_branch_kind(kind.c_str())) {
                    // Inline the orphaned delay slot from rom.
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
                    // Resolve branch.  Target was decoded as absolute virtual addr.
                    uint32_t target = tr.terminator_target;
                    register_cross_function_target(target);
                    out += fmt::format("    if (psx_taken_{:08X}) {{\n", addr);
                    out += emit_irq_check(target, "        ");
                    out += fmt::format("        cpu->pc = 0x{:08X}u; return;\n", target);
                    out += "    }\n";
                    // Not-taken: dispatch to delay-slot address (next function's entry).
                    // The next function's prologue will re-execute the delay slot — that's
                    // a duplicate side effect we accept for correctness on the not-taken
                    // path. (For unconditional branches like beq $zero,$zero this path
                    // is structurally dead.)
                    register_cross_function_target(ds_addr);
                    out += emit_irq_check(ds_addr);
                    out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", ds_addr);
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
                    // Inline orphaned delay slot, set $ra, dispatch to target.
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
                    uint32_t return_addr = relocate_ra(addr + 8);
                    if (cps) {
                        if (addr_to_raw.count(addr + 8)) {
                            uint32_t cn = normalize_address(addr + 8);
                            if (!all_function_entries_norm.count(cn))
                                local_continuations.push_back({addr + 8, cn, norm});
                        } else {
                            register_cross_function_target(addr + 8);
                        }
                        out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;\n", return_addr);
                        out += emit_irq_check(target);
                        out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", target);
                    } else {
                        out += "    { uint32_t _csp = cpu->gpr[29];\n";
                        out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;\n", return_addr);
                        out += emit_irq_check(target);
                        out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);\n", target);
                        out += fmt::format("    if (psx_call_contract(cpu, 0x{:08X}u, _csp)) return; }}\n",
                                           return_addr);
                        out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", return_addr);
                    }
                } else if (kind == "jalr") {
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
                    uint8_t rs = (raw >> 21) & 0x1F;
                    uint8_t rd = (raw >> 11) & 0x1F;
                    uint32_t return_addr = relocate_ra(addr + 8);
                    if (cps) {
                        if (addr_to_raw.count(addr + 8)) {
                            uint32_t cn = normalize_address(addr + 8);
                            if (!all_function_entries_norm.count(cn))
                                local_continuations.push_back({addr + 8, cn, norm});
                        } else {
                            register_cross_function_target(addr + 8);
                        }
                        out += fmt::format("    {{ uint32_t _t = cpu->gpr[{}];\n",
                                           static_cast<int>(rs));
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
                        out += emit_irq_check_expr(fmt::format("cpu->gpr[{}]", static_cast<int>(rs)));
                        out += fmt::format("    psx_dispatch(cpu, cpu->gpr[{}]);\n",
                                           static_cast<int>(rs));
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
        // Add new install slot PCs here as they're discovered.  See
        // docs/dynamic_handler_install.md for how to find them.
        auto rom_to_ram = [](uint32_t rom_addr) -> uint32_t {
            uint32_t phys = rom_addr & 0x1FFFFFFFu;
            if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
                return phys - 0x1FC10000u + 0x00000500u;
            }
            return phys;
        };
        uint32_t ram_pc = rom_to_ram(addr);
        bool is_install_slot =
            (ram_pc == 0x00000CF0u);  /* SIO data-byte handler dispatch slot */
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

        // Non-terminator: emit normally.
        out += fmt::format("    /* 0x{:08X}: {:08X}  {} */\n", addr, raw, tr.comment);
        out += fmt::format("    {}\n", tr.c_code);
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
                    register_cross_function_target(target);
                    out += fmt::format("    if (psx_taken_{:08X}) {{\n", pb.terminator_addr);
                    out += emit_irq_check(target, "        ");
                    out += fmt::format("        cpu->pc = 0x{:08X}u; return;\n", target);
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
                if (cps) {
                    // CPS jalr: capture the target register BEFORE writing the
                    // link reg (rd==rs alias-safe), then tail-transfer.
                    out += fmt::format("    {{ uint32_t _t = cpu->gpr[{}];\n",
                                       static_cast<int>(rs));
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
                    out += emit_irq_check_expr(fmt::format("cpu->gpr[{}]", static_cast<int>(rs)));
                    out += fmt::format("    psx_dispatch(cpu, cpu->gpr[{}]);\n",
                                       static_cast<int>(rs));
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
                if (rs == 31) {
                    if (ra_loaded_from_non_sp)
                        out += emit_irq_check_expr("cpu->gpr[31]") +
                               "    cpu->pc = cpu->gpr[31]; psx_restore_state_escape(); return;  /* longjmp-return */\n";
                    else if (cps)
                        out += emit_irq_check_expr("cpu->gpr[31]") +
                               "    cpu->pc = cpu->gpr[31]; return;  /* CPS: publish $ra for trampoline dispatch */\n";
                    else
                        out += emit_irq_check_expr("cpu->gpr[31]") +
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
                                out += fmt::format("    switch (cpu->gpr[{}]) {{\n",
                                                   static_cast<int>(jr_rs));
                                for (auto& [rt, rom_t] : targets) {
                                    out += fmt::format("        case 0x{:08X}u:\n", rt);
                                    out += emit_irq_check(rom_t, "            ");
                                    out += fmt::format("            goto label_{:08X};\n", rom_t);
                                }
                                out += "        default:\n";
                                out += emit_irq_check_expr(fmt::format("cpu->gpr[{}]", static_cast<int>(jr_rs)),
                                                           "            ");
                                out += fmt::format("            cpu->pc = cpu->gpr[{}]; return;\n",
                                                   static_cast<int>(jr_rs));
                                out += "    }\n";
                                emitted_switch = true;
                            }
                        }
                    }
                    if (!emitted_switch) {
                        // Tail call: set cpu->pc and return; dispatch loop re-dispatches.
                        out += emit_irq_check_expr(fmt::format("cpu->gpr[{}]", static_cast<int>(rs)));
                        out += fmt::format("    cpu->pc = cpu->gpr[{}]; return;\n",
                                           static_cast<int>(rs));
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
    out += fmt::format("void func_{:08X}(CPUState* cpu) {{\n", norm);
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
    out += "#include \"cpu_state.h\"\n";
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
            out += fmt::format("extern void func_{:08X}_cont_{:08X}(CPUState* cpu);\n",
                               cl.parent_func_norm, cl.rom_addr);
        } else {
            out += fmt::format("extern void func_{:08X}(CPUState* cpu);\n", norm);
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
        out += fmt::format("/* BIOS fixed alias: 0x{:08X} -> func_{:08X} */\n",
                           ba.ram_addr, ba.target_key);
        out += fmt::format("static void bios_alias_{:08X}(CPUState* cpu) "
                           "{{ func_{:08X}(cpu); }}\n\n",
                           ba.ram_addr, ba.target_key);
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
                    out += fmt::format("        /* case 0x{:02X}: func_{:08X} — "
                                       "not in dispatch table, skipped */\n", i, key);
                    continue;
                }
                out += fmt::format("        case 0x{:02X}: func_{:08X}(cpu); return;\n",
                                   i, key);
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
            out += fmt::format("    {{ 0x{:08X}u, func_{:08X}_cont_{:08X} }},\n",
                               norm, cl.parent_func_norm, cl.rom_addr);
        } else {
            out += fmt::format("    {{ 0x{:08X}u, func_{:08X} }},\n", norm, norm);
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
            if (norm < 0x500u || norm >= 0x8500u) continue;
            uint32_t owner = norm;
            if (continuations.count(norm))
                owner = continuations.at(norm).parent_func_norm;
            auto it = extent_by_norm.find(owner);
            if (it == extent_by_norm.end()) continue;
            uint32_t lo = it->second.first, hi = it->second.second;
            // Only bodies that live entirely inside the relocated window are
            // verifiable against the ROM source; skip anything straddling it.
            if (lo < 0x500u || hi > 0x8500u || lo >= hi) continue;
            kb += fmt::format("    {{ 0x{:08X}u, 0x{:08X}u, 0x{:08X}u }},\n",
                              norm, lo, hi);
            kb_count++;
        }
        out += "typedef struct {\n";
        out += "    uint32_t key;      /* dispatch key (RAM, normalized) */\n";
        out += "    uint32_t body_lo;  /* RAM extent of code reachable from key */\n";
        out += "    uint32_t body_hi;  /* exclusive */\n";
        out += "} PsxKernelBody;\n\n";
        out += fmt::format("const PsxKernelBody psx_bios_kernel_bodies[{}] = {{\n", kb_count);
        out += kb;
        out += "};\n";
        out += fmt::format("const uint32_t psx_bios_kernel_body_count = {}u;\n\n", kb_count);
    }

    // Dispatch function with binary search.
    out += "static uint32_t normalize(uint32_t addr) {\n";
    out += "    uint32_t phys = addr & 0x1FFFFFFFu;\n";
    out += "    /* Kernel Part 2: ROM 0x1FC10000+ -> RAM 0x500+ */\n";
    out += "    if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu)\n";
    out += "        phys = phys - 0x1FC10000u + 0x00000500u;\n";
    out += "    /* Shell: RAM 0x30000+ -> ROM physical 0x1FC18000+ */\n";
    out += "    if (phys >= 0x00030000u && phys <= 0x0005AFFFu)\n";
    out += "        phys = phys - 0x00030000u + 0x1FC18000u;\n";
    out += "    return phys;\n";
    out += "}\n\n";

    out += "extern int dirty_ram_dispatch(CPUState* cpu, uint32_t addr, uint32_t stop_addr);\n";
    out += "extern int dirty_ram_is_dirty(uint32_t phys);\n";
    out += "extern int psx_kernel_bless_dispatchable(uint32_t phys);\n";
    out += "extern void fntrace_record(CPUState* cpu, uint32_t target);\n";
    out += "extern uint64_t g_dispatch_static_hits;\n";
    out += "\n";
    out += "int g_psx_dispatch_depth = 0;\n\n";
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
    out += "        int game_shell_overlap = fntrace_is_game_started() &&\n";
    out += "            game_phys >= 0x00030000u && game_phys <= 0x0005AFFFu;\n";
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
    out += "        int game_text_in_shell_window =\n";
    out += "            game_phys >= 0x00030000u && game_phys <= 0x0005AFFFu &&\n";
    out += "            psx_game_address_in_text(addr) && dirty_ram_text_native_ok(game_phys);\n";
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
    out += "                /* Kernel-image bless guard (CLAUDE.md Rule 18). The keys in\n";
    out += "                 * the relocated kernel window [0x500,0x8500) were compiled\n";
    out += "                 * from the ROM source of the BIOS's boot-time kernel copy —\n";
    out += "                 * but the BIOS (and games) PATCH kernel RAM at runtime (pad/\n";
    out += "                 * SIO installs land inside compiled bodies). A static hit\n";
    out += "                 * here may only run if the live RAM bytes of everything the\n";
    out += "                 * function can execute still byte-match the ROM image\n";
    out += "                 * (memory.c psx_kernel_bless_dispatchable, lazily verified,\n";
    out += "                 * invalidated on writes). Mismatched/unverifiable bodies\n";
    out += "                 * fall through to the faithful dirty-RAM interpreter below.\n";
    out += "                 * Non-kernel keys are unaffected. */\n";
    out += "                if (phys - 0x500u < 0x8000u &&\n";
    out += "                    !psx_kernel_bless_dispatchable(phys))\n";
    out += "                    break; /* found stays 0 -> dirty_ram_dispatch */\n";
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
    out += "            if (cpu->gpr[29] != sp_at_call) {\n";
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
        // must emit the CPS contract (the overlay sljit JIT, overlay_sljit.c).
        out += "\n/* CPS runtime-mode marker (overlay sljit JIT reads g_psx_cps_mode). */\n";
        out += "__attribute__((constructor)) static void psx_cps_mark_bios(void) {\n";
        out += "    extern int g_psx_cps_mode; g_psx_cps_mode = 1;\n";
        out += "}\n";
    }
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
    const std::vector<BiosVectorTable>& bios_vectors,
    const std::vector<BiosAlias>&       bios_aliases)
{
    EmitStats stats;

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

    // Emit SCPH1001_full.c
    std::string full_c;
    full_c += "/* AUTO-GENERATED by psxrecomp-bios --emit-full. DO NOT EDIT.\n";
    full_c += " *\n";
    full_c += fmt::format(" * BIOS SHA256: {}\n", bios_sha256);
    full_c += fmt::format(" * Functions: {}\n", dr.functions.size());
    full_c += " */\n\n";
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
    full_c += "extern void debug_server_log_call_entry(uint32_t func_addr);\n";
    full_c += "extern void debug_server_log_probe(uint32_t pc, CPUState *cpu);\n";
    full_c += "#ifdef PSX_COSIM\n";
    full_c += "extern void cosim_block(uint32_t block_leader_phys);\n";
    full_c += "extern void cosim_instr(uint32_t pc);\n";
    full_c += "#endif\n";
    full_c += "#ifndef PSX_NO_DEBUG_TOOLS\n";
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
        full_c += fmt::format("void func_{:08X}(CPUState* cpu);\n", fn.normalized_addr);
    }
    full_c += "\n";

    std::set<uint32_t> emitted_normalized;
    std::vector<ContinuationLabel> all_continuations;

    // ---- PASS 1: dry run to collect cross-function tail-call targets ----
    std::map<uint32_t, std::set<uint32_t>> cross_targets_by_parent;
    {
        std::vector<ContinuationLabel> dry_run_continuations;
        std::vector<ContinuationLabel> dry_run_cross;
        for (const auto& fn : dr.functions) {
            std::string lineage = fn.discovered_by;
            uint32_t cap = hard_caps.count(fn.entry_addr)
                               ? hard_caps[fn.entry_addr]
                               : rom_end + 1;
            FunctionDiscovery::SingleFunctionResult sfr =
                FunctionDiscovery::walk_function(rom, base_addr, rom_end, fn.entry_addr, cap, lineage);
            if (!sfr.unsupported.empty()) continue;
            std::string tmp;
            std::set<uint32_t> empty;
            (void)emit_function(tmp, fn, sfr, all_function_entries_norm, rom, base_addr, rom_end,
                                dry_run_continuations, empty, dry_run_cross);
        }
        for (const auto& cl : dry_run_cross) {
            cross_targets_by_parent[cl.parent_func_norm].insert(cl.rom_addr);
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
        bool ok = emit_function(full_c, fn, sfr, all_function_entries_norm, rom, base_addr, rom_end,
                                all_continuations, injected, discard_cross);
        if (!ok) {
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
        full_c += fmt::format("void func_{:08X}(CPUState* cpu) {{\n", skip_norm);
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
            full_c += fmt::format("void func_{:08X}_cont_{:08X}(CPUState* cpu) {{\n",
                                  cl.parent_func_norm, cl.rom_addr);
            full_c += fmt::format("    cpu->pc = 0x{:08X}u;\n", cl.rom_addr);
            full_c += fmt::format("    func_{:08X}(cpu);\n", cl.parent_func_norm);
            full_c += "}\n\n";
            emitted_normalized.insert(cnorm);
        }
        stats.continuation_entries = static_cast<uint32_t>(unique_continuations.size());
        stats.dispatch_entries += stats.continuation_entries;
    }

    // Write SCPH1001_full.c
    {
        std::string path = out_dir + "/SCPH1001_full.c";
        std::ofstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error(fmt::format("cannot write {}", path));
        f.write(full_c.data(), static_cast<std::streamsize>(full_c.size()));
    }

    // Emit and write SCPH1001_dispatch.c
    {
        std::string dispatch_c;
        emit_dispatch(dispatch_c, dr, emitted_normalized, unique_continuations,
                      bios_sha256, rom, base_addr, bios_vectors, bios_aliases);
        std::string path = out_dir + "/SCPH1001_dispatch.c";
        std::ofstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error(fmt::format("cannot write {}", path));
        f.write(dispatch_c.data(), static_cast<std::streamsize>(dispatch_c.size()));
    }

    // Write skipped_functions.json
    if (!stats.skipped.empty()) {
        std::string json = "[\n";
        for (size_t i = 0; i < stats.skipped.size(); ++i) {
            json += fmt::format("  {{\"address\": \"0x{:08X}\", \"reason\": \"{}\"}}",
                                stats.skipped[i].first, stats.skipped[i].second);
            if (i + 1 < stats.skipped.size()) json += ",";
            json += "\n";
        }
        json += "]\n";
        std::string path = out_dir + "/skipped_functions.json";
        std::ofstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error(fmt::format("cannot write {}", path));
        f.write(json.data(), static_cast<std::streamsize>(json.size()));
    }

    return stats;
}

} // namespace PSXRecompV4
