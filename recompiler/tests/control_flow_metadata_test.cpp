#include "control_flow.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>

using namespace PSXRecomp;
using Edges = std::vector<std::pair<uint32_t, uint32_t>>;
static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
static ControlFlowGraph graph(uint32_t count, const Edges& edges) {
    ControlFlowGraph cfg{};
    cfg.function_start = 0;
    for (uint32_t i = 0; i < count; ++i) {
        BasicBlock b{};
        b.start_addr = i; b.is_entry = i == 0;
        cfg.blocks.emplace(i, b);
    }
    for (auto [a, b] : edges) cfg.blocks.at(a).successors.push_back(b);
    return cfg;
}
static std::set<uint32_t> reach(const ControlFlowGraph& cfg,
                               std::vector<uint32_t> roots, uint32_t omit = UINT32_MAX) {
    std::set<uint32_t> seen;
    while (!roots.empty()) {
        auto node = roots.back(); roots.pop_back();
        if (node == omit || !cfg.blocks.count(node) || !seen.insert(node).second) continue;
        const auto& next = cfg.blocks.at(node).successors;
        roots.insert(roots.end(), next.begin(), next.end());
    }
    return seen;
}
// Independent oracle: deleting a dominator must disconnect the source from
// ALL entries. No RPO/immediate-dominator algorithm is shared with production.
static void verify(ControlFlowGraph cfg, std::vector<uint32_t> extra = {}) {
    auto original = cfg.blocks;
    auto roots = extra;
    roots.push_back(cfg.function_start);
    for (auto& [a, b] : cfg.blocks) if (b.is_entry) roots.push_back(a);
    auto reachable = reach(cfg, roots);
    std::set<std::pair<uint32_t, uint32_t>> expected;
    for (auto& [target, block] : cfg.blocks) {
        auto without = reach(cfg, roots, target);
        for (auto& [source, b] : cfg.blocks)
            if (reachable.count(source) && !without.count(source) &&
                std::find(b.successors.begin(), b.successors.end(), target) != b.successors.end())
                expected.emplace(target, source);
    }
    rebuild_control_flow_metadata(cfg, extra);
    check(std::set(cfg.loops.begin(), cfg.loops.end()) == expected, "back edges disagree with vertex-removal oracle");
    check(cfg.loop_count == static_cast<int>(expected.size()), "duplicate loop edges");
    for (auto& [a, b] : cfg.blocks) {
        std::set<uint32_t> incoming;
        for (auto& [p, pb] : original)
            if (std::find(pb.successors.begin(), pb.successors.end(), a) != pb.successors.end()) incoming.insert(p);
        check(b.predecessors == std::vector(incoming.begin(), incoming.end()), "reverse edges incomplete/duplicated");
        check(b.successors == original.at(a).successors, "successor order was changed");
        check(b.is_reachable == bool(reachable.count(a)), "reachability incorrect");
        bool header = false;
        for (auto edge : expected) header |= edge.first == a;
        check(b.is_loop_header == header, "loop header flag incorrect");
    }
    auto loops = cfg.loops;
    rebuild_control_flow_metadata(cfg, extra);
    check(loops == cfg.loops, "metadata rebuild not idempotent");
    // Change edges after the first pass: no predecessor/loop state may linger.
    for (auto& [a, b] : cfg.blocks) b.successors.clear();
    rebuild_control_flow_metadata(cfg, extra);
    check(cfg.loops.empty() && cfg.loop_count == 0, "stale loop metadata");
    for (auto& [a, b] : cfg.blocks) check(b.predecessors.empty(), "stale predecessor metadata");
}
static void live_analyzer() {
    constexpr uint32_t base = 0x80010000;
    PS1Executable exe{};
    exe.header.load_address = exe.header.initial_pc = base;
    exe.header.file_size = 0x28; exe.code_data.resize(0x28);
    auto word = [&](uint32_t off, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) exe.code_data[off+i] = value >> (i*8);
    };
    auto jump = [&](uint32_t off) { return 0x08000000u | ((base+off)>>2 & 0x03ffffffu); };
    word(0x00, jump(0x20)); word(0x08, 0x03e00008);
    word(0x10, 0x03e00008); word(0x18, 0x03e00008); word(0x20, jump(0x10));
    Function f{}; f.start_addr = base; f.end_addr = base+0x28; f.size = 0x28;
    auto cfg = ControlFlowAnalyzer(exe).analyze_function(f);
    check(cfg.blocks.at(base+0x20).predecessors == std::vector<uint32_t>{base}, "live path omitted predecessor");
    check(cfg.blocks.at(base+0x10).predecessors == std::vector<uint32_t>{base+0x20}, "live backward edge omitted");
    check(cfg.loops.empty(), "acyclic backward jump was called a loop");
    check(!cfg.blocks.at(base+0x18).is_reachable, "padding is not entry-reachable");
    word(0x10, jump(0x20));
    cfg = ControlFlowAnalyzer(exe).analyze_function(f);
    check(cfg.loops == Edges{{base+0x20, base+0x10}}, "forward-address natural back edge missed");
    f.alias_walk_lo = base; f.start_addr = base+0x10;
    f.alias_group_entries = {base+0x10, base+0x18};
    cfg = ControlFlowAnalyzer(exe).analyze_function(f);
    check(cfg.loops.empty(), "independent alias entry was ignored by dominance");
    check(cfg.blocks.at(base+0x18).is_reachable, "alias root not reachable");
    word(0x00, 0x10000001); // beq zero,zero,+1: taken and fallthrough both +8
    f.alias_walk_lo = 0; f.start_addr = base; f.alias_group_entries.clear();
    cfg = ControlFlowAnalyzer(exe).analyze_function(f);
    check(cfg.blocks.at(base+8).predecessors == std::vector<uint32_t>{base}, "duplicate branch edges duplicated predecessor");
}
int main() {
    try {
        live_analyzer();
        verify(graph(0, {}));
        verify(graph(1, {{0,0}, {0,0}}));
        verify(graph(4, {{0,3}, {3,1}, {1,2}})); // backward, acyclic
        verify(graph(5, {{0,1}, {1,2}, {2,3}, {3,2}, {2,4}, {4,1}})); // nested
        verify(graph(4, {{0,1}, {1,2}, {1,3}, {2,1}, {3,1}})); // two latches
        verify(graph(4, {{0,1}, {0,2}, {1,2}, {2,1}, {2,3}})); // irreducible
        verify(graph(5, {{0,1}, {2,3}, {3,2}, {3,4}})); // unreachable cycle/tail
        verify(graph(4, {{0,1}, {1,2}, {2,1}, {2,3}}), {2,2,999}); // aliases
        verify(graph(3, {{0,1}, {0,1}, {1,999}, {1,2}})); // duplicate/external
        std::mt19937 rng(0x434647u);
        for (unsigned i = 0; i < 1000; ++i) {
            unsigned n = 2 + rng()%11;
            Edges edges;
            for (unsigned a = 0; a < n; ++a)
                for (unsigned b = 0; b < n; ++b) if (rng()%9 == 0) edges.emplace_back(a,b);
            verify(graph(n, edges), i%2 ? std::vector<uint32_t>{unsigned(rng()%n)} : std::vector<uint32_t>{});
        }
        Edges chain;
        for (unsigned i = 1; i < 12000; ++i) chain.emplace_back(i-1, i);
        auto deep = graph(12000, chain);
        rebuild_control_flow_metadata(deep);
        check(deep.blocks.at(11999).is_reachable && deep.loops.empty(), "deep CFG traversal failed");
        std::cout << "PASS: live CFG, aliases, loops, 1000 oracle graphs, 12000-block chain\n";
        return 0;
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
