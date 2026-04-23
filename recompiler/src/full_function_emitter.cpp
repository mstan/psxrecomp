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
    uint32_t                    /* rom_end */)
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

    // Emit function header.
    out += fmt::format("void func_{:08X}(CPUState* cpu) {{\n", norm);

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

    for (auto it = addr_to_raw.begin(); it != addr_to_raw.end(); ++it) {
        uint32_t addr = it->first;
        uint32_t raw = it->second;

        // Emit label if this is a block leader.
        // Include an interrupt check so tight loops (backward branches)
        // can service vblank and other hardware interrupts.
        if (block_leaders.count(addr)) {
            out += fmt::format("label_{:08X}:\n", addr);
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
                out += fmt::format("    int psx_taken_{:08X} = ({});\n", addr, cond);
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
            if (orphaned_delay_slots.count(addr) && kind == "jr") {
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
            }
            continue;
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
                    out += fmt::format("    cpu->pc = 0x{:08X}u; return;\n", target);
                }
            } else if (kind == "jal") {
                uint32_t target = pb.target;
                uint32_t return_addr = pb.terminator_addr + 8;
                out += fmt::format("    cpu->gpr[31] = 0x{:08X}u;\n", return_addr);
                // Regular call: always go through psx_dispatch (handles tail-call loop).
                out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);\n", target);
                // Safety net: if continuation falls outside this function, tail-call to it.
                if (!addr_to_raw.count(return_addr)) {
                    out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);  /* jal cont: outside func */\n", return_addr);
                }
            } else if (kind == "jalr") {
                uint8_t rs = (pb.raw >> 21) & 0x1F;
                uint8_t rd = (pb.raw >> 11) & 0x1F;
                uint32_t return_addr = pb.terminator_addr + 8;
                if (rd != 0) {
                    out += fmt::format("    cpu->gpr[{}] = 0x{:08X}u;\n",
                                       static_cast<int>(rd), return_addr);
                }
                out += fmt::format("    psx_dispatch(cpu, cpu->gpr[{}]);\n",
                                   static_cast<int>(rs));
                // Safety net: if continuation falls outside this function, tail-call to it.
                if (!addr_to_raw.count(return_addr)) {
                    out += fmt::format("    psx_dispatch(cpu, 0x{:08X}u);  /* jalr cont: outside func */\n", return_addr);
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
    return true;
}

// ---------------------------------------------------------------------------
// emit_dispatch: generate the dispatch table
// ---------------------------------------------------------------------------

void FullFunctionEmitter::emit_dispatch(
    std::string&              out,
    const DiscoveryResult&    dr,
    const std::set<uint32_t>& emitted_normalized,
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
    out += "extern uint32_t g_debug_current_func_addr;\n";
    out += "extern void debug_server_trace_dispatch(uint32_t func_addr);\n\n";

    // Forward declarations for all emitted functions.
    for (uint32_t norm : emitted_normalized) {
        out += fmt::format("extern void func_{:08X}(CPUState* cpu);\n", norm);
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
        out += fmt::format("    {{ 0x{:08X}u, func_{:08X} }},\n", norm, norm);
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
    out += "        if (!found) psx_unknown_dispatch(cpu, addr, phys);\n";
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
    full_c += "extern void gte_execute(CPUState* cpu, uint32_t cmd);\n\n";

    // Forward declare ALL functions so intra-file calls resolve.
    for (const auto& fn : dr.functions) {
        full_c += fmt::format("void func_{:08X}(CPUState* cpu);\n", fn.normalized_addr);
    }
    full_c += "\n";

    std::set<uint32_t> emitted_normalized;

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

        bool ok = emit_function(full_c, fn, sfr, all_function_entries_norm, rom, base_addr, rom_end);
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
        emit_dispatch(dispatch_c, dr, emitted_normalized, bios_sha256);
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
