// config_loader.h — shared TOML config loader for psxrecomp-{bios,game}.
//
// Mirrors the schema accepted by tools/audit_config.py (Python side). See
// docs/config_schema.md for the field reference.
//
// Two entry points:
//   load_bios_config(path)  reads bios/SCPH1001.toml — describes the BIOS
//   load_game_config(path)  reads <game>/game.toml   — describes a game EXE
//
// A runtime/process that needs both calls both; the BIOS one is the
// always-loaded base, the game one is layered on top (merge semantics are
// the caller's responsibility for now — recompiler tools consume one or
// the other independently).
//
// Paths inside the TOML are resolved relative to the detected project
// root (the nearest ancestor of the config file that has .gitignore,
// .git, or CMakeLists.txt).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PSXRecompV4 {

// Pad input mode (per player). Replaces the old analog on/off boolean.
//   hybrid  — auto-switch DualShock(analog)/digital per the most-recent input:
//             nudge the stick -> report DualShock (0x73, variable sticks);
//             press the D-pad -> report a digital pad (0x41) so the game runs
//             its OWN d-pad path at true digital sensitivity. Mirrors a
//             DualShock's analog LED toggling on/off (and Tomba Special
//             Edition's auto-detect). Default.
//   analog  — always present a DualShock/analog pad (id 0x73). The D-pad is
//             folded onto the stick at full deflection so it still moves you.
//   digital — always present a digital pad (id 0x41); sticks disabled.
enum PadMode { PAD_MODE_HYBRID = 0, PAD_MODE_ANALOG = 1, PAD_MODE_DIGITAL = 2 };

// Parse/format a pad mode. pad_mode_from_string accepts "hybrid"/"analog"/
// "digital" (case-insensitive) and returns `fallback` for anything else.
int         pad_mode_from_string(const std::string& s, int fallback);
const char* pad_mode_to_string(int mode);

// [runtime] block — consumed by runtime/src/main.cpp. All fields optional;
// callers that need them check has_* flags or use the supplied defaults.
struct RuntimeConfig {
    bool                  has_debug_port = false;
    uint16_t              debug_port     = 0;

    // Localization / on-the-fly string translation (docs/STRING_TRANSLATION.md).
    // language selects the translations/*.toml column; "jp"/"off"/"" disables
    // APPLY (capture still runs). From [localization].language or [runtime].
    // language; env PSX_LANG overrides at runtime. Also the launcher default.
    std::string           language = "en";

    // Optional launcher-facing language menu. When a game declares
    // [localization].languages, the launcher shows a "Localization" dropdown of
    // these {code,label} options (code feeds `language`; "off"/"jp"/"" = the
    // untranslated native game). Empty => no dropdown (the general default).
    struct LanguageOption { std::string code; std::string label; };
    std::vector<LanguageOption> languages;

    bool                  has_window_title = false;
    std::string           window_title;

    // controller: "digital" (default) | "dualshock"
    bool                  has_controller = false;
    std::string           controller;

    bool                  has_memcard_dir = false;
    std::filesystem::path memcard_dir;     // absolute path (resolved against project root)

    // disc_speed: "1x" (default) | "2x" | "4x" | "instant"
    // Controls how quickly CD-ROM timing delays fire. "instant" collapses all
    // seek/read delays to 1 cycle — correct INT sequence, no artificial wait.
    bool                  has_disc_speed = false;
    std::string           disc_speed;      // raw string; main.cpp converts to divisor

    // instant_max_per_frame: per-frame sector-IRQ budget while disc_speed =
    // "instant" (cdrom.c floors the per-sector period to VBLANK/N). Absent =
    // cdrom.c built-in default. Runtime-tunable via the cdrom_instant_rate
    // TCP command; the turbo-through-loads predicate drives the same knob.
    bool                  has_instant_max_per_frame = false;
    int                   instant_max_per_frame = 0;

    // fast_boot: DEPRECATED alias for the HLE boot shell-skip (see bios_hle
    // below). The old mechanism (snapshot BIOS state at first handoff, restore
    // on later launches) is gone; fast_boot=true now skips only the BIOS shell
    // (the boot animation) via the HLE tier's one-shot shell intercept, with
    // kernel init + game EXE load still executed by the real recompiled BIOS
    // at host speed. Kept so existing game.toml/settings.toml keep working.
    bool                  fast_boot = false;

    // bios_hle: High-Level Emulation tier for BIOS kernel services
    // (CLAUDE.md §0 amendment 2026-07-02, the gbarecomp model). DEFAULT ON as of
    // 2026-07-06 (user-directed player default: instant boot-skip for every
    // game). Opt OUT with [runtime] bios_hle = false or env PSX_BIOS_HLE=0 to run
    // pure LLE (the recompiled BIOS), which REMAINS the reference implementation
    // and the oracle — this only flips the default, LLE is still fully linked and
    // selectable. When on, implemented kernel services are computed in-runtime
    // against the real guest kernel structures and every other call falls through
    // to LLE. Implies the HLE boot shell-skip unless bios_hle_keep_intro.
    // PSX_BIOS_HLE / PSX_BIOS_HLE_KEEP_INTRO env override at launch.
    // Runtime: runtime/src/bios_hle.c.
    bool                  bios_hle = true;
    bool                  bios_hle_keep_intro = false;

    // hle_scheduler: the HLE tier's standing SUBSYSTEM REPLACEMENT for guest
    // thread switching (deterministic TCB scheduler vs the legacy
    // non-deterministic host-fiber bridge). Default ON under BOTH BIOS
    // backends (CLAUDE.md §0 amendment 2026-06-29 carve-out); PSX_HLE_SCHEDULER
    // env wins over this key. Runtime: traps.c psx_hle_scheduler_enabled().
    bool                  hle_scheduler = true;

    // overlay_cache: enable the overlay DLL cache + capture (Layer A). Off by
    // default. When true the runtime scans cache/<game_id>/ for precompiled
    // overlay DLLs (loaded ahead of the dirty-RAM interpreter) and records
    // overlay bytes to overlay_captures.json for offline compilation.
    bool                  overlay_cache = false;

    // turbo_loads: OPT-IN per game. While the game is loading (CD data
    // stream active, XA/FMV excluded, post-BIOS-handoff only) the frontend
    // skips wall-clock pacing so the guest runs at host speed — compressing
    // load wall-time. Streaming titles (e.g. Crash) must leave this off.
    bool                  turbo_loads = false;

    // overlay_autocompile_cmd: variant-capture automation (step 2.8). A
    // shell command (run via cmd.exe /C, cwd = project root) that compiles
    // overlay_captures.json into the cache — normally the project's
    // compile_overlays.py invocation. When set (and overlay_cache is on),
    // the runtime auto-captures on sustained capture-window interp pressure
    // and spawns this command in the background; on success the loader
    // rescans the cache and the new variant goes native in-session.
    bool                  has_overlay_autocompile_cmd = false;
    std::string           overlay_autocompile_cmd;

    // overlay_autocompile_cmd_tcc: the SAME pipeline as overlay_autocompile_cmd
    // but invoking the bundled, toolchain-free TinyCC compiler instead of gcc.
    // The runtime spawns THIS command instead of the gcc one when the resolved
    // backend is tcc (a machine without a gcc toolchain, or overlay_backend =
    // "tcc"/"auto-no-gcc"). Lets gcc stay the dev/production path while tcc is
    // the user fallback. When unset, a tcc-resolved backend has no compiler and
    // overlay gaps fall to the interpreter.
    bool                  has_overlay_autocompile_cmd_tcc = false;
    std::string           overlay_autocompile_cmd_tcc;

    // overlay_backend: overlay tier-selection (overlay_backend.h).
    // "auto" (default, empty == auto) | "gcc" | "tcc" | "auto-no-gcc" | "sljit"
    // (deprecated). auto resolves to gcc when a gcc TOOLCHAIN is present (a dev /
    // production box), else tcc (bundled, toolchain-free). "auto-no-gcc" forces
    // the tcc branch even with gcc present (simulate a toolchain-less user box;
    // gcc shards still load). The env var PSX_OVERLAY_BACKEND overrides at runtime.
    std::string           overlay_backend;

    // overlay_native_block: per-game overlay function entries that must stay on
    // the dirty-RAM interpreter even when a matching native DLL exists. Intended
    // for small timing-sensitive setup/task routines while the rest of the
    // overlay runs native.
    std::vector<uint32_t> overlay_native_block;

    // ---- [video] block — visual enhancement options ----
    // supersampling: internal-resolution SSAA factor (per axis). 1 = native
    // (default, behaves exactly as before). 2..4 render geometry/shading into
    // an N*-scaled mirror of VRAM and downsample on present — true ordered-grid
    // supersampling + edge anti-aliasing. Cost scales ~N^2 in fill rate.
    int                   video_supersampling = 1;

    // antialiasing: when true the present path uses linear filtering when
    // scaling the framebuffer to the window (smooths the supersample
    // downscale and any window resize). false = nearest (sharp pixels).
    // Defaults to true.
    bool                  video_antialiasing = true;

    // texture_filtering: "nearest" (default, native PSX look) | "bilinear"
    // (smooths textures and 2D backgrounds). Stored as 0/1.
    int                   video_texture_filter = 0;

    // renderer: "software" (default) | "opengl". Selects the rasterizer/present
    // backend. The OpenGL backend is a hardware-accelerated alternative; the
    // software rasterizer remains the fallback. Stored as 0=software, 1=opengl.
    int                   video_renderer = 0;

    // low_latency_input: re-sample the pad after the wall-clock pacer (just
    // before present) so the next CPU frame reads near-fresh input instead of
    // input ~one frame stale. Default on. vsync: present/swap mode —
    // 1=on (tear-free, default), 0=immediate (lowest display latency, may
    // tear), -1=adaptive. The wall-clock pacer holds 59.94Hz regardless.
    bool                  video_low_latency_input = true;
    int                   video_vsync             = 1;

    // crt_filter: present-time screen-colour model (verified-enhancement LUT).
    // "raw" (default, byte-identical 5->8 passthrough) | "crt" | "composite" |
    // "trinitron". Stored 0..3 to match ScreenKind in runtime/color_lut.h. The
    // PSX_SCREEN env var overrides this at runtime (debug path).
    int                   video_screen_kind = 0;

    // auto_skip_fmv: when true, full-motion videos (streaming XA audio + MDEC
    // video) are skipped the instant they're detected — presentation + pacing are
    // suppressed and audio muted for the duration, so an FMV ends in a fraction of
    // a second with nothing shown, landing on the next screen with side effects
    // intact. The skip is driven the GAME's own way (see fmv_skip_* below); with
    // no per-game config it falls back to holding the skip button.
    bool                  video_auto_skip_fmv = false;

    // fmv_skip_*: per-game FMV instant-skip via the game's own end-of-movie path.
    // Some players (Tomba) end a movie when the streamed frame number reaches that
    // movie's per-movie frame total minus a small offset. When auto_skip_fmv is on
    // and fmv_skip_total_table is set, the runtime writes the CURRENT movie's total
    // (at fmv_skip_total_table + movie_id*2, a u16 table indexed by the movie-id
    // byte at fmv_skip_movie_id) down to fmv_skip_end_total, so the player tears the
    // movie down on its next frame — a natural end that reaches EVERY movie (incl.
    // ones whose caller never polls the skip button). Only the active movie's entry
    // is touched. Addresses are game-specific (RE of the player loop); leave the
    // table 0 to fall back to button injection. Tomba: table 0x80077728,
    // movie_id 0x1F8001CD, end_total 3 (the player's "total - 3" offset).
    uint32_t              video_fmv_skip_total_table = 0;
    uint32_t              video_fmv_skip_movie_id    = 0;
    int                   video_fmv_skip_end_total   = 0;  // 0 => runtime default (3)

    // fmv_skip_no_xa: broaden FMV detection to MDEC-decode activity ALONE (no
    // XA-stream requirement). Some movies are silent and fully preloaded into
    // RAM (Tomba2's Whoopee Camp logo: no CD sectors, no XA during playback),
    // so the default MDEC+XA detector never fires and neither skip mechanism is
    // even attempted. With this on, such movies enter the same skip path:
    // pacing/presents suppressed (fast-forward at host speed), audio muted,
    // skip button held. The movie still executes every frame — presentation-
    // side only, guest timeline untouched. Per-game opt-in because MDEC use
    // outside movies (loading-screen stills) would briefly trigger it.
    bool                  video_fmv_skip_no_xa = false;
    // fmv_skip_no_xa_hold: presentation-side fast-forward latch, in guest
    // vblanks, after the most recent silent MDEC decode. Silent preloaded
    // logos can retain an authored post-decode wait; a title may opt into a
    // longer latch so auto-skip covers that wait too. Default preserves the
    // generic four-frame inter-decode hysteresis.
    int                   video_fmv_skip_no_xa_hold = 4;

    // aspect_ratio: display aspect "W:H" (default "4:3" = native). A wider
    // aspect (e.g. "16:9") enables the widescreen hack: the GTE squashes
    // screen-X by (4*H)/(3*W) around the game's projection centre and the
    // present path stretches the 4:3 frame to W:H — net effect is a wider
    // field of view for GTE-projected geometry. Screen-space 2D (HUD, FMV,
    // sprite widths) stretches; world geometry keeps correct proportions.
    int                   video_aspect_num = 4;
    int                   video_aspect_den = 3;

    // ---- [audio] block ----
    // spu_hq: enable the SPU float-shadow re-render (Catmull-Rom resample, float
    // headroom). Verified-enhancement, default OFF — spu_render output is
    // byte-identical to the canon hardware mix when off. The PSX_AUDIO_SHADOW
    // env var overrides this at runtime (debug path).
    bool                  audio_spu_hq = false;

    // ---- [controller] block — game-declared input defaults ----
    // default_mode: the pad input mode this game ships with (see PadMode):
    // "hybrid" (default) auto-switches DualShock/digital from the player's
    // input, "analog" pins DualShock (0x73), "digital" pins a digital pad
    // (0x41). A stick-capable title (e.g. Tomba) ships "hybrid" so the stick
    // gives variable run speed yet the D-pad keeps its classic digital feel,
    // with no launcher toggling. Per-install settings.toml [controller]
    // p1_mode/p2_mode still override. `default_mode` sets both ports;
    // `p1_mode`/`p2_mode` set one. Legacy `default_analog`/`p1_analog`/
    // `p2_analog` booleans are still accepted (true->analog, false->digital).
    bool                  has_default_mode = false;
    int                   default_p1_mode  = PAD_MODE_HYBRID;
    int                   default_p2_mode  = PAD_MODE_HYBRID;

    // allow_hybrid: whether the launcher offers the "Hybrid" pad mode at all.
    // Default true (Hybrid | Analog | D-Pad). A game that needs an explicit
    // analog/digital choice (e.g. one that hard-requires a DualShock) can set
    // [controller] allow_hybrid = false to drop Hybrid from the selector.
    bool                  controller_allow_hybrid = true;

    // lock_mode: when true the launcher HIDES the whole pad-mode selector
    // (Hybrid | Analog | D-Pad) and forces every port to default_p1_mode. For a
    // game that supports exactly one pad type — e.g. Tomba 2, whose driver only
    // works as a plain digital pad because the DualShock config-mode handshake
    // is unhandled — so the player can't pick a broken mode. Supersedes
    // allow_hybrid (which only hides the Hybrid segment). Default false.
    bool                  controller_lock_mode = false;

    // lock_device: when true the launcher HIDES the Player 1/2 controller cards
    // entirely (no device picker, no config) — the game's controller type is
    // fixed and auto-bound. For a title that presents exactly one hardcoded pad
    // type (e.g. Ape Escape = DualShock analog) where exposing a device/mode
    // choice is pointless. Distinct from lock_mode (which only hides the pad-mode
    // segment but keeps the device dropdown). Default false.
    bool                  controller_lock_device = false;

    // deadzone: default analog-stick deadzone in raw SDL axis units (0..32767).
    // Applied both to the stick->d-pad press threshold and the analog-axis centre
    // dead-band. Absent => runtime default (12000). Overridden per-install by
    // settings.toml [controller] deadzone and by input.ini.
    bool                  has_deadzone = false;
    int                   deadzone     = 0;

    // legacy_pad_config: per-game pad-protocol compatibility opt-in. false (default)
    // = the modern DualShock config state machine (proper 0x43 enter/exit, config id
    // 0xF3 only while in config) — required by MMX6 and the correct default for every
    // title. true = the pre-98aa688 behaviour (config commands always answer 0xF3, no
    // enter/exit tracking). Only Tomba opts in: its libpad re-detect — triggered by the
    // launcher Hybrid mode's analog<->digital type flip — manufactures a 1-frame "pad
    // unplugged" under the modern SM (menu unpause / phantom input). The legacy answers
    // make that re-detect benign. Scoped per-game; no other title's behaviour changes.
    // Wired to sio_set_legacy_cfg(); see sio.c g_pad_legacy_cfg.
    bool                  legacy_pad_config = false;
};

// One entry from [[recompiler.bios_vectors]].
// Describes a BIOS vector dispatch stub (A0/B0/C0) that the BIOS installs
// into low RAM at boot. The recompiler reads the function pointer table from
// the ROM binary at build time and emits a static C switch handler so these
// addresses are resolved as binary-search hits at runtime rather than falling
// through to dirty_ram_interp.
struct BiosVectorTable {
    uint32_t ram_addr;       // RAM address of the installed stub (e.g. 0xA0)
    int      index_reg;      // CPU register that holds the function index ($t1 = 9)
    uint32_t table_rom_addr; // ROM virtual address of the function pointer table
    uint32_t table_count;    // number of entries to read from the table
    // Runtime RAM address of the live function table (used as fallback for
    // Shell-patched entries not present in ROM). 0 = no runtime fallback.
    uint32_t table_ram_addr;
};

// One entry from [[recompiler.bios_aliases]].
// A RAM address the BIOS installs a simple fixed-target trampoline at
// (e.g. the SIO handler at 0x0CF0 which just jalrs to 0x641C). Emitted as
// a one-liner wrapper in the dispatch table — no table lookup, no switch.
struct BiosAlias {
    uint32_t ram_addr;    // the installed stub address (e.g. 0x0CF0)
    uint32_t target_key;  // normalized dispatch key of the target (e.g. 0x641C)
};

struct BiosConfig {
    std::filesystem::path config_path;   // the toml file itself
    std::filesystem::path project_root;  // resolved via .gitignore/.git/CMakeLists.txt walk

    // [program] block
    std::string           name;          // display name, e.g. "SCPH1001 BIOS"
    std::string           id;            // canonical id, e.g. "SCPH-1001"
    std::filesystem::path rom_path;      // absolute path to BIOS ROM
    uint32_t              load_address;
    uint32_t              entry_pc;
    uint32_t              text_size;

    // [recompiler] block
    std::filesystem::path seeds_path;    // absolute path to seeds JSON
    std::filesystem::path out_dir;       // absolute path to output dir
    bool                  strict;        // currently always true
    std::string           out_stem;      // derived if not explicit
    std::vector<BiosVectorTable> bios_vectors; // optional vector dispatch tables
    std::vector<BiosAlias>       bios_aliases; // optional fixed-target trampolines

    // [runtime] block (optional)
    RuntimeConfig         runtime;
};

struct GameConfig {
    std::filesystem::path config_path;
    std::filesystem::path project_root;

    // [game] block
    std::string           name;          // e.g. "Tomba!"
    std::string           id;            // e.g. "SCUS-94236"
    std::filesystem::path exe_path;      // absolute path to PS-X EXE
    uint32_t              load_address;
    uint32_t              entry_pc;
    uint32_t              text_size;
    uint32_t              stack_base;    // initial $sp
    // disc paths (Phase D will properly support multi-disc; for now we
    // accept either a single `disc = "..."` or `discs = [...]` and store
    // the union here).
    std::vector<std::filesystem::path> discs;

    // Optional expected disc identity, for the launcher's "Disc verified" badge.
    // disc_crc: full-file CRC32 (IEEE) of the data track. disc_sha1: lowercase
    // hex SHA-1. Either may be absent (has_disc_crc / disc_sha1.empty()).
    bool                  has_disc_crc = false;
    uint32_t              disc_crc = 0;
    std::string           disc_sha1;

    // [recompiler] block
    std::filesystem::path seeds_path;     // absolute path to seeds (text or json)
    std::filesystem::path bios_thunks_path; // optional; empty if not set
    std::filesystem::path out_dir;
    bool                  strict;
    std::string           out_stem;       // derived if not explicit

    // [runtime] block (optional)
    RuntimeConfig         runtime;

    // [widescreen] block (optional) — per-game knobs for the widescreen hack
    // ([video] aspect_ratio != 4:3). All default to inert; a game with no
    // [widescreen] block gets the plain GTE squash + stretched present only.
    //
    // sprite_tag_funcs: guest addresses of functions called once per
    //   character/billboard prim with the prim pointer in $a0 (the recompiler
    //   emits a psx_ws_sprite_tag(cpu) callback at their entry). Tagged prims
    //   get their X coords re-squashed around the prim's projected anchor at
    //   GP0 submission, undoing the present stretch so sprites keep correct
    //   proportions.
    // sprite_anchor_addr: scratchpad address holding the prim's projected
    //   anchor SXY (written by the game's RTPS preamble) at tag time.
    // hud_sprt_squash: center-squash every UNtagged textured-rect (SPRT)
    //   prim — pure screen-space 2D (HUD, menus) — so it presents at native
    //   proportions. Untextured TILEs (fades) are never touched.
    std::vector<uint32_t> ws_sprite_tag_funcs;
    uint32_t              ws_sprite_anchor_addr = 0;
    bool                  ws_hud_sprt_squash = false;

    // Cull-margin widening. The game's per-object draw classifier compares
    // (objX - camX + BIAS) against a RANGE derived from the 4:3 screen width;
    // the GTE squash shows ~33% more world, so the fixed margin collapses and
    // objects pop in/out near the wide-screen edges. We widen the window by
    // emitting a runtime margin term psx_ws_x_margin() (0 at 4:3/boot/menu/FMV,
    // ~the half-extra-width when stretching) into the relevant immediates:
    //   cull_bias_sites:  an addiu rT,rS,imm → rT = rS + (imm + margin)
    //   cull_range_sites: an sltiu rT,rS,imm → rT = rS <u (imm + 2*margin)
    //   cull_a1_sites:    a nop → a1 += margin, or move rD,a1 →
    //                     rD = a1 + margin (caller-margin classifier variants)
    // All Ghidra-evidenced; empty by default. Changing these requires a regen.
    std::vector<uint32_t> ws_cull_bias_sites;
    std::vector<uint32_t> ws_cull_range_sites;
    std::vector<uint32_t> ws_cull_a1_sites;
    // Explicit `sltiu rt,sx,W` render rejects for cases where codegen function
    // splitting separates the paired vertical test from auto_screen_x.
    std::vector<uint32_t> ws_cull_screen_x_sites;

    // [widescreen.cull] slti_sites — explicit signed right-edge widen sites
    // (`slti rt, sx, W` → psx_ws_cull_slti) for funnel functions the
    // auto-detector cannot qualify (e.g. an X-only test with no height compare
    // in the same function — Ape Escape 0x8004AB64). Empty by default; regen.
    std::vector<uint32_t> ws_cull_slti_sites;
    // Extra per-side actor overdraw beyond the visible widescreen edge.
    int                   ws_cull_guard_pixels = 0;

    // [widescreen.cull] screen_w_imms / screen_h_imms — the width/height
    // immediates of the GTE screen-extent reject signature, per game (the
    // display width varies: Tomba 320 → 0x140/0x141; Ape Escape 368 → 0x181).
    // Consumed by the auto_screen_x detector + emit on every backend (the
    // runtime mirrors get them via gpu_ws_set_cull_imms). Defaults preserve
    // the original Tomba signature.
    std::vector<uint32_t> ws_cull_w_imms;
    std::vector<uint32_t> ws_cull_h_imms;

    // Backdrop screen-X squash ([widescreen.backdrop] x_sites). The parallax
    // 2D backdrop layer (ocean/cloud/mountain/grass — overlay actor handlers)
    // computes screenX = (worldX - camX) >> parallax in pure integer math and
    // stores it to the object's screen-X field WITHOUT the GTE, so the GTE
    // X-squash that gives 3D the wider 16:9 FOV never reaches it; far pieces
    // sit past the 320px edge and are clipped (the edge "void"/pop-in). Each
    // x_site is a `sh rt,off(base)` storing the FINAL screenX; we emit it as
    // `write_half(base+off, psx_ws_backdrop_x(rt))` so the value is squashed
    // around screen centre (identity at 4:3). These addresses live in OVERLAY
    // code, so the overlay compile must see this config (--ws-config). A regen
    // is required; empty by default.
    std::vector<uint32_t> ws_backdrop_x_sites;

    // [widescreen.backdrop] unsquash_funcs — far-backdrop driver functions whose
    // body is bracketed with gte_ws_set_suppress(1)/(0) so the GTE X-squash is
    // OFF for their (far, parallax) draws: the backdrop fills the stretched 16:9
    // frame instead of leaving edge void (8C). Main-EXE addresses; regen-class.
    std::vector<uint32_t> ws_backdrop_unsquash_funcs;

    // [widescreen.dome] call_sites: exact guest JAL addresses whose GTE
    // projections belong to a finite curved backdrop mesh authored for 4:3.
    std::vector<uint32_t> ws_dome_call_sites;

    // [widescreen.cull] auto_screen_x — automatic horizontal-FOV cull widening.
    // GTE-projected render funnels reject a primitive when ALL its vertices fall
    // off the 4:3 frame: a per-vertex `sltiu vN, SX, 0x140` (right edge) paired
    // with `sltiu vN, SY, 0xE0` (bottom edge) in the same function. When this is
    // true the recompiler auto-detects that signature and emits every width
    // compare (0x140 / inclusive 0x141) with + 2*psx_ws_x_margin(), so the
    // wider 16:9 field of view is submitted instead of culled at 320 — no
    // per-site address list needed. 0 at 4:3 ⇒ byte-identical. Off by default;
    // a regen is required. (Vertical 0xE0 bound is left untouched.)
    bool ws_auto_screen_x_cull = false;

    // [widescreen.cull] auto_backdrop — automatic far-backdrop column PRELOAD.
    // Scrolling 2D backdrop layers (sky/ground/cloud/flower-field tile rows)
    // generate only a camera-windowed ~4:3 range of tile columns, so the 16:9
    // revealed margin shows void until the camera moves. When true the recompiler
    // auto-detects each column-window generator by its invariant (the /96 magic
    // 0x66666667 dividing the 0x176 camera-X, the sra-by-5 divide tail, and the
    // move/addiu loop-bound finalize — see ws_backdrop_detect.h) and rewrites the
    // window START to 0 and END high via psx_ws_backdrop_value(); the generator's
    // own low/high clamps then pin the loop to the whole finite row. Generators
    // are overlay-resident, so the overlay compile must see this (--ws-config).
    // 0 at 4:3 (native-wide inactive) => byte-identical. Off by default; regen.
    bool ws_auto_backdrop_preload = false;

    // [widescreen] full_2d — pure-2D sprite game (e.g. Mega Man X6) that never
    // emits the sprite_tag hook the 3D detector relies on. When true, every
    // in-game frame is treated as "gameplay" so native-wide engages and the 2D
    // scene is presented widescreen (the framework's normal full-2D screens
    // pillarbox 4:3, which is wrong for a game that IS 2D end-to-end). Runtime-
    // only (read at startup; no codegen impact) — no regen required. The wider
    // field of view itself is supplied by [widescreen.cull]/engine hooks; this
    // flag only opts the title into the 2D widescreen present path. Off by
    // default; the env var PSX_WS_FORCE_2D=1 forces it on for testing.
    bool ws_full_2d = false;

    // [widescreen] gte_game_mode — GTE-activity gameplay detector for a fully
    // 3D title with no sprite-tag helper (e.g. Ape Escape). When true, a frame
    // that projects enough vertices through RTPS/RTPT is stamped as gameplay
    // (native-wide engages); genuine full-2D screens (save/options) still
    // pillarbox 4:3. Runtime-only — no regen required. Off by default.
    bool ws_gte_game_mode = false;

    // [widescreen] native_wide — select the newer wide render-target path.
    // Defaults on for compatibility. Titles can keep the original GTE-squash
    // + stretched-present path when native-wide is not regression-free.
    bool ws_native_wide = true;

    // [widescreen] nw_hud_corners — in native-wide, push outer-third screen-
    // space HUD sprites out to the true wide-frame corners (they otherwise sit
    // inset by the reveal offset). Runtime-only — no regen. Off by default.
    bool ws_nw_hud_corners = false;

    // [widescreen] nw_left_hud_packet_lo / nw_left_hud_packet_hi — optional
    // half-open physical-RAM source range for a specifically identified HUD
    // pool. In native-wide, primitives from this pool anchor by screen third
    // (left/right), without moving similarly placed scenery from other pools
    // (essential for pure-2D games whose world is also screen-space prims).
    // Both values must be present together. Runtime-only; no regen required.
    uint32_t ws_nw_left_hud_packet_lo = 0;
    uint32_t ws_nw_left_hud_packet_hi = 0;

    // [widescreen] nw_backdrop — in native-wide, stretch a screen-space quad
    // that covers the whole 4:3 framebuffer (sky gradient / backdrop image) to
    // fill the wide frame, so it stops pillarboxing at the reveal margins.
    // Runtime-only — no regen. Off by default.
    bool ws_nw_backdrop = false;

    // [widescreen] clear_reveal — opt a title into synthetic native-wide margin
    // cleanup. A game-specific stage/map boundary can clear only proven-void
    // sides while preserving the canonical 4:3 surface and guest VRAM. Runtime-
    // only gate; any game-specific init hook remains regen-class. Off by default.
    bool ws_clear_reveal = false;

    // [widescreen] nw_flat_backdrop — in the native-wide mirror only, stretch
    // untextured flat primitives across the wider output. This is for games
    // whose authored 4:3 sky/backdrop is emitted as flat polygons rather than
    // a recognizable full-frame textured quad. Runtime-only; off by default.
    bool ws_nw_flat_backdrop = false;

    // [widescreen] nw_phase_backdrop — stretch textured primitives emitted
    // before the frame's first shaded 3D primitive. This isolates an authored
    // 2D sky/backdrop phase from the later textured foreground. Runtime-only;
    // off by default because draw ordering is title-specific.
    bool ws_nw_phase_backdrop = false;

    // [widescreen] offer — whether the launcher OFFERS its EXPERIMENTAL
    // Widescreen toggle for this title. Default true. Set false while a
    // title's widescreen is unported/unvalidated (e.g. MMX4 at bring-up):
    // the toggle is hidden in the launcher UI AND the runtime clamps the
    // display aspect to 4:3 after all config sources, so a stale persisted
    // 16:9 in settings.toml can never engage the hack. Runtime-only (read at
    // startup; no codegen impact) — no regen required.
    bool ws_offered = true;

    // [widescreen] offer_ultrawide — expose a separate experimental 21:9
    // launcher choice for titles that have explicitly tested it. Default off;
    // ordinary widescreen offer remains the independent 16:9 choice.
    bool ws_ultrawide_offered = false;

    // [widescreen.bg2d] — pure-2D background tile-loop widen (e.g. MMX6's
    // FUN_800270d0). Three instruction addresses in the per-layer BG renderer
    // whose column count and loop start are rewritten so the loop draws the
    // 16:9 reveal columns on both sides of the 320 view (see gpu.c
    // psx_ws_bg2d_* helpers — identity at 4:3 / 512 hi-res mode). Regen-class.
    //   count_site:    either the `li rt,21` column-count load (addiu/ori), or
    //                  the loop-closing inline `slti[u] rt,index,21` bound
    //                  compare (MMX4/MMX5).
    //   startcol_site: the `andi rt,rs,0x3f` start tile-column mask.
    //   startx_site:   the `sra rd,rt,sa` or `subu rd,zero,rt` start screen-x.
    // 0 = unset (feature off). Verified by opcode at gen time.
    uint32_t ws_bg2d_count_site    = 0;
    uint32_t ws_bg2d_startcol_site = 0;
    uint32_t ws_bg2d_startx_site   = 0;
    //   stream_left_site / stream_right_site: the addiu instructions in the tile-
    //   RING STREAMER that compute the left (scrollX-16) and right (scrollX+16,
    //   before +width) leading-edge world-X. Pushed out by LEFT*16 so the ring is
    //   populated across the widened column window (else the extra columns show
    //   empty/stale tiles). 0 = unset.
    uint32_t ws_bg2d_stream_left_site  = 0;
    uint32_t ws_bg2d_stream_right_site = 0;
    //   bufbase_site: the driver addu computing the BG packet-buffer address
    //   (base 0x800B91C0 + bufidx*0x4000); relocated to a larger free buffer when the
    //   widen is active. cap_site: the renderer's per-frame tile-cap slti (counter<1000);
    //   raised to match the bigger buffer. Together they cure the dense-stage overflow.
    uint32_t ws_bg2d_bufbase_site = 0;
    uint32_t ws_bg2d_cap_site     = 0;
    // Tile-ring layout used by the once-per-frame freshness refill. Defaults
    // describe MMX6; sibling engines such as MMX5 override the shifted RAM
    // addresses and smaller ring width in game.toml.
    uint32_t ws_bg2d_layer_base       = 0x800971F8u;
    uint32_t ws_bg2d_ring_base        = 0x800A21B8u;
    uint32_t ws_bg2d_map_size_addr    = 0x800CD338u;
    uint32_t ws_bg2d_layer_stride_addr = 0x8008EC10u;
    uint32_t ws_bg2d_ring_cols        = 64;
    uint32_t ws_bg2d_layer_count      = 3;
    uint32_t ws_bg2d_layer_struct_stride = 0x54;
    //   init_func: full tile-ring initializer called only when an independent
    //   layer's stage data is dirty. A callback at entry invalidates stale host
    //   reveal pixels once before the new stage background is submitted.
    uint32_t ws_bg2d_init_func    = 0;
    uint32_t ws_bg2d_packet_cap       = 1000;
};

// UserSettings — the launcher-written, user-editable override layer.
//
// Lives in a `settings.toml` next to the runtime exe (NOT in the repo). It is
// layered on top of the bundled game.toml at startup: any field present here
// overrides the corresponding game.toml value, and the command line overrides
// both. Absent fields fall through to game.toml. The launcher seeds this file
// from game.toml defaults the first time it writes.
//
// Unlike game.toml, paths here are stored verbatim (the user picked them); they
// are NOT resolved against a project root.
struct UserSettings {
    // [video]
    bool has_renderer       = false; int  renderer       = 0; // 0=software,1=opengl
    bool has_supersampling  = false; int  supersampling  = 1; // 1..4
    // Window size: width in px; height is always width*3/4 (PSX 4:3). Applies to
    // both the launcher and the emulator window so they boot at the same size.
    bool has_window_width   = false; int  window_width   = 1280; // -> 1280x960
    bool has_antialiasing   = false; bool antialiasing   = true;
    bool has_texture_filter = false; int  texture_filter = 0; // 0=nearest,1=bilinear
    bool has_screen_kind    = false; int  screen_kind    = 0; // 0..3 (ScreenKind)
    bool has_auto_skip_fmv  = false; bool auto_skip_fmv  = false; // skip FMVs
    // Turbo through in-game load screens: while the CD data stream is active, run
    // the guest unpaced (host speed) to compress load wall-time. All guest timing
    // (VBlanks/callbacks/sectors) is preserved and audio plays through. Default ON
    // (the per-game game.toml value seeds the launcher toggle; user choice in
    // settings.toml overrides it).
    bool has_turbo_loads    = false; bool turbo_loads    = true;
    bool has_fast_boot      = false; bool fast_boot      = false;
    // HLE BIOS tier toggle (see RuntimeConfig::bios_hle). Overrides game.toml.
    bool has_bios_hle       = false; bool bios_hle       = false;
    // [video] fullscreen: launch the game window in desktop fullscreen (the
    // launcher's "Fullscreen on launch" toggle; the in-game F11 / Alt+Enter
    // hotkey still toggles it live). false => windowed (default).
    bool has_fullscreen     = false; bool fullscreen     = false;
    // Low-latency present knobs. low_latency_input re-samples the pad after the
    // wall-clock pacer (just before present) so the next CPU frame reads fresh
    // input instead of input ~one frame stale (the dominant input->photon cost
    // on a vsync-light box). vsync: 1=on (tear-free), 0=immediate (lowest
    // display latency, may tear), -1=adaptive.
    bool has_low_latency_input = false; bool low_latency_input = true;
    bool has_vsync             = false; int  vsync             = 1;
    // [launcher] — when true, boot straight into the game and skip the GUI
    // launcher window (mirrors snesrecomp's SkipLauncher). Overridable per-run:
    // `--launcher` forces the GUI back on; `PSX_NO_LAUNCHER=1` forces it off.
    bool has_skip_launcher  = false; bool skip_launcher  = false;
    bool has_aspect_ratio   = false; int  aspect_num     = 4; // display aspect W:H
                                     int  aspect_den     = 3; // (4:3 = native)
    // [audio]
    bool has_spu_hq         = false; bool spu_hq         = false;
    // [bios] / [disc] / [memcard]
    bool has_bios_path      = false; std::filesystem::path bios_path;
    bool has_disc_path      = false; std::filesystem::path disc_path;
    bool has_memcard_dir    = false; std::filesystem::path memcard_dir;
    // Per-slot memory-card overrides. An explicit card path overrides the
    // <dir>/card<N>.mcd default; the enable flag inserts/removes the card.
    bool has_memcard1_path    = false; std::filesystem::path memcard1_path;
    bool has_memcard2_path    = false; std::filesystem::path memcard2_path;
    bool has_memcard1_enabled = false; bool memcard1_enabled = true;
    bool has_memcard2_enabled = false; bool memcard2_enabled = true;

    // [controller] — per-player input device + pad type. device is one of:
    //   "none"     — no pad in this port (port not connected)
    //   "keyboard" — driven by the keyboard map (input.ini)
    //   "<GUID>"   — an SDL game-controller GUID (SDL_JoystickGetGUIDString)
    // p1_mode/p2_mode select the emulated pad behaviour (see PadMode):
    // hybrid (default) / analog / digital. Defaults: P1 keyboard, P2 none.
    bool has_p1_device = false; std::string p1_device = "keyboard";
    bool has_p2_device = false; std::string p2_device = "none";
    // Pad input mode per player (see PadMode): hybrid (default) / analog /
    // digital. Persisted as p1_mode/p2_mode strings. Legacy p1_analog/p2_analog
    // booleans are still read for back-compat (true->analog, false->digital).
    bool has_p1_mode = false; int p1_mode = PAD_MODE_HYBRID;
    bool has_p2_mode = false; int p2_mode = PAD_MODE_HYBRID;
    // Analog-stick deadzone, raw SDL axis units (0..32767). The launcher edits
    // this as 0-100% (raw = pct*32767/100), mirroring snesrecomp's GamepadDeadzone.
    bool has_deadzone  = false; int  deadzone  = 12000;
    // Localization: the launcher's chosen language code (feeds RuntimeConfig
    // .language / g_lang). "off"/"jp"/"" = untranslated native game. Persisted to
    // settings.toml [localization].language.
    bool has_language = false; std::string language = "en";
};

// GameOptions — the game's OWN native OPTION-screen settings, declared in a
// dedicated, self-contained `game_options.toml` next to game.toml. Kept entirely
// separate from GameConfig (recompiler/aftermarket config) and UserSettings
// (launcher overrides): these describe the GAME's settings, persisted across
// launches because some titles keep them only in a per-boot RAM global with no
// memory-card config block (issue #5). See game_options.{h,c} for the runtime.
struct GameOption {
    std::string name;          // key written to the saved-values state file
    uint32_t    addr = 0;      // guest RAM address of the canonical config global
    int         size = 1;      // 1 or 2 bytes
    uint32_t    init_store_pc = 0; // PC of the boot-init sb/sh that writes this
                                   // global's default; the recompiler rewrites it
                                   // to apply the persisted value (restore-at-init).
    // Optional valid-value range for RESTORE-time validation. When has_range is
    // true (game_options.toml declared `max`, and optionally `min`), a persisted
    // value outside [vmin, vmax] is rejected at load — the game's own default is
    // used instead — so a corrupt/stale options file can never inject an out-of-
    // range value (e.g. an enum used as a jump-table index -> wild jump). Absent
    // => no validation (e.g. signed screen offsets). Only the runtime consumes
    // this; the recompiler ignores it (no codegen impact, no regen needed).
    bool        has_range = false;
    int64_t     vmin = 0;
    int64_t     vmax = 0;
};
struct GameOptions {
    std::vector<GameOption> options;     // empty => feature off for this game
};

// Load game_options.toml ([[option]] array of {name, addr, size}). Returns an
// empty GameOptions if the file is missing/unreadable; throws on a malformed
// declared entry (bad addr / size) so a typo is surfaced, not silently dropped.
GameOptions load_game_options(const std::filesystem::path& path);

// Load settings.toml. Returns an all-defaults (all has_*=false) struct if the
// file is missing or unreadable. Malformed values are skipped (best-effort:
// the launcher must still be able to start so the user can fix them), so this
// never throws.
UserSettings load_user_settings(const std::filesystem::path& path);

// Write settings.toml deterministically. Returns false on I/O failure.
bool save_user_settings(const std::filesystem::path& path, const UserSettings& s);

// Locate the project root by walking upward from `config_path` until a
// directory containing `.gitignore`, `.git`, or `CMakeLists.txt` is found.
// Throws std::runtime_error if not found within 8 levels.
std::filesystem::path find_project_root(const std::filesystem::path& config_path);

// Load a BIOS config TOML. Throws std::runtime_error on schema violations.
BiosConfig load_bios_config(const std::filesystem::path& config_path);

// Load a game config TOML. Throws std::runtime_error on schema violations.
GameConfig load_game_config(const std::filesystem::path& config_path);

} // namespace PSXRecompV4
