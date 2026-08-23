// main_analyze.cpp — `psxrecomp-analyze`, the developer-facing function
// discovery tool.
//
// This binary deliberately links NOTHING from runtime/. That is the guarantee
// the tool sells: every function, edge, and jump table it reports was proven
// from the executable image alone. Coverage that would need dynamic evidence
// is reported as a gap, never filled in silently.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "fmt/format.h"

#include "analysis_db.h"
#include "analysis_export.h"
#include "widescreen_scan.h"
#include "ps1_exe_parser.h"

namespace fs = std::filesystem;
using namespace PSXRecomp;
using namespace PSXRecomp::Analysis;

namespace {

void usage() {
    fmt::print(R"(psxrecomp-analyze — static function discovery for PS-X EXE images

USAGE
  psxrecomp-analyze <EXE> [options]

INPUT
  --symbols <file>     symbols.toml to read names from (and merge back into
                       with --emit-symbols). Existing names always win.
  --entry <0xADDR>     extra seed entry point; repeatable
  --seeds <file>       file of one address per line (# comments allowed)
  --exact              reachability-only partition (no whole-image prologue
                       scan). Fewer, higher-confidence functions.

OUTPUT
  --out <dir>          write analysis.json / edges.json / indirect.json
                       (+ refs.json unless --no-refs)
  --no-refs            skip refs.json (it is the largest artifact)
  --emit-symbols       merge discovered functions into --symbols path
  --min-confidence <c> gate for --emit-symbols: verified|high|medium|low
                       (default: high)
  --emit-symbol-addrs <file>   decomp-style "name = 0xADDR;" map
  --emit-ghidra <file>         Ghidra import script (.py)
  --emit-tsv <file>            one line per function; also the --diff input
  --disasm <file>              annotated listing for --at (or whole image)
  --at <0xADDR>        function to disassemble with --disasm
  --diff <file>        compare against a previous run's TSV

WIDESCREEN
  --scan-widescreen    find [widescreen.cull] site candidates statically
  --ws-w-imms <list>   screen_w_imms override, e.g. 0x140,0x141 (else discovered)
  --ws-h-imms <list>   screen_h_imms override, e.g. 0xF0,0xE0
  --emit-ws-sites <f>  write a ready-to-paste [widescreen.cull] TOML block
  --ws-min-confidence <n>  gate for --emit-ws-sites (default 70)
  --top <n>            rows in the stdout report's top lists (default 15)
  --quiet              suppress the stdout report

Static analysis only. No runtime, trace, or overlay-capture input is consulted.
)");
}

bool parse_addr(const std::string& s, uint32_t& out) {
    try {
        size_t idx = 0;
        unsigned long v = std::stoul(s, &idx,
                                     (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0)
                                         ? 16 : 0);
        if (idx != s.size()) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool load_seed_file(const fs::path& p, std::vector<uint32_t>& out,
                    std::string& err) {
    std::ifstream in(p);
    if (!in) { err = fmt::format("cannot read {}", p.string()); return false; }
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        // Seed files in the wild carry a trailing label; take the first token.
        std::istringstream ls(line);
        std::string tok;
        if (!(ls >> tok)) continue;
        uint32_t a = 0;
        if (parse_addr(tok, a)) out.push_back(a);
    }
    return true;
}

// FunctionAnalyzer narrates its boundary scan straight to stdout. That is
// useful in the log pane of a GUI, but --quiet has to mean quiet, so redirect
// the descriptor around the call rather than editing the shared analyzer.
class StdoutMute {
public:
    explicit StdoutMute(bool active) : active_(active) {
        if (!active_) return;
        std::fflush(stdout);
#ifdef _WIN32
        saved_ = _dup(_fileno(stdout));
        FILE* null_fp = nullptr;
        fopen_s(&null_fp, "NUL", "w");
        if (null_fp) { _dup2(_fileno(null_fp), _fileno(stdout)); fclose(null_fp); }
#else
        saved_ = dup(fileno(stdout));
        FILE* null_fp = std::fopen("/dev/null", "w");
        if (null_fp) { dup2(fileno(null_fp), fileno(stdout)); std::fclose(null_fp); }
#endif
    }
    ~StdoutMute() {
        if (!active_ || saved_ < 0) return;
        std::fflush(stdout);
#ifdef _WIN32
        _dup2(saved_, _fileno(stdout));
        _close(saved_);
#else
        dup2(saved_, fileno(stdout));
        close(saved_);
#endif
    }
    StdoutMute(const StdoutMute&) = delete;
    StdoutMute& operator=(const StdoutMute&) = delete;

private:
    bool active_;
    int  saved_ = -1;
};

Confidence parse_confidence(const std::string& s) {
    if (s == "verified") return Confidence::Verified;
    if (s == "high")     return Confidence::High;
    if (s == "medium")   return Confidence::Medium;
    return Confidence::Low;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }

    fs::path exe_path;
    Options opts;
    fs::path out_dir, symbol_addrs, ghidra, tsv, disasm, diff;
    bool emit_symbols = false, quiet = false, no_refs = false;
    bool scan_ws = false;
    fs::path ws_sites;
    WsScanOptions ws_opts;
    int ws_min_conf = 70;
    Confidence min_conf = Confidence::High;
    uint32_t disasm_at = 0;
    int top_n = 15;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                fmt::print(stderr, "error: {} requires a value\n", what);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--symbols")   opts.symbols_toml = need("--symbols");
        else if (a == "--out")       out_dir = need("--out");
        else if (a == "--no-refs")   no_refs = true;
        else if (a == "--exact")     opts.exact_entries = true;
        else if (a == "--emit-symbols") emit_symbols = true;
        else if (a == "--min-confidence") min_conf = parse_confidence(need("--min-confidence"));
        else if (a == "--emit-symbol-addrs") symbol_addrs = need("--emit-symbol-addrs");
        else if (a == "--emit-ghidra") ghidra = need("--emit-ghidra");
        else if (a == "--emit-tsv")  tsv = need("--emit-tsv");
        else if (a == "--disasm")    disasm = need("--disasm");
        else if (a == "--diff")      diff = need("--diff");
        else if (a == "--quiet")     quiet = true;
        else if (a == "--scan-widescreen") scan_ws = true;
        else if (a == "--emit-ws-sites") { ws_sites = need("--emit-ws-sites"); scan_ws = true; }
        else if (a == "--ws-min-confidence") ws_min_conf = std::atoi(need("--ws-min-confidence").c_str());
        else if (a == "--ws-w-imms" || a == "--ws-h-imms") {
            const bool width = (a == "--ws-w-imms");
            std::string list = need(a.c_str());
            auto& dst = width ? ws_opts.w_imms : ws_opts.h_imms;
            std::string tok;
            std::istringstream ls(list);
            while (std::getline(ls, tok, ',')) {
                uint32_t v = 0;
                if (!tok.empty() && parse_addr(tok, v)) dst.push_back(v);
            }
            scan_ws = true;
        }
        else if (a == "--top")       top_n = std::atoi(need("--top").c_str());
        else if (a == "--at") {
            uint32_t v = 0;
            std::string s = need("--at");
            if (!parse_addr(s, v)) {
                fmt::print(stderr, "error: bad address '{}'\n", s);
                return 1;
            }
            disasm_at = v;
        } else if (a == "--entry") {
            uint32_t v = 0;
            std::string s = need("--entry");
            if (!parse_addr(s, v)) {
                fmt::print(stderr, "error: bad address '{}'\n", s);
                return 1;
            }
            opts.extra_entries.push_back(v);
        } else if (a == "--seeds") {
            std::string err;
            if (!load_seed_file(need("--seeds"), opts.extra_entries, err)) {
                fmt::print(stderr, "error: {}\n", err);
                return 1;
            }
        } else if (!a.empty() && a[0] == '-') {
            fmt::print(stderr, "error: unknown option '{}'\n", a);
            return 1;
        } else if (exe_path.empty()) {
            exe_path = a;
        } else {
            fmt::print(stderr, "error: unexpected argument '{}'\n", a);
            return 1;
        }
    }

    if (exe_path.empty()) { usage(); return 1; }
    if (no_refs) opts.collect_data_refs = false;

    std::string err;
    auto exe = PS1ExeParser::parse_file(exe_path, err);
    if (!exe) {
        fmt::print(stderr, "error: {}\n", err);
        return 1;
    }

    std::sort(opts.extra_entries.begin(), opts.extra_entries.end());
    opts.extra_entries.erase(
        std::unique(opts.extra_entries.begin(), opts.extra_entries.end()),
        opts.extra_entries.end());

    if (!quiet) {
        fmt::print("Analyzing {} ({} bytes at 0x{:08X})",
                   exe_path.filename().string(), exe->code_size(),
                   exe->load_address());
        if (!opts.extra_entries.empty())
            fmt::print(" with {} seed(s)", opts.extra_entries.size());
        fmt::print("\n");
    }

    AnalysisDb db;
    {
        StdoutMute mute(quiet);
        db = build_analysis_db(*exe, exe_path.filename().string(), opts, err);
    }
    if (!err.empty()) {
        fmt::print(stderr, "error: {}\n", err);
        return 1;
    }

    if (!quiet) print_report(db, top_n);

    if (!out_dir.empty()) {
        if (!write_json_bundle(db, out_dir, !no_refs, err)) {
            fmt::print(stderr, "error: {}\n", err);
            return 1;
        }
        fmt::print("  wrote {}/analysis.json, edges.json, indirect.json{}\n",
                   out_dir.string(), no_refs ? "" : ", refs.json");
    }
    if (emit_symbols) {
        if (opts.symbols_toml.empty()) {
            fmt::print(stderr,
                       "error: --emit-symbols requires --symbols <path>\n");
            return 1;
        }
        if (!write_symbols_toml(db, opts.symbols_toml, min_conf, true, err)) {
            fmt::print(stderr, "error: {}\n", err);
            return 1;
        }
    }
    if (!symbol_addrs.empty()) {
        if (!write_symbol_addrs(db, symbol_addrs, err)) {
            fmt::print(stderr, "error: {}\n", err); return 1;
        }
        fmt::print("  wrote {}\n", symbol_addrs.string());
    }
    if (!ghidra.empty()) {
        if (!write_ghidra_script(db, ghidra, err)) {
            fmt::print(stderr, "error: {}\n", err); return 1;
        }
        fmt::print("  wrote {}\n", ghidra.string());
    }
    if (!tsv.empty()) {
        if (!write_tsv(db, tsv, err)) {
            fmt::print(stderr, "error: {}\n", err); return 1;
        }
        fmt::print("  wrote {}\n", tsv.string());
    }
    if (!disasm.empty()) {
        if (!write_disasm(db, *exe, disasm_at, disasm, err)) {
            fmt::print(stderr, "error: {}\n", err); return 1;
        }
        fmt::print("  wrote {}\n", disasm.string());
    }
    if (scan_ws) {
        WsScanResult ws = scan_widescreen(*exe, db, ws_opts);
        if (!quiet) print_ws_report(ws, db, top_n);
        if (!out_dir.empty()) {
            if (!write_ws_sites_json(ws, out_dir / "widescreen_sites.json", err)) {
                fmt::print(stderr, "error: {}\n", err); return 1;
            }
            fmt::print("  wrote {}/widescreen_sites.json\n", out_dir.string());
        }
        if (!ws_sites.empty()) {
            if (!write_ws_sites_toml(ws, ws_sites, ws_min_conf, err)) {
                fmt::print(stderr, "error: {}\n", err); return 1;
            }
            fmt::print("  wrote {}\n", ws_sites.string());
        }
    }
    if (!diff.empty()) {
        if (!print_diff(db, diff, err)) {
            fmt::print(stderr, "error: {}\n", err); return 1;
        }
    }
    return 0;
}
