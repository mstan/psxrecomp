// psxrecomp-toml — PS-X EXE → game.toml generator.
//
// Analyzes a PS-X EXE and generates a complete game.toml config
// with auto-detected fields (load_address, entry_pc, text_size,
// stack_base) and an optional seeds file (JAL targets).
//
// Usage:
//   psxrecomp-toml <exe> [--output <path>] [--seeds <path>]
//                    [--name <str>] [--id <str>] [--stdout]

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "fmt/format.h"
#include "ps1_exe_parser.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void print_usage(const char* argv0) {
    fmt::print(stderr,
        "Usage: {} <PS1-EXE> [options]\n"
        "\n"
        "Options:\n"
        "  --output <path>   Write game.toml to <path> (default: stdout)\n"
        "  --seeds <path>    Also write JAL-target seed file to <path>\n"
        "  --name <str>      Game name in TOML (default: derived from EXE)\n"
        "  --id <str>        Game ID (default: auto-detect or empty)\n"
        "  --stdout          Force output to stdout even with --output\n"
        "  --include-after-return\n"
        "                    Add addresses after jr $ra to seeds (more coverage,\n"
        "                    may include some data addresses)\n"
        "  -h, --help         Show this help\n",
        argv0);
}

// Derive a game name from the EXE filename (fallback when --name not given).
static std::string name_from_exe(const fs::path& exe_path) {
    std::string stem = exe_path.stem().string();
    // Replace underscores/hyphens with spaces
    for (char& c : stem) {
        if (c == '_' || c == '-') c = ' ';
    }
    // Capitalise first letter of each word
    bool cap = true;
    for (char& c : stem) {
        if (c == ' ') {
            cap = true;
        } else if (cap) {
            c = (char)std::toupper((unsigned char)c);
            cap = false;
        }
    }
    return stem;
}

// Try to extract a Sony game ID from the EXE filename
// (e.g. "SCES_028.34" -> "SCES-02834", "SLUS-12345" -> "SLUS-12345").
static std::string id_from_exe(const fs::path& exe_path) {
    // Use the full filename (not stem) because Sony IDs use dots as
    // field separators within the ID, not as extension separators
    // (e.g. "SCES_028.34" -> full ID "SCES-02834").
    std::string fname = exe_path.filename().string();
    // Strip all non-alphanumeric characters, keeping alpha prefix + digits
    std::string clean;
    for (char c : fname) {
        if (std::isalnum((unsigned char)c)) {
            clean += (char)std::toupper((unsigned char)c);
        }
    }
    if (clean.empty()) return fname;
    // Re-format: prefix (letters) + "-" + number (digits)
    // e.g. "SCES02834" -> "SCES-02834"
    if (clean.find('-') == std::string::npos) {
        size_t split = 0;
        while (split < clean.size() && std::isalpha((unsigned char)clean[split]))
            split++;
        if (split > 0 && split < clean.size()) {
            clean.insert(split, "-");
        }
    }
    return clean;
}

// Scan the EXE code for JAL instructions and return the set of unique targets.
static std::set<uint32_t> find_jal_targets(const PSXRecomp::PS1Executable& exe) {
    std::set<uint32_t> targets;
    const uint32_t base = exe.load_address();
    const uint32_t end = exe.end_address();

    for (uint32_t addr = base; addr + 4 <= end; addr += 4) {
        auto word = exe.read_word(addr);
        if (!word) continue;
        const uint32_t raw = *word;
        const uint32_t opcode = (raw >> 26) & 0x3F;
        if (opcode == 0x03) { // JAL
            const uint32_t target = (addr & 0xF0000000u) | ((raw & 0x03FFFFFFu) << 2);
            if (target >= base && target < end) {
                targets.insert(target);
            }
        }
    }
    return targets;
}

// Scan the EXE code for addresses that immediately follow a `jr $ra`
// instruction (including its delay slot). These are likely function
// boundaries (the next function after a return). Returns them as seeds.
static std::set<uint32_t> find_after_return(const PSXRecomp::PS1Executable& exe) {
    std::set<uint32_t> after_ret;
    const uint32_t base = exe.load_address();
    const uint32_t end = exe.end_address();

    for (uint32_t addr = base; addr + 8 <= end; addr += 4) {
        auto word = exe.read_word(addr);
        if (!word) continue;
        if (*word == 0x03E00008u) { // jr $ra
            const uint32_t next_func = addr + 8; // skip delay slot
            if (next_func < end) {
                after_ret.insert(next_func);
            }
        }
    }
    return after_ret;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Parse arguments
    fs::path exe_path;
    fs::path output_path;
    fs::path seeds_path;
    std::string game_name;
    std::string game_id;
    bool force_stdout = false;
    bool include_after_return = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--seeds" && i + 1 < argc) {
            seeds_path = argv[++i];
        } else if (arg == "--name" && i + 1 < argc) {
            game_name = argv[++i];
        } else if (arg == "--id" && i + 1 < argc) {
            game_id = argv[++i];
        } else if (arg == "--stdout") {
            force_stdout = true;
        } else if (arg == "--include-after-return") {
            include_after_return = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg[0] == '-') {
            fmt::print(stderr, "Unknown option: {}\n", arg);
            print_usage(argv[0]);
            return 1;
        } else {
            exe_path = arg;
        }
    }

    if (exe_path.empty()) {
        fmt::print(stderr, "Error: no EXE file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    if (!fs::exists(exe_path)) {
        fmt::print(stderr, "Error: EXE file not found: {}\n", exe_path.string());
        return 1;
    }

    // Parse EXE header
    std::string parse_err;
    auto exe_opt = PSXRecomp::PS1ExeParser::parse_file(exe_path, parse_err);
    if (!exe_opt) {
        fmt::print(stderr, "Error parsing EXE: {}\n", parse_err);
        return 1;
    }
    const auto& exe = *exe_opt;

    // Derive name/id from filename if not provided
    if (game_name.empty()) game_name = name_from_exe(exe_path);
    if (game_id.empty())   game_id   = id_from_exe(exe_path);

    // Compute fields
    const uint32_t load_address = exe.load_address();
    const uint32_t entry_pc     = exe.entry_point();
    uint32_t text_size          = exe.code_size();
    if (text_size & 0xFFF) {
        text_size = (text_size + 0xFFF) & ~0xFFFu;
    }
    const uint32_t stack_base   = exe.header.stack_base != 0
                                      ? exe.header.stack_base
                                      : 0x801FFFF0u;

    // Generate TOML
    const auto exe_rel = fs::relative(exe_path, fs::current_path());
    const std::string exe_str = exe_rel.empty() ? exe_path.filename().string() : exe_rel.string();

    std::string toml;
    toml += "# Auto-generated by psxrecomp-toml\n";
    toml += "# https://github.com/nstse/psxrecomp\n";
    toml += "\n";
    toml += "[game]\n";
    toml += fmt::format("name = \"{}\"\n", game_name);
    toml += fmt::format("id = \"{}\"\n", game_id);
    toml += fmt::format("exe = \"{}\"\n", exe_str);
    toml += fmt::format("load_address = \"0x{:08X}\"\n", load_address);
    toml += fmt::format("entry_pc = \"0x{:08X}\"\n", entry_pc);
    toml += fmt::format("text_size = \"0x{:X}\"\n", text_size);
    toml += fmt::format("stack_base = \"0x{:08X}\"\n", stack_base);
    toml += "\n";
    toml += "[recompiler]\n";
    if (!seeds_path.empty()) {
        const auto seeds_rel = fs::relative(seeds_path, fs::current_path());
        toml += fmt::format("seeds = \"{}\"\n", seeds_rel.string());
    } else {
        toml += "# seeds = \"seeds/funcs.txt\"\n";
    }
    toml += "strict = true\n";
    toml += "out_dir = \"generated\"\n";
    toml += "\n";
    toml += "[runtime]\n";
    toml += "# debug_port = 4370\n";
    toml += "window_title = \"" + game_name + " Recompiled\"\n";
    toml += "bios_hle = false\n";
    toml += "\n";
    toml += "[video]\n";
    toml += "renderer = \"opengl\"\n";

    // Write output
    if (!output_path.empty() && !force_stdout) {
        std::ofstream ofs(output_path);
        if (!ofs) {
            fmt::print(stderr, "Error: cannot write to {}\n", output_path.string());
            return 1;
        }
        ofs << toml;
        ofs.close();
        fmt::print("Wrote {}\n", output_path.string());
    } else {
        fmt::print("{}", toml);
    }

    // Generate seeds file if requested
    std::set<uint32_t> jal_targets;
    std::set<uint32_t> after_ret;
    if (!seeds_path.empty()) {
        jal_targets = find_jal_targets(exe);
        after_ret   = find_after_return(exe);

        std::set<uint32_t> seeds = jal_targets;
        if (include_after_return) {
            seeds.insert(after_ret.begin(), after_ret.end());
        }
        seeds.insert(entry_pc);

        // Write seeds file
        const fs::path seeds_dir = seeds_path.parent_path();
        if (!seeds_dir.empty() && !fs::exists(seeds_dir)) {
            fs::create_directories(seeds_dir);
        }

        std::ofstream sfs(seeds_path);
        if (!sfs) {
            fmt::print(stderr, "Error: cannot write seeds to {}\n", seeds_path.string());
            return 1;
        }
        sfs << fmt::format("# Seeds for {} ({})\n", game_name, game_id);
        sfs << fmt::format("# JAL targets: {}\n", jal_targets.size());
        if (include_after_return) {
            sfs << fmt::format("# After-return: {}\n", after_ret.size());
            sfs << fmt::format("#   (overlap with JAL: {})\n",
                jal_targets.size() + after_ret.size() - seeds.size());
        }
        sfs << fmt::format("# Total seeds: {}\n", seeds.size());
        sfs << "# Auto-generated by psxrecomp-toml\n\n";
        for (uint32_t s : seeds) {
            sfs << fmt::format("0x{:08X}\n", s);
        }
        sfs.close();
        fmt::print("Wrote {} seeds to {}\n", seeds.size(), seeds_path.string());
    }

    // Summary
    fmt::print(stderr, "EXE:      {} ({})\n", exe_path.string(), game_id);
    fmt::print(stderr, "Entry:    0x{:08X}\n", entry_pc);
    fmt::print(stderr, "Load:     0x{:08X}\n", load_address);
    fmt::print(stderr, "Text:     0x{:X} ({} KB)\n", exe.code_size(), exe.code_size() / 1024);
    fmt::print(stderr, "Stack:    0x{:08X}\n", stack_base);
    if (!seeds_path.empty()) {
        const uint32_t seed_total = jal_targets.size()
            + (include_after_return ? after_ret.size() : 0) + 1; // +1 for entry
        fmt::print(stderr, "Seeds:    {} JAL targets", jal_targets.size());
        if (include_after_return)
            fmt::print(stderr, ", {} after-return", after_ret.size());
        fmt::print(stderr, " -> {}\n", seeds_path.string());
    }

    return 0;
}
