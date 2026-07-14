#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <algorithm>
#include <set>
#include <vector>

#include "ps1_exe_parser.h"
#include "gte.h"
#include "function_analysis.h"
#include "control_flow.h"
#include "code_generator.h"
#include "annotations.hpp"
#include "config_loader.h"
#include "rabbitizer.hpp"
#include "fmt/format.h"

/* Baked emitter-source hash (recompiler/CMakeLists.txt custom command; same
 * hash_codegen.cmake + canonical source list as the runtime's overlay cache
 * tag). Exposed via --codegen-hash so compile_overlays.py can verify THIS
 * binary was built from the same emitter sources as the cg tag it stamps —
 * the stale-recompiler-binary guard. 0 when built without the bake (guard
 * consumers treat that as "cannot verify" and fail loudly). */
#if defined(__has_include)
#  if __has_include("psxrecomp_baked_codegen_hash.h")
#    include "psxrecomp_baked_codegen_hash.h"
#  endif
#endif
#ifndef PSX_OVERLAY_CODEGEN_HASH
#  define PSX_OVERLAY_CODEGEN_HASH 0u
#endif

namespace {

struct AliasEntry { uint32_t addr, host_start, host_end; };

// Materialize alias entries as overlapping functions, grouped by host. Each
// group shares one emitted body (entry switch + host-range blocks); every
// member lists its siblings so all group CFGs carry identical block leaders.
// Host functions are emitted unchanged — aliases never cap or truncate them.
void materialize_alias_groups(PSXRecomp::FunctionAnalysisResult& result,
                              const std::vector<AliasEntry>& alias_entries) {
    std::map<uint32_t, std::vector<const AliasEntry*>> by_host;
    for (const auto& ae : alias_entries) {
        by_host[ae.host_start].push_back(&ae);
    }
    for (const auto& [host_start, group] : by_host) {
        std::vector<uint32_t> entries;
        for (const AliasEntry* ae : group) entries.push_back(ae->addr);
        for (const AliasEntry* ae : group) {
            PSXRecomp::Function af;
            af.start_addr = ae->addr;
            af.end_addr = ae->host_end;
            af.size = af.end_addr - af.start_addr;
            af.has_prologue = false;
            af.has_epilogue = false;
            af.stack_frame_size = 0;
            af.name = fmt::format("func_{:08X}", ae->addr);
            af.is_data_section = false;
            af.alias_walk_lo = ae->host_start;
            af.alias_group_entries = entries;
            result.functions.push_back(af);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    /* --codegen-hash: print the baked emitter-source hash and exit. Handled
     * before the banner so the output is machine-parseable (one hex line).
     * compile_overlays.py compares this to the hash from --runtime-include and
     * refuses to build shards with a stale recompiler binary. */
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--codegen-hash") {
            fmt::print("{:08x}\n", (unsigned)PSX_OVERLAY_CODEGEN_HASH);
            return 0;
        }
    }

    fmt::print("PSXRecomp - PlayStation 1 Static Recompiler\n");
    fmt::print("============================================\n\n");

    // ── --config <path> short-form ────────────────────────────────────
    // If --config is provided, all paths come from the TOML and other
    // CLI flags are ignored. Positional form below stays for back-compat.
    std::filesystem::path config_path;
    // --ws-config <path>: load ONLY the [widescreen] site lists from this TOML
    // without treating it as the full game config. Used by compile_overlays.py
    // so overlay compilation gets the widescreen emits (sprite-tag / cull /
    // backdrop) whose addresses live in overlay code — the overlay path is
    // positional (no --config), so this is the channel for those sites.
    std::filesystem::path ws_config_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
        if (a.rfind("--config=", 0) == 0) {
            config_path = a.substr(std::string("--config=").size());
            break;
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ws-config" && i + 1 < argc) { ws_config_path = argv[i + 1]; break; }
        if (a.rfind("--ws-config=", 0) == 0) {
            ws_config_path = a.substr(std::string("--ws-config=").size());
            break;
        }
    }

    std::filesystem::path exe_path;
    std::string           extra_funcs_storage;  // lifetime anchor for the .c_str() below
    const char*           extra_funcs_path = nullptr;
    bool                  inspect_mode = false;
    bool                  overlay_mode = false;
    std::set<uint32_t>    ws_tag_funcs;         // [widescreen] sprite_tag_funcs
    std::set<uint32_t>    ws_cull_bias, ws_cull_range, ws_cull_a1; // [widescreen.cull]
    std::set<uint32_t>    ws_cull_screen_x;    // [widescreen.cull] screen_x_sites
    std::set<uint32_t>    ws_cull_slti;         // [widescreen.cull] slti_sites
    std::vector<uint32_t> ws_cull_w_imms = { 0x140, 0x141 }; // [widescreen.cull] screen_w_imms
    std::vector<uint32_t> ws_cull_h_imms = { 0xE0, 0xF1 };   // [widescreen.cull] screen_h_imms
    std::set<uint32_t>    ws_backdrop_x;        // [widescreen.backdrop] x_sites
    std::set<uint32_t>    ws_backdrop_unsquash; // [widescreen.backdrop] unsquash_funcs
    bool                  ws_auto_screen_x_cull = false; // [widescreen.cull] auto_screen_x
    std::set<uint32_t>    persist_init_sites;   // [persist_options] init-store hooks (game_options.toml)
    bool                  ws_auto_backdrop_preload = false; // [widescreen.cull] auto_backdrop
    uint32_t              ws_bg2d_count_site = 0, ws_bg2d_startcol_site = 0,
                          ws_bg2d_startx_site = 0,
                          ws_bg2d_stream_left_site = 0,
                          ws_bg2d_stream_right_site = 0,
                          ws_bg2d_bufbase_site = 0,
                          ws_bg2d_cap_site = 0,
                          ws_bg2d_init_func = 0; // [widescreen.bg2d]
    std::filesystem::path out_dir = "generated";

    if (!config_path.empty()) {
        const auto cfg = PSXRecompV4::load_game_config(config_path);
        exe_path             = cfg.exe_path;
        extra_funcs_storage  = cfg.seeds_path.string();
        extra_funcs_path     = extra_funcs_storage.c_str();
        out_dir              = cfg.out_dir;
        ws_tag_funcs.insert(cfg.ws_sprite_tag_funcs.begin(),
                            cfg.ws_sprite_tag_funcs.end());
        ws_cull_bias.insert(cfg.ws_cull_bias_sites.begin(), cfg.ws_cull_bias_sites.end());
        ws_cull_range.insert(cfg.ws_cull_range_sites.begin(), cfg.ws_cull_range_sites.end());
        ws_cull_a1.insert(cfg.ws_cull_a1_sites.begin(), cfg.ws_cull_a1_sites.end());
        ws_cull_screen_x.insert(cfg.ws_cull_screen_x_sites.begin(), cfg.ws_cull_screen_x_sites.end());
        ws_cull_slti.insert(cfg.ws_cull_slti_sites.begin(), cfg.ws_cull_slti_sites.end());
        ws_cull_w_imms = cfg.ws_cull_w_imms;
        ws_cull_h_imms = cfg.ws_cull_h_imms;
        ws_backdrop_x.insert(cfg.ws_backdrop_x_sites.begin(), cfg.ws_backdrop_x_sites.end());
        ws_backdrop_unsquash.insert(cfg.ws_backdrop_unsquash_funcs.begin(), cfg.ws_backdrop_unsquash_funcs.end());
        ws_auto_screen_x_cull = ws_auto_screen_x_cull || cfg.ws_auto_screen_x_cull;
        ws_auto_backdrop_preload = ws_auto_backdrop_preload || cfg.ws_auto_backdrop_preload;
        if (cfg.ws_bg2d_count_site)    ws_bg2d_count_site    = cfg.ws_bg2d_count_site;
        if (cfg.ws_bg2d_startcol_site) ws_bg2d_startcol_site = cfg.ws_bg2d_startcol_site;
        if (cfg.ws_bg2d_startx_site)   ws_bg2d_startx_site   = cfg.ws_bg2d_startx_site;
        if (cfg.ws_bg2d_stream_left_site)  ws_bg2d_stream_left_site  = cfg.ws_bg2d_stream_left_site;
        if (cfg.ws_bg2d_stream_right_site) ws_bg2d_stream_right_site = cfg.ws_bg2d_stream_right_site;
        if (cfg.ws_bg2d_bufbase_site) ws_bg2d_bufbase_site = cfg.ws_bg2d_bufbase_site;
        if (cfg.ws_bg2d_cap_site)     ws_bg2d_cap_site     = cfg.ws_bg2d_cap_site;
        if (cfg.ws_bg2d_init_func)    ws_bg2d_init_func    = cfg.ws_bg2d_init_func;
        // [persist_options] init-store hook sites live in a dedicated
        // game_options.toml next to game.toml (the game's own native OPTION
        // settings, kept separate from game.toml/settings.toml). Best-effort:
        // a missing/bad file just means no persistence hooks this build.
        try {
            std::filesystem::path go_path =
                std::filesystem::path(config_path).parent_path() / "game_options.toml";
            const auto goc = PSXRecompV4::load_game_options(go_path);
            for (const auto& o : goc.options)
                if (o.init_store_pc) persist_init_sites.insert(o.init_store_pc);
        } catch (const std::exception& e) {
            fmt::print(stderr, "  game_options.toml ignored: {}\n", e.what());
        }
        fmt::print("config:         {}\n", config_path.string());
        fmt::print("  exe         = {}\n", exe_path.string());
        fmt::print("  seeds       = {}\n", extra_funcs_storage);
        fmt::print("  out_dir     = {}\n", out_dir.string());
        if (!ws_tag_funcs.empty())
            fmt::print("  ws_tag_funcs= {}\n", ws_tag_funcs.size());
        fmt::print("\n");
    } else {
        if (argc < 2) {
            fmt::print("Usage: {} --config <game.toml>                  # going-forward\n", argv[0]);
            fmt::print("       {} <PS1-EXE file> [--seeds <file>] [--out-dir <dir>] [--strict] [--inspect]\n", argv[0]);
            fmt::print("Example: {} SCUS_942.36 --seeds seeds/ghidra_funcs.txt --out-dir generated --strict\n\n", argv[0]);
            return 0;
        }

        exe_path = argv[1];
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if ((arg == "--extra-funcs" || arg == "--seeds") && i + 1 < argc) {
                extra_funcs_path = argv[++i];
            } else if (arg == "--out-dir" && i + 1 < argc) {
                out_dir = argv[++i];
            } else if (arg == "--strict") {
                /* The PS-X EXE path is fail-loud by default; accepted for parity
                 * with psxrecomp-bios and project scripts. */
            } else if (arg == "--inspect") {
                inspect_mode = true;
            } else if (arg == "--overlay") {
                /* Overlay-compilation contract: this input is a runtime-captured
                 * overlay with execution evidence, so discovery is evidence-scoped
                 * (no whole-byte sweep) and branch/jump-table targets stay as
                 * in-parent labels. Set unconditionally by compile_overlays.py for
                 * every overlay input — never a human toggle. */
                overlay_mode = true;
            }
        }
    }

    // --ws-config: merge the [widescreen] site lists from a side config WITHOUT
    // adopting its exe/paths. The overlay path is positional (no --config), so
    // this is how overlay compilation receives the widescreen emits whose
    // addresses live in overlay code (backdrop screenX, and any sprite-tag /
    // cull sites that resolve there). No-op when --config already provided the
    // sites (same TOML) — std::set insert dedupes.
    if (!ws_config_path.empty()) {
        const auto wscfg = PSXRecompV4::load_game_config(ws_config_path);
        ws_tag_funcs.insert(wscfg.ws_sprite_tag_funcs.begin(), wscfg.ws_sprite_tag_funcs.end());
        ws_cull_bias.insert(wscfg.ws_cull_bias_sites.begin(), wscfg.ws_cull_bias_sites.end());
        ws_cull_range.insert(wscfg.ws_cull_range_sites.begin(), wscfg.ws_cull_range_sites.end());
        ws_cull_a1.insert(wscfg.ws_cull_a1_sites.begin(), wscfg.ws_cull_a1_sites.end());
        ws_cull_screen_x.insert(wscfg.ws_cull_screen_x_sites.begin(), wscfg.ws_cull_screen_x_sites.end());
        ws_cull_slti.insert(wscfg.ws_cull_slti_sites.begin(), wscfg.ws_cull_slti_sites.end());
        ws_cull_w_imms = wscfg.ws_cull_w_imms;
        ws_cull_h_imms = wscfg.ws_cull_h_imms;
        ws_backdrop_x.insert(wscfg.ws_backdrop_x_sites.begin(), wscfg.ws_backdrop_x_sites.end());
        ws_backdrop_unsquash.insert(wscfg.ws_backdrop_unsquash_funcs.begin(), wscfg.ws_backdrop_unsquash_funcs.end());
        ws_auto_screen_x_cull = ws_auto_screen_x_cull || wscfg.ws_auto_screen_x_cull;
        ws_auto_backdrop_preload = ws_auto_backdrop_preload || wscfg.ws_auto_backdrop_preload;
        if (wscfg.ws_bg2d_init_func) ws_bg2d_init_func = wscfg.ws_bg2d_init_func;
        fmt::print("ws-config:      {} (backdrop_x sites={}, unsquash funcs={})\n",
                   ws_config_path.string(), ws_backdrop_x.size(), ws_backdrop_unsquash.size());
    }

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

    if (inspect_mode) {
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
    }

    // Function boundary detection
    fmt::print("Performing function analysis...\n");

    std::vector<uint32_t> exact_entries;
    exact_entries.push_back(exe->header.initial_pc);

    // Explicit entry points: functions called through pointers but not
    // automatically detected by the heuristic-based function analysis.
    // Pass game-specific entry points via --extra-funcs file.

    /* Load extra function addresses from a file (one hex address per line).
     * Lines of the form `interior 0xXXXXXXXX` mark dispatch-proven interior
     * addresses: alias candidates, never walk roots. Lines of the form
     * `dispatch_root 0xXXXXXXXX` mark classifier-proven dispatch roots
     * WITHOUT a callable boundary (the kernel install-slot class, e.g. RAM
     * 0xCF0): the static recompiler's install-slot hooks tail-dispatch into
     * exactly these PCs, so they are real execution roots even though no
     * prologue / preceding jr $ra exists. They are trusted walk roots and
     * exempt from the overlay-mode boundary re-check below.
     *
     * Seeds are accepted within the loaded image's own bounds — overlay mode
     * wraps arbitrary regions (kernel RAM at 0x80000000, overlays at
     * 0x800E7000, ...), so a hardcoded game-RAM window would silently drop
     * valid seeds. */
    std::vector<uint32_t> file_seeds;
    std::vector<uint32_t> interior_seeds;
    std::set<uint32_t>    trusted_root_seeds;
    if (extra_funcs_path) {
        std::ifstream ef(extra_funcs_path);
        if (ef.is_open()) {
            const uint32_t seed_lo = exe->header.load_address;
            const uint32_t seed_hi = exe->end_address();
            std::string line;
            while (std::getline(ef, line)) {
                if (line.empty() || line[0] == '#') continue;
                bool interior = false;
                bool trusted_root = false;
                const char* p = line.c_str();
                if (line.rfind("interior", 0) == 0) {
                    interior = true;
                    p += 8;
                } else if (line.rfind("dispatch_root", 0) == 0) {
                    trusted_root = true;
                    p += 13;
                }
                uint32_t addr = (uint32_t)std::strtoul(p, nullptr, 16);
                if (addr >= seed_lo && addr < seed_hi) {
                    if (interior) {
                        interior_seeds.push_back(addr);
                    } else {
                        if (trusted_root) trusted_root_seeds.insert(addr);
                        file_seeds.push_back(addr);
                        exact_entries.push_back(addr);
                    }
                }
            }
            fmt::print("Loaded {} extra function addresses ({} interior, "
                       "{} dispatch-root) from {}\n",
                       file_seeds.size() + interior_seeds.size(),
                       interior_seeds.size(), trusted_root_seeds.size(),
                       extra_funcs_path);
        } else {
            fmt::print("WARNING: Cannot open extra-funcs file: {}\n", extra_funcs_path);
        }
    }

    PSXRecomp::FunctionAnalysisResult analysis_result;

    if (overlay_mode) {
        // ── Overlay exact mode: partition seeds into walk roots and interior
        // alias candidates. `interior`-marked seeds (classifier-proven
        // dispatch targets without a callable boundary) are NEVER walk roots —
        // a root inside a host function hard-caps (truncates) it, the
        // mid-function-seed softlock class. Unmarked seeds are re-verified
        // against the same boundary rule as defense in depth.
        const uint32_t exe_lo = exe->header.load_address;
        auto read_w = [&](uint32_t a) -> uint32_t {
            auto w = exe->read_word(a);
            return w.has_value() ? *w : 0u;
        };
        auto callable_boundary = [&](uint32_t a) -> bool {
            uint32_t w = read_w(a);
            if (!PSXRecomp::FunctionAnalyzer::is_valid_mips_word(w)) return false;
            if (a == exe_lo) return true;
            if (a >= exe_lo + 8 && read_w(a - 8) == 0x03E00008u) return true;
            int32_t frame = 0;
            return PSXRecomp::FunctionAnalyzer::is_prologue(w, frame) &&
                   !(a >= exe_lo + 4 &&
                     PSXRecomp::FunctionAnalyzer::is_branch_or_jump(read_w(a - 4)));
        };

        std::set<uint32_t> roots;
        std::set<uint32_t> interior;
        for (uint32_t a : exact_entries) {
            if (trusted_root_seeds.count(a)) {
                /* Classifier-proven dispatch root (install-slot class): the
                 * static code tail-dispatches into this PC, so it is a real
                 * execution root despite having no callable boundary. */
                fmt::print("  seed 0x{:08X} accepted as dispatch root "
                           "(install-slot class, no boundary evidence)\n", a);
                roots.insert(a);
            } else if (callable_boundary(a)) {
                roots.insert(a);
            } else {
                fmt::print("  seed 0x{:08X} fails the boundary check — "
                           "treating as interior alias candidate\n", a);
                interior.insert(a);
            }
        }
        for (uint32_t a : interior_seeds) interior.insert(a);

        auto containing = [&](uint32_t a) -> const PSXRecomp::Function* {
            const PSXRecomp::Function* best = nullptr;
            for (const auto& f : analysis_result.functions) {
                if (a >= f.start_addr && a < f.end_addr) { best = &f; break; }
            }
            return best;
        };

        // Interior candidates attach only to functions the EVIDENCE-walked
        // root set already owns. No backward-boundary guessing: a false
        // prologue/jr-ra in embedded data mints a root that decodes data as
        // code (audit-fatal). Orphans stay with the interpreter — it
        // self-heals via recapture once real call evidence appears.
        {
            PSXRecomp::FunctionAnalyzer analyzer(*exe);
            std::vector<uint32_t> roots_vec(roots.begin(), roots.end());
            analysis_result = analyzer.analyze_exact_entries(roots_vec);
        }

        std::vector<AliasEntry> alias_entries;
        std::set<uint32_t> alias_seen;
        for (uint32_t a : interior) {
            const PSXRecomp::Function* host = containing(a);
            if (host && a == host->start_addr) continue;  // became a real entry
            if (host) {
                if (alias_seen.insert(a).second)
                    alias_entries.push_back({a, host->start_addr, host->end_addr});
            } else {
                fmt::print("  WARNING: interior seed 0x{:08X} has no host "
                           "function — left to the interpreter\n", a);
            }
        }

        // Range-ownership completeness (B): a DIRECT CALL (jal) whose target
        // lands strictly INSIDE an already-discovered function is a separate
        // routine the host's discovered range absorbed (e.g. via a break/trap
        // fall-through in discovery — the crt0 `jal main; break` that swallowed
        // the next function, the Tomba 2 FMV-freeze root cause). Register every
        // such target as an ALIAS entry so the host's entry-switch routes a
        // dispatch of that PC to the correct interior block (running the absorbed
        // routine natively) instead of failing closed to the interpreter. This is
        // derived purely from static jal evidence in the discovered code, so it
        // does not depend on the PC having been captured/executed. jal encodes a
        // code target, so this never mints code from data.
        size_t jal_alias_added = 0;
        {
            std::set<uint32_t> known_starts;
            for (const auto& f : analysis_result.functions)
                known_starts.insert(f.start_addr);
            std::set<uint32_t> jal_targets;
            for (const auto& f : analysis_result.functions) {
                for (uint32_t a = f.start_addr; a + 4 <= f.end_addr; a += 4) {
                    uint32_t w = read_w(a);
                    if ((w >> 26) == 0x03u) {  // jal
                        uint32_t tgt = (a & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
                        jal_targets.insert(tgt);
                    }
                }
            }
            for (uint32_t t : jal_targets) {
                if (known_starts.count(t)) continue;          // already a real entry
                if (!alias_seen.count(t)) {
                    const PSXRecomp::Function* host = containing(t);
                    if (host && t != host->start_addr) {
                        alias_seen.insert(t);
                        alias_entries.push_back({t, host->start_addr, host->end_addr});
                        jal_alias_added++;
                    }
                }
            }
        }
        if (jal_alias_added)
            fmt::print("  +{} in-function jal-target alias entries "
                       "(range-ownership completeness)\n", jal_alias_added);

        materialize_alias_groups(analysis_result, alias_entries);
        fmt::print("Overlay alias entries emitted: {}\n\n", alias_entries.size());
    } else {
        // ── Iterative discovery: seed classification + static data-table scan ──
        //
        // Every candidate entry (explicit seed or scanned data pointer) is
        // classified against the current function set:
        //   * matches a function start            → already covered
        //   * INSIDE a function's range           → ALIAS entry (overlapping
        //     emission; the host is never capped/truncated — forcing such an
        //     entry is the mid-function-seed softlock class)
        //   * in a gap between functions          → forced entry (explicit
        //     seeds are trusted; scanned pointers additionally need boundary
        //     evidence: a jr $ra two slots earlier or a non-delay-slot
        //     prologue)
        //
        // The data scan walks every word of the image OUTSIDE code-function
        // ranges (pointer tables live in data regions) and collects words that
        // point at valid instructions inside code-function ranges. Promotions
        // change function boundaries, so iterate to a fixed point.
        std::vector<AliasEntry> alias_entries;

        std::set<uint32_t> forced;
        forced.insert(exe->header.initial_pc);
        const uint32_t exe_lo = exe->header.load_address;
        const uint32_t exe_hi = exe->end_address();
        auto read_w = [&](uint32_t a) -> uint32_t {
            auto w = exe->read_word(a);
            return w.has_value() ? *w : 0u;
        };

        const int kMaxDiscoveryIters = 6;
        for (int iter = 0; iter < kMaxDiscoveryIters; ++iter) {
            PSXRecomp::FunctionAnalyzer analyzer(*exe);
            for (uint32_t a : forced) analyzer.add_forced_entry(a);
            analysis_result = analyzer.analyze();

            std::vector<const PSXRecomp::Function*> code_funcs;
            for (const auto& f : analysis_result.functions) {
                if (!f.is_data_section) code_funcs.push_back(&f);
            }
            auto containing = [&](uint32_t a) -> const PSXRecomp::Function* {
                auto it = std::upper_bound(code_funcs.begin(), code_funcs.end(), a,
                    [](uint32_t v, const PSXRecomp::Function* f) { return v < f->start_addr; });
                if (it == code_funcs.begin()) return nullptr;
                --it;
                return (a >= (*it)->start_addr && a < (*it)->end_addr) ? *it : nullptr;
            };

            struct Candidate { uint32_t addr; bool trusted; bool table_evidence; };
            std::vector<Candidate> candidates;
            for (uint32_t s : file_seeds) candidates.push_back({s, true, true});
            for (uint32_t s : interior_seeds) candidates.push_back({s, true, true});
            uint32_t scanned_words = 0;
            auto is_text_ptr = [&](uint32_t v) {
                return (v & 3u) == 0 && v >= exe_lo && v < exe_hi && containing(v) != nullptr;
            };
            for (uint32_t p = exe_lo; p + 4 <= exe_hi; p += 4) {
                if (containing(p)) continue;  // instruction word, not a data word
                uint32_t w = read_w(p);
                if ((w & 3u) || w < exe_lo || w >= exe_hi) continue;
                // Table evidence: an adjacent data word that is also a text
                // pointer. Interior handler entries live in dense pointer
                // tables; a lone pointer-shaped data word is too weak to mint
                // an alias from (random data aliases into loose code ranges).
                bool table_evidence =
                    (p >= exe_lo + 4 && is_text_ptr(read_w(p - 4))) ||
                    (p + 8 <= exe_hi && is_text_ptr(read_w(p + 4)));
                candidates.push_back({w, false, table_evidence});
                scanned_words++;
            }

            alias_entries.clear();
            std::set<uint32_t> alias_seen;
            bool promoted_new = false;
            for (const auto& [a, trusted, table_evidence] : candidates) {
                if ((a & 3u) || a < exe_lo || a >= exe_hi) continue;
                const PSXRecomp::Function* host = containing(a);
                if (host && a == host->start_addr) continue;  // already an entry
                uint32_t w = read_w(a);
                if (!PSXRecomp::FunctionAnalyzer::is_valid_mips_word(w)) continue;
                if (host) {
                    if ((trusted || table_evidence) && alias_seen.insert(a).second) {
                        alias_entries.push_back({a, host->start_addr, host->end_addr});
                    }
                    continue;
                }
                bool promote = trusted;
                if (!promote) {
                    bool after_ret = (a >= exe_lo + 8) && (read_w(a - 8) == 0x03E00008u);
                    int32_t frame = 0;
                    bool prologue =
                        PSXRecomp::FunctionAnalyzer::is_prologue(w, frame) &&
                        !(a >= exe_lo + 4 &&
                          PSXRecomp::FunctionAnalyzer::is_branch_or_jump(read_w(a - 4)));
                    promote = after_ret || prologue;
                }
                if (promote && forced.insert(a).second) promoted_new = true;
            }

            fmt::print("Discovery iteration {}: {} data pointers scanned, "
                       "{} forced entries, {} alias entries\n",
                       iter + 1, scanned_words, forced.size(), alias_entries.size());
            if (!promoted_new) break;
        }

        materialize_alias_groups(analysis_result, alias_entries);
        fmt::print("Alias entries emitted: {}\n\n", alias_entries.size());
    }

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
    codegen_config.split_mid_function_targets = !overlay_mode;
    codegen_config.overlay_mode = overlay_mode;
    codegen_config.ws_sprite_tag_funcs = ws_tag_funcs;
    codegen_config.ws_bg2d_init_func = ws_bg2d_init_func;
    codegen_config.ws_cull_bias_sites  = ws_cull_bias;
    codegen_config.ws_cull_range_sites = ws_cull_range;
    codegen_config.ws_cull_a1_sites    = ws_cull_a1;
    codegen_config.ws_cull_screen_x_sites = ws_cull_screen_x;
    codegen_config.ws_cull_slti_sites  = ws_cull_slti;
    codegen_config.ws_cull_w_imms      = ws_cull_w_imms;
    codegen_config.ws_cull_h_imms      = ws_cull_h_imms;
    codegen_config.ws_backdrop_x_sites = ws_backdrop_x;
    codegen_config.ws_backdrop_unsquash_funcs = ws_backdrop_unsquash;
    codegen_config.ws_auto_screen_x_cull = ws_auto_screen_x_cull;
    codegen_config.persist_init_store_sites = persist_init_sites;
    codegen_config.ws_auto_backdrop_preload = ws_auto_backdrop_preload;
    codegen_config.ws_bg2d_count_site    = ws_bg2d_count_site;
    codegen_config.ws_bg2d_startcol_site = ws_bg2d_startcol_site;
    codegen_config.ws_bg2d_startx_site   = ws_bg2d_startx_site;
    codegen_config.ws_bg2d_stream_left_site  = ws_bg2d_stream_left_site;
    codegen_config.ws_bg2d_stream_right_site = ws_bg2d_stream_right_site;
    codegen_config.ws_bg2d_bufbase_site = ws_bg2d_bufbase_site;
    codegen_config.ws_bg2d_cap_site     = ws_bg2d_cap_site;
    if (ws_bg2d_count_site || ws_bg2d_startcol_site || ws_bg2d_startx_site)
        fmt::print("  ws_bg2d 2D-background widen = ON (count=0x{:08X} startcol=0x{:08X} startx=0x{:08X})\n",
                   ws_bg2d_count_site, ws_bg2d_startcol_site, ws_bg2d_startx_site);
    if (ws_auto_screen_x_cull)
        fmt::print("  ws_auto_screen_x_cull = ON (render-funnel FOV widening)\n");
    if (!persist_init_sites.empty())
        fmt::print("  persist_options = {} init-store hook(s)\n", persist_init_sites.size());
    if (ws_auto_backdrop_preload)
        fmt::print("  ws_auto_backdrop_preload = ON (far-backdrop column preload)\n");

    // Load per-game annotations: annotations/<exe_stem>_annotations.csv
    // Silently skipped if the file doesn't exist.
    PSXRecomp::AnnotationTable annotations;
    {
        std::string stem = exe_path.filename().string();
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
    // Use argv[2] as output path if provided, otherwise derive from input filename
    // Use filename() not stem() because ".36" in SCUS_942.36 is part of the serial, not an extension
    std::string exe_stem = exe_path.filename().string();
    std::filesystem::create_directories(out_dir);
    std::filesystem::path output_filename = out_dir / (exe_stem + "_full.c");
    fmt::print("Generating complete C file: {}\n", output_filename.string());

    std::string full_c_code = codegen.generate_file(analysis_result.functions, all_cfgs);
    std::set<uint32_t> dispatch_addrs;
    {
        const std::string marker = "void func_";
        size_t pos = 0;
        while ((pos = full_c_code.find(marker, pos)) != std::string::npos) {
            size_t hex_pos = pos + marker.size();
            if (hex_pos + 8 <= full_c_code.size() &&
                full_c_code.compare(hex_pos + 8, 14, "(CPUState* cpu") == 0) {
                std::string hex = full_c_code.substr(hex_pos, 8);
                dispatch_addrs.insert((uint32_t)std::strtoul(hex.c_str(), nullptr, 16));
            }
            pos = hex_pos + 8;
        }
    }
    if (dispatch_addrs.empty()) {
        for (const auto& func : analysis_result.functions) {
            dispatch_addrs.insert(func.start_addr);
        }
    }

    std::ofstream out_file(output_filename);
    if (out_file.is_open()) {
        out_file << full_c_code;
        out_file.close();
        fmt::print("✓ Saved {} lines to {}\n\n",
                  std::count(full_c_code.begin(), full_c_code.end(), '\n'),
                  output_filename.string());
    } else {
        fmt::print(stderr, "⚠ Failed to write output file\n\n");
    }

    // Per-function code-range manifest (design §8): consumed by the overlay
    // loader's per-entry validity hash. Emitted alongside _full.c for every
    // build; only the overlay path actually loads it.
    const std::string ranges_manifest = codegen.last_ranges_manifest().empty()
        ? codegen.generate_ranges_manifest(analysis_result.functions, all_cfgs)
        : codegen.last_ranges_manifest();
    {
        std::filesystem::path ranges_filename = out_dir / (exe_stem + "_full.ranges");
        std::ofstream rf(ranges_filename);
        if (rf.is_open()) {
            rf << ranges_manifest;
            rf.close();
            fmt::print("✓ Saved code-range manifest to {}\n\n",
                      ranges_filename.string());
        }
    }

    // Generate dispatch table (tomba_dispatch.c)
    // This maps PS1 addresses to compiled C functions so call_by_address() can
    // dispatch dynamic jalr/jr calls to the right compiled function.
    {
        std::filesystem::path dispatch_filename = out_dir / (exe_stem + "_dispatch.c");
        fmt::print("Generating dispatch table: {}\n", dispatch_filename.string());

        std::ostringstream ds;
        ds << "/* Generated by PSXRecomp - dynamic dispatch table */\n";
        ds << "#include \"psx_runtime.h\"\n\n";
        ds << "extern void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t resume_pc);\n\n";
        ds << "extern int dirty_ram_text_native_ok_ranges(const uint32_t* lo_len_pairs, uint32_t count);\n\n";

        // Forward declarations
        ds << "/* Forward declarations for all recompiled functions */\n";
        for (uint32_t addr : dispatch_addrs) {
            ds << fmt::format("extern void func_{:08X}(CPUState* cpu);\n", addr);
        }
        ds << "\n";

        // Dispatch function
        uint32_t game_text_start = exe->load_address() & 0x1FFFFFFFu;
        uint32_t game_text_end = game_text_start + exe->code_size();
        ds << "int psx_game_address_in_text(uint32_t addr) {\n";
        ds << "    uint32_t phys = addr & 0x1FFFFFFFu;\n";
        ds << fmt::format("    return phys >= 0x{:08X}u && phys < 0x{:08X}u;\n",
                          game_text_start, game_text_end);
        ds << "}\n\n";

        // Materialize one sorted data table for dispatch and entry probes. A
        // giant sparse switch is catastrophically slow in Debug/-O0 builds:
        // GCC lowers X4's roughly 60K cases to a multi-megabyte linear compare
        // chain. Binary search stays O(log N) at every optimizer level and
        // avoids emitting the same case set twice.
        struct DispatchRecord {
            uint32_t addr;
            uint32_t resume;
            uint32_t owner;
            uint32_t range_index;
            uint32_t range_count;
        };
        std::vector<DispatchRecord> records;
        records.reserve(dispatch_addrs.size() +
                        (codegen.cps_enabled() ? codegen.cps_continuations().size() : 0));
        for (uint32_t addr : dispatch_addrs)
            records.push_back({addr, 0, addr, 0, 0});
        if (codegen.cps_enabled()) {
            const auto& conts = codegen.cps_continuations();
            for (const auto& [cont, owner] : conts) {
                if (dispatch_addrs.count(cont)) continue;
                records.push_back({cont, cont, owner, 0, 0});
            }
        }
        std::sort(records.begin(), records.end(),
                  [](const DispatchRecord& a, const DispatchRecord& b) {
                      return a.addr < b.addr;
                  });

        // Attach the exact CFG instruction ranges from the manifest to every
        // dispatch owner. Mid-function alias wrappers have no separate F record,
        // so resolve them to the containing host range. Mutable data and
        // jump-table gaps sharing a page remain deliberately excluded.
        using CodeRange = std::pair<uint32_t, uint32_t>;  // virtual lo, byte len
        std::map<uint32_t, std::vector<CodeRange>> function_ranges;
        {
            std::istringstream input(ranges_manifest);
            std::string line;
            uint32_t current_owner = 0;
            while (std::getline(input, line)) {
                std::istringstream fields(line);
                char kind = 0;
                fields >> kind;
                if (kind == 'F') {
                    fields >> std::hex >> current_owner;
                    function_ranges[current_owner];
                } else if (kind == 'R' && current_owner != 0) {
                    uint32_t lo = 0, len = 0;
                    fields >> std::hex >> lo >> len;
                    if (len != 0)
                        function_ranges[current_owner].push_back({lo, len});
                }
            }
        }
        std::vector<CodeRange> flat_ranges;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> owner_spans;
        auto resolve_owner_ranges = [&](uint32_t owner)
            -> const std::vector<CodeRange>* {
            auto exact = function_ranges.find(owner);
            if (exact != function_ranges.end() && !exact->second.empty())
                return &exact->second;
            for (const auto& [entry, ranges] : function_ranges) {
                (void)entry;
                for (const auto& [lo, len] : ranges) {
                    const uint64_t hi = (uint64_t)lo + len;
                    if (owner >= lo && (uint64_t)owner < hi) return &ranges;
                }
            }
            return nullptr;
        };
        for (auto& rec : records) {
            auto cached = owner_spans.find(rec.owner);
            if (cached == owner_spans.end()) {
                const uint32_t first = (uint32_t)flat_ranges.size();
                const auto* ranges = resolve_owner_ranges(rec.owner);
                if (ranges)
                    flat_ranges.insert(flat_ranges.end(), ranges->begin(), ranges->end());
                const uint32_t count = (uint32_t)flat_ranges.size() - first;
                cached = owner_spans.emplace(rec.owner,
                                             std::make_pair(first, count)).first;
            }
            rec.range_index = cached->second.first;
            rec.range_count = cached->second.second;
        }

        ds << "typedef struct { uint32_t lo; uint32_t len; } PsxGameCodeRange;\n";
        ds << "static const PsxGameCodeRange k_psx_game_code_ranges[] = {\n";
        if (flat_ranges.empty()) ds << "    {0u, 0u},\n";
        for (const auto& [lo, len] : flat_ranges)
            ds << fmt::format("    {{0x{:08X}u, 0x{:X}u}},\n", lo, len);
        ds << "};\n\n";

        ds << "typedef void (*PsxGameDispatchFn)(CPUState* cpu);\n";
        ds << "typedef struct {\n";
        ds << "    uint32_t addr;\n";
        ds << "    uint32_t resume_pc;\n";
        ds << "    uint32_t range_index;\n";
        ds << "    uint32_t range_count;\n";
        ds << "    PsxGameDispatchFn fn;\n";
        ds << "} PsxGameDispatchEntry;\n\n";
        ds << "static const PsxGameDispatchEntry k_psx_game_dispatch[] = {\n";
        for (const auto& rec : records) {
            ds << fmt::format("    {{0x{:08X}u, 0x{:08X}u, {}u, {}u, func_{:08X}}},\n",
                              rec.addr, rec.resume, rec.range_index,
                              rec.range_count, rec.owner);
        }
        ds << "};\n";
        ds << fmt::format("#define PSX_GAME_DISPATCH_COUNT {}u\n\n", records.size());
        ds << "static const PsxGameDispatchEntry* psx_game_find_entry(uint32_t addr) {\n";
        ds << "    uint32_t lo = 0, hi = PSX_GAME_DISPATCH_COUNT;\n";
        ds << "    while (lo < hi) {\n";
        ds << "        uint32_t mid = lo + (hi - lo) / 2;\n";
        ds << "        uint32_t key = k_psx_game_dispatch[mid].addr;\n";
        ds << "        if (addr < key) hi = mid;\n";
        ds << "        else if (addr > key) lo = mid + 1;\n";
        ds << "        else return &k_psx_game_dispatch[mid];\n";
        ds << "    }\n";
        ds << "    return 0;\n";
        ds << "}\n\n";

        ds << "/* Exact static-code validity for this entry's emitted CFG ranges. */\n";
        ds << "int psx_game_text_native_ok(uint32_t addr) {\n";
        ds << "    const PsxGameDispatchEntry* entry = psx_game_find_entry(addr);\n";
        ds << "    if (!entry || entry->range_count == 0) return 0;\n";
        ds << "    return dirty_ram_text_native_ok_ranges(\n";
        ds << "        &k_psx_game_code_ranges[entry->range_index].lo, entry->range_count);\n";
        ds << "}\n\n";

        ds << "/* Maps PS1 address to compiled game code. Returns 1 if dispatched, 0 if unknown. */\n";
        ds << "int psx_dispatch_game_compiled(CPUState* cpu, uint32_t addr) {\n";
        ds << "    const PsxGameDispatchEntry* entry = psx_game_find_entry(addr);\n";
        ds << "    if (!entry) return 0;\n";
        ds << "    if (entry->range_count == 0 || !dirty_ram_text_native_ok_ranges(\n";
        ds << "            &k_psx_game_code_ranges[entry->range_index].lo, entry->range_count)) return 0;\n";
        ds << "    psx_check_interrupts_dispatch_entry(cpu, addr);\n";
        if (codegen.cps_enabled())
            ds << "    cpu->pc = entry->resume_pc;\n";
        ds << "    entry->fn(cpu);\n";
        ds << "    return 1;\n";
        ds << "}\n";

        // Non-destructive companion to psx_dispatch_game_compiled: is `addr` a PC
        // the top-level dispatch can re-enter (a compiled function entry, or a CPS
        // continuation resume-point)? The precise event slicer (dirty_ram_interp.c)
        // needs this to know when it may hand a resume PC back to the dispatcher:
        // a MID-function clean-text PC is NOT re-enterable (the switch falls to
        // `default: return 0` and the dirty path returns 0 -> top-level PC=0 exit),
        // so the slicer must keep interpreting until cpu->pc lands on a true entry.
        // Same precise address set as dispatch, without executing the entry.
        ds << "\n/* 1 iff addr is a re-enterable compiled entry/continuation (no exec). */\n";
        ds << "int psx_game_is_function_entry(uint32_t addr) {\n";
        ds << "    return psx_game_find_entry(addr) != 0;\n";
        ds << "}\n";

        if (codegen.cps_enabled()) {
            // RECURSION_BUG.md §25 — mark CPS mode at startup so the overlay sljit
            // JIT (overlay_sljit.c) emits the CPS contract. Static ctor: no clash
            // with the BIOS dispatch's marker.
            ds << "\n/* CPS runtime-mode marker (overlay sljit JIT reads g_psx_cps_mode). */\n";
            ds << "__attribute__((constructor)) static void psx_cps_mark_game(void) {\n";
            ds << "    extern int g_psx_cps_mode; g_psx_cps_mode = 1;\n";
            ds << "}\n";
        }

        std::ofstream dispatch_file(dispatch_filename);
        if (dispatch_file.is_open()) {
            dispatch_file << ds.str();
            dispatch_file.close();
            fmt::print("✓ Dispatch table written ({} entries)\n\n",
                       dispatch_addrs.size());
        } else {
            fmt::print(stderr, "⚠ Failed to write dispatch file\n\n");
        }
    }

    return 0;
}
