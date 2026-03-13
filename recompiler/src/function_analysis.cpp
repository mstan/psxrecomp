#include "function_analysis.h"
#include <algorithm>
#include <set>
#include <fmt/format.h>

namespace PSXRecomp {

FunctionAnalyzer::FunctionAnalyzer(const PS1Executable& exe) : exe_(exe) {}

void FunctionAnalyzer::add_forced_entry(uint32_t addr) {
    // Validate address is within the EXE range
    if (addr >= exe_.header.load_address && addr < exe_.end_address()) {
        forced_entry_points_.push_back(addr);
    }
}

bool FunctionAnalyzer::is_jr_ra(uint32_t instr) {
    // jr $ra: opcode=0, rs=31 ($ra), rt=0, rd=0, shamt=0, funct=8 (jr)
    // Format: 000000 11111 00000 00000 00000 001000
    // Hex: 0x03E00008
    return instr == 0x03E00008;
}

bool FunctionAnalyzer::is_prologue(uint32_t instr, int32_t& stack_size) {
    // addiu $sp, $sp, -N
    // Format: 001001 11101 11101 <16-bit signed immediate>
    // Opcode: 0x27 (addiu), rs=$sp (29), rt=$sp (29)
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    int16_t imm = (int16_t)(instr & 0xFFFF);

    if (opcode == 0x09 && rs == 29 && rt == 29 && imm < 0) {
        stack_size = -imm; // Store positive stack frame size
        return true;
    }
    return false;
}

bool FunctionAnalyzer::is_epilogue(uint32_t instr, int32_t& stack_size) {
    // addiu $sp, $sp, +N
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = (instr >> 21) & 0x1F;
    uint32_t rt = (instr >> 16) & 0x1F;
    int16_t imm = (int16_t)(instr & 0xFFFF);

    if (opcode == 0x09 && rs == 29 && rt == 29 && imm > 0) {
        stack_size = imm;
        return true;
    }
    return false;
}

bool FunctionAnalyzer::is_branch_or_jump(uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    // J, JAL
    if (opcode == 0x02 || opcode == 0x03) return true;
    // BEQ, BNE, BLEZ, BGTZ
    if (opcode >= 0x04 && opcode <= 0x07) return true;
    // REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL
    if (opcode == 0x01) return true;
    // SPECIAL: JR, JALR
    if (opcode == 0x00) {
        uint32_t funct = instr & 0x3F;
        if (funct == 0x08 || funct == 0x09) return true;
    }
    // COP1/COP2 branches (BC1F, BC1T, BC2F, BC2T) — opcode 0x11 or 0x12, rs=0x08
    if ((opcode == 0x11 || opcode == 0x12) && ((instr >> 21) & 0x1F) == 0x08) return true;
    return false;
}

uint32_t FunctionAnalyzer::find_function_start(uint32_t return_addr) {
    // Scan backward from jr $ra to find function start
    // Heuristic: Look for prologue or function alignment (16-byte boundary after prev function)

    uint32_t search_addr = return_addr;
    const uint32_t max_search = 4096; // Search up to 4096 instructions backward (16 KB)

    for (uint32_t i = 0; i < max_search; i++) {
        search_addr -= 4;

        if (search_addr < exe_.header.load_address) {
            // Reached beginning of code
            return exe_.header.load_address;
        }

        auto word_opt = exe_.read_word(search_addr);
        if (!word_opt.has_value()) {
            return return_addr; // Can't read, assume current position
        }

        uint32_t instr = *word_opt;
        int32_t stack_size;

        // Check if this is a prologue
        if (is_prologue(instr, stack_size)) {
            // Verify this isn't a delay slot of a branch/jump instruction.
            // MIPS compilers often place stack allocation in the delay slot of
            // the function's first conditional branch, e.g.:
            //   beq v0, zero, skip
            //   addiu sp, sp, -N   <- delay slot, looks like prologue but isn't a function start
            // If the preceding instruction is a branch/jump, skip this candidate.
            if (search_addr >= exe_.header.load_address + 4) {
                auto prev_opt = exe_.read_word(search_addr - 4);
                if (prev_opt.has_value() && is_branch_or_jump(*prev_opt)) {
                    continue;  // delay slot, not a real prologue — keep scanning
                }
            }
            return search_addr;
        }

        // Check if we hit another function's return
        if (is_jr_ra(instr)) {
            // We've gone too far backward and hit another function
            // Return the address after this jr $ra (+ 8 for delay slot and alignment)
            return search_addr + 8;
        }
    }

    // Couldn't find clear start, assume max search distance
    return return_addr - (max_search * 4);
}

bool FunctionAnalyzer::is_likely_data_section(uint32_t start_addr, uint32_t end_addr) const {
    uint32_t size = end_addr - start_addr;
    if (size < 100) return false;  // Minimum check size

    uint32_t total_words = size / 4;
    uint32_t invalid_jal_count = 0;
    uint32_t undefined_opcode_count = 0;

    // Valid PS1 (MIPS R3000) primary opcodes
    static const bool valid_opcode[64] = {
        true,  true,  true,  true,  true,  true,  true,  true,   // 0x00-0x07
        true,  true,  true,  true,  true,  true,  true,  true,   // 0x08-0x0F
        true,  false, true,  false, false, false, false, false,   // 0x10-0x17 (COP0=0x10, COP2=0x12)
        false, false, false, false, false, false, false, false,   // 0x18-0x1F
        true,  true,  true,  true,  true,  true,  true,  false,  // 0x20-0x27
        true,  true,  true,  true,  false, false, true,  false,  // 0x28-0x2F
        true,  false, true,  false, false, false, false, false,  // 0x30-0x37 (LWC0=0x30, LWC2=0x32)
        true,  false, true,  false, false, false, false, false,  // 0x38-0x3F (SWC0=0x38, SWC2=0x3A)
    };

    for (uint32_t addr = start_addr; addr < end_addr; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;

        uint32_t opcode = (instr >> 26) & 0x3F;

        // Check for JAL with invalid target
        if (opcode == 3) {  // JAL opcode
            // PS1 JAL target: upper 4 bits from PC region (0x80000000), low 28 bits from instr
            uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;
            if (target > 0x801FFFFFu) {
                invalid_jal_count++;
            }
        }

        // Check for undefined opcode
        if (!valid_opcode[opcode]) {
            undefined_opcode_count++;
        }
    }

    // Size-graduated JAL threshold: higher ratio needed for smaller functions
    uint32_t jal_ratio_x100 = (total_words > 0) ? (invalid_jal_count * 100u / total_words) : 0u;
    if (size >= 10000 && jal_ratio_x100 > 5u)  return true;  // Large: >5% invalid JALs
    if (size >= 1000  && jal_ratio_x100 > 30u) return true;  // Medium: >30% invalid JALs
    if (size >= 100   && jal_ratio_x100 > 60u) return true;  // Small: >60% invalid JALs

    // Undefined opcode check:
    // Real PS1 code consistently uses 0% undefined opcodes (calibrated on Tomba!).
    // Data sections masquerading as functions have ~9-27% undefined opcodes.
    // A safe threshold of 7% catches all observed data sections with no false positives.
    uint32_t undef_ratio_x100 = (total_words > 0) ? (undefined_opcode_count * 100u / total_words) : 0u;
    if (size >= 1000 && undef_ratio_x100 > 7u) return true;   // Large: >7% undefined opcodes
    if (size >= 400  && undef_ratio_x100 > 50u) return true;  // Small: >50% undefined opcodes (conservative)

    return false;
}

FunctionAnalysisResult FunctionAnalyzer::analyze() {
    FunctionAnalysisResult result;
    result.total_instructions = 0;
    result.jr_ra_count = 0;
    result.prologue_count = 0;
    result.call_discovered_count = 0;

    fmt::print("\n=== Function Boundary Detection ===\n\n");

    // Pass 1: Find all jr $ra instructions
    std::vector<uint32_t> return_addresses;

    uint32_t current_addr = exe_.header.load_address;
    uint32_t end_addr = exe_.end_address();

    fmt::print("Scanning {} KB of code for function returns...\n",
               (end_addr - current_addr) / 1024);

    while (current_addr < end_addr) {
        auto word_opt = exe_.read_word(current_addr);
        if (!word_opt.has_value()) {
            break;
        }

        uint32_t instr = *word_opt;
        result.total_instructions++;

        if (is_jr_ra(instr)) {
            return_addresses.push_back(current_addr);
            result.jr_ra_count++;
        }

        int32_t stack_size;
        if (is_prologue(instr, stack_size)) {
            result.prologue_count++;
        }

        current_addr += 4;
    }

    fmt::print("✓ Found {} jr $ra instructions\n", result.jr_ra_count);
    fmt::print("✓ Found {} function prologues\n", result.prologue_count);
    fmt::print("✓ Scanned {} total instructions\n\n", result.total_instructions);

    // Pass 2: For each jr $ra, find function boundaries
    fmt::print("Analyzing function boundaries...\n");

    std::set<uint32_t> function_starts; // Use set to avoid duplicates

    for (uint32_t return_addr : return_addresses) {
        uint32_t func_start = find_function_start(return_addr);
        function_starts.insert(func_start);
    }

    size_t jr_ra_discovered = function_starts.size();
    fmt::print("✓ Identified {} unique functions from jr $ra scan\n", jr_ra_discovered);

    // Pass 2.5: Follow JAL call targets to discover additional functions
    // This finds functions that don't have standard jr $ra prologues
    fmt::print("Following JAL call targets to discover additional functions...\n");

    uint32_t exe_start = exe_.header.load_address;
    uint32_t exe_end   = exe_.end_address();

    for (uint32_t addr = exe_start; addr < exe_end; addr += 4) {
        auto word_opt = exe_.read_word(addr);
        if (!word_opt.has_value()) break;
        uint32_t instr = *word_opt;

        uint32_t opcode = (instr >> 26) & 0x3F;
        if (opcode == 3) {  // JAL
            // PS1 JAL target: upper 4 bits from KSEG0 region (0x80000000)
            uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;

            // Only add if target is within EXE range and word-aligned
            if (target >= exe_start && target < exe_end && (target & 3) == 0) {
                function_starts.insert(target);
            }
        }
    }

    result.call_discovered_count = static_cast<int>(function_starts.size() - jr_ra_discovered);
    fmt::print("✓ Identified {} unique functions ({} call-discovered)\n\n",
               function_starts.size(), result.call_discovered_count);

    // Pass 2.7: Add forced entry points
    // These are function starts that do not have a standard prologue (e.g. the
    // PS1 EXE entry point, which is launched directly by the BIOS without a
    // JAL and starts with a BSS-zeroing loop rather than ADDIU $sp, $sp, -N).
    if (!forced_entry_points_.empty()) {
        fmt::print("Adding {} forced entry point(s)...\n", forced_entry_points_.size());
        for (uint32_t addr : forced_entry_points_) {
            bool inserted = function_starts.insert(addr).second;
            if (inserted) {
                fmt::print("  Forced entry 0x{:08X} added\n", addr);
            } else {
                fmt::print("  Forced entry 0x{:08X} already present (skipped)\n", addr);
            }
        }
        fmt::print("\n");
    }

    // Pass 3: Build function table with details
    std::vector<uint32_t> starts_vec(function_starts.begin(), function_starts.end());
    std::sort(starts_vec.begin(), starts_vec.end());

    for (size_t i = 0; i < starts_vec.size(); i++) {
        Function func;
        func.start_addr = starts_vec[i];

        // Estimate end address (next function start or end of executable)
        if (i + 1 < starts_vec.size()) {
            func.end_addr = starts_vec[i + 1];
        } else {
            func.end_addr = end_addr;
        }

        func.size = func.end_addr - func.start_addr;
        func.name = fmt::format("func_{:08X}", func.start_addr);

        // Check for prologue at start
        auto first_instr = exe_.read_word(func.start_addr);
        if (first_instr.has_value()) {
            int32_t stack_size;
            func.has_prologue = is_prologue(*first_instr, stack_size);
            func.stack_frame_size = func.has_prologue ? stack_size : 0;
        } else {
            func.has_prologue = false;
            func.stack_frame_size = 0;
        }

        // Check for jr $ra before end
        func.has_epilogue = false;
        uint32_t search_end = func.end_addr - 4;
        for (uint32_t addr = func.start_addr; addr <= search_end && addr < func.end_addr; addr += 4) {
            auto instr_opt = exe_.read_word(addr);
            if (instr_opt.has_value() && is_jr_ra(*instr_opt)) {
                func.has_epilogue = true;
                break;
            }
        }

        // Check if this "function" is actually a data section
        func.is_data_section = is_likely_data_section(func.start_addr, func.end_addr);

        result.functions.push_back(func);
    }

    return result;
}

} // namespace PSXRecomp
