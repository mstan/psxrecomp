#pragma once
// analysis_db.h
// ----------------------------------------------------------------------------
// Developer-facing static analysis database for a PS-X EXE.
//
// This is NOT part of the recompilation path. It exists so that the function
// knowledge the recompiler already derives — boundaries, call graph, indirect
// sites, jump tables, argument/return usage — becomes a queryable artifact for
// modders and decompilers instead of a throwaway intermediate.
//
// Two hard rules distinguish it from the emitters:
//
//   1. STATIC ONLY. Nothing here links, reads, or consults the runtime. There
//      is no trace import, no overlay capture set, no executed-PC feedback.
//      Every claim in the database is derived from the executable image alone.
//      Coverage that requires dynamic evidence is reported as *missing*, never
//      silently filled in.
//
//   2. TOLERANT, NOT FABRICATING. Unlike the recompiler (CLAUDE.md §0: an
//      unsupported opcode is a hard failure), the analyzer degrades. A region
//      it cannot fully decode is reported `partial` with the offending PCs
//      listed. That is not a stub: it emits no code and asserts no semantics
//      it did not prove. Reporting *incomplete* is permitted here; reporting
//      *fabricated* is not.

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ps1_exe_parser.h"

namespace PSXRecomp::Analysis {

// How much the analyzer is willing to claim about a function boundary.
// Ordered strongest-first; the JSON/report renders the lowercase name.
enum class Confidence {
    Verified,   // prologue + epilogue + jr $ra exit, fully decoded, reached
                // from the entry point by direct calls only
    High,       // fully decoded, jr $ra exit, reached from the entry point
    Medium,     // prologue-shaped or reached only via a recovered jump table
    Low,        // boundary inferred; unreached, no prologue, or partial decode
    DataRegion, // classified as data masquerading as code
};

const char* confidence_name(Confidence c);

// MIPS o32 register indices used in the bitmasks below.
constexpr uint8_t REG_ZERO = 0, REG_AT = 1, REG_V0 = 2, REG_V1 = 3;
constexpr uint8_t REG_A0 = 4, REG_A3 = 7, REG_S0 = 16, REG_S7 = 23;
constexpr uint8_t REG_GP = 28, REG_SP = 29, REG_FP = 30, REG_RA = 31;

// Inferred calling-convention usage. Every field here is a HEURISTIC derived
// from a linear read-before-write scan over the function's own range; loops
// that read a register before the backward edge writes it will over-report an
// argument. Treat as a starting hypothesis for a decomp, not as ground truth —
// `sig_confident` is false whenever the scan hit a construct that weakens it.
struct Signature {
    uint8_t  arg_mask = 0;        // bit i set => $a<i> read before written
    int      arg_count = 0;       // highest contiguous $a register used, + 1
    bool     returns_v0 = false;  // $v0 written on some path to a jr $ra
    bool     returns_v1 = false;
    uint32_t saved_mask = 0;      // registers stored to the stack frame
    bool     is_leaf = true;      // no JAL/JALR anywhere in range
    bool     uses_gte = false;    // any COP2 op
    bool     uses_syscall = false;
    bool     uses_break = false;
    bool     touches_mmio = false; // a statically resolved 0x1F80'xxxx access
    bool     reads_gp = false;
    int32_t  stack_frame = 0;
    bool     sig_confident = true;
    std::string prototype;        // rendered C prototype guess
};

struct FunctionRecord {
    uint32_t addr = 0;
    uint32_t end = 0;             // exclusive
    uint32_t size = 0;
    uint32_t instruction_count = 0;

    std::string name;             // symbols.toml name, else func_XXXXXXXX
    bool        user_named = false;
    std::string status;           // symbols.toml status (guessed|confirmed|hot)
    std::string note;             // symbols.toml note

    Confidence  confidence = Confidence::Low;
    std::string confidence_reason;

    bool has_prologue = false;
    bool has_epilogue = false;
    bool ends_jr_ra = false;
    bool is_data = false;
    bool partial = false;         // contained words that would not decode
    bool reachable = false;       // from the EXE entry point
    bool address_taken = false;   // a statically resolved pointer names it
    uint32_t alias_of = 0;        // interior entry into another function
    std::string bios_call;        // "A0:3F printf" for a kernel dispatch thunk

    Signature sig;
    std::vector<uint32_t> block_leaders;
    std::vector<uint32_t> undecoded_pcs;

    // Filled by the graph pass.
    uint32_t in_degree = 0;       // distinct callers
    uint32_t out_degree = 0;      // distinct callees
    uint32_t unresolved_indirect = 0;
};

struct CallEdge {
    enum class Kind { Direct, JumpTable, AddressTaken, Indirect };
    uint32_t from_func = 0;
    uint32_t site_pc = 0;
    uint32_t to_addr = 0;
    Kind     kind = Kind::Direct;
};

const char* edge_kind_name(CallEdge::Kind k);

// A statically resolved memory reference. Only pairs the analyzer could prove
// (lui/addiu, lui+load/store, $gp-relative) appear here — nothing inferred.
struct DataRef {
    uint32_t from_func = 0;
    uint32_t site_pc = 0;
    uint32_t target = 0;
    bool     is_write = false;
    uint8_t  width = 0;           // 1/2/4, 0 for address-materialization
    std::string kind;             // "ram" | "mmio" | "code" | "gp" | "addr"
};

struct IndirectSite {
    uint32_t from_func = 0;
    uint32_t pc = 0;
    uint8_t  reg = 0;
    std::string kind;             // "jr" | "jalr"
    std::string classification;   // "jump_table" | "bios_thunk" | "return_reg"
                                  // | "unresolved"
    std::vector<uint32_t> targets;
    uint32_t table_base = 0;
    uint32_t table_count = 0;
    std::vector<std::string> context; // ~16 disassembled instructions before pc
};

struct Stats {
    uint32_t total_functions = 0;
    uint32_t total_instructions = 0;
    uint32_t bytes_covered = 0;
    uint32_t bytes_image = 0;
    uint32_t confidence_counts[5] = {0, 0, 0, 0, 0};
    uint32_t reachable_functions = 0;
    uint32_t orphan_functions = 0;
    uint32_t named_functions = 0;
    uint32_t direct_edges = 0;
    uint32_t jump_tables_resolved = 0;
    uint32_t jump_table_targets = 0;
    uint32_t indirect_unresolved = 0;
    uint32_t partial_functions = 0;
    uint32_t undecoded_words = 0;
    std::map<std::string, uint32_t> opcode_histogram;
};

struct AnalysisDb {
    std::string image_name;
    uint32_t load_address = 0;
    uint32_t entry_point = 0;
    uint32_t initial_gp = 0;
    uint32_t image_size = 0;

    std::vector<FunctionRecord> functions;   // sorted by addr
    std::vector<CallEdge>       edges;
    std::vector<DataRef>        data_refs;
    std::vector<IndirectSite>   indirect;
    Stats                       stats;

    const FunctionRecord* find(uint32_t addr) const;
    // Function whose [addr,end) contains pc, or nullptr.
    const FunctionRecord* containing(uint32_t pc) const;
};

struct Options {
    std::filesystem::path symbols_toml;   // optional, for name round-trip
    std::vector<uint32_t> extra_entries;  // seed addresses to force
    bool resolve_jump_tables = true;
    bool collect_data_refs = true;
    bool exact_entries = false;           // reachability-only partition
};

// A parsed symbols.toml entry. Kept separate from FunctionRecord so names
// survive a re-analysis that no longer discovers the address.
struct SymbolEntry {
    uint32_t pc = 0;
    std::string name;
    std::string status = "guessed";
    std::string note;
    bool emit = false;
};

// Name for a PSX kernel A0/B0/C0 call, or "" if not carried. See
// bios_call_names.cpp for the table and its source.
const char* bios_call_name(uint32_t table, uint32_t index);

bool load_symbols(const std::filesystem::path& path,
                  std::vector<SymbolEntry>& out,
                  std::string& error);

AnalysisDb build_analysis_db(const PS1Executable& exe,
                             const std::string& image_name,
                             const Options& opts,
                             std::string& error);

// Disassemble [lo, hi) from the image. Used for the context windows and for
// the `disasm` export.
std::vector<std::string> disassemble_range(const PS1Executable& exe,
                                           uint32_t lo, uint32_t hi,
                                           const AnalysisDb* db);

} // namespace PSXRecomp::Analysis
