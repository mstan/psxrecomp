// full_function_emitter.cpp — Phase 2 full BIOS C emitter.
// See full_function_emitter.h for the design contract.

#include "full_function_emitter.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "fmt/format.h"
#include "mips_decoder.h"
#include "strict_translator.h"

namespace PSXRecompV4 {

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
        if (phys >= 0x1FC18000u && phys <= 0x1FC427FFu) {
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
        uint32_t phys = rom_ra & 0x1FFFFFFFu;
        if (phys >= 0x1FC18000u && phys <= 0x1FC427FFu) {
            return 0x80030000u + (phys - 0x1FC18000u);
        }
        if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
            return phys - 0x1FC10000u + 0x00000500u;
        }
        return rom_ra;
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
            count++;
            ++walker;
            if (walker == addr_to_raw.end()) break;
            if (block_leaders.count(walker->first)) break;
        }
        block_cycles[leader] = count;
    }

    /* Emit branch-predicate variable declarations at function entry,
     * each initialized to 0.  Subsequent terminator-emit assigns instead of
     * declares, so a `goto label_X` that lands on the delay slot of a
     * preceding beq does NOT execute the dangling `if (psx_taken_X) goto Y`
     * with an indeterminate value (would be UB per C; on MinGW the stack
     * garbage was non-zero, causing a spurious branch — root cause of the
     * memcard COPY confirm-modal freeze in widget 0xB → BFC21800 loop). */
    for (const auto& [ds_addr, pb] : pending_at) {
        if (is_branch_kind(pb.kind.c_str())) {
            out += fmt::format("    int psx_taken_{:08X} = 0;\n", pb.terminator_addr);
        }
    }

    for (auto it = addr_to_raw.begin(); it != addr_to_raw.end(); ++it) {
        uint32_t addr = it->first;
        uint32_t raw = it->second;

        // Emit label if this is a block leader.
        // Include an interrupt check so tight loops (backward branches)
        // can service vblank and other hardware interrupts.
        if (block_leaders.count(addr)) {
            out += fmt::format("label_{:08X}:\n", addr);
            // Phase 1.0e-d: advance guest cycles for this block. Macro-
            // gated; when off, generated code matches pre-1.0e-d output.
            uint32_t bcyc = 0;
            auto bcit = block_cycles.find(addr);
            if (bcit != block_cycles.end()) bcyc = bcit->second;
            if (bcyc > 0) {
                out += "#ifdef PSX_ENABLE_BLOCK_CYCLES\n";
                out += fmt::format("    psx_advance_cycles({}u);\n", bcyc);
                out += "#endif\n";
            }
            out += "    psx_check_interrupts(cpu);\n";
        }

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
                        // Emit JR resolution.
                        uint8_t rs = (pb.raw >> 21) & 0x1F;
                        if (rs == 31) {
                            if (ra_loaded_from_non_sp)
                                out += "    cpu->pc = cpu->gpr[31]; psx_restore_state_escape(); return;  /* longjmp-return */\n";
                            else
                                out += "    return;\n";
                        } else {
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
                            out += "    cpu->pc = cpu->gpr[31]; psx_restore_state_escape(); return;  /* longjmp-return */\n";
                        else
                            out += "    return;\n";
                    } else {
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
                            out += fmt::format("    {}\n", ds_tr.c_code);
                        }
                    }
                    // Resolve branch.  Target was decoded as absolute virtual addr.
                    uint32_t target = tr.terminator_target;
                    register_cross_function_target(target);
                    out += fmt::format("    if (psx_taken_{:08X}) {{ cpu->pc = 0x{:08X}u; return; }}\n",
                                       addr, target);
                    // Not-taken: dispatch to delay-slot address (next function's entry).
                    // The next function's prologue will re-execute the delay slot — that's
                    // a duplicate side effect we accept for correctness on the not-taken
                    // path. (For unconditional branches like beq $zero,$zero this path
                    // is structurally dead.)
                    register_cross_function_target(ds_addr);
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
                            out += fmt::format("    {}\n", ds_tr.c_code);
                        }
                    }
                    uint32_t target = relocate_j_target(addr, tr.terminator_target);
                    register_cross_function_target(target);
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
                            out += fmt::format("    {}\n", ds_tr.c_code);
                        }
                    }
                    uint32_t target = relocate_j_target(addr, tr.terminator_target);
                    uint32_t return_addr = relocate_ra(addr + 8);
                    out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;\n", return_addr);
                    out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);\n", target);
                    out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", return_addr);
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
                            out += fmt::format("    {}\n", ds_tr.c_code);
                        }
                    }
                    uint8_t rs = (raw >> 21) & 0x1F;
                    uint8_t rd = (raw >> 11) & 0x1F;
                    uint32_t return_addr = relocate_ra(addr + 8);
                    if (rd != 0) {
                        out += fmt::format("    cpu->gpr[{}] = 0x{:08X}u;\n",
                                           static_cast<int>(rd), return_addr);
                    }
                    out += fmt::format("    psx_dispatch(cpu, cpu->gpr[{}]);\n",
                                       static_cast<int>(rs));
                    out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", return_addr);
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
                "        cpu->pc = 0x{:08X}u; return;\n"
                "    }}\n",
                addr, ram_pc, ram_pc + 0x10u, ram_pc, ram_pc);
        }

        // Non-terminator: emit normally.
        out += fmt::format("    /* 0x{:08X}: {:08X}  {} */\n", addr, raw, tr.comment);
        out += fmt::format("    {}\n", tr.c_code);

        // Check if this instruction is a delay slot with pending resolution.
        if (pending_at.count(addr)) {
            const PendingBranch& pb = pending_at[addr];
            const std::string& kind = pb.kind;

            if (is_branch_kind(kind.c_str())) {
                // Conditional branch resolution.
                uint32_t target = pb.target;
                bool target_in_function = addr_to_raw.count(target) != 0;
                if (target_in_function) {
                    out += fmt::format("    if (psx_taken_{:08X}) goto label_{:08X};\n",
                                       pb.terminator_addr, target);
                } else {
                    // Tail call: set cpu->pc and return; dispatch loop re-dispatches.
                    // Register the cross-function target as a continuation so
                    // the dispatch table can route to it (otherwise mid-function
                    // targets miss the table — root cause of card-read failure).
                    register_cross_function_target(target);
                    out += fmt::format("    if (psx_taken_{:08X}) {{ cpu->pc = 0x{:08X}u; return; }}\n",
                                       pb.terminator_addr, target);
                }
            } else if (kind == "j") {
                uint32_t target = pb.target;
                bool target_in_function = addr_to_raw.count(target) != 0;
                if (target_in_function) {
                    out += fmt::format("    goto label_{:08X};\n", target);
                } else {
                    // Tail call: set cpu->pc and return; dispatch loop re-dispatches.
                    register_cross_function_target(target);
                    out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", target);
                }
            } else if (kind == "jal") {
                uint32_t target = pb.target;
                uint32_t return_addr = relocate_ra(pb.terminator_addr + 8);
                out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;\n", return_addr);
                // Regular call: always go through psx_dispatch (handles tail-call loop).
                out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);\n", target);
                // Safety net: if continuation falls outside this function, tail-call to it.
                if (!addr_to_raw.count(pb.terminator_addr + 8)) {
                    out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);  /* jal cont: outside func */\n", return_addr);
                }
                // Register continuation label if the return point is inside
                // this function and the callee is a separate dispatch entry.
                // The callee's jr $ra will set cpu->pc = return_addr, and the
                // dispatch loop needs a table entry to route back here.
                if (addr_to_raw.count(pb.terminator_addr + 8)) {
                    uint32_t cont_rom = pb.terminator_addr + 8;
                    uint32_t cont_norm = normalize_address(cont_rom);
                    // Only needed if the continuation isn't already a function entry.
                    if (!all_function_entries_norm.count(cont_norm)) {
                        local_continuations.push_back({cont_rom, cont_norm, norm});
                    }
                }
            } else if (kind == "jalr") {
                uint8_t rs = (pb.raw >> 21) & 0x1F;
                uint8_t rd = (pb.raw >> 11) & 0x1F;
                uint32_t return_addr = relocate_ra(pb.terminator_addr + 8);
                if (rd != 0) {
                    out += fmt::format("    cpu->gpr[{}] = 0x{:08X}u;\n",
                                       static_cast<int>(rd), return_addr);
                }
                out += fmt::format("    psx_dispatch(cpu, cpu->gpr[{}]);\n",
                                   static_cast<int>(rs));
                // Safety net: if continuation falls outside this function, tail-call to it.
                if (!addr_to_raw.count(pb.terminator_addr + 8)) {
                    out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);  /* jalr cont: outside func */\n", return_addr);
                }
                // Register continuation for jalr too (same logic as jal).
                if (addr_to_raw.count(pb.terminator_addr + 8)) {
                    uint32_t cont_rom = pb.terminator_addr + 8;
                    uint32_t cont_norm = normalize_address(cont_rom);
                    if (!all_function_entries_norm.count(cont_norm)) {
                        local_continuations.push_back({cont_rom, cont_norm, norm});
                    }
                }
            } else if (kind == "jr") {
                uint8_t rs = (pb.raw >> 21) & 0x1F;
                if (rs == 31) {
                    if (ra_loaded_from_non_sp)
                        out += "    cpu->pc = cpu->gpr[31]; psx_restore_state_escape(); return;  /* longjmp-return */\n";
                    else
                        out += "    return;\n";
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
                            uint32_t tb_phys = tb & 0x1FFFFFFFu;
                            uint32_t rom_tb = tb;
                            if (tb_phys >= 0x00030000u && tb_phys <= 0x0005AFFFu)
                                rom_tb = 0xBFC18000u + (tb_phys - 0x00030000u);

                            // Read table entries and map to ROM labels.
                            // Deduplicate by runtime address to avoid duplicate
                            // case values in the switch.  Only include targets
                            // that exist as code in this function.
                            std::vector<std::pair<uint32_t,uint32_t>> targets; // {runtime, rom}
                            std::set<uint32_t> seen_runtime;
                            uint32_t rom_off = rom_tb - base_addr;
                            for (uint32_t i = 0; i < tc; i++) {
                                if (rom_off + i*4 + 3 >= rom.size()) break;
                                uint32_t rv = read_u32_le(rom, rom_off + i * 4);
                                if (!seen_runtime.insert(rv).second) continue;
                                uint32_t rv_phys = rv & 0x1FFFFFFFu;
                                uint32_t rom_target = rv;
                                if (rv_phys >= 0x00030000u && rv_phys <= 0x0005AFFFu)
                                    rom_target = 0xBFC18000u + (rv_phys - 0x00030000u);
                                if (addr_to_raw.count(rom_target) && block_leaders.count(rom_target))
                                    targets.push_back({rv, rom_target});
                            }
                            if (!targets.empty()) {
                                out += fmt::format("    /* jump table at 0x{:08X}, {} entries */\n",
                                                   tb, tc);
                                out += fmt::format("    switch (cpu->gpr[{}]) {{\n",
                                                   static_cast<int>(jr_rs));
                                for (auto& [rt, rom_t] : targets) {
                                    out += fmt::format("        case 0x{:08X}u: goto label_{:08X};\n",
                                                       rt, rom_t);
                                }
                                out += fmt::format("        default: cpu->pc = cpu->gpr[{}]; return;\n",
                                                   static_cast<int>(jr_rs));
                                out += "    }\n";
                                emitted_switch = true;
                            }
                        }
                    }
                    if (!emitted_switch) {
                        // Tail call: set cpu->pc and return; dispatch loop re-dispatches.
                        out += fmt::format("    cpu->pc = cpu->gpr[{}]; return;\n",
                                           static_cast<int>(rs));
                    }
                }
            }
            // RFE: already emitted at terminator address, no delay slot.
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
    std::string&              out,
    const DiscoveryResult&    dr,
    const std::set<uint32_t>& emitted_normalized,
    const std::map<uint32_t, ContinuationLabel>& continuations,
    const std::string&        bios_sha256)
{
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
    out += "extern void psx_restore_state_escape(void);\n";
    out += "extern void gte_execute(CPUState* cpu, uint32_t cmd);\n";
    out += "extern void gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);\n";
    out += "extern uint32_t gte_read_data(CPUState* cpu, uint8_t reg);\n";
    out += "extern uint32_t g_debug_current_func_addr;\n";
    out += "extern void debug_server_trace_dispatch(uint32_t func_addr);\n\n";

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

    // Dispatch table: sorted array.
    out += "typedef void (*PsxRecompFunc)(CPUState*);\n\n";
    out += "typedef struct {\n";
    out += "    uint32_t addr;\n";
    out += "    PsxRecompFunc func;\n";
    out += "} DispatchEntry;\n\n";

    out += fmt::format("static const DispatchEntry dispatch_table[{}] = {{\n",
                       emitted_normalized.size());
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

    out += "extern int dirty_ram_dispatch(CPUState* cpu, uint32_t addr);\n";
    out += "\n";
    out += "void psx_dispatch(CPUState* cpu, uint32_t addr) {\n";
    out += "    /* Tail-call trampoline: functions signal tail calls by setting\n";
    out += "     * cpu->pc to the target and returning. We loop here to re-dispatch\n";
    out += "     * without growing the native stack. */\n";
    out += "    for (;;) {\n";
    out += "        cpu->pc = 0;\n";
    out += "        uint32_t phys = normalize(addr);\n";
    out += fmt::format("        int lo = 0, hi = {} - 1;\n", emitted_normalized.size());
    out += "        int found = 0;\n";
    out += "        while (lo <= hi) {\n";
    out += "            int mid = (lo + hi) / 2;\n";
    out += "            if (dispatch_table[mid].addr == phys) {\n";
    out += "                g_debug_current_func_addr = phys;\n";
    out += "                debug_server_trace_dispatch(phys);\n";
    out += "                dispatch_table[mid].func(cpu);\n";
    out += "                found = 1;\n";
    out += "                break;\n";
    out += "            } else if (dispatch_table[mid].addr < phys) {\n";
    out += "                lo = mid + 1;\n";
    out += "            } else {\n";
    out += "                hi = mid - 1;\n";
    out += "            }\n";
    out += "        }\n";
    out += "        /* Static dispatch miss.  Self-modifying / install-at-runtime RAM\n";
    out += "         * (CLAUDE.md Rule 18): the BIOS writes dispatch stubs into kernel\n";
    out += "         * RAM at runtime.  If the target page has been written-to since\n";
    out += "         * boot, interpret the basic block on cpu state.  Falls back to\n";
    out += "         * psx_unknown_dispatch for genuinely unmapped PCs. */\n";
    out += "        if (!found) {\n";
    out += "            if (dirty_ram_dispatch(cpu, addr)) {\n";
    out += "                found = 1;\n";
    out += "            } else {\n";
    out += "                psx_unknown_dispatch(cpu, addr, phys);\n";
    out += "            }\n";
    out += "        }\n";
    out += "        if (cpu->pc == 0) { psx_check_interrupts(cpu); return; }\n";
    out += "        addr = cpu->pc;  /* tail call: re-dispatch */\n";
    out += "    }\n";
    out += "}\n";
}

// ---------------------------------------------------------------------------
// emit: top-level entry point
// ---------------------------------------------------------------------------

EmitStats FullFunctionEmitter::emit(
    const std::vector<uint8_t>& rom,
    uint32_t                    base_addr,
    uint32_t                    rom_end,
    const DiscoveryResult&      dr,
    const std::string&          bios_sha256,
    const std::string&          out_dir)
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
    full_c += "extern void psx_restore_state_escape(void);\n";
    full_c += "extern void gte_execute(CPUState* cpu, uint32_t cmd);\n";
    full_c += "extern void gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);\n";
    full_c += "extern uint32_t gte_read_data(CPUState* cpu, uint8_t reg);\n";
    full_c += "extern void debug_server_log_call_entry(uint32_t func_addr);\n";
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
        emit_dispatch(dispatch_c, dr, emitted_normalized, unique_continuations, bios_sha256);
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
