#pragma once
// analysis_export.h — writers that turn an AnalysisDb into artifacts.
//
// The JSON files are the stable public interface: RetComM Studio and any other
// front end consume those, never the analyzer's in-process types. Everything
// else here exists to hand the data to tools people already use rather than to
// make them adopt a new one.

#include <filesystem>
#include <string>

#include "analysis_db.h"

namespace PSXRecomp::Analysis {

// analysis.json / edges.json / refs.json / indirect.json in `dir`.
bool write_json_bundle(const AnalysisDb& db,
                       const std::filesystem::path& dir,
                       bool include_refs,
                       std::string& error);

// Merge discovered functions into a symbols.toml, preserving every existing
// entry's name/status/note verbatim. New entries land as status="guessed".
// `min_conf` gates which newly discovered functions are worth writing.
bool write_symbols_toml(const AnalysisDb& db,
                        const std::filesystem::path& path,
                        Confidence min_conf,
                        bool include_unnamed,
                        std::string& error);

// decomp-toolchain symbol map: "name = 0xADDR; // size:0xNN".
bool write_symbol_addrs(const AnalysisDb& db,
                        const std::filesystem::path& path,
                        std::string& error);

// Ghidra headless/script importer: creates functions, applies names, and
// annotates unresolved indirect sites as bookmarks.
bool write_ghidra_script(const AnalysisDb& db,
                         const std::filesystem::path& path,
                         std::string& error);

// Line-oriented listing for grep/awk/spreadsheet workflows. Also the input
// `--diff` consumes: a stable one-line-per-function format diffs cleanly and
// needs no JSON parser on the read side.
bool write_tsv(const AnalysisDb& db,
               const std::filesystem::path& path,
               std::string& error);

// Annotated disassembly for one function (or the whole image when addr == 0).
bool write_disasm(const AnalysisDb& db,
                  const PS1Executable& exe,
                  uint32_t addr,
                  const std::filesystem::path& path,
                  std::string& error);

// Human-readable summary to stdout.
void print_report(const AnalysisDb& db, int top_n);

// Difference between a previous run's functions.tsv and the current db:
// what appeared, what vanished, what changed boundary or confidence.
bool print_diff(const AnalysisDb& db,
                const std::filesystem::path& previous_tsv,
                std::string& error);

} // namespace PSXRecomp::Analysis
