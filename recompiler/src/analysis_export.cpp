// analysis_export.cpp — artifact writers for the static analysis database.

#include "analysis_export.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "fmt/format.h"

namespace PSXRecomp::Analysis {

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
            else
                out += c;
        }
    }
    return out;
}

std::string q(const std::string& s) { return "\"" + json_escape(s) + "\""; }

bool open_out(const std::filesystem::path& p, std::ofstream& f,
              std::string& error) {
    std::error_code ec;
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);
    f.open(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = fmt::format("cannot write {}", p.string());
        return false;
    }
    return true;
}

std::string saved_regs_str(uint32_t mask) {
    static const char* names[32] = {
        "zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra"};
    std::string s;
    for (int i = 0; i < 32; ++i) {
        if (mask & (1u << i)) {
            if (!s.empty()) s += "|";
            s += names[i];
        }
    }
    return s;
}

std::string tag_string(const FunctionRecord& f) {
    std::string t;
    auto put = [&](const char* s) { if (!t.empty()) t += "|"; t += s; };
    if (!f.bios_call.empty()) put("bios");
    if (f.sig.is_leaf)        put("leaf");
    if (f.sig.uses_gte)       put("gte");
    if (f.sig.uses_syscall)   put("syscall");
    if (f.sig.uses_break)     put("break");
    if (f.sig.touches_mmio)   put("mmio");
    if (f.sig.reads_gp)       put("gp");
    if (f.address_taken)      put("addr_taken");
    if (!f.reachable)         put("orphan");
    if (f.partial)            put("partial");
    if (f.unresolved_indirect) put("indirect");
    if (f.alias_of)           put("alias");
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
bool write_json_bundle(const AnalysisDb& db, const std::filesystem::path& dir,
                       bool include_refs, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    {
        std::ofstream f;
        if (!open_out(dir / "analysis.json", f, error)) return false;
        f << "{\n";
        f << "  \"schema\": \"psxrecomp static analysis v1\",\n";
        f << "  \"image\": " << q(db.image_name) << ",\n";
        f << fmt::format("  \"load_address\": {},\n", db.load_address);
        f << fmt::format("  \"entry_point\": {},\n", db.entry_point);
        f << fmt::format("  \"initial_gp\": {},\n", db.initial_gp);
        f << fmt::format("  \"image_size\": {},\n", db.image_size);
        f << "  \"static_only\": true,\n";
        f << "  \"stats\": {\n";
        const auto& s = db.stats;
        f << fmt::format("    \"total_functions\": {},\n", s.total_functions);
        f << fmt::format("    \"total_instructions\": {},\n", s.total_instructions);
        f << fmt::format("    \"bytes_covered\": {},\n", s.bytes_covered);
        f << fmt::format("    \"bytes_image\": {},\n", s.bytes_image);
        f << fmt::format("    \"verified\": {},\n", s.confidence_counts[0]);
        f << fmt::format("    \"high\": {},\n", s.confidence_counts[1]);
        f << fmt::format("    \"medium\": {},\n", s.confidence_counts[2]);
        f << fmt::format("    \"low\": {},\n", s.confidence_counts[3]);
        f << fmt::format("    \"data\": {},\n", s.confidence_counts[4]);
        f << fmt::format("    \"reachable\": {},\n", s.reachable_functions);
        f << fmt::format("    \"orphans\": {},\n", s.orphan_functions);
        f << fmt::format("    \"named\": {},\n", s.named_functions);
        f << fmt::format("    \"direct_edges\": {},\n", s.direct_edges);
        f << fmt::format("    \"jump_tables_resolved\": {},\n", s.jump_tables_resolved);
        f << fmt::format("    \"jump_table_targets\": {},\n", s.jump_table_targets);
        f << fmt::format("    \"indirect_unresolved\": {},\n", s.indirect_unresolved);
        f << fmt::format("    \"partial_functions\": {},\n", s.partial_functions);
        f << fmt::format("    \"undecoded_words\": {}\n", s.undecoded_words);
        f << "  },\n";
        f << "  \"functions\": [\n";
        for (size_t i = 0; i < db.functions.size(); ++i) {
            const auto& fn = db.functions[i];
            f << "    {";
            f << fmt::format("\"addr\": {}, ", fn.addr);
            f << fmt::format("\"end\": {}, ", fn.end);
            f << fmt::format("\"size\": {}, ", fn.size);
            f << fmt::format("\"instructions\": {}, ", fn.instruction_count);
            f << "\"name\": " << q(fn.name) << ", ";
            f << fmt::format("\"user_named\": {}, ", fn.user_named ? "true" : "false");
            f << "\"status\": " << q(fn.status) << ", ";
            f << "\"note\": " << q(fn.note) << ", ";
            f << "\"confidence\": " << q(confidence_name(fn.confidence)) << ", ";
            f << "\"confidence_reason\": " << q(fn.confidence_reason) << ", ";
            f << fmt::format("\"reachable\": {}, ", fn.reachable ? "true" : "false");
            f << fmt::format("\"address_taken\": {}, ", fn.address_taken ? "true" : "false");
            f << fmt::format("\"partial\": {}, ", fn.partial ? "true" : "false");
            f << fmt::format("\"is_data\": {}, ", fn.is_data ? "true" : "false");
            f << fmt::format("\"has_prologue\": {}, ", fn.has_prologue ? "true" : "false");
            f << fmt::format("\"has_epilogue\": {}, ", fn.has_epilogue ? "true" : "false");
            f << fmt::format("\"ends_jr_ra\": {}, ", fn.ends_jr_ra ? "true" : "false");
            f << fmt::format("\"alias_of\": {}, ", fn.alias_of);
            f << "\"bios_call\": " << q(fn.bios_call) << ", ";
            f << fmt::format("\"in_degree\": {}, ", fn.in_degree);
            f << fmt::format("\"out_degree\": {}, ", fn.out_degree);
            f << fmt::format("\"unresolved_indirect\": {}, ", fn.unresolved_indirect);
            f << fmt::format("\"blocks\": {}, ", fn.block_leaders.size());
            f << "\"sig\": {";
            f << fmt::format("\"args\": {}, ", fn.sig.arg_count);
            f << fmt::format("\"arg_mask\": {}, ", fn.sig.arg_mask);
            f << fmt::format("\"returns\": {}, ",
                             (fn.sig.returns_v0 || fn.sig.returns_v1) ? "true" : "false");
            f << fmt::format("\"leaf\": {}, ", fn.sig.is_leaf ? "true" : "false");
            f << fmt::format("\"gte\": {}, ", fn.sig.uses_gte ? "true" : "false");
            f << fmt::format("\"syscall\": {}, ", fn.sig.uses_syscall ? "true" : "false");
            f << fmt::format("\"mmio\": {}, ", fn.sig.touches_mmio ? "true" : "false");
            f << fmt::format("\"stack_frame\": {}, ", fn.sig.stack_frame);
            f << "\"saved\": " << q(saved_regs_str(fn.sig.saved_mask)) << ", ";
            f << fmt::format("\"confident\": {}, ", fn.sig.sig_confident ? "true" : "false");
            f << "\"prototype\": " << q(fn.sig.prototype);
            f << "}";
            f << "}" << (i + 1 < db.functions.size() ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
    }

    {
        std::ofstream f;
        if (!open_out(dir / "edges.json", f, error)) return false;
        f << "{\n  \"schema\": \"psxrecomp call edges v1\",\n  \"edges\": [\n";
        for (size_t i = 0; i < db.edges.size(); ++i) {
            const auto& e = db.edges[i];
            f << fmt::format("    {{\"from\": {}, \"pc\": {}, \"to\": {}, \"kind\": \"{}\"}}{}\n",
                             e.from_func, e.site_pc, e.to_addr,
                             edge_kind_name(e.kind),
                             i + 1 < db.edges.size() ? "," : "");
        }
        f << "  ]\n}\n";
    }

    {
        std::ofstream f;
        if (!open_out(dir / "indirect.json", f, error)) return false;
        f << "{\n  \"schema\": \"psxrecomp indirect sites v1\",\n  \"sites\": [\n";
        for (size_t i = 0; i < db.indirect.size(); ++i) {
            const auto& s = db.indirect[i];
            f << "    {";
            f << fmt::format("\"func\": {}, \"pc\": {}, \"reg\": {}, ",
                             s.from_func, s.pc, s.reg);
            f << "\"kind\": " << q(s.kind) << ", ";
            f << "\"classification\": " << q(s.classification) << ", ";
            f << fmt::format("\"table_base\": {}, \"table_count\": {}, ",
                             s.table_base, s.table_count);
            f << "\"targets\": [";
            for (size_t k = 0; k < s.targets.size(); ++k)
                f << (k ? "," : "") << s.targets[k];
            f << "], \"context\": [";
            for (size_t k = 0; k < s.context.size(); ++k)
                f << (k ? ", " : "") << q(s.context[k]);
            f << "]}";
            f << (i + 1 < db.indirect.size() ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
    }

    if (include_refs) {
        std::ofstream f;
        if (!open_out(dir / "refs.json", f, error)) return false;
        f << "{\n  \"schema\": \"psxrecomp data refs v1\",\n  \"refs\": [\n";
        for (size_t i = 0; i < db.data_refs.size(); ++i) {
            const auto& r = db.data_refs[i];
            f << fmt::format("    {{\"func\": {}, \"pc\": {}, \"target\": {}, "
                             "\"write\": {}, \"width\": {}, \"kind\": \"{}\"}}{}\n",
                             r.from_func, r.site_pc, r.target,
                             r.is_write ? "true" : "false", r.width, r.kind,
                             i + 1 < db.data_refs.size() ? "," : "");
        }
        f << "  ]\n}\n";
    }

    return true;
}

// ---------------------------------------------------------------------------
bool write_symbols_toml(const AnalysisDb& db, const std::filesystem::path& path,
                        Confidence min_conf, bool include_unnamed,
                        std::string& error) {
    // APPEND-ONLY. An earlier version regenerated the file from the [[func]]
    // entries it had parsed, which silently deleted everything else in it —
    // and symbols.toml carries more than functions: Ape Escape has 11
    // [[object]] entries and a top-level `game` key, Crash Team Racing has 33
    // [[site]] entries, all hand-researched. Existing bytes are now never
    // rewritten; newly discovered functions are appended and nothing else is
    // touched.
    std::vector<SymbolEntry> existing;
    if (!load_symbols(path, existing, error)) return false;

    std::set<uint32_t> have;
    for (const auto& s : existing) have.insert(s.pc);

    std::string original;
    {
        std::ifstream in(path, std::ios::binary);
        if (in)
            original.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
    }

    std::string added_text;
    uint32_t added = 0;
    if (include_unnamed) {
        for (const auto& f : db.functions) {
            if (f.is_data) continue;
            if (static_cast<int>(f.confidence) > static_cast<int>(min_conf)) continue;
            if (have.count(f.addr)) continue;
            const std::string note =
                f.bios_call.empty()
                    ? fmt::format("{} — {} arg(s), {} caller(s), {}",
                                  confidence_name(f.confidence), f.sig.arg_count,
                                  f.in_degree, f.sig.is_leaf ? "leaf" : "non-leaf")
                    : fmt::format("kernel dispatch thunk {} — {} caller(s)",
                                  f.bios_call, f.in_degree);
            added_text += fmt::format(
                "\n[[func]]\npc = 0x{:08X}\nname = \"{}\"\nemit = false\n"
                "status = \"guessed\"\nnote = \"{}\"\n",
                f.addr, f.name, note);
            added++;
        }
    }

    if (added == 0) {
        fmt::print("  symbols.toml: {} entries, none new\n", existing.size());
        return true;
    }

    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    if (original.empty()) {
        f << "# " << db.image_name << " — progressive symbol map\n";
        f << "# Discover → label here → manipulate via PSX_FN_* (sync_symbols.py).\n";
        f << "# See psxrecomp/docs/SYMBOLS.md and docs/FUNCTION_DISCOVERY.md.\n";
    } else {
        f << original;
        if (original.back() != '\n') f << "\n";
    }
    f << "\n# --- appended by `psxrecomp-analyze --emit-symbols` ("
      << confidence_name(min_conf) << " or better) ---\n";
    f << added_text;
    fmt::print("  symbols.toml: {} existing entries kept, {} appended\n",
               existing.size(), added);
    return true;
}

// ---------------------------------------------------------------------------
bool write_symbol_addrs(const AnalysisDb& db, const std::filesystem::path& path,
                        std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    f << "// " << db.image_name << " — generated by psxrecomp-analyze\n";
    f << "// Static analysis only; no runtime evidence was consulted.\n";
    for (const auto& fn : db.functions) {
        if (fn.is_data) continue;
        f << fmt::format("{} = 0x{:08X}; // type:func size:0x{:X} conf:{}{}\n",
                         fn.name, fn.addr, fn.size,
                         confidence_name(fn.confidence),
                         fn.user_named ? " named" : "");
    }
    return true;
}

// ---------------------------------------------------------------------------
bool write_ghidra_script(const AnalysisDb& db, const std::filesystem::path& path,
                         std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    f << "# psxrecomp_import.py — generated by psxrecomp-analyze\n";
    f << "# Ghidra script: creates functions, applies names, and bookmarks the\n";
    f << "# indirect sites static analysis could not resolve.\n";
    f << "# Run against a project where " << db.image_name
      << " is loaded at its EXE load address.\n";
    f << "# @category PSXRecomp\n\n";
    f << "from ghidra.program.model.symbol import SourceType\n\n";
    f << "fm = currentProgram.getFunctionManager()\n";
    f << "st = currentProgram.getSymbolTable()\n";
    f << "af = currentProgram.getAddressFactory().getDefaultAddressSpace()\n\n";
    f << "def addr(v):\n    return af.getAddress(v)\n\n";
    f << "FUNCS = [\n";
    for (const auto& fn : db.functions) {
        if (fn.is_data) continue;
        f << fmt::format("    (0x{:08X}, 0x{:X}, \"{}\", \"{}\"),\n",
                         fn.addr, fn.size, json_escape(fn.name),
                         confidence_name(fn.confidence));
    }
    f << "]\n\n";
    f << "UNRESOLVED = [\n";
    for (const auto& s : db.indirect) {
        if (s.classification != "unresolved") continue;
        f << fmt::format("    (0x{:08X}, \"{} ${}\"),\n", s.pc, s.kind, s.reg);
    }
    f << "]\n\n";
    f << "created = renamed = 0\n";
    f << "for a, size, name, conf in FUNCS:\n";
    f << "    ea = addr(a)\n";
    f << "    fn = fm.getFunctionAt(ea)\n";
    f << "    if fn is None:\n";
    f << "        fn = createFunction(ea, name)\n";
    f << "        if fn is not None:\n";
    f << "            created += 1\n";
    f << "    if fn is not None and not name.startswith(\"func_\"):\n";
    f << "        fn.setName(name, SourceType.IMPORTED)\n";
    f << "        renamed += 1\n";
    f << "    if fn is not None:\n";
    f << "        setPlateComment(ea, \"psxrecomp: confidence=%s\" % conf)\n\n";
    f << "for a, what in UNRESOLVED:\n";
    f << "    createBookmark(addr(a), \"psxrecomp\", \"unresolved %s\" % what)\n\n";
    f << "print(\"psxrecomp: %d functions created, %d named, %d unresolved \"\n";
    f << "      \"indirect sites bookmarked\" % (created, renamed, len(UNRESOLVED)))\n";
    return true;
}

// ---------------------------------------------------------------------------
bool write_tsv(const AnalysisDb& db, const std::filesystem::path& path,
               std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    f << "addr\tsize\tname\tconfidence\targs\tret\tcallers\tcallees\t"
         "frame\tsaved\ttags\n";
    for (const auto& fn : db.functions) {
        f << fmt::format("0x{:08X}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                         fn.addr, fn.size, fn.name,
                         confidence_name(fn.confidence),
                         fn.sig.arg_count,
                         (fn.sig.returns_v0 || fn.sig.returns_v1) ? "u32" : "void",
                         fn.in_degree, fn.out_degree, fn.sig.stack_frame,
                         saved_regs_str(fn.sig.saved_mask),
                         tag_string(fn));
    }
    return true;
}

// ---------------------------------------------------------------------------
bool write_disasm(const AnalysisDb& db, const PS1Executable& exe, uint32_t addr,
                  const std::filesystem::path& path, std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;

    auto emit_one = [&](const FunctionRecord& fn) {
        f << "\n/* ------------------------------------------------------------ */\n";
        f << fmt::format("/* {}  [0x{:08X}..0x{:08X})  {} bytes */\n",
                         fn.name, fn.addr, fn.end, fn.size);
        f << fmt::format("/* {}  confidence={} ({}) */\n", fn.sig.prototype,
                         confidence_name(fn.confidence), fn.confidence_reason);
        f << fmt::format("/* callers={} callees={} frame={} saved={} */\n",
                         fn.in_degree, fn.out_degree, fn.sig.stack_frame,
                         saved_regs_str(fn.sig.saved_mask));
        if (!fn.sig.sig_confident)
            f << "/* NOTE: signature scan is approximate here (loop or partial "
                 "decode) */\n";
        std::set<uint32_t> leaders(fn.block_leaders.begin(),
                                   fn.block_leaders.end());
        for (const auto& line : disassemble_range(exe, fn.addr, fn.end, &db)) {
            uint32_t pc = 0;
            std::istringstream(line.substr(0, 8)) >> std::hex >> pc;
            if (leaders.count(pc) && pc != fn.addr)
                f << fmt::format("\n.L_{:08X}:\n", pc);
            f << "    " << line << "\n";
        }
    };

    if (addr) {
        const FunctionRecord* fn = db.find(addr);
        if (!fn) {
            fn = db.containing(addr);
            if (!fn) {
                error = fmt::format("no function at or containing 0x{:08X}", addr);
                return false;
            }
        }
        emit_one(*fn);
    } else {
        for (const auto& fn : db.functions)
            if (!fn.is_data) emit_one(fn);
    }
    return true;
}

// ---------------------------------------------------------------------------
void print_report(const AnalysisDb& db, int top_n) {
    const auto& s = db.stats;
    fmt::print("\n=== {} ===\n", db.image_name);
    fmt::print("  load 0x{:08X}  entry 0x{:08X}  gp 0x{:08X}  size 0x{:X}\n",
               db.load_address, db.entry_point, db.initial_gp, db.image_size);
    fmt::print("\nFunctions: {}  ({} instructions, {:.1f}% of image covered)\n",
               s.total_functions, s.total_instructions,
               s.bytes_image ? 100.0 * s.bytes_covered / s.bytes_image : 0.0);
    fmt::print("  verified {:6}   high {:6}   medium {:6}   low {:6}   data {:6}\n",
               s.confidence_counts[0], s.confidence_counts[1],
               s.confidence_counts[2], s.confidence_counts[3],
               s.confidence_counts[4]);
    fmt::print("  reachable from entry: {}   orphans: {}   named: {}\n",
               s.reachable_functions, s.orphan_functions, s.named_functions);

    fmt::print("\nControl flow:\n");
    fmt::print("  direct calls               {}\n", s.direct_edges);
    fmt::print("  jump tables recovered      {} ({} targets)\n",
               s.jump_tables_resolved, s.jump_table_targets);
    fmt::print("  indirect sites unresolved  {}\n", s.indirect_unresolved);
    if (s.partial_functions)
        fmt::print("  partial decodes            {} functions, {} words\n",
                   s.partial_functions, s.undecoded_words);

    // Coverage honesty: say plainly what static analysis did not reach.
    fmt::print("\nStatic coverage gap (NOT filled from any runtime source):\n");
    fmt::print("  {} indirect transfer(s) have no proven target set.\n",
               s.indirect_unresolved);
    uint32_t uncovered = (s.bytes_image > s.bytes_covered)
                             ? s.bytes_image - s.bytes_covered : 0;
    fmt::print("  {} bytes ({:.1f}%) of the image belong to no code function.\n",
               uncovered, s.bytes_image ? 100.0 * uncovered / s.bytes_image : 0.0);

    if (top_n > 0) {
        std::vector<const FunctionRecord*> by_callers;
        for (const auto& f : db.functions)
            if (!f.is_data) by_callers.push_back(&f);
        std::sort(by_callers.begin(), by_callers.end(),
                  [](const FunctionRecord* a, const FunctionRecord* b) {
                      if (a->in_degree != b->in_degree)
                          return a->in_degree > b->in_degree;
                      return a->size > b->size;
                  });
        fmt::print("\nMost-called functions (best first targets for naming):\n");
        int n = 0;
        for (const auto* f : by_callers) {
            if (n++ >= top_n) break;
            fmt::print("  {:>4} callers  0x{:08X}  {:<28} {:<9} {}\n",
                       f->in_degree, f->addr, f->name,
                       confidence_name(f->confidence), f->sig.prototype);
        }

        std::vector<const IndirectSite*> unres;
        for (const auto& s2 : db.indirect)
            if (s2.classification == "unresolved") unres.push_back(&s2);
        if (!unres.empty()) {
            fmt::print("\nUnresolved indirect sites (the discovery worklist):\n");
            n = 0;
            for (const auto* u : unres) {
                if (n++ >= top_n) break;
                const FunctionRecord* owner = db.find(u->from_func);
                fmt::print("  0x{:08X}  {} ${:<2}  in {}\n", u->pc, u->kind,
                           u->reg, owner ? owner->name : "?");
            }
            if (static_cast<int>(unres.size()) > top_n)
                fmt::print("  … {} more\n", unres.size() - top_n);
        }
    }
    fmt::print("\n");
}

// ---------------------------------------------------------------------------
bool print_diff(const AnalysisDb& db, const std::filesystem::path& previous_tsv,
                std::string& error) {
    std::ifstream in(previous_tsv);
    if (!in) {
        error = fmt::format("cannot read {}", previous_tsv.string());
        return false;
    }
    struct Prev { uint32_t size; std::string name, conf; };
    std::map<uint32_t, Prev> prev;
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string addr_s, size_s, name, conf;
        if (!std::getline(ls, addr_s, '\t')) continue;
        if (!std::getline(ls, size_s, '\t')) continue;
        if (!std::getline(ls, name, '\t')) continue;
        if (!std::getline(ls, conf, '\t')) continue;
        uint32_t a = 0, sz = 0;
        try {
            a = static_cast<uint32_t>(std::stoul(addr_s, nullptr, 16));
            sz = static_cast<uint32_t>(std::stoul(size_s));
        } catch (...) { continue; }
        prev[a] = {sz, name, conf};
    }

    int added = 0, removed = 0, resized = 0, reconf = 0;
    fmt::print("\n=== diff vs {} ===\n", previous_tsv.string());
    for (const auto& f : db.functions) {
        auto it = prev.find(f.addr);
        if (it == prev.end()) {
            fmt::print("  + 0x{:08X}  {:<28} {} ({} bytes)\n", f.addr, f.name,
                       confidence_name(f.confidence), f.size);
            added++;
        } else {
            if (it->second.size != f.size) {
                fmt::print("  ~ 0x{:08X}  {:<28} size {} -> {}\n", f.addr,
                           f.name, it->second.size, f.size);
                resized++;
            }
            if (it->second.conf != confidence_name(f.confidence)) {
                fmt::print("  ~ 0x{:08X}  {:<28} {} -> {}\n", f.addr, f.name,
                           it->second.conf, confidence_name(f.confidence));
                reconf++;
            }
        }
    }
    for (const auto& [a, p] : prev) {
        if (!db.find(a)) {
            fmt::print("  - 0x{:08X}  {:<28} {} (no longer discovered)\n", a,
                       p.name, p.conf);
            removed++;
        }
    }
    fmt::print("\n  {} added, {} removed, {} resized, {} confidence changes\n\n",
               added, removed, resized, reconf);
    return true;
}

} // namespace PSXRecomp::Analysis
