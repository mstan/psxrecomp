#pragma once
// widescreen_scan.h
// ----------------------------------------------------------------------------
// Static discovery of [widescreen.cull] sites.
//
// Widescreen bring-up on a new title currently means finding a handful of exact
// instruction addresses by hand — Tomba's game.toml carries four bias/range
// pairs and three a1 nops, each located by reading disassembly or mining the
// runtime's overlay capture set with tools/overlay_xref.py. Every one of those
// sites has a crisp syntactic signature, so for a main-EXE title the search is
// a static one and needs no capture set at all.
//
// The site kinds and their REQUIRED instruction forms are dictated by
// code_generator.cpp, which hard-errors on a mismatch in main-EXE mode. This
// scanner therefore proposes only sites whose instruction already satisfies the
// emitter's check: a suggestion that would break the build is not a suggestion.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "analysis_db.h"
#include "ps1_exe_parser.h"

namespace PSXRecomp::Analysis {

struct WsCandidate {
    // Matches the game.toml key it belongs under: bias_sites, range_sites,
    // a1_sites, screen_x_sites, depth_sites.
    std::string kind;
    uint32_t pc = 0;
    uint32_t func = 0;
    uint32_t partner_pc = 0;   // paired site (bias <-> range), else 0
    int      confidence = 0;   // 0..100
    std::string evidence;
    std::string instr;         // disassembled text at pc
};

struct WsScanOptions {
    // Empty means "discover them": a title's screen extents are per-game
    // (Tomba tests 0x140/0x141 on a 320 display, Ape Escape 0x181 on 368,
    // Wipeout 3 0x200/0x240). Guessing wrong makes every later pass useless,
    // so the scan derives candidates from the image before it uses them.
    std::vector<uint32_t> w_imms;
    std::vector<uint32_t> h_imms;
    int max_candidates = 512;
};

struct WsScanResult {
    std::vector<uint32_t> w_imms;          // used (configured or discovered)
    std::vector<uint32_t> h_imms;
    bool imms_discovered = false;
    // Whether the chosen sets land on real console display modes. A discovery
    // that matches neither is a guess worth saying out loud rather than
    // presenting with the same face as one that hit 320x240.
    bool w_canonical = false;
    bool h_canonical = false;

    // Functions carrying the screen-extent signature (a W compare and an H
    // compare in the same function). `auto_screen_x = true` handles these
    // without any per-address list, which is the single most useful thing to
    // know before hand-listing anything.
    std::vector<uint32_t> extent_funcs;

    std::vector<WsCandidate> candidates;
    std::map<uint32_t, uint32_t> w_hist;   // width-range slt immediate -> count
    std::map<uint32_t, uint32_t> h_hist;   // height-range slt immediate -> count
    uint32_t gte_funcs = 0;
};

WsScanResult scan_widescreen(const PS1Executable& exe,
                             const AnalysisDb& db,
                             const WsScanOptions& opts);

// Ready-to-paste [widescreen] / [widescreen.cull] TOML.
bool write_ws_sites_toml(const WsScanResult& scan,
                         const std::filesystem::path& path,
                         int min_confidence,
                         std::string& error);

bool write_ws_sites_json(const WsScanResult& scan,
                         const std::filesystem::path& path,
                         std::string& error);

void print_ws_report(const WsScanResult& scan, const AnalysisDb& db, int top_n);

} // namespace PSXRecomp::Analysis
