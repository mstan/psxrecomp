#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <set>
#include "ps1_exe_parser.h"
#include "function_analysis.h"
#include "control_flow.h"
#include "../src/annotations.hpp"

namespace PSXRecomp {

// CPU state structure (registers + memory access)
struct CPUState {
    // General purpose registers (32 total)
    uint32_t gpr[32];  // $0-$31
    uint32_t pc;       // Program counter
    uint32_t hi, lo;   // Multiply/divide results

    // Memory access functions (to be implemented in runtime)
    uint32_t (*read_word)(uint32_t addr);
    void (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void (*write_half)(uint32_t addr, uint16_t value);
    uint8_t (*read_byte)(uint32_t addr);
    void (*write_byte)(uint32_t addr, uint8_t value);
};

// Code generation configuration
struct CodeGenConfig {
    bool emit_comments;           // Include MIPS instruction comments
    bool emit_line_numbers;       // Include original address as comments
    bool optimize_zero_reg;       // Optimize away $zero assignments
    bool use_switch_for_blocks;   // Use switch instead of goto labels
    bool split_mid_function_targets; // Split branch targets into funcs for legacy main-EXE analysis
    // True when compiling an overlay image (psxrecomp-game --overlay). Overlay
    // regions hold different scene VARIANTS at the same addresses (the same addr
    // is backdrop code in one scene, unrelated code/data in another), so an
    // explicit per-address widescreen site (x_sites / cull_* sites) authored for
    // one variant won't match the bytes in another. In overlay mode such a shape
    // mismatch is EXPECTED (apply the transform only where the bytes match, skip
    // it elsewhere) rather than a hard config error.
    bool overlay_mode = false;
    std::string indent;           // Indentation string (default: "    ")
    // Widescreen sprite-tag hooks ([widescreen] sprite_tag_funcs): functions
    // that get a psx_ws_sprite_tag(cpu) callback emitted at entry, so the
    // runtime can record the per-prim pointer ($a0) + projected anchor for
    // proportion correction at GP0 submission. Empty = no hooks (default).
    std::set<uint32_t> ws_sprite_tag_funcs;

    // MMX6-class 2D tile-ring stage initializer. Emit a reveal-invalidation
    // callback at entry so host-only wide pixels cannot survive a stage swap.
    uint32_t ws_bg2d_init_func = 0;

    // Widescreen far-backdrop un-squash ([widescreen] backdrop_unsquash_funcs):
    // functions whose body is bracketed by gte_ws_set_suppress(1)/(0) so the
    // GTE X-squash is OFF for their (far-backdrop) draws — the backdrop fills
    // the stretched 16:9 frame instead of leaving edge void (8C). Suppress is
    // set at entry and cleared before every jr-$ra return. Empty = none.
    std::set<uint32_t> ws_backdrop_unsquash_funcs;

    // Widescreen cull-margin widening ([widescreen] cull_* sites). At these
    // addresses the immediate is emitted with a runtime margin term
    // psx_ws_x_margin() so the world-space draw cull widens with the aspect
    // (0 at 4:3). bias = addiu (+margin); range = sltiu (+2*margin); a1 = a
    // nop replaced with `a1 += margin`, or move rD,a1 replaced with
    // `rD = a1 + margin`. See config_loader.h. Empty = default.
    std::set<uint32_t> ws_cull_bias_sites;
    std::set<uint32_t> ws_cull_range_sites;
    std::set<uint32_t> ws_cull_a1_sites;
    std::set<uint32_t> ws_cull_screen_x_sites;

    // Explicit signed right-edge widen sites ([widescreen.cull] slti_sites):
    // `slti rt, sx, W` emitted through psx_ws_cull_slti for funnel functions
    // the auto-detector cannot qualify (X-only test, no height compare).
    std::set<uint32_t> ws_cull_slti_sites;

    // Screen-extent signature immediates ([widescreen.cull] screen_w_imms /
    // screen_h_imms) — per-game display-width-derived bounds. Defaults are the
    // Tomba signature; Ape Escape uses 0x181 (+ 0xF1 height).
    std::vector<uint32_t> ws_cull_w_imms = { 0x140, 0x141 };
    std::vector<uint32_t> ws_cull_h_imms = { 0xE0, 0xF1 };

    // Widescreen automatic horizontal-FOV cull widening ([widescreen.cull]
    // auto_screen_x). When true, any function carrying the GTE screen-extent
    // reject signature — at least one `sltiu …,0x140/0x141` (right/width) AND
    // one `sltiu …,0xE0/0xF1` (bottom/height) — has every width compare emitted
    // with + 2*psx_ws_x_margin() (0 at 4:3). This routes the whole render-funnel
    // (≈100 sites in Tomba) through one helper with no per-address list, and is
    // generic to any PSX GTE title. The vertical (0xE0) bound is never touched.
    bool ws_auto_screen_x_cull = false;

    // Widescreen automatic far-backdrop column PRELOAD ([widescreen.cull]
    // auto_backdrop). When true, every scrolling-backdrop column-window generator
    // is auto-detected by its invariant (see ws_backdrop_detect.h) and its window
    // START/END finalize instructions are emitted through psx_ws_backdrop_value()
    // so the whole finite tile row preloads in 16:9 (0 at 4:3 => byte-identical).
    // Generators are overlay-resident, so this must reach the overlay compile.
    bool ws_auto_backdrop_preload = false;

    // Widescreen backdrop screenX squash ([widescreen.backdrop] x_sites). Each
    // address is a `sh rt,off(base)` storing a parallax 2D backdrop layer's
    // final screen-X; emitted as write_half(base+off, psx_ws_backdrop_x(rt))
    // so the un-GTE'd backdrop is squashed around screen centre for the 16:9
    // FOV (identity at 4:3). Overlay-resident; see config_loader.h. Empty=default.
    std::set<uint32_t> ws_backdrop_x_sites;

    // Widescreen pure-2D background tile-loop widen ([widescreen.bg2d]). Three
    // instruction addresses in a 2D game's per-layer BG renderer (MMX6's
    // FUN_800270d0): the column-count load/compare, the start tile-column mask,
    // and the start screen-x calculation (MMX6: li count + sra start; MMX4/MMX5:
    // inline slti[u] count, sra or subu-zero start), rewritten through the gpu.c
    // psx_ws_bg2d_* helpers so the loop draws the 16:9 reveal columns on both
    // sides (identity at 4:3 / 512 hi-res). 0 = unset. Main-EXE addresses;
    // verified by opcode at gen time.
    uint32_t ws_bg2d_count_site    = 0;
    uint32_t ws_bg2d_startcol_site = 0;
    uint32_t ws_bg2d_startx_site   = 0;
    uint32_t ws_bg2d_stream_left_site  = 0;  // tile-ring streamer left leading-edge addiu
    uint32_t ws_bg2d_stream_right_site = 0;  // tile-ring streamer right leading-edge addiu
    uint32_t ws_bg2d_bufbase_site = 0;       // driver addu: BG packet-buffer address (relocate)
    uint32_t ws_bg2d_cap_site     = 0;       // renderer slti: per-frame BG tile cap (raise)

    // Persistent game-option init-store hooks ([persist_options] in
    // game_options.toml). Each entry is the PC of a boot-init sb/sh that writes a
    // config global's DEFAULT value; the store is rewritten to route the value
    // through psx_game_option_store(addr,val), so a value persisted from a prior
    // session overrides the default at initialization (issue #5). Identity when
    // nothing is persisted, so a fresh install is byte-identical. Empty=default.
    std::set<uint32_t> persist_init_store_sites;

    CodeGenConfig()
        : emit_comments(true)
        , emit_line_numbers(true)
        , optimize_zero_reg(true)
        , use_switch_for_blocks(false)
        , split_mid_function_targets(true)
        , indent("    ") {}
};

// Result of code generation for a function
struct GeneratedFunction {
    std::string function_name;    // e.g., "func_80016940"
    std::string signature;        // e.g., "void func_80016940(CPUState* cpu)"
    std::string body;             // C code body
    std::string full_code;        // signature + body
    int line_count;               // Number of lines generated
};

class CodeGenerator {
public:
    explicit CodeGenerator(const PS1Executable& exe, const CodeGenConfig& config = CodeGenConfig());

    // Generate C code for a single function.
    // fallthrough_name: if non-empty and the last block has no explicit control
    // flow (ControlFlowType::None), emit a tail call to that function before '}'.
    // This handles MIPS functions that fall through into the next function without
    // an explicit jr/j instruction.
    GeneratedFunction generate_function(
        const Function& func,
        const ControlFlowGraph& cfg,
        const std::string& fallthrough_name = "");

    // Generate C code for all functions
    std::vector<GeneratedFunction> generate_all_functions(
        const std::vector<Function>& functions,
        const std::map<uint32_t, ControlFlowGraph>& cfgs);

    // Generate a group of overlapping-alias entries that share one host
    // function: ONE static body holding the host-range blocks (restricted to
    // the union of blocks reachable from the entries) with an entry switch,
    // plus a dispatchable wrapper per entry. Returns one GeneratedFunction per
    // entry; the first carries the shared body.
    std::vector<GeneratedFunction> generate_alias_group(
        const std::vector<const Function*>& aliases,
        const ControlFlowGraph& cfg,
        const std::string& fallthrough_name);

    // Generate complete C file with all functions
    std::string generate_file(
        const std::vector<Function>& functions,
        const std::map<uint32_t, ControlFlowGraph>& cfgs);

    // Generate the per-function code-range manifest consumed by the overlay
    // loader's per-entry validity hash (design §8). For each function, emits its
    // compiled code byte-ranges (union of basic-block extents), coalesced;
    // interleaved jump tables / constant pools fall in the gaps and are
    // excluded — which is what makes the runtime hash stable across reloads.
    //   F <entry_hex>          one per function (virtual entry address)
    //   R <lo_hex> <len_hex>   one per coalesced code range (virtual addr)
    std::string generate_ranges_manifest(
        const std::vector<Function>& functions,
        const std::map<uint32_t, ControlFlowGraph>& cfgs);

    // Exact manifest for the final function/CFG set emitted by the most recent
    // generate_file() call, including functions synthesized by its split pass.
    const std::string& last_ranges_manifest() const { return last_ranges_manifest_; }

    // Set known functions for this compilation unit (for linking)
    void set_known_functions(const std::set<uint32_t>& functions) {
        known_functions_ = functions;
    }

    // Set annotation table (optional — no-op if not called)
    void set_annotations(const AnnotationTable* at) { annotations_ = at; }

private:
    const PS1Executable& exe_;
    CodeGenConfig config_;
    std::set<uint32_t> known_functions_;  // Addresses of functions in this compilation unit
    std::string last_ranges_manifest_;
    std::set<uint32_t> extra_labels_;    // Mid-block addresses that need inline labels (jump table targets)
    const AnnotationTable* annotations_ = nullptr;

    // RECURSION_BUG.md §25 — continuation-passing call/return (the universal fix
    // for the idle-freeze host-stack leak). When set (gen-time env PSX_CPS),
    // guest calls (jal/jalr/jr-table/external) are emitted as TAIL-TRANSFERS
    // (set $ra, cpu->pc = target, return) and each call's return point is
    // registered as a dispatchable continuation; jr $ra publishes cpu->pc.
    // The unified flat trampoline (psx_dispatch_impl) drives the whole
    // call/return via the guest's own $ra/$sp — so the host stack can never
    // mirror unbounded guest re-entrancy. The §8 leaking chain (func_8001A954
    // ...) lives in GAME code, so the game emitter MUST honor CPS too.
    bool cps_enabled_ = false;
    // Continuations (return points) collected during the CURRENT function's
    // block translation; consumed by generate_function to emit the entry-switch.
    std::vector<uint32_t> cps_cur_continuations_;
    // Global map: continuation address -> owning function entry, for the game
    // dispatch table (psx_dispatch_game_compiled) to route a returned-to
    // continuation into its function's entry-switch.
    std::map<uint32_t, uint32_t> cps_continuation_owner_;

    // Cycle accounting for legal entries that land inside an already-emitted
    // block instead of at its leader. Block leaders charge the whole block; an
    // entry-switch or jump-table target that bypasses the leader must charge the
    // remaining instructions from the interior label to the end of that block.
    uint32_t partial_block_cycle_count(uint32_t addr, const ControlFlowGraph& cfg) const;
    std::string emit_mid_block_cycle_charge(uint32_t addr, const ControlFlowGraph& cfg,
                                            const std::string& indent) const;
    std::string emit_interrupt_check(uint32_t resume_pc, const std::string& indent) const;
    std::string emit_interrupt_check_expr(const std::string& resume_pc_expr,
                                          const std::string& indent) const;

public:
    bool cps_enabled() const { return cps_enabled_; }
    // continuation_addr -> owning function entry (CPS dispatch routing).
    const std::map<uint32_t, uint32_t>& cps_continuations() const {
        return cps_continuation_owner_;
    }
private:

    // Set per-function by generate_function when config_.ws_auto_screen_x_cull is
    // on AND the function carries the GTE screen-extent reject signature; read by
    // translate_instruction to widen that function's width compares. See
    // func_has_screen_extent_cull().
    bool ws_auto_cull_func_ = false;
    // LEFT-edge funnel bltz addresses in the current function (auto_screen_x,
    // signed min/max + center±halfwidth idioms — ws_cull_detect.h). Populated
    // per function alongside ws_auto_cull_func_; generate_branch_condition
    // emits a listed bltz through psx_ws_cull_bltz. Cleared per function.
    std::set<uint32_t> ws_cull_bltz_pcs_;

    // True iff the function's instruction stream contains both a screen-width
    // reject (`sltiu …,0x140` or `…,0x141`) and a screen-height reject
    // (`sltiu …,0xE0` or `…,0xF1`) — the uniform GTE per-vertex trivial-reject.
    bool func_has_screen_extent_cull(const ControlFlowGraph& cfg) const;

    // Backdrop-preload rewrite sites for the CURRENT function ([widescreen.cull]
    // auto_backdrop). Maps each detected window-bound instruction address to
    // {kind (WS_BD_START / WS_BD_END), window_cols (|addiu offset|, the widen
    // margin scale)}. Repopulated per function by detect_backdrop_windows()
    // (gated on config_.ws_auto_backdrop_preload) and consulted by
    // translate_instruction; cleared every function so it never leaks.
    std::map<uint32_t, std::pair<int,int>> ws_backdrop_sites_;

    // Populate ws_backdrop_sites_ from the shared psx_ws_find_backdrop_windows()
    // detector over this function's instruction words; returns the window count.
    int detect_backdrop_windows(const ControlFlowGraph& cfg);
    // Populate ws_cull_bltz_pcs_ (LEFT-edge funnel bltz addresses) for the
    // current function; no-op unless ws_auto_cull_func_ is set.
    int detect_cull_bltz_sites(const ControlFlowGraph& cfg);

    // Instruction translation
    std::string translate_instruction(uint32_t addr, uint32_t instr);

    // Register name mapping
    static std::string reg_name(int reg_num);

    // Basic block translation
    std::string translate_basic_block(
        const BasicBlock& block,
        const ControlFlowGraph& cfg);

    // Detect jr jump tables across all blocks of a CFG. Fills extra_labels_
    // (mid-block table targets needing inline labels) and out_edges
    // (jr block -> in-range rom targets), used for alias reachability.
    void scan_jr_tables(
        const ControlFlowGraph& cfg,
        std::map<uint32_t, std::vector<uint32_t>>& out_edges);

    // Control flow translation
    std::string generate_branch_condition(uint32_t instr, uint32_t addr);
    std::string translate_branch(const ControlFlowInstr& cf, uint32_t fall_through);
    std::string translate_jump(const ControlFlowInstr& cf);

    // Arithmetic instructions
    std::string translate_add(uint32_t instr);
    std::string translate_addu(uint32_t instr);
    std::string translate_sub(uint32_t instr);
    std::string translate_subu(uint32_t instr);
    std::string translate_mult(uint32_t instr);
    std::string translate_multu(uint32_t instr);
    std::string translate_div(uint32_t instr);
    std::string translate_divu(uint32_t instr);
    std::string translate_mfhi(uint32_t instr);
    std::string translate_mflo(uint32_t instr);
    std::string translate_mthi(uint32_t instr);
    std::string translate_mtlo(uint32_t instr);

    // Logical instructions
    std::string translate_and(uint32_t instr);
    std::string translate_or(uint32_t instr);
    std::string translate_xor(uint32_t instr);
    std::string translate_nor(uint32_t instr);

    // Shift instructions
    std::string translate_sll(uint32_t instr);
    std::string translate_srl(uint32_t instr);
    std::string translate_sra(uint32_t instr);
    std::string translate_sllv(uint32_t instr);
    std::string translate_srlv(uint32_t instr);
    std::string translate_srav(uint32_t instr);

    // Load/store instructions
    std::string translate_lw(uint32_t instr);
    std::string translate_sw(uint32_t instr);
    std::string translate_lb(uint32_t instr);
    std::string translate_lbu(uint32_t instr);
    std::string translate_lh(uint32_t instr);
    std::string translate_lhu(uint32_t instr);
    std::string translate_lwl(uint32_t instr);
    std::string translate_lwr(uint32_t instr);
    std::string translate_swl(uint32_t instr);
    std::string translate_swr(uint32_t instr);
    std::string translate_sb(uint32_t instr);
    std::string translate_sh(uint32_t instr);

    // Immediate instructions
    std::string translate_addiu(uint32_t instr);
    std::string translate_andi(uint32_t instr);
    std::string translate_ori(uint32_t instr);
    std::string translate_xori(uint32_t instr);
    std::string translate_lui(uint32_t instr);

    // Set instructions
    std::string translate_slt(uint32_t instr);
    std::string translate_sltu(uint32_t instr);
    std::string translate_slti(uint32_t instr);
    std::string translate_sltiu(uint32_t instr);

    // Helper: Extract register fields
    static uint32_t get_rs(uint32_t instr) { return (instr >> 21) & 0x1F; }
    static uint32_t get_rt(uint32_t instr) { return (instr >> 16) & 0x1F; }
    static uint32_t get_rd(uint32_t instr) { return (instr >> 11) & 0x1F; }
    static uint32_t get_shamt(uint32_t instr) { return (instr >> 6) & 0x1F; }
    static uint32_t get_funct(uint32_t instr) { return instr & 0x3F; }
    static int16_t get_imm16(uint32_t instr) { return (int16_t)(instr & 0xFFFF); }
    static uint16_t get_imm16_u(uint32_t instr) { return (uint16_t)(instr & 0xFFFF); }
};

} // namespace PSXRecomp
