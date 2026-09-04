// widescreen_scan.cpp — see widescreen_scan.h.

#include "widescreen_scan.h"

#include <algorithm>
#include <fstream>
#include <set>

#include "fmt/format.h"
#include "mips_decoder.h"

// The SAME detector the recompiler and the runtime interpreter use for
// auto_screen_x. Sharing it verbatim is the point: a function this scanner
// calls a screen-extent funnel is exactly one the emitter will widen, so the
// report's "already covered by auto_screen_x" claim cannot drift from reality.
#include "ws_cull_detect.h"

namespace PSXRecomp::Analysis {

namespace {

// Plausible PSX display extents. The console's horizontal modes are 256/320/
// 368/384/512/640 and the vertical 240/256/480; a trivial-reject immediate is
// one of those (occasionally +1 for an inclusive bound), so these windows are
// wide enough to catch any real title and narrow enough to keep the histogram
// meaningful.
constexpr uint32_t kWLo = 0x100, kWHi = 0x290;   // 256 .. 656
constexpr uint32_t kHLo = 0xC0,  kHHi = 0x201;   // 192 .. 513

bool plausible_w(uint32_t imm) { return imm >= kWLo && imm <= kWHi; }
bool plausible_h(uint32_t imm) { return imm >= kHLo && imm <= kHHi; }

// The console's actual display modes. A trivial-reject immediate is normally
// one of these (or one past, for an inclusive bound), so hitting one is real
// evidence rather than coincidence — it is weighted, never required, because a
// title is free to reject against a custom viewport.
bool canonical_w(uint32_t v) {
    static const uint32_t k[] = {256, 320, 368, 384, 512, 576, 640};
    for (uint32_t c : k) if (v == c || v == c + 1) return true;
    return false;
}
bool canonical_h(uint32_t v) {
    static const uint32_t k[] = {224, 240, 256, 480};
    for (uint32_t c : k) if (v == c || v == c + 1) return true;
    return false;
}

uint32_t rd_of(uint32_t w) { return (w >> 11) & 0x1F; }
uint32_t rt_of(uint32_t w) { return (w >> 16) & 0x1F; }
uint32_t rs_of(uint32_t w) { return (w >> 21) & 0x1F; }
uint32_t op_of(uint32_t w) { return w >> 26; }
uint32_t funct_of(uint32_t w) { return w & 0x3F; }
int32_t  simm_of(uint32_t w) { return static_cast<int16_t>(w & 0xFFFF); }
uint32_t uimm_of(uint32_t w) { return w & 0xFFFF; }

std::string dis1(const PS1Executable& exe, uint32_t pc, const AnalysisDb& db) {
    auto lines = disassemble_range(exe, pc, pc + 4, &db);
    if (lines.empty()) return {};
    // disassemble_range prefixes "PC  WORD  "; keep just the mnemonic text.
    const std::string& l = lines.front();
    const size_t cut = l.find("  ", 18);
    return cut == std::string::npos ? l : l.substr(20);
}

struct FnWords {
    uint32_t addr = 0;
    uint32_t end = 0;
    bool uses_gte = false;
    bool takes_a1 = false;      // $a1 read before written — a real parameter
    std::vector<uint32_t> words;
};

} // namespace

WsScanResult scan_widescreen(const PS1Executable& exe,
                             const AnalysisDb& db,
                             const WsScanOptions& opts) {
    WsScanResult out;

    // ---- collect each code function's instruction words once ---------------
    std::vector<FnWords> fns;
    fns.reserve(db.functions.size());
    for (const auto& f : db.functions) {
        if (f.is_data || f.size == 0) continue;
        FnWords fw;
        fw.addr = f.addr;
        fw.end = f.end;
        fw.uses_gte = f.sig.uses_gte;
        fw.takes_a1 = (f.sig.arg_mask & 0x2u) != 0;   // bit 1 == $a1
        fw.words.reserve(f.instruction_count);
        for (uint32_t pc = f.addr; pc < f.end; pc += 4) {
            auto w = exe.read_word(pc);
            if (!w.has_value()) break;
            fw.words.push_back(*w);
        }
        if (fw.uses_gte) out.gte_funcs++;
        fns.push_back(std::move(fw));
    }

    // ---- pass 1: discover the per-game W/H immediates ----------------------
    // A trivial-reject compare shows up many times across the render funnels,
    // so the right immediates are the frequent ones — but only inside GTE code.
    // Restricting the histogram that way is what keeps ordinary bounds checks
    // (array sizes, string lengths) from drowning out the display extents.
    for (const auto& fw : fns) {
        for (uint32_t w : fw.words) {
            const uint32_t op = op_of(w);
            if (op != 0x0A && op != 0x0B) continue;
            const uint32_t imm = uimm_of(w);
            if (plausible_w(imm)) out.w_hist[imm]++;
            if (plausible_h(imm)) out.h_hist[imm]++;
        }
    }

    out.w_imms = opts.w_imms;
    out.h_imms = opts.h_imms;
    if (out.w_imms.empty() || out.h_imms.empty()) {
        out.imms_discovered = true;
        // Frequency alone picks the wrong immediates: ordinary bounds checks in
        // 83 GTE functions outvoted Wipeout 3's real 0x200/0x240/0x100, which
        // appear in only three funnels. What actually identifies the pair is
        // CO-OCCURRENCE — the screen-extent signature is a width compare and a
        // height compare in the SAME function — so the search scores pairs, not
        // singletons. That also resolves the genuine ambiguity where a value is
        // plausible as both (0x100 is a 256-wide mode and Wipeout 3's height):
        // it wins whichever side its partner puts it on.
        // Weighted, not gated, on GTE use: Wipeout 3's three extent funnels
        // carry no COP2 op of their own (they reject before projecting), so a
        // hard `uses_gte` filter found nothing at all there. Weighting keeps the
        // precision that filter was buying without discarding those titles.
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> pairs;
        for (const auto& fw : fns) {
            const uint32_t weight = fw.uses_gte ? 3u : 1u;
            std::set<uint32_t> imms;
            for (uint32_t w : fw.words) {
                const uint32_t op = op_of(w);
                if (op == 0x0A || op == 0x0B) imms.insert(uimm_of(w));
            }
            for (uint32_t a : imms) {
                if (!plausible_w(a)) continue;
                for (uint32_t b : imms) {
                    if (a == b || !plausible_h(b)) continue;
                    pairs[{a, b}] += weight;
                }
            }
        }
        auto score = [&](uint32_t a, uint32_t b, uint32_t n) {
            return n * (canonical_w(a) ? 3u : 1u) * (canonical_h(b) ? 3u : 1u);
        };
        uint32_t best_w = 0, best_h = 0, best = 0;
        for (const auto& [ab, n] : pairs) {
            const uint32_t sc = score(ab.first, ab.second, n);
            if (sc > best) { best = sc; best_w = ab.first; best_h = ab.second; }
        }
        if (best) {
            auto partners = [&](bool want_w) {
                std::vector<std::pair<uint32_t, uint32_t>> v;
                for (const auto& [ab, n] : pairs) {
                    const bool fixed = want_w ? (ab.second == best_h) : (ab.first == best_w);
                    if (!fixed) continue;
                    const uint32_t key = want_w ? ab.first : ab.second;
                    v.push_back({key, score(ab.first, ab.second, n)});
                }
                std::sort(v.begin(), v.end(), [](auto& x, auto& y) {
                    if (x.second != y.second) return x.second > y.second;
                    return x.first < y.first;
                });
                std::vector<uint32_t> r;
                for (const auto& [k, sc] : v) {
                    if (r.size() >= 3) break;
                    if (sc * 12 < v.front().second) break;  // noise, not a second extent
                    r.push_back(k);
                }
                return r;
            };
            if (out.w_imms.empty()) out.w_imms = partners(true);
            if (out.h_imms.empty()) {
                out.h_imms = partners(false);
                const std::vector<uint32_t> chosen_w = out.w_imms;
                out.h_imms.erase(
                    std::remove_if(out.h_imms.begin(), out.h_imms.end(),
                                   [&](uint32_t v) {
                                       return std::find(chosen_w.begin(), chosen_w.end(),
                                                        v) != chosen_w.end();
                                   }),
                    out.h_imms.end());
            }
        }

        // A title usually tests BOTH the exclusive and the inclusive bound
        // (the framework default is {0x140, 0x141}), and the pair scoring only
        // surfaces whichever is more common. Dropping the sibling is not
        // cosmetic: Tomba's bias/range pairing keys on
        // `sltiu_imm - 2*bias == W`, and 449 - 128 is 0x141, so without it
        // three of its four pairs go undetected. The sibling is added only when
        // the image actually contains a compare against it.
        auto complete_bounds = [](std::vector<uint32_t>& set,
                                  const std::map<uint32_t, uint32_t>& hist) {
            std::vector<uint32_t> add;
            for (uint32_t v : set)
                for (int d : {-1, 1}) {
                    const uint32_t sib = static_cast<uint32_t>(static_cast<int>(v) + d);
                    if (hist.count(sib) &&
                        std::find(set.begin(), set.end(), sib) == set.end() &&
                        std::find(add.begin(), add.end(), sib) == add.end())
                        add.push_back(sib);
                }
            set.insert(set.end(), add.begin(), add.end());
            std::sort(set.begin(), set.end());
        };
        complete_bounds(out.w_imms, out.w_hist);
        complete_bounds(out.h_imms, out.h_hist);
    }
    for (uint32_t v : out.w_imms) if (canonical_w(v)) out.w_canonical = true;
    for (uint32_t v : out.h_imms) if (canonical_h(v)) out.h_canonical = true;

    if (out.w_imms.empty() || out.h_imms.empty()) return out;   // nothing to key off

    // ---- pass 2: screen-extent funnels (auto_screen_x coverage) ------------
    for (const auto& fw : fns) {
        if (psx_ws_cull_scan(fw.words.data(), static_cast<int>(fw.words.size()),
                             out.w_imms.data(), static_cast<int>(out.w_imms.size()),
                             out.h_imms.data(), static_cast<int>(out.h_imms.size())))
            out.extent_funcs.push_back(fw.addr);
    }
    const std::set<uint32_t> extent(out.extent_funcs.begin(), out.extent_funcs.end());

    // ---- pass 3: bias/range pairs (masked-u16 X window) --------------------
    // The idiom, verbatim from Tomba 0x80022E78:
    //     addiu $v0, $v0, 64          <- bias_site   (+halfwidth)
    //     andi  $v0, $v0, 0xFFFF      <- optional u16 mask
    //     sltiu $v0, $v0, 449         <- range_site  (W + 2*halfwidth)
    //     beq   $v0, $zero, reject
    // The arithmetic tie between the two immediates is what makes this safe to
    // propose: `sltiu_imm - 2*bias_imm` must land on a configured W immediate.
    // Without that check any `addiu`+`sltiu` neighbours would qualify.
    for (const auto& fw : fns) {
        for (size_t i = 0; i + 2 < fw.words.size(); ++i) {
            const uint32_t a = fw.words[i];
            if (op_of(a) != 0x08 && op_of(a) != 0x09) continue;   // addi/addiu
            const int32_t k = simm_of(a);
            if (k <= 0 || k > 1024) continue;
            const uint32_t dest = rt_of(a);
            if (dest == 0) continue;

            for (size_t j = i + 1; j <= i + 3 && j < fw.words.size(); ++j) {
                const uint32_t s = fw.words[j];
                if (op_of(s) == 0x0C && rt_of(s) == dest && rs_of(s) == dest)
                    continue;                                     // andi mask
                if (op_of(s) != 0x0B) break;                      // must be sltiu
                if (rs_of(s) != dest) break;                      // on the biased value
                const uint32_t win = uimm_of(s);
                const int32_t base = static_cast<int32_t>(win) - 2 * k;
                bool keyed = false;
                for (uint32_t wi : out.w_imms)
                    if (base == static_cast<int32_t>(wi)) keyed = true;
                if (!keyed) break;

                const uint32_t bias_pc = fw.addr + static_cast<uint32_t>(i * 4);
                const uint32_t range_pc = fw.addr + static_cast<uint32_t>(j * 4);
                const int conf = extent.count(fw.addr) ? 95 : 85;
                const std::string why = fmt::format(
                    "masked-u16 X window: +{} bias, sltiu {} = W({}) + 2*{}",
                    k, win, base, k);
                out.candidates.push_back({"bias_sites", bias_pc, fw.addr, range_pc,
                                          conf, why, dis1(exe, bias_pc, db)});
                out.candidates.push_back({"range_sites", range_pc, fw.addr, bias_pc,
                                          conf, why, dis1(exe, range_pc, db)});
                break;
            }
        }
    }

    // ---- pass 4: a1 margin sites ------------------------------------------
    // Some classifiers take the horizontal margin as a PARAMETER and are called
    // by the render funnel rather than being one; Tomba's live at 0x80022C08,
    // which uses no GTE op and has no static caller at all (it is reached
    // through a dispatch). Gating this pass on the containing function being a
    // screen-extent funnel therefore excluded every real site. The signature
    // that actually holds is local: the function takes $a1, folds it into a
    // coordinate with `addu`, and has a repurposable nop just before — a nop is
    // the only form code_generator.cpp will rewrite here, and never one already
    // owned by a branch delay slot.
    for (const auto& fw : fns) {
        if (!fw.takes_a1) continue;
        bool emitted = false;
        for (size_t i = 0; i < fw.words.size() && !emitted; ++i) {
            const uint32_t w = fw.words[i];
            if (op_of(w) != 0x00 || funct_of(w) != 0x21) continue;      // addu
            if (rs_of(w) != 5 && rt_of(w) != 5) continue;               // uses $a1
            if (rd_of(w) == 5) continue;                                // not a1 = a1 + x
            for (size_t back = 1; back <= 4 && back <= i; ++back) {
                const size_t n = i - back;
                if (fw.words[n] != 0u) continue;                        // nop only
                if (n > 0) {
                    DecodedInstruction prev = MipsDecoder::decode(
                        fw.words[n - 1], fw.addr + static_cast<uint32_t>((n - 1) * 4));
                    if (prev.is_branch || prev.is_jump) break;          // delay slot
                }
                const uint32_t pc = fw.addr + static_cast<uint32_t>(n * 4);
                const bool near_gte = fw.uses_gte || extent.count(fw.addr);
                out.candidates.push_back(
                    {"a1_sites", pc, fw.addr, fw.addr + static_cast<uint32_t>(i * 4),
                     near_gte ? 65 : 50,
                     fmt::format("takes $a1; repurposable nop {} word(s) before `addu` "
                                 "folds it into a coordinate{}", back,
                                 near_gte ? " (GTE/extent context)" : ""),
                     dis1(exe, pc, db)});
                emitted = true;   // one site per classifier
                break;
            }
        }
    }

    // ---- pass 5: X compares outside any extent funnel ----------------------
    // These are the ones auto_screen_x will NOT reach, so they are exactly the
    // addresses that still need hand-listing under screen_x_sites.
    for (const auto& fw : fns) {
        if (extent.count(fw.addr)) continue;
        for (size_t i = 0; i < fw.words.size(); ++i) {
            const uint32_t w = fw.words[i];
            if (op_of(w) != 0x0B) continue;                             // sltiu only
            bool keyed = false;
            for (uint32_t wi : out.w_imms)
                if (uimm_of(w) == wi) keyed = true;
            if (!keyed) continue;
            const uint32_t pc = fw.addr + static_cast<uint32_t>(i * 4);
            bool already = false;
            for (const auto& c : out.candidates)
                if (c.pc == pc) already = true;
            if (already) continue;
            out.candidates.push_back(
                {"screen_x_sites", pc, fw.addr, 0, fw.uses_gte ? 70 : 40,
                 fw.uses_gte
                     ? "W compare in a GTE function with no height compare — "
                       "outside auto_screen_x's reach"
                     : "W compare in a non-GTE function; verify before use",
                 dis1(exe, pc, db)});
        }
    }

    std::sort(out.candidates.begin(), out.candidates.end(),
              [](const WsCandidate& a, const WsCandidate& b) {
                  if (a.confidence != b.confidence) return a.confidence > b.confidence;
                  return a.pc < b.pc;
              });
    if (static_cast<int>(out.candidates.size()) > opts.max_candidates)
        out.candidates.resize(opts.max_candidates);
    std::sort(out.extent_funcs.begin(), out.extent_funcs.end());
    return out;
}

} // namespace PSXRecomp::Analysis

namespace PSXRecomp::Analysis {

namespace {

std::vector<const WsCandidate*> of_kind(const WsScanResult& s, const char* kind,
                                        int min_conf) {
    std::vector<const WsCandidate*> v;
    for (const auto& c : s.candidates)
        if (c.kind == kind && c.confidence >= min_conf) v.push_back(&c);
    std::sort(v.begin(), v.end(),
              [](const WsCandidate* a, const WsCandidate* b) { return a->pc < b->pc; });
    return v;
}

std::string imm_list(const std::vector<uint32_t>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += fmt::format("{}\"0x{:X}\"", i ? ", " : "", v[i]);
    return s;
}

} // namespace

bool write_ws_sites_toml(const WsScanResult& scan,
                         const std::filesystem::path& path,
                         int min_confidence,
                         std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { error = fmt::format("cannot write {}", path.string()); return false; }

    f << "# [widescreen] candidates from `psxrecomp-analyze --scan-widescreen`.\n";
    f << "# Static analysis only — no runtime, trace, or overlay capture was read.\n";
    f << "#\n";
    f << "# These are CANDIDATES. Every one satisfies the instruction form\n";
    f << "# code_generator.cpp requires, so none can break the build, but only\n";
    f << "# playtesting shows whether a site is the right one to widen.\n";
    f << "# Paste into game.toml and cut what does not help.\n\n";

    f << "[widescreen.cull]\n";
    f << "auto_screen_x = true";
    if (!scan.extent_funcs.empty())
        f << fmt::format("   # {} function(s) carry the screen-extent signature",
                         scan.extent_funcs.size());
    f << "\n";
    if (scan.imms_discovered && !(scan.w_canonical && scan.h_canonical))
        f << "# WARNING: these do not match a console display mode — treat as a guess.\n";
    if (scan.imms_discovered) {
        f << "# Discovered from the image: width/height compare immediates scored by\n";
        f << "# CO-OCCURRENCE in the same function (the screen-extent signature),\n";
        f << "# weighted toward GTE code and the console's real display modes.\n";
        f << "# Verify before trusting — extra entries only widen what auto_screen_x\n";
        f << "# considers, but a wrong one widens the wrong compare.\n";
    }
    f << fmt::format("screen_w_imms = [{}]\n", imm_list(scan.w_imms));
    f << fmt::format("screen_h_imms = [{}]\n", imm_list(scan.h_imms));

    auto emit = [&](const char* key, const char* note) {
        auto v = of_kind(scan, key, min_confidence);
        if (v.empty()) return;
        f << "\n# " << note << "\n";
        for (const auto* c : v)
            f << fmt::format("#   0x{:08X}  {:<28} {}\n", c->pc, c->instr, c->evidence);
        f << key << " = [";
        for (size_t i = 0; i < v.size(); ++i)
            f << fmt::format("{}\"0x{:08X}\"", i ? ", " : "", v[i]->pc);
        f << "]\n";
    };
    emit("bias_sites", "Horizontal-margin adds (addi/addiu).");
    emit("range_sites", "X window compares paired with the bias above (sltiu).");
    emit("screen_x_sites",
         "W compares outside any screen-extent funnel — auto_screen_x cannot reach these.");
    emit("a1_sites", "Repurposable nops before a caller-supplied margin joins an X term.");
    return true;
}

bool write_ws_sites_json(const WsScanResult& scan,
                         const std::filesystem::path& path,
                         std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { error = fmt::format("cannot write {}", path.string()); return false; }

    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            o += c;
        }
        return o;
    };

    f << "{\n  \"schema\": \"psxrecomp widescreen sites v1\",\n";
    f << "  \"static_only\": true,\n";
    f << fmt::format("  \"imms_discovered\": {},\n", scan.imms_discovered ? "true" : "false");
    f << fmt::format("  \"w_canonical\": {},\n", scan.w_canonical ? "true" : "false");
    f << fmt::format("  \"h_canonical\": {},\n", scan.h_canonical ? "true" : "false");
    f << "  \"w_imms\": [";
    for (size_t i = 0; i < scan.w_imms.size(); ++i) f << (i ? "," : "") << scan.w_imms[i];
    f << "],\n  \"h_imms\": [";
    for (size_t i = 0; i < scan.h_imms.size(); ++i) f << (i ? "," : "") << scan.h_imms[i];
    f << "],\n  \"extent_funcs\": [";
    for (size_t i = 0; i < scan.extent_funcs.size(); ++i)
        f << (i ? "," : "") << scan.extent_funcs[i];
    f << "],\n  \"candidates\": [\n";
    for (size_t i = 0; i < scan.candidates.size(); ++i) {
        const auto& c = scan.candidates[i];
        f << fmt::format("    {{\"kind\": \"{}\", \"pc\": {}, \"func\": {}, "
                         "\"partner\": {}, \"confidence\": {}, \"instr\": \"{}\", "
                         "\"evidence\": \"{}\"}}{}\n",
                         c.kind, c.pc, c.func, c.partner_pc, c.confidence,
                         esc(c.instr), esc(c.evidence),
                         i + 1 < scan.candidates.size() ? "," : "");
    }
    f << "  ]\n}\n";
    return true;
}

void print_ws_report(const WsScanResult& scan, const AnalysisDb& db, int top_n) {
    fmt::print("\n=== widescreen site scan (static) ===\n");
    if (scan.w_imms.empty() || scan.h_imms.empty()) {
        fmt::print("  No screen-extent immediates found. This title may not use a\n"
                   "  GTE trivial-reject funnel, or its extents fall outside the\n"
                   "  scanned range — pass --ws-w-imms / --ws-h-imms explicitly.\n\n");
        return;
    }
    fmt::print("  screen_w_imms = [{}]   screen_h_imms = [{}]   ({})\n",
               [&] { std::string s; for (size_t i=0;i<scan.w_imms.size();++i) s += fmt::format("{}0x{:X}", i?", ":"", scan.w_imms[i]); return s; }(),
               [&] { std::string s; for (size_t i=0;i<scan.h_imms.size();++i) s += fmt::format("{}0x{:X}", i?", ":"", scan.h_imms[i]); return s; }(),
               scan.imms_discovered ? "discovered from the image" : "configured");
    fmt::print("  {} GTE function(s); {} carry the screen-extent signature and are\n"
               "  already handled by `auto_screen_x = true`.\n",
               scan.gte_funcs, scan.extent_funcs.size());
    if (scan.imms_discovered && !(scan.w_canonical && scan.h_canonical)) {
        fmt::print("  NOTE: the discovered {} do not match any console display mode,\n"
                   "  so this pair is a guess. Confirm against the game's own\n"
                   "  SetDefDispEnv/display setup before trusting the sites below,\n"
                   "  or pass --ws-w-imms / --ws-h-imms explicitly.\n",
                   !scan.w_canonical && !scan.h_canonical ? "width and height"
                   : !scan.w_canonical                    ? "widths" : "heights");
    }

    struct Row { const char* key; const char* label; };
    const Row rows[] = {
        {"bias_sites", "bias_sites   (addi/addiu horizontal margin)"},
        {"range_sites", "range_sites  (paired sltiu X window)"},
        {"screen_x_sites", "screen_x_sites (W compare auto_screen_x misses)"},
        {"a1_sites", "a1_sites     (repurposable nop before an $a1 margin)"},
    };
    for (const auto& r : rows) {
        auto v = of_kind(scan, r.key, 0);
        fmt::print("\n  {} — {} candidate(s)\n", r.label, v.size());
        int n = 0;
        for (const auto* c : v) {
            if (n++ >= top_n) { fmt::print("      … {} more\n", v.size() - top_n); break; }
            const FunctionRecord* fn = db.find(c->func);
            fmt::print("      0x{:08X}  {:>3}%  {:<26} in {}\n", c->pc, c->confidence,
                       c->instr, fn ? fn->name : "?");
            fmt::print("                      {}\n", c->evidence);
        }
    }
    fmt::print("\n");
}

} // namespace PSXRecomp::Analysis
