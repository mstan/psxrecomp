// analysis_db.cpp — see analysis_db.h for the two rules this file obeys.

#include "analysis_db.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "fmt/format.h"
#include "function_analysis.h"
#include "mips_decoder.h"

// toml11 is header-only; used only to read an existing symbols.toml so that
// hand-applied names survive re-analysis.
#include "toml.hpp"

namespace PSXRecomp::Analysis {

namespace {

constexpr uint32_t kMmioLo = 0x1F801000;
constexpr uint32_t kMmioHi = 0x1F803000;

inline uint32_t norm(uint32_t a) { return a & 0x1FFFFFFF; }

// ---------------------------------------------------------------------------
// Register effect model
//
// Deliberately explicit rather than derived from DecodedInstruction's coarse
// is_load/is_alu flags: the signature pass needs to know *which* registers an
// instruction reads, and a wrong answer there silently corrupts every inferred
// prototype. Anything this table does not recognize contributes no reads and
// no writes, which is the conservative direction for read-before-write (it can
// only under-report arguments, never invent them).
// ---------------------------------------------------------------------------
struct RegEffect {
    uint32_t reads = 0;
    uint32_t writes = 0;
};

inline void add(uint32_t& mask, uint8_t r) {
    if (r != 0) mask |= (1u << r);
}

RegEffect reg_effect(const DecodedInstruction& d) {
    RegEffect e;
    switch (d.opcode) {
    case 0x00: // SPECIAL
        switch (d.funct) {
        case 0x00: case 0x02: case 0x03:            // SLL/SRL/SRA
            add(e.reads, d.rt); add(e.writes, d.rd); break;
        case 0x04: case 0x06: case 0x07:            // SLLV/SRLV/SRAV
            add(e.reads, d.rt); add(e.reads, d.rs); add(e.writes, d.rd); break;
        case 0x08:                                   // JR
            add(e.reads, d.rs); break;
        case 0x09:                                   // JALR
            add(e.reads, d.rs); add(e.writes, d.rd); break;
        case 0x0C: case 0x0D:                        // SYSCALL/BREAK
            break;
        case 0x10: case 0x12:                        // MFHI/MFLO
            add(e.writes, d.rd); break;
        case 0x11: case 0x13:                        // MTHI/MTLO
            add(e.reads, d.rs); break;
        case 0x18: case 0x19: case 0x1A: case 0x1B:  // MULT/MULTU/DIV/DIVU
            add(e.reads, d.rs); add(e.reads, d.rt); break;
        case 0x20: case 0x21: case 0x22: case 0x23:  // ADD/ADDU/SUB/SUBU
        case 0x24: case 0x25: case 0x26: case 0x27:  // AND/OR/XOR/NOR
        case 0x2A: case 0x2B:                        // SLT/SLTU
            add(e.reads, d.rs); add(e.reads, d.rt); add(e.writes, d.rd); break;
        default: break;
        }
        break;
    case 0x01: // REGIMM
        add(e.reads, d.rs);
        if (d.rt == 0x10 || d.rt == 0x11) add(e.writes, REG_RA); // BLTZAL/BGEZAL
        break;
    case 0x02: break;                                // J
    case 0x03: add(e.writes, REG_RA); break;         // JAL
    case 0x04: case 0x05:                            // BEQ/BNE
        add(e.reads, d.rs); add(e.reads, d.rt); break;
    case 0x06: case 0x07:                            // BLEZ/BGTZ
        add(e.reads, d.rs); break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:      // ADDI/ADDIU/SLTI/SLTIU
    case 0x0C: case 0x0D: case 0x0E:                 // ANDI/ORI/XORI
        add(e.reads, d.rs); add(e.writes, d.rt); break;
    case 0x0F: add(e.writes, d.rt); break;           // LUI
    case 0x10:                                       // COP0
        if (d.rs == 0x00) add(e.writes, d.rt);       // MFC0
        else if (d.rs == 0x04) add(e.reads, d.rt);   // MTC0
        break;
    case 0x12:                                       // COP2 / GTE
        if (d.rs == 0x00 || d.rs == 0x02) add(e.writes, d.rt);  // MFC2/CFC2
        else if (d.rs == 0x04 || d.rs == 0x06) add(e.reads, d.rt); // MTC2/CTC2
        break;
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: // LB/LH/LW/LBU/LHU
        add(e.reads, d.rs); add(e.writes, d.rt); break;
    case 0x22: case 0x26:                            // LWL/LWR: partial merge
        add(e.reads, d.rs); add(e.reads, d.rt); add(e.writes, d.rt); break;
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: // SB/SH/SWL/SW/SWR
        add(e.reads, d.rs); add(e.reads, d.rt); break;
    case 0x32: case 0x3A:                            // LWC2/SWC2
        add(e.reads, d.rs); break;
    default: break;
    }
    return e;
}

int mem_width(uint8_t opcode) {
    switch (opcode) {
    case 0x20: case 0x24: case 0x28: return 1;
    case 0x21: case 0x25: case 0x29: return 2;
    case 0x22: case 0x23: case 0x26: case 0x2A: case 0x2B: case 0x2E:
    case 0x32: case 0x3A: return 4;
    default: return 0;
    }
}

bool is_store_op(uint8_t opcode) {
    return opcode == 0x28 || opcode == 0x29 || opcode == 0x2A ||
           opcode == 0x2B || opcode == 0x2E || opcode == 0x3A;
}

bool is_load_op(uint8_t opcode) {
    return opcode == 0x20 || opcode == 0x21 || opcode == 0x22 ||
           opcode == 0x23 || opcode == 0x24 || opcode == 0x25 ||
           opcode == 0x26 || opcode == 0x32;
}

std::string classify_target(uint32_t target, uint32_t lo, uint32_t hi) {
    uint32_t k = norm(target);
    if (k >= norm(kMmioLo) && k < norm(kMmioHi)) return "mmio";
    if (target >= lo && target < hi) return "image";
    if (k < 0x00800000) return "ram";
    return "other";
}

// ---------------------------------------------------------------------------
// Disassembly formatting
// ---------------------------------------------------------------------------
std::string format_instr(const DecodedInstruction& d,
                         const AnalysisDb* db) {
    auto rn = [](uint8_t r) { return MipsDecoder::register_name(r); };
    auto sym = [&](uint32_t a) -> std::string {
        if (db) {
            const FunctionRecord* f = db->find(a);
            if (f) return fmt::format("0x{:08X} <{}>", a, f->name);
        }
        return fmt::format("0x{:08X}", a);
    };

    if (d.is_nop) return "nop";
    const char* m = d.mnemonic ? d.mnemonic : "???";
    std::string mn = m;
    std::transform(mn.begin(), mn.end(), mn.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    switch (d.opcode) {
    case 0x02: case 0x03: // J / JAL
        return fmt::format("{:<8}{}", mn, sym(d.jump_target));
    case 0x04: case 0x05: // BEQ/BNE
        return fmt::format("{:<8}{}, {}, 0x{:08X}", mn, rn(d.rs), rn(d.rt),
                           d.branch_target);
    case 0x06: case 0x07: // BLEZ/BGTZ
        return fmt::format("{:<8}{}, 0x{:08X}", mn, rn(d.rs), d.branch_target);
    case 0x01: // REGIMM
        return fmt::format("{:<8}{}, 0x{:08X}", mn, rn(d.rs), d.branch_target);
    case 0x0F: // LUI
        return fmt::format("{:<8}{}, 0x{:04X}", mn, rn(d.rt), d.uimm16);
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        return fmt::format("{:<8}{}, {}, {}", mn, rn(d.rt), rn(d.rs),
                           d.sign_ext_imm);
    case 0x0C: case 0x0D: case 0x0E:
        return fmt::format("{:<8}{}, {}, 0x{:04X}", mn, rn(d.rt), rn(d.rs),
                           d.uimm16);
    default: break;
    }

    if (d.is_load || d.is_store)
        return fmt::format("{:<8}{}, {}({})", mn, rn(d.rt), d.sign_ext_imm,
                           rn(d.rs));

    if (d.opcode == 0x00) {
        switch (d.funct) {
        case 0x08: return fmt::format("{:<8}{}", mn, rn(d.rs));
        case 0x09: return fmt::format("{:<8}{}, {}", mn, rn(d.rd), rn(d.rs));
        case 0x00: case 0x02: case 0x03:
            return fmt::format("{:<8}{}, {}, {}", mn, rn(d.rd), rn(d.rt),
                               (int)d.shamt);
        case 0x0C: case 0x0D: return mn;
        case 0x10: case 0x12: return fmt::format("{:<8}{}", mn, rn(d.rd));
        case 0x11: case 0x13: return fmt::format("{:<8}{}", mn, rn(d.rs));
        case 0x18: case 0x19: case 0x1A: case 0x1B:
            return fmt::format("{:<8}{}, {}", mn, rn(d.rs), rn(d.rt));
        default:
            return fmt::format("{:<8}{}, {}, {}", mn, rn(d.rd), rn(d.rs),
                               rn(d.rt));
        }
    }
    if (d.format == InstrFormat::UNKNOWN)
        return fmt::format(".word   0x{:08X}", d.raw);
    return fmt::format("{:<8}{}, {}, {}", mn, rn(d.rd), rn(d.rs), rn(d.rt));
}

// ---------------------------------------------------------------------------
// General jump-table recovery
//
// function_analysis.cpp's resolve_exact_bounded_jump_table() is tried first and
// is authoritative when it fires. It accepts exactly one instruction schedule
// (`sltiu; beq; nop; sll; addu; lw; jr`) because a wrong table there emits
// wrong CODE. This recoverer exists because that is the wrong trade for a
// discovery tool: on a real title the far more common schedule puts the `sll`
// in the branch's delay slot and materializes the table base between the `sll`
// and the `addu`, which the strict form rejects outright — leaving every switch
// in the image reported as an unresolved indirect jump.
//
// So this walks the actual backward slice instead of matching a fixed window,
// and labels how it terminated: `guarded` (an sltiu bound on the index register
// was found — the count is proven) or `scanned` (no bound found; the table was
// walked until a word stopped looking like a code address). A `scanned` table
// is a hypothesis and is reported as such; it never silently reads as proven.
struct RecoveredTable {
    uint32_t base = 0;
    uint32_t count = 0;
    std::vector<uint32_t> targets;
    const char* method = "";
};

// Nearest preceding instruction that writes `reg`, searching back at most
// `window` instructions and never before `lo`. Returns false if none.
bool find_def(const PS1Executable& exe, uint32_t lo, uint32_t from_pc,
              uint8_t reg, uint32_t window, DecodedInstruction& out) {
    uint32_t limit = (from_pc > lo + window * 4) ? from_pc - window * 4 : lo;
    for (uint32_t pc = from_pc; pc >= limit; pc -= 4) {
        auto w = exe.read_word(pc);
        if (!w.has_value()) break;
        DecodedInstruction d = MipsDecoder::decode(*w, pc);
        if (reg_effect(d).writes & (1u << reg)) { out = d; return true; }
        if (pc < 4 || pc == limit) break;
    }
    return false;
}

// Constant value of `reg` at `at_pc`, if it comes from LUI or LUI+ADDIU.
bool const_value(const PS1Executable& exe, uint32_t lo, uint32_t at_pc,
                 uint8_t reg, uint32_t& out) {
    DecodedInstruction d;
    if (!find_def(exe, lo, at_pc, reg, 48, d)) return false;
    if (d.opcode == 0x0F) {                       // LUI
        out = static_cast<uint32_t>(d.uimm16) << 16;
        return true;
    }
    if (d.opcode == 0x09) {                       // ADDIU rt, rs, imm
        uint32_t hi = 0;
        if (d.address < lo + 4) return false;
        if (!const_value(exe, lo, d.address - 4, d.rs, hi)) return false;
        out = hi + static_cast<uint32_t>(d.sign_ext_imm);
        return true;
    }
    return false;
}

bool recover_jump_table(const PS1Executable& exe,
                        uint32_t f_start, uint32_t f_end,
                        uint32_t jr_pc, uint8_t jr_rs,
                        const std::unordered_map<uint32_t, size_t>& fn_index,
                        RecoveredTable& out) {
    const uint32_t img_lo = exe.load_address();
    const uint32_t img_hi = exe.end_address();

    // jr $rD  <-  lw $rD, off($rB)
    DecodedInstruction lw;
    if (jr_pc < f_start + 4) return false;
    if (!find_def(exe, f_start, jr_pc - 4, jr_rs, 8, lw)) return false;
    if (lw.opcode != 0x23) return false;          // must be LW
    const int32_t lw_off = lw.sign_ext_imm;
    const uint8_t base_reg = lw.rs;

    // $rB  <-  addu $rB, <scaled index>, <table base>
    DecodedInstruction addu;
    if (lw.address < f_start + 4) return false;
    if (!find_def(exe, f_start, lw.address - 4, base_reg, 8, addu)) return false;
    if (!(addu.opcode == 0x00 && (addu.funct == 0x21 || addu.funct == 0x20)))
        return false;                              // ADDU / ADD

    // One operand is `sll rX, rIdx, 2`; the other is a materialized constant.
    uint8_t index_reg = 0;
    uint32_t table_base = 0;
    bool have_base = false;
    const uint8_t ops[2] = {addu.rs, addu.rt};
    for (int k = 0; k < 2; ++k) {
        DecodedInstruction d;
        if (addu.address < f_start + 4) break;
        if (!find_def(exe, f_start, addu.address - 4, ops[k], 16, d)) continue;
        if (d.opcode == 0x00 && d.funct == 0x00 && d.shamt == 2) {
            index_reg = d.rt;                      // SLL rd, rt, 2
        }
    }
    for (int k = 0; k < 2; ++k) {
        uint32_t v = 0;
        if (addu.address < f_start + 4) break;
        if (const_value(exe, f_start, addu.address - 4, ops[k], v)) {
            table_base = v;
            have_base = true;
        }
    }
    if (!index_reg || !have_base) return false;

    table_base += static_cast<uint32_t>(lw_off);
    if ((table_base & 3u) || table_base < img_lo || table_base + 3 >= img_hi)
        return false;

    // Bound: nearest `sltiu/slti rX, rIdx, N` before the jr.
    uint32_t count = 0;
    const char* method = "scanned";
    {
        uint32_t limit = (jr_pc > f_start + 64 * 4) ? jr_pc - 64 * 4 : f_start;
        for (uint32_t pc = jr_pc; pc >= limit; pc -= 4) {
            auto w = exe.read_word(pc);
            if (!w.has_value()) break;
            DecodedInstruction d = MipsDecoder::decode(*w, pc);
            if ((d.opcode == 0x0B || d.opcode == 0x0A) && d.rs == index_reg &&
                d.uimm16 > 0 && d.uimm16 <= 4096) {
                count = d.uimm16;
                method = "guarded";
                break;
            }
            if (pc < 4 || pc == limit) break;
        }
    }

    auto plausible_target = [&](uint32_t t) {
        if (t & 3u) return false;
        if (t < img_lo || t + 3 >= img_hi) return false;
        auto w = exe.read_word(t);
        if (!w.has_value()) return false;
        // A case label is either inside this function or a known function
        // start; anything else is a coincidental word, not a target.
        if (!(t >= f_start && t < f_end) && !fn_index.count(t)) return false;
        return FunctionAnalyzer::is_valid_mips_word(*w);
    };

    if (count == 0) {
        for (uint32_t k = 0; k < 256; ++k) {
            auto w = exe.read_word(table_base + k * 4);
            if (!w.has_value() || !plausible_target(*w)) break;
            count = k + 1;
        }
        if (count < 2) return false;   // a lone match is noise, not a table
    }

    for (uint32_t k = 0; k < count; ++k) {
        auto w = exe.read_word(table_base + k * 4);
        if (!w.has_value()) return false;
        if (!plausible_target(*w)) {
            // A guarded count that does not validate means the slice was
            // misread. Report nothing rather than a half-right table.
            if (method[0] == 'g') return false;
            break;
        }
        out.targets.push_back(*w);
    }
    if (out.targets.size() < 2) return false;

    out.base = table_base;
    out.count = static_cast<uint32_t>(out.targets.size());
    out.method = method;
    return true;
}

// Resolve `jalr $rD` / `jr $rD` where $rD holds a statically-known function
// address: either materialized inline (lui/addiu) or loaded from a fixed
// address whose stored word is a known function start. This is the callback /
// dispatch-slot pattern, and it is the second largest indirect class after
// switches. Anything whose pointer comes from a struct field or a computed
// address stays unresolved — correctly, since static analysis cannot know it.
bool recover_indirect_call(const PS1Executable& exe, uint32_t f_start,
                           uint32_t call_pc, uint8_t reg,
                           const std::unordered_map<uint32_t, size_t>& fn_index,
                           uint32_t& target, const char*& how) {
    if (call_pc < f_start + 4) return false;
    DecodedInstruction def;
    if (!find_def(exe, f_start, call_pc - 4, reg, 16, def)) return false;

    // Materialized directly: lui + addiu naming a function.
    if (def.opcode == 0x09 || def.opcode == 0x0F) {
        uint32_t v = 0;
        if (const_value(exe, f_start, def.address, reg, v) && fn_index.count(v)) {
            target = v;
            how = "immediate";
            return true;
        }
        return false;
    }
    // Loaded from a statically-known address.
    if (def.opcode == 0x23) {                     // LW rt, off(rB)
        uint32_t base = 0;
        if (def.address < f_start + 4) return false;
        if (!const_value(exe, f_start, def.address - 4, def.rs, base))
            return false;
        uint32_t slot = base + static_cast<uint32_t>(def.sign_ext_imm);
        auto w = exe.read_word(slot);
        if (!w.has_value() || !fn_index.count(*w)) return false;
        target = *w;
        how = "static_slot";
        return true;
    }
    return false;
}

std::string render_prototype(const FunctionRecord& f) {
    std::string ret = (f.sig.returns_v0 || f.sig.returns_v1) ? "u32" : "void";
    std::string args;
    for (int i = 0; i < f.sig.arg_count; ++i) {
        if (i) args += ", ";
        args += fmt::format("u32 a{}", i);
    }
    if (args.empty()) args = "void";
    return fmt::format("{} {}({})", ret, f.name, args);
}

} // namespace

const char* confidence_name(Confidence c) {
    switch (c) {
    case Confidence::Verified:   return "verified";
    case Confidence::High:       return "high";
    case Confidence::Medium:     return "medium";
    case Confidence::Low:        return "low";
    case Confidence::DataRegion: return "data";
    }
    return "low";
}

const char* edge_kind_name(CallEdge::Kind k) {
    switch (k) {
    case CallEdge::Kind::Direct:       return "direct";
    case CallEdge::Kind::JumpTable:    return "jump_table";
    case CallEdge::Kind::AddressTaken: return "address_taken";
    case CallEdge::Kind::Indirect:     return "indirect";
    }
    return "direct";
}

const FunctionRecord* AnalysisDb::find(uint32_t addr) const {
    auto it = std::lower_bound(functions.begin(), functions.end(), addr,
        [](const FunctionRecord& f, uint32_t v) { return f.addr < v; });
    if (it != functions.end() && it->addr == addr) return &*it;
    return nullptr;
}

const FunctionRecord* AnalysisDb::containing(uint32_t pc) const {
    auto it = std::upper_bound(functions.begin(), functions.end(), pc,
        [](uint32_t v, const FunctionRecord& f) { return v < f.addr; });
    if (it == functions.begin()) return nullptr;
    --it;
    return (pc >= it->addr && pc < it->end) ? &*it : nullptr;
}

// ---------------------------------------------------------------------------
// symbols.toml
// ---------------------------------------------------------------------------
bool load_symbols(const std::filesystem::path& path,
                  std::vector<SymbolEntry>& out,
                  std::string& error) {
    out.clear();
    if (path.empty()) return true;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true; // absent is not an error
    try {
        const auto root = toml::parse(path.string());
        if (!root.contains("func")) return true;
        const auto& arr = toml::find(root, "func");
        if (!arr.is_array()) return true;
        for (const auto& e : arr.as_array()) {
            SymbolEntry s;
            if (!e.contains("pc")) continue;
            s.pc = static_cast<uint32_t>(toml::find<std::int64_t>(e, "pc"));
            s.name = e.contains("name") ? toml::find<std::string>(e, "name") : "";
            if (s.name.empty()) continue;
            if (e.contains("status")) s.status = toml::find<std::string>(e, "status");
            if (e.contains("note")) s.note = toml::find<std::string>(e, "note");
            if (e.contains("emit")) s.emit = toml::find<bool>(e, "emit");
            out.push_back(s);
        }
    } catch (const std::exception& ex) {
        error = fmt::format("symbols.toml parse failed: {}", ex.what());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Main build
// ---------------------------------------------------------------------------
AnalysisDb build_analysis_db(const PS1Executable& exe,
                             const std::string& image_name,
                             const Options& opts,
                             std::string& error) {
    AnalysisDb db;
    db.image_name = image_name;
    db.load_address = exe.load_address();
    db.entry_point = exe.entry_point();
    db.initial_gp = exe.header.initial_gp;
    db.image_size = exe.code_size();

    const uint32_t lo = exe.load_address();
    const uint32_t hi = exe.end_address();

    auto read_w = [&](uint32_t a) -> uint32_t {
        auto w = exe.read_word(a);
        return w.has_value() ? *w : 0u;
    };
    auto in_image = [&](uint32_t a) { return a >= lo && a + 3 < hi; };

    // ---- pass 1: boundaries ------------------------------------------------
    FunctionAnalyzer analyzer(exe);
    analyzer.add_forced_entry(exe.entry_point());
    for (uint32_t a : opts.extra_entries) analyzer.add_forced_entry(a);

    FunctionAnalysisResult ar;
    if (opts.exact_entries) {
        std::vector<uint32_t> entries{exe.entry_point()};
        entries.insert(entries.end(), opts.extra_entries.begin(),
                       opts.extra_entries.end());
        ar = analyzer.analyze_exact_entries(entries);
    } else {
        ar = analyzer.analyze();
    }

    db.functions.reserve(ar.functions.size());
    for (const auto& f : ar.functions) {
        FunctionRecord r;
        r.addr = f.start_addr;
        r.end = f.end_addr;
        r.size = (f.end_addr > f.start_addr) ? (f.end_addr - f.start_addr) : 0;
        r.instruction_count = r.size / 4;
        r.name = fmt::format("func_{:08X}", f.start_addr);
        r.has_prologue = f.has_prologue;
        r.has_epilogue = f.has_epilogue;
        r.is_data = f.is_data_section;
        r.alias_of = f.alias_walk_lo;
        r.sig.stack_frame = f.stack_frame_size;
        db.functions.push_back(std::move(r));
    }
    std::sort(db.functions.begin(), db.functions.end(),
              [](const FunctionRecord& a, const FunctionRecord& b) {
                  return a.addr < b.addr;
              });
    db.functions.erase(
        std::unique(db.functions.begin(), db.functions.end(),
                    [](const FunctionRecord& a, const FunctionRecord& b) {
                        return a.addr == b.addr;
                    }),
        db.functions.end());

    std::unordered_map<uint32_t, size_t> index;
    for (size_t i = 0; i < db.functions.size(); ++i)
        index[db.functions[i].addr] = i;

    auto containing_idx = [&](uint32_t pc) -> long {
        auto it = std::upper_bound(db.functions.begin(), db.functions.end(), pc,
            [](uint32_t v, const FunctionRecord& f) { return v < f.addr; });
        if (it == db.functions.begin()) return -1;
        --it;
        if (pc >= it->addr && pc < it->end)
            return static_cast<long>(it - db.functions.begin());
        return -1;
    };

    // ---- pass 2: per-function decode, CFG, edges, refs, signature ----------
    std::unordered_set<uint32_t> callee_set; // for in_degree
    std::vector<std::unordered_set<uint32_t>> callers(db.functions.size());
    std::vector<std::unordered_set<uint32_t>> callees(db.functions.size());

    for (size_t fi = 0; fi < db.functions.size(); ++fi) {
        FunctionRecord& f = db.functions[fi];
        if (f.is_data || f.size == 0) continue;

        // Block leaders: entry, every intra-range branch/jump target, and the
        // instruction after each branch's delay slot.
        std::set<uint32_t> leaders{f.addr};
        std::vector<DecodedInstruction> code;
        code.reserve(f.instruction_count);
        for (uint32_t pc = f.addr; pc < f.end; pc += 4) {
            if (!in_image(pc)) break;
            DecodedInstruction d = MipsDecoder::decode(read_w(pc), pc);
            if (d.format == InstrFormat::UNKNOWN) {
                f.partial = true;
                if (f.undecoded_pcs.size() < 64) f.undecoded_pcs.push_back(pc);
                db.stats.undecoded_words++;
            }
            code.push_back(d);
        }
        for (const auto& d : code) {
            if (d.is_branch) {
                if (d.branch_target >= f.addr && d.branch_target < f.end)
                    leaders.insert(d.branch_target);
                if (d.address + 8 < f.end) leaders.insert(d.address + 8);
            } else if (d.opcode == 0x02) { // J
                if (d.jump_target >= f.addr && d.jump_target < f.end)
                    leaders.insert(d.jump_target);
                if (d.address + 8 < f.end) leaders.insert(d.address + 8);
            } else if (d.is_jump) {
                if (d.address + 8 < f.end) leaders.insert(d.address + 8);
            }
        }
        f.block_leaders.assign(leaders.begin(), leaders.end());

        // Terminal shape.
        for (auto it = code.rbegin(); it != code.rend(); ++it) {
            if (it->is_nop) continue;
            if (it->is_jr_ra) { f.ends_jr_ra = true; }
            break;
        }
        if (!f.ends_jr_ra) {
            for (const auto& d : code) if (d.is_jr_ra) { f.ends_jr_ra = true; break; }
        }

        // Signature scan + edge/ref collection in one walk.
        uint32_t defined = 0;         // registers written so far
        uint32_t arg_mask = 0;
        bool saw_backward_branch = false;
        uint32_t hi_val[32] = {0};
        bool     hi_ok[32] = {false};

        for (size_t i = 0; i < code.size(); ++i) {
            const DecodedInstruction& d = code[i];
            if (leaders.count(d.address)) {
                std::memset(hi_ok, 0, sizeof(hi_ok));
            }

            db.stats.total_instructions++;
            if (d.mnemonic) db.stats.opcode_histogram[d.mnemonic]++;

            RegEffect e = reg_effect(d);
            for (uint8_t r = REG_A0; r <= REG_A3; ++r) {
                if ((e.reads & (1u << r)) && !(defined & (1u << r)))
                    arg_mask |= (1u << (r - REG_A0));
            }
            if (e.reads & (1u << REG_GP)) f.sig.reads_gp = true;
            if (e.writes & (1u << REG_V0)) f.sig.returns_v0 = true;
            if (e.writes & (1u << REG_V1)) f.sig.returns_v1 = true;
            defined |= e.writes;

            if (d.is_branch && d.branch_target <= d.address)
                saw_backward_branch = true;
            if (d.opcode == 0x12) f.sig.uses_gte = true;
            if (d.is_syscall) f.sig.uses_syscall = true;
            if (d.is_break) f.sig.uses_break = true;

            // Callee-saved: `sw sN/ra/fp, off($sp)` inside the frame.
            if (d.opcode == 0x2B && d.rs == REG_SP) {
                if ((d.rt >= REG_S0 && d.rt <= REG_S7) || d.rt == REG_FP ||
                    d.rt == REG_RA)
                    f.sig.saved_mask |= (1u << d.rt);
            }

            // --- constant tracking for data refs / address-taken ------------
            if (d.opcode == 0x0F) {                       // LUI
                hi_val[d.rt] = static_cast<uint32_t>(d.uimm16) << 16;
                hi_ok[d.rt] = true;
            } else {
                uint32_t resolved = 0;
                bool have = false;
                bool materialize = false;

                if (d.opcode == 0x09 && hi_ok[d.rs]) {    // ADDIU rt, rs(hi), imm
                    resolved = hi_val[d.rs] + static_cast<uint32_t>(d.sign_ext_imm);
                    have = true;
                    materialize = true;
                } else if ((is_load_op(d.opcode) || is_store_op(d.opcode))) {
                    if (hi_ok[d.rs]) {
                        resolved = hi_val[d.rs] +
                                   static_cast<uint32_t>(d.sign_ext_imm);
                        have = true;
                    } else if (d.rs == REG_GP && db.initial_gp) {
                        resolved = db.initial_gp +
                                   static_cast<uint32_t>(d.sign_ext_imm);
                        have = true;
                    }
                }

                if (have && opts.collect_data_refs) {
                    DataRef ref;
                    ref.from_func = f.addr;
                    ref.site_pc = d.address;
                    ref.target = resolved;
                    ref.is_write = is_store_op(d.opcode);
                    ref.width = static_cast<uint8_t>(mem_width(d.opcode));
                    ref.kind = materialize ? "addr"
                                           : classify_target(resolved, lo, hi);
                    if (ref.kind == "mmio") f.sig.touches_mmio = true;
                    db.data_refs.push_back(std::move(ref));
                }
                if (materialize) {
                    auto ti = index.find(resolved);
                    if (ti != index.end()) {
                        db.functions[ti->second].address_taken = true;
                        db.edges.push_back({f.addr, d.address, resolved,
                                            CallEdge::Kind::AddressTaken});
                        callers[ti->second].insert(f.addr);
                        callees[fi].insert(resolved);
                    }
                }

                // Invalidate any hi constant this instruction clobbers.
                for (uint8_t r = 1; r < 32; ++r)
                    if ((e.writes & (1u << r)) && !(d.opcode == 0x09 && r == d.rt && hi_ok[d.rs]))
                        hi_ok[r] = false;
                if (d.opcode == 0x09 && hi_ok[d.rs] && d.rt != d.rs)
                    hi_ok[d.rt] = false; // the sum is an address, not a hi part
            }

            // --- call edges --------------------------------------------------
            if (d.opcode == 0x03) {                       // JAL
                f.sig.is_leaf = false;
                db.edges.push_back({f.addr, d.address, d.jump_target,
                                    CallEdge::Kind::Direct});
                db.stats.direct_edges++;
                callees[fi].insert(d.jump_target);
                auto ti = index.find(d.jump_target);
                if (ti != index.end()) callers[ti->second].insert(f.addr);
            } else if (d.opcode == 0x00 && d.funct == 0x09) { // JALR
                f.sig.is_leaf = false;
            }
        }

        f.sig.arg_mask = static_cast<uint8_t>(arg_mask);
        f.sig.arg_count = 0;
        for (int i = 0; i < 4; ++i) {
            if (arg_mask & (1u << i)) f.sig.arg_count = i + 1;
        }
        f.sig.sig_confident = !saw_backward_branch && !f.partial;
    }

    // ---- pass 2.5: PSY-Q kernel dispatch thunks ---------------------------
    // Shape: an immediate load of 0xA0/0xB0/0xC0 into some register, a `jr` on
    // that register, and an immediate load of the call index into $t1. This is
    // an exact pattern, not a heuristic, so a match is Verified — which matters
    // because the generic rules score these stubs `low` (no prologue, no
    // jr $ra) despite them being among the most-called code in any image.
    for (auto& f : db.functions) {
        if (f.is_data || f.size < 12 || f.size > 32) continue;
        uint32_t table = 0, idx = 0, tbl_reg = 32;
        bool have_jr = false;
        for (uint32_t pc = f.addr; pc < f.end && pc < f.addr + 16; pc += 4) {
            if (!in_image(pc)) break;
            DecodedInstruction d = MipsDecoder::decode(read_w(pc), pc);
            const bool imm_load = (d.opcode == 0x09 || d.opcode == 0x0D) &&
                                  d.rs == REG_ZERO;   // ADDIU/ORI rt, $zero, k
            if (imm_load) {
                if (d.uimm16 == 0xA0 || d.uimm16 == 0xB0 || d.uimm16 == 0xC0) {
                    table = d.uimm16;
                    tbl_reg = d.rt;
                } else if (d.rt == 9) {               // $t1 carries the index
                    idx = d.uimm16;
                }
            } else if (d.opcode == 0x00 && d.funct == 0x08 && d.rs == tbl_reg) {
                have_jr = true;
            }
        }
        if (!table || !have_jr) continue;

        const char* nm = bios_call_name(table, idx);
        f.bios_call = fmt::format("{:02X}:{:02X}{}{}", table, idx,
                                  *nm ? " " : "", nm);
        f.name = *nm ? fmt::format("bios_{:02X}_{:02X}_{}", table, idx, nm)
                     : fmt::format("bios_{:02X}_{:02X}", table, idx);
        f.confidence = Confidence::Verified;
        f.confidence_reason =
            fmt::format("PSY-Q kernel dispatch thunk to {:02X}({:02X}h)",
                        table, idx);
        f.sig.prototype = fmt::format("/* kernel {:02X}({:02X}h) */ {}",
                                      table, idx, f.name);
    }

    // ---- pass 3: indirect sites + static jump-table recovery ---------------
    for (size_t fi = 0; fi < db.functions.size(); ++fi) {
        FunctionRecord& f = db.functions[fi];
        if (f.is_data || f.size == 0) continue;
        for (uint32_t pc = f.addr; pc < f.end; pc += 4) {
            if (!in_image(pc)) break;
            DecodedInstruction d = MipsDecoder::decode(read_w(pc), pc);
            const bool is_jr = (d.opcode == 0x00 && d.funct == 0x08);
            const bool is_jalr = (d.opcode == 0x00 && d.funct == 0x09);
            if (!is_jr && !is_jalr) continue;
            if (is_jr && d.rs == REG_RA) continue;      // ordinary return

            IndirectSite site;
            site.from_func = f.addr;
            site.pc = pc;
            site.reg = d.rs;
            site.kind = is_jr ? "jr" : "jalr";
            site.classification = "unresolved";

            // The `jr` inside a recognized kernel thunk IS the dispatch — its
            // target is the A0/B0/C0 vector, which pass 2.5 already identified
            // exactly. Leaving it in the unresolved bucket would inflate the
            // coverage gap with the one class of indirect transfer the tool
            // understands best.
            if (!f.bios_call.empty()) {
                site.classification = fmt::format("bios_dispatch:{}", f.bios_call);
                db.indirect.push_back(std::move(site));
                continue;
            }

            if (is_jr && opts.resolve_jump_tables) {
                // The emitter's proven form first; the general slice walk only
                // if it declines. Provenance is kept in `classification` so a
                // consumer can tell a proven table from an inferred one.
                ExactJumpTable table;
                if (resolve_exact_bounded_jump_table(exe, f.addr, f.end, pc,
                                                     d.rs, table) &&
                    table.table_base && table.table_count) {
                    site.classification = "jump_table:proven";
                    site.table_base = table.table_base;
                    site.table_count = table.table_count;
                    for (const auto& t : table.targets)
                        site.targets.push_back(t.second);
                } else {
                    RecoveredTable rt;
                    if (recover_jump_table(exe, f.addr, f.end, pc, d.rs, index,
                                           rt)) {
                        site.classification =
                            fmt::format("jump_table:{}", rt.method);
                        site.table_base = rt.base;
                        site.table_count = rt.count;
                        site.targets = rt.targets;
                    }
                }

                if (!site.targets.empty()) {
                    for (uint32_t t : site.targets) {
                        if (t >= f.addr && t < f.end) {
                            // Intra-function switch case: control flow, not a
                            // call. It becomes a block leader instead of an edge.
                            f.block_leaders.push_back(t);
                            continue;
                        }
                        db.edges.push_back({f.addr, pc, t,
                                            CallEdge::Kind::JumpTable});
                        callees[fi].insert(t);
                        auto ti = index.find(t);
                        if (ti != index.end()) callers[ti->second].insert(f.addr);
                    }
                    std::sort(f.block_leaders.begin(), f.block_leaders.end());
                    f.block_leaders.erase(
                        std::unique(f.block_leaders.begin(), f.block_leaders.end()),
                        f.block_leaders.end());
                    db.stats.jump_tables_resolved++;
                    db.stats.jump_table_targets +=
                        static_cast<uint32_t>(site.targets.size());
                }
            }

            if (site.classification == "unresolved") {
                uint32_t t = 0;
                const char* how = "";
                if (recover_indirect_call(exe, f.addr, pc, d.rs, index, t, how)) {
                    site.classification = fmt::format("call_ptr:{}", how);
                    site.targets.push_back(t);
                    db.edges.push_back({f.addr, pc, t,
                                        CallEdge::Kind::Indirect});
                    callees[fi].insert(t);
                    auto ti = index.find(t);
                    if (ti != index.end()) callers[ti->second].insert(f.addr);
                }
            }

            if (site.classification == "unresolved") {
                f.unresolved_indirect++;
                db.stats.indirect_unresolved++;
                uint32_t ctx_lo = (pc >= f.addr + 64) ? pc - 64 : f.addr;
                site.context = disassemble_range(exe, ctx_lo, pc + 8, nullptr);
            }
            db.indirect.push_back(std::move(site));
        }
    }

    // ---- pass 4: pointer tables in data -----------------------------------
    // A run of >=2 consecutive 4-aligned words each naming a distinct known
    // function start is treated as a dispatch table. A single isolated word is
    // NOT: at this density a lone match is as likely to be a coincidental
    // integer, and a false function pointer is worse than a missing one.
    {
        uint32_t run_start = 0;
        std::vector<uint32_t> run;
        auto flush = [&]() {
            if (run.size() >= 2) {
                for (size_t k = 0; k < run.size(); ++k) {
                    auto ti = index.find(run[k]);
                    if (ti == index.end()) continue;
                    db.functions[ti->second].address_taken = true;
                    db.edges.push_back({0,
                                        run_start + static_cast<uint32_t>(k * 4),
                                        run[k], CallEdge::Kind::AddressTaken});
                }
            }
            run.clear();
        };
        for (uint32_t a = lo; a + 3 < hi; a += 4) {
            uint32_t w = read_w(a);
            if (index.count(w)) {
                if (run.empty()) run_start = a;
                run.push_back(w);
            } else {
                flush();
            }
        }
        flush();
    }

    // ---- pass 5: reachability from the EXE entry point ---------------------
    {
        std::unordered_map<uint32_t, std::vector<uint32_t>> succ;
        // Every edge kind here is statically proven — an Indirect edge only
        // exists when the pointer's value was resolved — so all of them carry
        // reachability. Nothing is reached "because the runtime saw it".
        for (const auto& e : db.edges)
            succ[e.from_func].push_back(e.to_addr);
        long ei = containing_idx(db.entry_point);
        std::queue<uint32_t> q;
        std::unordered_set<uint32_t> seen;
        if (ei >= 0) {
            q.push(db.functions[ei].addr);
            seen.insert(db.functions[ei].addr);
        }
        // Pointer-table edges have from_func == 0 (no owning function); seed
        // their targets so a table-only-reachable handler is not called an
        // orphan.
        for (const auto& e : db.edges) {
            if (e.from_func == 0 && seen.insert(e.to_addr).second)
                q.push(e.to_addr);
        }
        while (!q.empty()) {
            uint32_t cur = q.front(); q.pop();
            auto it = succ.find(cur);
            if (it == succ.end()) continue;
            for (uint32_t nx : it->second)
                if (index.count(nx) && seen.insert(nx).second) q.push(nx);
        }
        for (auto& f : db.functions) f.reachable = seen.count(f.addr) > 0;
    }

    // ---- pass 6: degrees ---------------------------------------------------
    for (size_t i = 0; i < db.functions.size(); ++i) {
        db.functions[i].in_degree =
            static_cast<uint32_t>(callers[i].size());
        db.functions[i].out_degree =
            static_cast<uint32_t>(callees[i].size());
    }

    // ---- pass 7: symbols ---------------------------------------------------
    if (!opts.symbols_toml.empty()) {
        std::vector<SymbolEntry> syms;
        std::string serr;
        if (!load_symbols(opts.symbols_toml, syms, serr)) {
            error = serr;
            return db;
        }
        for (const auto& s : syms) {
            auto it = index.find(s.pc);
            if (it == index.end()) continue;
            FunctionRecord& f = db.functions[it->second];
            f.name = s.name;
            f.user_named = true;
            f.status = s.status;
            f.note = s.note;
        }
    }

    // ---- pass 8: confidence + prototypes ----------------------------------
    for (auto& f : db.functions) {
        if (!f.bios_call.empty()) continue; // pass 2.5 already decided
        if (f.is_data) {
            f.confidence = Confidence::DataRegion;
            f.confidence_reason = "classified as data";
        } else if (f.partial) {
            f.confidence = Confidence::Low;
            f.confidence_reason = fmt::format(
                "{} word(s) did not decode", f.undecoded_pcs.size());
        } else if (f.has_prologue && f.has_epilogue && f.ends_jr_ra &&
                   f.reachable && f.in_degree > 0) {
            f.confidence = Confidence::Verified;
            f.confidence_reason =
                "prologue + epilogue + jr $ra, called from reachable code";
        } else if (f.ends_jr_ra && f.reachable) {
            f.confidence = Confidence::High;
            f.confidence_reason = "jr $ra exit, reachable from entry";
        } else if (f.has_prologue || f.address_taken) {
            f.confidence = Confidence::Medium;
            f.confidence_reason = f.address_taken
                ? "address taken but no call-graph path from entry"
                : "prologue-shaped, no proven call path";
        } else {
            f.confidence = Confidence::Low;
            f.confidence_reason = f.reachable
                ? "reachable but no prologue and no jr $ra exit"
                : "no prologue, no jr $ra exit, unreachable from entry";
        }
        f.sig.prototype = render_prototype(f);
    }

    // ---- stats -------------------------------------------------------------
    db.stats.total_functions = static_cast<uint32_t>(db.functions.size());
    db.stats.bytes_image = hi - lo;
    for (const auto& f : db.functions) {
        db.stats.confidence_counts[static_cast<int>(f.confidence)]++;
        if (f.reachable) db.stats.reachable_functions++;
        else db.stats.orphan_functions++;
        if (f.user_named) db.stats.named_functions++;
        if (f.partial) db.stats.partial_functions++;
        if (!f.is_data) db.stats.bytes_covered += f.size;
    }

    std::sort(db.edges.begin(), db.edges.end(),
              [](const CallEdge& a, const CallEdge& b) {
                  if (a.from_func != b.from_func) return a.from_func < b.from_func;
                  return a.site_pc < b.site_pc;
              });
    std::sort(db.data_refs.begin(), db.data_refs.end(),
              [](const DataRef& a, const DataRef& b) {
                  return a.site_pc < b.site_pc;
              });
    std::sort(db.indirect.begin(), db.indirect.end(),
              [](const IndirectSite& a, const IndirectSite& b) {
                  return a.pc < b.pc;
              });
    return db;
}

std::vector<std::string> disassemble_range(const PS1Executable& exe,
                                           uint32_t lo, uint32_t hi,
                                           const AnalysisDb* db) {
    std::vector<std::string> out;
    for (uint32_t pc = lo; pc < hi; pc += 4) {
        auto w = exe.read_word(pc);
        if (!w.has_value()) break;
        DecodedInstruction d = MipsDecoder::decode(*w, pc);
        out.push_back(fmt::format("{:08X}  {:08X}  {}", pc, *w,
                                  format_instr(d, db)));
    }
    return out;
}

} // namespace PSXRecomp::Analysis
