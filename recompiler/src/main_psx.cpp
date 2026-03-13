#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>

#include "ps1_exe_parser.h"
#include "gte.h"
#include "function_analysis.h"
#include "control_flow.h"
#include "code_generator.h"
#include "annotations.hpp"
#include "rabbitizer.hpp"
#include "fmt/format.h"

int main(int argc, char** argv) {
    fmt::print("PSXRecomp - PlayStation 1 Static Recompiler\n");
    fmt::print("============================================\n\n");

    if (argc < 2) {
        fmt::print("Usage: {} <PS1-EXE file>\n", argv[0]);
        fmt::print("Example: {} SLUS_006.30\n\n", argv[0]);
        return 0;
    }

    std::filesystem::path exe_path = argv[1];

    // Parse the PS1-EXE file
    std::string error_msg;
    fmt::print("Parsing PS1-EXE: {}\n", exe_path.string());

    auto exe = PSXRecomp::PS1ExeParser::parse_file(exe_path, error_msg);

    if (!exe.has_value()) {
        fmt::print(stderr, "Failed to parse PS1-EXE: {}\n", error_msg);
        return 1;
    }

    fmt::print("✓ Successfully parsed PS1-EXE!\n\n");

    // Print header information
    fmt::print("PS1-EXE Header Info:\n");
    fmt::print("  Entry Point:    0x{:08X}\n", exe->header.initial_pc);
    fmt::print("  Load Address:   0x{:08X}\n", exe->header.load_address);
    fmt::print("  Code Size:      {} bytes ({} KB)\n",
               exe->header.file_size, exe->header.file_size / 1024);
    fmt::print("  End Address:    0x{:08X}\n", exe->end_address());
    fmt::print("  Global Pointer: 0x{:08X}\n", exe->header.initial_gp);
    fmt::print("  Stack Pointer:  0x{:08X}\n\n", exe->header.initial_sp);

    // Validate entry point
    if (!exe->header.entry_in_range()) {
        fmt::print(stderr, "⚠ Warning: Entry point 0x{:08X} is outside loaded code range!\n\n",
                   exe->header.initial_pc);
    }

    // Disassemble first 20 instructions from entry point
    fmt::print("Disassembly at Entry Point (0x{:08X}):\n", exe->header.initial_pc);
    fmt::print("---------------------------------------\n");

    uint32_t current_addr = exe->header.initial_pc;
    const int num_instructions = 20;

    for (int i = 0; i < num_instructions; i++) {
        auto word_opt = exe->read_word(current_addr);

        if (!word_opt.has_value()) {
            fmt::print("  [End of code or read error]\n");
            break;
        }

        uint32_t instr_word = *word_opt;

        // Use Rabbitizer to disassemble the instruction
        // Note: Rabbitizer expects big-endian, PS1 is little-endian, so the word is already in correct format
        rabbitizer::InstructionCpu instr(instr_word, current_addr);

        // Get disassembly (parameter is flags, 0 = default)
        std::string disasm = instr.disassemble(0);

        // Print address, hex, and disassembly
        fmt::print("  {:08X}:  {:08X}  {}\n", current_addr, instr_word, disasm);

        current_addr += 4;
    }

    fmt::print("\n");
    fmt::print("✓ Disassembly complete!\n");

    // Instruction frequency analysis
    fmt::print("\n");
    fmt::print("Analyzing instruction frequency...\n");

    std::map<std::string, int> instr_freq;
    std::map<uint32_t, int> opcode_freq;
    std::map<uint32_t, int> special_funct_freq;

    uint32_t total_instructions = 0;
    uint32_t addr = exe->header.load_address;
    while (addr < exe->end_address()) {
        auto word_opt = exe->read_word(addr);
        if (!word_opt.has_value()) break;

        uint32_t instr_word = *word_opt;
        rabbitizer::InstructionCpu instr(instr_word, addr);
        std::string mnemonic = instr.getOpcodeName();

        instr_freq[mnemonic]++;
        total_instructions++;

        // Track opcode distribution
        uint32_t opcode = (instr_word >> 26) & 0x3F;
        opcode_freq[opcode]++;

        // Track SPECIAL function codes
        if (opcode == 0x00) {
            uint32_t funct = instr_word & 0x3F;
            special_funct_freq[funct]++;
        }

        addr += 4;
    }

    fmt::print("Total instructions: {}\n\n", total_instructions);

    // Sort by frequency and print top 20
    std::vector<std::pair<std::string, int>> sorted_freq(instr_freq.begin(), instr_freq.end());
    std::sort(sorted_freq.begin(), sorted_freq.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    fmt::print("=== Top 20 Most Common Instructions ===\n\n");
    fmt::print("{:4s}  {:12s}  {:10s}  {:s}\n", "Rank", "Instruction", "Count", "Percentage");
    fmt::print("-----------------------------------------------------\n");

    for (size_t i = 0; i < std::min(size_t(20), sorted_freq.size()); i++) {
        const auto& [instr_name, count] = sorted_freq[i];
        double pct = 100.0 * count / total_instructions;
        fmt::print("{:4d}  {:12s}  {:10d}  {:5.2f}%\n", i + 1, instr_name, count, pct);
    }

    fmt::print("\n");

    // Function boundary detection
    fmt::print("Performing function analysis...\n");

    PSXRecomp::FunctionAnalyzer analyzer(*exe);

    // Forced entry points: functions called from the dispatch table but not
    // automatically detected by the heuristic-based function analysis.
    // These are typically functions without standard prologues or epilogues.
    // 0x8006B58C: game entrypoint (main boot sequence, called from test harness
    //             and dispatch table; absorbed into func_8006B4EC without this).
    analyzer.add_forced_entry(0x8006B58Cu);

    auto analysis_result = analyzer.analyze();

    // Print summary statistics
    fmt::print("\n=== Function Analysis Summary ===\n\n");
    fmt::print("Total Functions: {}\n", analysis_result.functions.size());
    fmt::print("Code Coverage:   {} KB analyzed\n",
               (exe->end_address() - exe->header.load_address) / 1024);

    // Count functions with prologues/epilogues
    int with_prologue = 0;
    int with_epilogue = 0;
    int with_both = 0;

    for (const auto& func : analysis_result.functions) {
        if (func.has_prologue) with_prologue++;
        if (func.has_epilogue) with_epilogue++;
        if (func.has_prologue && func.has_epilogue) with_both++;
    }

    fmt::print("With Prologue:   {} ({:.1f}%)\n", with_prologue,
               100.0 * with_prologue / analysis_result.functions.size());
    fmt::print("With Epilogue:   {} ({:.1f}%)\n", with_epilogue,
               100.0 * with_epilogue / analysis_result.functions.size());
    fmt::print("Complete:        {} ({:.1f}%)\n\n", with_both,
               100.0 * with_both / analysis_result.functions.size());

    // Print first 20 functions
    fmt::print("First 20 Functions:\n");
    fmt::print("---------------------------------------\n");

    for (size_t i = 0; i < std::min(size_t(20), analysis_result.functions.size()); i++) {
        const auto& func = analysis_result.functions[i];
        fmt::print("  {:08X}-{:08X}  {:5} bytes  {}{}\n",
                   func.start_addr,
                   func.end_addr,
                   func.size,
                   func.name,
                   func.has_prologue ? fmt::format(" (frame={})", func.stack_frame_size) : "");
    }

    fmt::print("\n... ({} more functions)\n\n",
               analysis_result.functions.size() - std::min(size_t(20), analysis_result.functions.size()));

    // Control flow analysis
    PSXRecomp::ControlFlowAnalyzer cfg_analyzer(*exe);
    auto all_cfgs = cfg_analyzer.analyze_all_functions(analysis_result.functions);

    // Print CFG summary statistics
    fmt::print("=== Control Flow Summary ===\n\n");

    int total_basic_blocks = 0;
    int total_loops = 0;
    int total_branches = 0;

    for (const auto& [func_addr, cfg] : all_cfgs) {
        total_basic_blocks += static_cast<int>(cfg.blocks.size());
        total_loops += cfg.loop_count;

        // Count branch instructions across all blocks
        for (const auto& [block_addr, block] : cfg.blocks) {
            if (block.exit_instr.type == PSXRecomp::ControlFlowType::Branch) {
                total_branches++;
            }
        }
    }

    fmt::print("Basic Blocks:       {}\n", total_basic_blocks);
    fmt::print("Loops Detected:     {}\n", total_loops);
    fmt::print("Branch Instructions: {}\n", total_branches);
    fmt::print("Avg Blocks/Function: {:.1f}\n\n",
               static_cast<double>(total_basic_blocks) / all_cfgs.size());

    // Show detailed CFG for first function (as an example)
    if (!analysis_result.functions.empty() && !all_cfgs.empty()) {
        const auto& first_func = analysis_result.functions[0];
        const auto& first_cfg = all_cfgs.at(first_func.start_addr);

        fmt::print("Example: CFG for {} (0x{:08X}):\n", first_func.name, first_func.start_addr);
        fmt::print("---------------------------------------\n");
        fmt::print("  Blocks: {}\n", first_cfg.blocks.size());
        fmt::print("  Loops:  {}\n", first_cfg.loop_count);

        if (first_cfg.blocks.size() > 1) {
            fmt::print("\n  Block Details:\n");
            int count = 0;
            for (const auto& block_addr : first_cfg.block_order) {
                const auto& block = first_cfg.blocks.at(block_addr);
                fmt::print("    Block 0x{:08X}: {} instructions", block.start_addr, block.instruction_count);

                if (block.is_entry) fmt::print(" [ENTRY]");
                if (block.is_exit) fmt::print(" [EXIT]");
                if (block.is_loop_header) fmt::print(" [LOOP]");

                fmt::print("\n");
                fmt::print("      Successors: {}", block.successors.size());
                if (!block.successors.empty()) {
                    fmt::print(" (");
                    for (size_t i = 0; i < block.successors.size(); i++) {
                        fmt::print("0x{:08X}", block.successors[i]);
                        if (i + 1 < block.successors.size()) fmt::print(", ");
                    }
                    fmt::print(")");
                }
                fmt::print("\n");

                // Only show first 5 blocks for brevity
                if (++count >= 5 && first_cfg.blocks.size() > 5) {
                    fmt::print("    ... ({} more blocks)\n", first_cfg.blocks.size() - 5);
                    break;
                }
            }
        }
        fmt::print("\n");
    }

    // C code generation
    PSXRecomp::CodeGenConfig codegen_config;
    codegen_config.emit_comments = true;
    codegen_config.emit_line_numbers = true;

    // Load per-game annotations: annotations/<exe_stem>_annotations.csv
    // Silently skipped if the file doesn't exist.
    PSXRecomp::AnnotationTable annotations;
    {
        std::string stem = exe_path.stem().string();
        std::string ann_path = "annotations/" + stem + "_annotations.csv";
        if (annotations.load(ann_path.c_str()))
            fmt::print("✓ Loaded {} annotations from {}\n\n", annotations.count(), ann_path);
        else
            fmt::print("  (No annotations file found at {})\n\n", ann_path);
    }

    PSXRecomp::CodeGenerator codegen(*exe, codegen_config);
    codegen.set_annotations(&annotations);

    // Generate code for first 5 functions (as examples)
    fmt::print("=== C Code Generation Examples ===\n\n");
    fmt::print("Generating code for first 5 functions...\n\n");

    std::vector<PSXRecomp::Function> sample_funcs;
    for (size_t i = 0; i < std::min(size_t(5), analysis_result.functions.size()); i++) {
        sample_funcs.push_back(analysis_result.functions[i]);
    }

    auto generated = codegen.generate_all_functions(sample_funcs, all_cfgs);

    // Print first generated function as example
    if (!generated.empty()) {
        fmt::print("Example Generated Function:\n");
        fmt::print("---------------------------------------\n");
        fmt::print("{}\n", generated[0].full_code);
        fmt::print("---------------------------------------\n\n");
    }

    // Generate full C file and save to the generated directory
    // Use argv[2] as output path if provided, otherwise use the default generated path
    std::string output_filename;
    if (argc >= 3) {
        output_filename = argv[2];
    } else {
        output_filename = "F:\\Projects\\psxrecomp-v2\\generated\\tomba_full.c";
    }
    fmt::print("Generating complete C file: {}\n", output_filename);

    std::string full_c_code = codegen.generate_file(analysis_result.functions, all_cfgs);

    std::ofstream out_file(output_filename);
    if (out_file.is_open()) {
        out_file << full_c_code;
        out_file.close();
        fmt::print("✓ Saved {} lines to {}\n\n",
                  std::count(full_c_code.begin(), full_c_code.end(), '\n'),
                  output_filename);
    } else {
        fmt::print(stderr, "⚠ Failed to write output file\n\n");
    }

    // Generate dispatch table (tomba_dispatch.c)
    // This maps PS1 addresses to compiled C functions so call_by_address() can
    // dispatch dynamic jalr/jr calls to the right compiled function.
    {
        std::string dispatch_filename = "F:\\Projects\\psxrecomp-v2\\generated\\tomba_dispatch.c";
        fmt::print("Generating dispatch table: {}\n", dispatch_filename);

        std::ostringstream ds;
        ds << "/* Generated by PSXRecomp - dynamic dispatch table */\n";
        ds << "#include \"psx_runtime.h\"\n\n";

        // Forward declarations
        ds << "/* Forward declarations for all recompiled functions */\n";
        for (const auto& func : analysis_result.functions) {
            ds << fmt::format("extern void func_{:08X}(CPUState* cpu);\n", func.start_addr);
        }
        ds << "\n";

        // Dispatch function
        ds << "/* Maps PS1 address to compiled function. Returns 1 if dispatched, 0 if unknown. */\n";
        ds << "int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {\n";
        ds << "    switch (addr) {\n";
        for (const auto& func : analysis_result.functions) {
            ds << fmt::format("        case 0x{:08X}u: func_{:08X}(cpu); return 1;\n",
                              func.start_addr, func.start_addr);
        }
        ds << "        default: return 0;\n";
        ds << "    }\n";
        ds << "}\n";

        std::ofstream dispatch_file(dispatch_filename);
        if (dispatch_file.is_open()) {
            dispatch_file << ds.str();
            dispatch_file.close();
            fmt::print("✓ Dispatch table written ({} entries)\n\n",
                       analysis_result.functions.size());
        } else {
            fmt::print(stderr, "⚠ Failed to write dispatch file\n\n");
        }
    }

    return 0;
}
