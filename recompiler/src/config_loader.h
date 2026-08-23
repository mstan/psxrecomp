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
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "bios_address_model.h"   // BiosAddrCopy (BiosConfig::address_copies)
#include "recompiler_patch.h"

namespace PSXRecompV4 {

// Pad input mode (per player). Replaces the old analog on/off boolean.
//   hybrid  — MOD-ONLY. Not selectable in the launcher, not a valid game.toml
//             value, never a default. It survives solely so a trusted game mod
//             can request it via psx_mod_set_controller_mode_override() (Tomba's
//             hybrid controller plugin). Auto-switches analog/digital per the
//             most-recent input:
//             nudge the stick -> report DualShock (0x73, variable sticks);
//             press the D-pad -> report a digital pad (0x41) so the game runs
//             its OWN d-pad path at true digital sensitivity. Mirrors a
//             DualShock's analog LED toggling on/off (and Tomba Special
//             Edition's auto-detect).
//   analog  — always present a DualShock/analog pad (id 0x73). The D-pad is
//             folded onto the stick at full deflection so it still moves you.
//             Default.
//   digital — always present a digital pad (id 0x41); sticks disabled.
enum PadMode { PAD_MODE_HYBRID = 0, PAD_MODE_ANALOG = 1, PAD_MODE_DIGITAL = 2 };

// Renderer IDs shared by game.toml/settings parsing and runtime startup.
// OpenGL is the default because the Windows software/SDL_Renderer path is slow
// enough to mask interpreter/AOT performance work and can present as a false
// 30 FPS regression. Software remains an explicit opt-in fallback.
inline constexpr int VIDEO_RENDERER_SOFTWARE = 0;
inline constexpr int VIDEO_RENDERER_OPENGL = 1;
inline constexpr int VIDEO_RENDERER_VULKAN = 2;
inline constexpr int DEFAULT_VIDEO_RENDERER = VIDEO_RENDERER_OPENGL;

// Controller-hotkey bind encoding, mirroring recomp-ui's RECOMP_LAUNCHER_PAD_*
// (recomp_launcher.h): 0 = unbound, 1..99 = button (1 + SDL button code),
// 100..999 = axis, 1000+ = a CHORD, encoded as 1000 + a bitmask of SDL button
// codes. Two buttons held together is the normal case (select+r3), so a real
// value here is routinely five digits.
//
// The bound matters: a naive `< 256` check silently rejects every chord — which
// is exactly what shipped, so a saved rewind_pad = 1272 was dropped on load and
// the pad hotkeys were only ever configurable to single buttons. SDL defines
// ~21 gamepad buttons, so cap the mask at 32 bits' worth and let the runtime
// ignore bits no controller can produce.
inline constexpr int PAD_BIND_MAX = 1000 + (1 << 21);
inline bool pad_bind_value_ok(long long n) { return n >= 0 && n < PAD_BIND_MAX; }

// FMV present reconstruction, ordered least → most smoothing. See
// RuntimeConfig::video_fmv_filter for what each one does and why bicubic is
// the default. Shared by game.toml/settings parsing and runtime startup.
inline constexpr int VIDEO_FMV_FILTER_NEAREST  = 0;
inline constexpr int VIDEO_FMV_FILTER_BILINEAR = 1;
inline constexpr int VIDEO_FMV_FILTER_SHARP    = 2;
inline constexpr int VIDEO_FMV_FILTER_BICUBIC  = 3;
inline constexpr int VIDEO_FMV_FILTER_COUNT    = 4;
inline constexpr int VIDEO_FMV_FILTER_DEFAULT  = VIDEO_FMV_FILTER_BICUBIC;

// Canonical settings.toml / game.toml spelling for each value, and its parse.
inline const char* video_fmv_filter_name(int v) {
    switch (v) {
        case VIDEO_FMV_FILTER_NEAREST:  return "nearest";
        case VIDEO_FMV_FILTER_BILINEAR: return "bilinear";
        case VIDEO_FMV_FILTER_SHARP:    return "sharp";
        default:                        return "bicubic";
    }
}
inline bool video_fmv_filter_parse(const std::string& s, int* out) {
    if (s == "nearest")       { *out = VIDEO_FMV_FILTER_NEAREST;  return true; }
    else if (s == "bilinear") { *out = VIDEO_FMV_FILTER_BILINEAR; return true; }
    else if (s == "sharp")    { *out = VIDEO_FMV_FILTER_SHARP;    return true; }
    else if (s == "bicubic")  { *out = VIDEO_FMV_FILTER_BICUBIC;  return true; }
    return false;
}

struct WidescreenSignedBoundSite {
    uint32_t address = 0;
    uint32_t expected = 0; // guarded LUI instruction
};

// One exact compare whose verdict is forced while a widescreen reveal is
// active. The full instruction word is part of the identity because overlay
// variants routinely place unrelated code at the same virtual address.
struct WidescreenCullKeepSite {
    uint32_t address = 0;
    uint32_t expected = 0; // guarded SLT/SLTU/SLTI/SLTIU instruction
    uint32_t result = 0;   // forced comparison result (0 or 1)
};

// Screen-edge compare whose BOUND follows the live reveal margin, rather than
// having its verdict pinned like a keep site.
//
// `keep` is right only for a separately proven binary decision. At a clip-code
// packer it is wrong: pinning the classifier tells the clipper that nothing
// crosses the screen edge, so crossing polygons are never subdivided, are
// submitted with coordinates outside the GPU's legal primitive range, and the
// hardware discards them whole. Moving the bound keeps the classification
// honest and simply clips at the revealed edge.
//
// `mode` names which operand carries the bound and which way it travels. Every
// mode is identity at margin 0, so 4:3 output stays bit-for-bit unchanged.
enum class WsCullWidenMode {
    ImmUpper,  // SLTI/SLTIU: coord < imm + m   (right/bottom edge)
    ImmLower,  // SLTI/SLTIU: coord < imm - m   (left/top edge)
    BoundRt,   // SLT/SLTU:   rs    < rt  + m   (coord in rs, bound in rt)
    BoundRs,   // SLT/SLTU:   rs + m <  rt      (bound in rs, coord in rt)
};

struct WidescreenCullWidenSite {
    uint32_t address = 0;
    uint32_t expected = 0; // guarded SLT/SLTU/SLTI/SLTIU instruction
    WsCullWidenMode mode = WsCullWidenMode::ImmUpper;
};

// Aspect-scaled 12-bit angular half-extent. These sites load a positive angle
// constant with `addi[u] rt,zero,imm`; the runtime widens tan(angle) by the
// live horizontal reveal factor. Full-word guards prevent overlay-address
// aliases from changing unrelated immediates.
struct WidescreenAngleSite {
    uint32_t address = 0;
    uint32_t expected = 0; // guarded ADDI/ADDIU with rs == zero
};

// Aspect-aware horizontal participation cone. The exact compare sites are
// full-word guarded because overlay variants can reuse a virtual address for
// unrelated code. Registers are MIPS GPR indices captured at each comparison.
// Queue metadata is optional; when present it lets guard/hysteresis candidates
// leave configured headroom without changing the game's fixed capacities.
struct WidescreenAspectConeSite {
    uint32_t address = 0;
    uint32_t expected = 0; // guarded signed SLTI or SLT reject comparison
    // Q10 cosine threshold. Zero derives the threshold from an SLTI
    // immediate; register-register SLT sites must provide it explicitly.
    uint32_t cosine_threshold = 0;
    // Per-site register overrides. UINT32_MAX inherits the enclosing
    // aspect-cone defaults.
    uint32_t object_reg = 0xFFFFFFFFu;
    uint32_t x_reg = 0xFFFFFFFFu;
    uint32_t z_reg = 0xFFFFFFFFu;
    uint32_t y_reg = 0xFFFFFFFFu;
    // False for lower-level model/child predicates that do not append to the
    // configured fixed-capacity queues.
    bool queue_guard = true;
};

struct WidescreenAspectConeConfig {
    std::vector<WidescreenAspectConeSite> sites;
    uint32_t forward_addr = 0;       // three signed Q12 halfwords: X,Z,Y
    uint32_t object_type_offset = 0;
    uint32_t object_reg = 0;
    uint32_t x_reg = 0;
    uint32_t z_reg = 0;
    uint32_t y_reg = 0;
    uint32_t hysteresis_pixels = 0;
    uint32_t queue_reserve = 0;
    std::array<uint32_t, 3> queue_count_addrs{};
    std::array<uint32_t, 3> queue_capacities{};
    std::array<uint32_t, 3> queue_type_masks{};
};
// Parse/format a pad mode. Strict game.toml parsing accepts "analog" or
// "digital" (case-insensitive), returns `fallback` for unknown values, and
// THROWS on "hybrid" — the mode is mod-only and must not be declared by a game.
int         pad_mode_from_string(const std::string& s, int fallback);
// Lenient: for a user's settings.toml, where a stale persisted "hybrid" must
// migrate to analog rather than refuse to launch.
int         pad_mode_from_settings_string(const std::string& s, int fallback);
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

    // Optional, game-specific warm-load route accelerators. A matching SetLoc
    // at arm_lba followed by the exact ordered lbas sequence switches only
    // those data reads to the bounded instant cadence. Any sequence mismatch
    // fails closed to the configured disc_speed. The parser accepts the legacy
    // [runtime.warm_cd_route] table and the reusable
    // [[runtime.warm_cd_routes]] array-of-tables.
    struct WarmCdRoute {
        int              arm_lba = -1;
        std::vector<int> lbas;
        int              instant_max_per_frame = 32;
    };
    std::vector<WarmCdRoute> warm_cd_routes;

    // fast_boot: DEPRECATED alias for the BIOS boot shell-skip (see bios_hle
    // below). The old mechanism (snapshot BIOS state at first handoff, restore
    // on later launches) is gone; fast_boot=true now skips only the BIOS shell
    // (the boot animation) via the HLE tier's one-shot shell intercept, with
    // kernel init + game EXE load still executed by the real recompiled BIOS
    // at host speed. It is boot-only: it never enables the kernel-call HLE
    // tier, and it works on any BIOS image that exports a shell entry.
    // Kept so existing game.toml/settings.toml keep working.
    bool                  fast_boot = false;

    // bios_hle: High-Level Emulation tier for BIOS kernel services
    // (CLAUDE.md §0 amendment 2026-07-02, the gbarecomp model). DEFAULT ON as of
    // 2026-07-06 (user-directed player default: instant boot-skip for every
    // game). Opt OUT with [runtime] bios_hle = false or env PSX_BIOS_HLE=0 to run
    // pure LLE (the recompiled BIOS), which REMAINS the reference implementation
    // and the oracle — this only flips the default, LLE is still fully linked and
    // selectable. When on, implemented kernel services are computed in-runtime
    // against the real guest kernel structures and every other call falls through
    // to LLE. Implies the BIOS boot shell-skip unless bios_hle_keep_intro.
    // PSX_BIOS_HLE / PSX_BIOS_HLE_KEEP_INTRO env override at launch.
    // Runtime: runtime/src/bios_hle.c.
    //
    // The flag drives TWO axes with different per-image requirements, resolved
    // in runtime/src/bios_hle_plan.c:
    //   * boot shell-skip — needs only the image's shell_entry_phys anchor and
    //     works under pure LLE, so it fires the SAME WAY on every linked BIOS
    //     (retail SCPH-1001 and the bundled OpenBIOS alike). "Skip the BIOS and
    //     go straight to the game" is one behaviour, not a per-BIOS lottery.
    //   * kernel-call HLE — needs the image's deliver_event_ret anchor; an
    //     image without it (OpenBIOS, until its B0 semantics are validated)
    //     refuses this axis and says so at startup, WITHOUT cancelling the
    //     boot-skip. Collapsing the two is exactly the bug fixed 2026-07-27.
    // openbios: may this title run on the bundled, redistributable OpenBIOS?
    // Default true — a player who chooses no BIOS gets OpenBIOS and never has
    // to find one (docs/BIOS_SELECTION.md). Set false ONLY for a title with a
    // verified OpenBIOS incompatibility; a retail image is then required and
    // the player is prompted for it.
    //
    // Deliberately NOT overridable by a player's settings.toml: this records a
    // developer's compatibility finding, not a preference.
    bool                  openbios = true;

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

    // overlay_capture_history: opt-in durable capture history. The runtime
    // keeps overlay_captures.json as an atomic latest snapshot for the live
    // compiler and additionally appends every changed coherent snapshot to
    // overlay_captures.addendum.jsonl beside the executable. A malformed tail
    // from a hard kill cannot destroy earlier records.
    bool                  overlay_capture_history = false;

    // overlay_capture_persist_dir: optional DEV-only, project-relative safe
    // directory for one immutable JSON file per changed snapshot. Rooted,
    // UNC, drive-qualified and drive-relative paths are rejected, as is any
    // `..` component or an embedded NUL, under both `/` and `\` separators.
    // Production normally leaves this unset and retains only the addendum
    // beside the executable.
    std::string           overlay_capture_persist_dir;

    // turbo_loads: DEPRECATED AND IGNORED. Load acceleration is owned by the
    // Mods catalog — psx.enhancement.fast-loading ("Fast Loading (host
    // pacing)") and psx.enhancement.cd-speed, both `game_id = "*"` so they
    // ship with every title, both default-off, and both exposing the
    // multiplier / instant-scheduler detail a single opaque bool never could.
    // recomp-ui correspondingly draws no generic Turbo loads row.
    //
    // The key is still parsed so old configs load without error, but the
    // runtime NO LONGER honours it: it logs one deprecation line naming the
    // Fast Loading mod and leaves acceleration off. Retired because leaving
    // the legacy switch live forced turbo on in any title that had not
    // explicitly migrated, with no UI to turn it back off (MegaManX6Recomp#14
    // shipped that way in v1.0.4/v1.0.5). Development toggling still works
    // through the `turbo_loads` TCP debug command.
    bool                  turbo_loads = false;
    bool                  has_turbo_loads = false;   // key present in game.toml

    // offer_turbo_loads: DEPRECATED AND IGNORED, now that the generic switch
    // it gated no longer exists. Defaults false and is never consulted; the
    // migrated titles that set it false stay correct, and the titles that
    // never set it are no longer punished for it. Parsed only so old configs
    // load, and so the runtime can tell a developer the key is now a no-op.
    bool                  offer_turbo_loads = false;
    bool                  has_offer_turbo_loads = false;

    // turbo_audio_sink: while turbo_loads is actively running unpaced, keep
    // rendering the exact guest-time SPU sample budget (so voice/CD state
    // advances) but discard those samples before the host playback queue.
    // Opt-in while the experiment is under live audio QA.
    bool                  turbo_audio_sink = false;

    // idle_skip: proof-gated fast-forward through repeated CPU polling
    // loops with no stores/MMIO and stable register state. Guest time and
    // device events still advance exactly. Opt-in per game; the idle_skip
    // debug command and PSX_IDLE_SKIP environment variable support live A/B.
    bool                  idle_skip = false;

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
    // "auto" (default, empty == auto) | "gcc" | "tcc" | "auto-no-gcc". ("sljit"
    // was removed 2026-07-15; a stale value degrades to auto.) auto resolves to
    // gcc when a gcc TOOLCHAIN is present (a dev /
    // production box), else tcc (bundled, toolchain-free). "auto-no-gcc" forces
    // the tcc branch even with gcc present (simulate a toolchain-less user box;
    // gcc shards still load). The env var PSX_OVERLAY_BACKEND overrides at runtime.
    std::string           overlay_backend;

    // overlay_native_block: per-game overlay function entries that must stay on
    // the dirty-RAM interpreter even when a matching native DLL exists. Intended
    // for small timing-sensitive setup/task routines while the rest of the
    // overlay runs native.
    std::vector<uint32_t> overlay_native_block;

    bool                  has_parappa_timing = false;
    std::string           parappa_timing_mode = "stock";
    int                   parappa_timing_extra_early = 0;
    int                   parappa_timing_extra_late = 0;

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

    // fmv_filter: how the 24-bit FMV present reconstructs its low-res source
    // (a 320x192-class movie blown up to the window). Only consulted when
    // antialiasing is on — AA off means nearest, as it does everywhere else.
    //   0 nearest   point-sampled; hard pixels, uneven pixel widths at a
    //               non-integer scale
    //   1 bilinear  plain GL_LINEAR; smoothest, but blurs the whole texel
    //   2 sharp     sharp-bilinear; flat texel interiors, ramp confined to a
    //               one-output-pixel band at the boundary
    //   3 bicubic   Catmull-Rom (default) — removes most of the staircase
    //               while holding overall sharpness at the nearest level
    int                   video_fmv_filter = VIDEO_FMV_FILTER_DEFAULT;

    // renderer: "software" | "opengl" (default) | "vulkan". Selects the
    // rasterizer/present backend. The software rasterizer remains the explicit
    // fallback. Stored as VIDEO_RENDERER_*.
    int                   video_renderer = DEFAULT_VIDEO_RENDERER;

    // hd_textures: load a Beetle PSX HW-format HD texture pack from
    // <disc dir>/<disc stem>-texture-replacements/. See runtime/include/tex_pack.h.
    bool                  video_hd_textures = false;

    // hd_texture_dump: write every texture the game draws to
    // <disc dir>/<disc stem>-texture-dump/ as <texhash>-<palhash>.png. The
    // authoring path for new packs, and the instrument that proves our hashes
    // match Beetle's (diff the two dumps' filename sets). Costs a synchronous
    // PNG write per newly-seen texture, so it is a tool, not a play setting.
    bool                  video_hd_texture_dump = false;

    // [runtime] cpu_overclock — percent of stock CPU speed. 100 = stock.
    // The guest runs as native code, so this scales the per-instruction cycle
    // CHARGE down: the CPU completes more work inside the same CRTC period
    // while timers, SPU, CDROM and refresh keep their real rates.
    // hueponik's pal100full8 patch requires >900%.
    uint32_t              runtime_cpu_overclock = 100;

    // [video] bezel — a still image drawn behind the frame, filling whatever
    // the letterbox or pillarbox leaves over, the way vertical shmups fill the
    // dead space on a horizontal display. Never samples the frame, so unlike
    // the edge fill it cannot smear moving content and needs no menu-vs-
    // gameplay test. Relative paths resolve against the disc directory.
    // Empty = none. For WipEout 3, the team wallpapers by MotorcycleEmptiness
    // (awesome-wipeout.github.io) suit this exactly.
    std::string           video_bezel;

    // hd_texture_dir: parent directory for both folders above. Empty (default)
    // means the directory the disc image lives in, which is where a pack
    // authored for RetroArch already expects to sit.
    std::string           video_hd_texture_dir;

    // geometry_correction: sub-pixel vertex precision (the PGXP-style fix for
    // PS1 polygon jitter/wobble). The GTE projects in 16.16 and then throws the
    // fraction away when it saturates SXY to integer screen pixels; vertices of
    // a moving mesh therefore snap between whole pixels and the model appears to
    // shimmer. With this on, the GTE keeps the discarded fraction in a side
    // cache and the rasterizer places the vertex between native pixels.
    //
    // The PS1-visible SXY FIFO stays integer and fully faithful — the game's own
    // screen-bounds culls and any SXY readback see exactly what hardware would
    // produce. This is visual-only. Default off (faithful floor).
    //
    // Only observable at [video] supersampling >= 2: at native resolution the
    // corrected position rounds back to the same pixel it started on.
    bool                  video_geometry_correction = false;

    // perspective_texturing: perspective-correct UV interpolation for textured
    // world polygons (the PS1 GPU interpolates UV affinely, which warps textures
    // on large floor/wall polygons as the camera moves). Uses the exact SWC2
    // projection provenance — a polygon only qualifies when every one of its
    // position words was written to that DMA packet address by a projection
    // store — so CPU-built UI and 2D sprites are never touched.
    //
    // Default OFF (faithful floor), same as geometry_correction above — but for a
    // different reason. geometry_correction is off because it is BROKEN at the
    // coverage we can reach (it moves vertices and splits shared edges); this one
    // is off because it is a deliberate departure from hardware output that has
    // only been validated on one title and one renderer. It is structurally safe
    // — it never moves a vertex, only alters UV interpolation inside a polygon
    // whose provenance is already proven, so a non-qualifying polygon simply keeps
    // the PS1's affine interpolation and neighbours can never disagree about a
    // shared edge. Safe is not the same as validated, so it stays opt-in.
    //
    // Players opt in from the launcher's Display panel (unlike
    // geometry_correction, which has no control at all); per-game with
    // [video] perspective_texturing = true.
    bool                  video_perspective_texturing = false;

    // pgxp_depth: depth-test polygons using PGXP's recovered per-vertex W
    // instead of relying solely on the ordering table.
    //
    // The PS1 has no depth buffer. Primitives are linked into an ordering table
    // indexed by ONE averaged depth per primitive (AVSZ3/AVSZ4), quantised into
    // a fixed number of buckets, and drawn back to front. Two polygons that
    // interpenetrate — or sit near-coplanar in the same bucket — therefore
    // resolve by submission order. The hardware cannot express the right
    // answer, so no amount of faithful emulation produces one; WipEout 3's
    // trackside scenery shows it as pipes sorting through walls.
    //
    // PGXP already recovers a precise per-vertex W, and the textured path
    // already carries it to the shader as a_q, so this writes a real depth
    // buffer from data that is present rather than deriving anything new.
    //
    // Default OFF, and for a stronger reason than perspective_texturing: this
    // one CHANGES WHICH PIXELS SURVIVE. A primitive without valid W (2D, UI,
    // anything the provenance test rejects) must not participate at all, or it
    // depth-tests against garbage and disappears. Opt-in per game with
    // [video] pgxp_depth = true.
    bool                  video_pgxp_depth = false;

    // pgxp_cpu_mode: propagate sub-pixel precision through CPU arithmetic as
    // well as memory moves (the PGXP engine's tier-2 hooks). Off by default —
    // the same default as the reference implementations — because some games
    // deliberately rely on integer truncation in their own math; value
    // validation keeps it SAFE either way, this only trades coverage.
    // Meaningful only in a pgxp-flavour build; live-tunable over TCP.
    bool                  video_pgxp_cpu_mode = false;

    // pgxp_tolerance: reject a corrected vertex whose sub-pixel offset from
    // the native integer position exceeds this many pixels (the truncation-
    // agreement check already bounds offsets to < 1px; this narrows them
    // further). Default 0.5 — user-validated on Ape Escape (2026-08-15):
    // unclamped, sparse hairline background-bleed seams appear where a
    // corrected triangle borders an uncorrected one; at 0.5 the seams are
    // gone and only sub-half-pixel misalignment remains. Negative disables
    // the clamp. Live-tunable over TCP (pgxp verb).
    double                video_pgxp_tolerance = 0.5;

    // offer_vulkan: expose the experimental Vulkan renderer in the launcher.
    // Defaults false even for Vulkan-enabled builds; developers must opt in per
    // game once visuals are validated.
    bool                  video_offer_vulkan = false;

    // low_latency_input: re-sample the pad after the wall-clock pacer (just
    // before present) so the next CPU frame reads near-fresh input instead of
    // input ~one frame stale. Default on. vsync: present/swap mode —
    // 1=on (tear-free, default), 0=immediate (lowest display latency, may
    // tear), -1=adaptive. The wall-clock pacer holds 59.94Hz regardless.
    bool                  video_low_latency_input = true;
    int                   video_vsync             = 1;
    bool                  video_frame_interpolation = false;
    int                   video_frame_interpolation_fps = 0; // 0 = display refresh
    // offer_frame_interpolation: expose the generic interpolation controls
    // through recomp-ui Settings. Defaults true for compatibility. A game
    // migrating interpolation into its mod catalog sets this false; stale
    // persisted Settings values are then ignored and a trusted activation
    // plugin owns the runtime switch.
    bool                  video_offer_frame_interpolation = true;

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

    // offer_skip_fmv: expose auto_skip_fmv through recomp-ui Settings.
    // Defaults true for compatibility. A game migrating the feature into its
    // mod catalog sets this false; stale persisted Settings values are then
    // ignored and a trusted activation plugin owns the runtime switch.
    bool                  video_offer_skip_fmv = true;

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
    // buffer_ms: steady-state host playback cushion. The ecosystem default
    // remains 180 ms because it survives long streamed-stage production gaps;
    // games with smoother production cadence may opt into a lower value to
    // reduce controller-to-sound latency. This is a game-developer setting,
    // not a player preference.
    int                   audio_buffer_ms = 180;

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
    int                   default_p1_mode  = PAD_MODE_ANALOG;
    int                   default_p2_mode  = PAD_MODE_ANALOG;

    // p1_device / p2_device: optional per-game default input sources for
    // fresh settings and launcher-less boots. Same vocabulary as settings.toml:
    // "none", "keyboard", "auto"/"gamepad"/"controller", or an SDL GUID.
    // Per-install settings.toml still overrides these defaults.
    bool                  has_default_p1_device = false;
    bool                  has_default_p2_device = false;
    std::string           default_p1_device;
    std::string           default_p2_device;

    // lock_mode: when true the launcher HIDES the whole pad-mode selector
    // (Hybrid | Analog | D-Pad) and forces every port to default_p1_mode. For a
    // game that supports exactly one pad type — e.g. Tomba 2, whose driver only
    // works as a plain digital pad because the DualShock config-mode handshake
    // is unhandled — so the player can't pick a broken mode. Supersedes
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

    // multitap_port: console port that hosts the SCPH-1070 when offline/netplay
    // arms multitap (players/slot_count >= 3). 1 = Port 1 (default, most games),
    // 2 = Port 2 (Bomberman Party Edition, Jigsaw Madness, S.C.A.R.S., …).
    bool                  has_multitap_port = false;
    int                   multitap_port     = 1;

    // multitap_analog: DualShock-on-tap hack (default true). When true,
    // multitap bulk seats may report 0x73 + stick bytes; when false (faithful),
    // tap seats stay plain digital. Overridable by settings.toml / match_caps.
    bool                  has_multitap_analog = false;
    bool                  multitap_analog     = true;

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
    // anti_deadzone: minimum radial analog output after leaving deadzone, in
    // raw SDL axis units (0..32767). This is a game-owned response setting used
    // to compensate a title's own internal stick deadzone. Absent => 0.
    bool                  has_anti_deadzone = false;
    int                   anti_deadzone     = 0;

    // [runtime.link]: SIO1 serial-link peer (docs/config_schema.md).
    // `link_enabled` gates the PEER, not the register file -- the SIO1
    // registers respond whether or not a cable is plugged in (see
    // accuracy/axis4_sio1_serial.md). Default off => "null" endpoint
    // (no cable: DSR/CTS low, TX discarded).
    bool                  has_link            = false;
    bool                  link_enabled        = false;
    std::string           link_backend        = "null";
    int                   link_latency_cycles = 0;
    bool                  link_trace          = false;
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

    // [program.image] block (optional): declared image identity. When
    // sha256 is present the recompiler REFUSES to emit from a ROM whose
    // computed sha doesn't match — regenerating from the wrong image is a
    // build defect, not a warning. redistributable=true marks a BIOS that
    // ships WITH the game (OpenBIOS): the runtime then hides the whole
    // BIOS-selection surface (couriered via psx_bios_image.image_bundled).
    std::string           image_sha256;         // empty = unchecked
    bool                  image_redistributable = false;

    // [recompiler] block
    std::filesystem::path seeds_path;    // absolute path to seeds JSON
    std::filesystem::path out_dir;       // absolute path to output dir
    bool                  strict;        // currently always true
    std::string           out_stem;      // derived if not explicit
    std::vector<BiosVectorTable> bios_vectors; // optional vector dispatch tables
    std::vector<BiosAlias>       bios_aliases; // optional fixed-target trampolines

    // [recompiler.address_model] block: the BIOS's boot-time ROM->RAM code
    // copies, semantic validation and consumption in BiosAddressModel
    // (bios_address_model.h). Empty = the BIOS runs entirely from ROM.
    std::vector<BiosAddrCopy> address_copies;
    // [[recompiler.install_slots]]: kernel-RAM PCs the BIOS overwrites with
    // dispatch stubs at runtime (see docs/dynamic_handler_install.md).
    std::vector<uint32_t>     install_slots;

    // [recompiler.runtime_exports]: per-image anchors the emitter couriers
    // into the generated C (psx_bios_image, runtime/include/psx_bios_image.h)
    // for the runtime's HLE tier. 0 = this BIOS has no such anchor — the
    // consumer treats the feature as structurally unavailable.
    // shell_entry_phys gates the BIOS boot-skip and NOTHING else; it works
    // under pure LLE, so every image that exports it skips the boot the same
    // way. deliver_event_ret gates the kernel-call HLE tier, separately. The
    // runtime decides the two axes in psx_bios_hle_plan()
    // (runtime/include/bios_hle_plan.h) — read that header before touching
    // either, it records why collapsing them broke OpenBIOS boot-skip.
    uint32_t shell_entry_phys  = 0;  // BIOS boot-skip trigger (bios_hle.c)
    uint32_t deliver_event_ret = 0;  // $ra after the kernel DeliverEvent jalr

    // NOTE: a BIOS profile has NO [runtime] block. It describes an IMAGE —
    // facts about bytes — never a preference; runtime options belong to
    // game.toml/settings.toml, where the player and the title can both be
    // heard. There used to be a RuntimeConfig here, parsed and never read by
    // anything, and bios/OpenBIOS.toml carried a `bios_hle = false` in it that
    // looked like the reason HLE was off on OpenBIOS. It was not (the real gate
    // is the absent deliver_event_ret anchor), and reading it as one is how the
    // boot-skip regression got rationalized instead of fixed. load_bios_config
    // now REJECTS a [runtime] block rather than silently ignoring it.
};

struct GameConfig {
    std::filesystem::path config_path;
    std::filesystem::path project_root;

    // [game] block
    std::string           name;          // e.g. "Tomba!"
    std::string           id;            // e.g. "SCUS-94236"
    // Optional region label shown by the launcher (e.g. "(USA)", "(Europe)",
    // "(Japan)"). Empty = derive from the `id` serial prefix at the call site
    // (SCUS/SLUS/LSP -> USA, SCES/SLES -> Europe, SCPS/SLPS/SLPM -> Japan);
    // an unrecognized/empty serial yields no region badge.
    std::string           region;
    // How many players the game supports (1 or 2). Defaults to 1; most PSX
    // titles configs never set this explicitly.
    int                    players = 1;
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

    // [netplay] disc mount policy (portable across psxrecomp titles).
    // Data-track CRC proves "right game"; these prove "same CD geometry"
    // (GetTN track count / cue layout) so peers cannot join with Track-01-only
    // dumps vs full Redump multi-track cues. 0 / empty = do not check that field.
    bool                  netplay_require_cue = false;
    int                   netplay_required_tracks = 0;
    bool                  has_netplay_required_leadout = false;
    uint32_t              netplay_required_leadout_lba = 0;
    std::string           netplay_required_disc_fp;  // lowercase hex SHA-256
    bool                  netplay_link_lobby = false; // PSX-Link lobby type offered
    // Shared dev channel tag. Local / unreleased builds advertise
    // "dev+<tag>" instead of a release pin, so they find each other in the
    // lobby browser and never appear to players on a versioned release.
    std::string           netplay_dev_tag;
    // local_viewport = "vertical_split": while real netplay is active, crop
    // presentation to this peer's left/right split-screen half. This is a
    // presentation-only helper for titles that still render native split-screen
    // in netplay; unset keeps every peer seeing the full framebuffer.
    std::string           netplay_local_viewport;
    // Optional display aspect to use with local_viewport. Accepted values:
    // "16:9", "21:9", or "adaptive" (initial 16:9, live-window capped 21:9).
    // Unset keeps netplay at the title's normal mod-cleared aspect.
    std::string           netplay_local_viewport_aspect;

    // [recompiler] block
    std::filesystem::path seeds_path;     // absolute path to seeds (text or json)
    std::filesystem::path bios_thunks_path; // optional; empty if not set
    // [recompiler] bios_config — BIOS profile this game builds against
    // (empty = main_psx resolves the SCPH1001 profile default).
    std::filesystem::path bios_config_path;
    std::filesystem::path out_dir;
    bool                  strict;
    std::string           discovery;     // "whole-image" (default) or "reachable"
    std::string           out_stem;       // derived if not explicit
    // Game-owned, exact MIPS word replacements. IDs and physical instruction
    // addresses are unique within a config; see docs/config_schema.md.
    std::vector<RecompilerPatch> recompiler_patches;

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
    // auto_ui_squash: proportion-correct textured screen-space primitives in
    // the final ordering-table layer. Repeated glyph/icon rows share an anchor
    // so centred text and edge counters cannot split at thirds boundaries.
    bool                  ws_auto_ui_squash = false;

    // [data_shards] funcs: functions that get the memoized pure-function
    // replay entry/return hooks (psx_datashard_enter/psx_datashard_ret).
    // Enhancement-phase load-time work; see docs/DATA_SHARDS.md. Capture is
    // self-proving (byte-verified read-set), so listing a function that turns
    // out to be impure only costs a poisoned capture, never a wrong replay.
    std::vector<uint32_t> data_shard_funcs;

    // [recompiler] mod_function_entry_funcs: narrowly selected guest function
    // entries that dispatch trusted, statically linked mod callbacks. Empty by
    // default, so projects that do not opt in emit no callback overhead.
    std::vector<uint32_t> mod_function_entry_funcs;

    // [recompiler] hot_funcs: guest addresses that get __attribute__((hot))
    // on their generated C bodies (profile/host locality; no guest semantics).
    std::vector<uint32_t> hot_funcs;

    // [recompiler] load_charge_batch: when true, emit function-local cycle
    // accumulators for load_charge_batch_funcs (or hot_funcs if that list is
    // empty). Requires regen; guest totals at IRQ/MMIO barriers unchanged.
    bool                  load_charge_batch = false;
    std::vector<uint32_t> load_charge_batch_funcs;

    // [load_accel.vsync_query] opt-in for a byte-verified PsyQ VSync(mode)
    // implementation.  mode=-1 returns vsync_counter_addr while bypassing two
    // unused MMIO reads; every other mode executes the original function.
    uint32_t              vsync_query_func = 0;
    uint32_t              vsync_counter_addr = 0;
    uint32_t              vsync_gpustat_ptr_addr = 0;
    uint32_t              vsync_timer1_ptr_addr = 0;
    uint32_t              vsync_timer1_cache_addr = 0;
    // Return addresses of verified CD wait-loop VSync(-1) calls.  At these
    // sites only, the runtime may advance guest time to the next deliverable
    // device event while a sustained CD load is active.
    std::vector<uint32_t> vsync_event_horizon_sites;
    // Separately togglable second tier for additional verified loops, allowing
    // per-feature A/B without disabling the accepted base site set.
    std::vector<uint32_t> vsync_event_horizon_extra_sites;
    // When true, any VSync(-1) may event-horizon while CD load/read is busy
    // (not only listed return PCs). For titles whose FMV polls many sites.
    bool                  vsync_event_horizon_any = false;

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
    // Explicit signed lower-bound sites (`slti rt, sx, -W`). The threshold is
    // moved left by the live reveal margin. Empty by default; regen required.
    std::vector<uint32_t> ws_cull_slti_lower_sites;
    // [widescreen.cull] bltz_sites — explicit signed LEFT-edge widen sites
    // (`bltz rs, reject` -> psx_ws_cull_bltz), the counterpart to slti_sites.
    // detect_cull_bltz_sites only classifies left-edge bltz for functions
    // auto_screen_x qualified, so an X-only funnel wired through explicit
    // slti_sites has no left-edge widen without this. Empty by default;
    // identity at 4:3; regen required.
    std::vector<uint32_t> ws_cull_bltz_sites;
    // Horizontal low-edge form `subu rd,zero,rs` -> `-rs-x_margin`.
    // Empty by default; configured sites require regenerated native code.
    std::vector<uint32_t> ws_cull_negsub_sites;
    // `sltiu rt,rs,imm` where rs is an ANDI-masked 16-bit screen X.
    // Widens both edges in 16-bit space; empty by default; regen required.
    std::vector<uint32_t> ws_cull_vxrange_sites;
    // Aspect-scaled slti/sltiu far-bound sites. Empty by default; use only for
    // pure visibility gates. Configured sites require regenerated native code.
    std::vector<uint32_t> ws_cull_depth_sites;
    // `lw rt,off(rs)` sites loading the X component of a side frustum-plane
    // normal feeding a sign test (dot = nx*px + nz*pz). Scaled by the inverse
    // aspect factor (4*den)/(3*num) while revealed, which widens the plane
    // cone by exactly atan((3*num)/(4*den)*tan(theta)); identity at 4:3.
    std::vector<uint32_t> ws_cull_plane_nx_sites;

    // `lw rt,off(rs)` sites loading a per-primitive X-reject bound that is
    // compared (sltu) against ANDI-masked u16 screen X. While the margins are
    // revealed the load yields INT32_MAX (reject disabled; the wide-surface
    // scissor clips the overflow and wrapped off-left coords pass); the
    // vanilla loaded value at 4:3. Empty by default; regen required.
    std::vector<uint32_t> ws_cull_xclip_load_sites;
    // Exact `bltz MAC0, reject`-style NCLIP/backface rejects that are forced
    // not-taken only while widescreen reveals extra world. This is deliberately
    // separate from bltz_sites, whose helper adjusts screen-X edge thresholds.
    std::vector<uint32_t> ws_cull_nclip_keep_sites;
    // Exact branch PCs whose reject path is forced not-taken only while
    // widescreen reveals extra world. Use only after screenshot-validated
    // evidence that the target is a visibility reject.
    std::vector<uint32_t> ws_cull_branch_keep_sites;
    // Exact comparison sites whose result is forced only while widescreen
    // reveals extra world. Used for proven object/model participation gates
    // where maximal overdraw is preferable to range guessing. Each entry is
    // guarded by the complete MIPS word; 4:3 executes the vanilla comparison.
    std::vector<WidescreenCullKeepSite> ws_cull_keep_sites;
    std::vector<WidescreenCullWidenSite> ws_cull_widen_sites;
    // Exact 12-bit angular half-extents used by terrain-cell frusta.
    std::vector<WidescreenAngleSite> ws_cull_angle_sites;
    // Full-word-guarded model-participation cosine compares widened only in
    // the camera-horizontal plane. Empty/default is completely inert.
    WidescreenAspectConeConfig ws_aspect_cone;
    // Extra per-side render/terrain participation beyond the visible edge.
    int                   ws_cull_guard_pixels = 0;
    // Additional per-side lead used only by the explicit bias_sites and
    // range_sites world-space activation windows. This lets a game activate
    // already-resident objects well before the visible edge without widening
    // terrain producers or fixed-capacity render cones by the same amount.
    int                   ws_cull_activation_guard_pixels = 0;

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

    // [widescreen] precise_nclip — use the runtime's unsaturated GTE projection
    // provenance for NCLIP/backface tests while classic adaptive widescreen is
    // active. This is for 3D titles whose wide side geometry otherwise hits the
    // PS1 SXY +/-1024 clamp and then disappears from game-side visibility tests.
    // Runtime-only; off by default.
    bool ws_precise_nclip = false;

    // Optional authoritative game-state gate for titles whose menus also
    // render enough 3D geometry to fool gte_game_mode. When configured,
    // native-wide is active only while the guest word matches one listed
    // value. Runtime-only; both fields must be supplied together.
    uint32_t ws_gameplay_state_addr = 0;
    std::vector<uint32_t> ws_gameplay_state_values;

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

    // Expand only textured polygon vertices that already lie beyond the
    // canonical 4:3 boundary. Useful for finite arena/background meshes while
    // leaving actors, HUD, sprites, and centre geometry untouched.
    bool ws_nw_textured_edges = false;
    int  ws_nw_textured_edge_scale = 0; // percent; 0 = aspect-derived

    // Render the complete wide mirror instead of splicing its centre from
    // canonical VRAM. Required when edge-crossing polygon interpolation is
    // transformed in the mirror.
    bool ws_nw_full_mirror = false;

    // [[widescreen.signed_x_bound]] guarded LUI sites whose signed Q16
    // constants scale with the active native-wide field and remain identity in
    // 4:3/menus/FMV. Shared by static codegen, overlay JIT, and interpreter.
    std::vector<WidescreenSignedBoundSite> ws_signed_x_bound_sites;
    // [widescreen] offer — whether the launcher OFFERS its EXPERIMENTAL
    // Widescreen toggle for this title. Default true. Set false while a
    // title's widescreen is unported/unvalidated (e.g. MMX4 at bring-up):
    // the toggle is hidden in the launcher UI AND the runtime clamps the
    // display aspect to 4:3 after all config sources, so a stale persisted
    // 16:9 in settings.toml can never engage the hack. Runtime-only (read at
    // startup; no codegen impact) — no regen required.
    bool ws_offered = true;
    bool vulkan_offered = false;

    // [widescreen] adaptive_view — let the user opt into a live, resize-driven
    // aspect instead of selecting only fixed 4:3/16:9/21:9 modes. The fixed
    // aspect remains the initial window shape; the live view is clamped to the
    // widest aspect this title offers.
    bool ws_adaptive_view = false;

    // [widescreen] menu_edge_fill — fill the pillarbox margins of a 4:3-pinned
    // present with the frame's own edge columns instead of black. A deliberate
    // look for 2D screens on a very wide display, and WRONG whenever the edge
    // columns carry picture (it smears them across the bars), so it is opt-in
    // per title. Runtime-only (read at startup; no codegen impact).
    bool ws_menu_edge_fill = false;

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
    // Set when the file existed but failed to parse as TOML: every field below
    // is defaults and the caller must warn loudly — silently dropping a user's
    // hand-edited settings looks like the settings "don't work", and a later
    // launcher save would overwrite their file with defaults.
    bool parse_error = false;

    // [video]
    bool has_renderer       = false; int  renderer       = DEFAULT_VIDEO_RENDERER; // 0=software,1=opengl,2=vulkan
    bool has_supersampling  = false; int  supersampling  = 1; // 1..4
    // Window size: width in px; height is always width*3/4 (PSX 4:3). Applies to
    // both the launcher and the emulator window so they boot at the same size.
    bool has_window_width   = false; int  window_width   = 1280; // -> 1280x960
    bool has_antialiasing   = false; bool antialiasing   = true;
    bool has_texture_filter = false; int  texture_filter = 0; // 0=nearest,1=bilinear
    // HD texture pack. Persisted here (not only in game.toml) because both the
    // in-exe recomp-ui toggle and retcomm-launcher's per-title pack manager need
    // to write them, and settings.toml is the file both already co-own.
    //
    // hd_texture_pack is the ACTIVE PACK FOLDER itself, not a parent — that is
    // what a manager selects, and a managed pack is named by its own id rather
    // than after the disc. Empty falls back to the Beetle convention (a
    // <disc stem>-texture-replacements folder beside the disc image), so a
    // hand-dropped pack still works with nothing persisted here. Distinct from
    // RuntimeConfig::video_hd_texture_dir, which relocates the whole convention
    // (both pack and dump) and stays a game.toml/env developer knob.
    bool has_hd_textures     = false; bool hd_textures = false;
    bool has_hd_texture_pack = false; std::filesystem::path hd_texture_pack;
    // FMV present reconstruction (VIDEO_FMV_FILTER_*). Only consulted when
    // antialiasing is on. See RuntimeConfig::video_fmv_filter.
    bool has_fmv_filter     = false; int  fmv_filter     = VIDEO_FMV_FILTER_DEFAULT;
    // Sub-pixel vertex precision / perspective-correct UVs (see RuntimeConfig).
    // Both default off — the faithful floor — and are player-selectable.
    bool has_geometry_correction   = false; bool geometry_correction   = false;
    bool has_perspective_texturing = false; bool perspective_texturing = false;
    bool has_screen_kind    = false; int  screen_kind    = 0; // 0..3 (ScreenKind)
    bool has_auto_skip_fmv  = false; bool auto_skip_fmv  = false; // skip FMVs
    // [video] turbo_loads: DEPRECATED AND IGNORED — the legacy home of the
    // generic Turbo loads switch, back when the launcher drew a row for it.
    // Load acceleration now lives in the Mods catalog (see
    // RuntimeConfig::turbo_loads), so this row is neither restored at startup
    // nor written back out; it survives only to be reported and then dropped
    // the next time settings.toml is saved. Never re-restore it without also
    // restoring a UI control — an unreachable persisted value that overrides a
    // later game.toml change is exactly the write-only latch that shipped
    // turbo-on to MegaManX6Recomp users who had no way to turn it off.
    bool has_turbo_loads    = false; bool turbo_loads    = false;
    bool has_fast_boot      = false; bool fast_boot      = false;
    // HLE BIOS tier toggle (see RuntimeConfig::bios_hle). Overrides game.toml.
    bool has_bios_hle       = false; bool bios_hle       = false;
    // [video] fullscreen: universal tri-state (matches every recomp-ui console's
    // launcher control). 0 = windowed (default), 1 = borderless desktop
    // fullscreen, 2 = exclusive fullscreen (real display-mode change). The
    // in-game Alt+Enter / Cmd+Ctrl+F hotkey toggles live between windowed and
    // whichever of these is configured.
    bool has_fullscreen     = false; int  fullscreen     = 0;
    // Low-latency present knobs. low_latency_input re-samples the pad after the
    // wall-clock pacer (just before present) so the next CPU frame reads fresh
    // input instead of input ~one frame stale (the dominant input->photon cost
    // on a vsync-light box). vsync: 1=on (tear-free), 0=immediate (lowest
    // display latency, may tear), -1=adaptive.
    bool has_low_latency_input = false; bool low_latency_input = true;
    bool has_vsync             = false; int  vsync             = 1;
    bool has_frame_interpolation = false; bool frame_interpolation = false;
    bool has_frame_interpolation_fps = false; int frame_interpolation_fps = 0;
    // [launcher] — when true, boot straight into the game and skip the GUI
    // launcher window (mirrors snesrecomp's SkipLauncher). Overridable per-run:
    // `--launcher` forces the GUI back on; `PSX_NO_LAUNCHER=1` forces it off.
    bool has_skip_launcher  = false; bool skip_launcher  = false;
    // [netplay] — display name for lobby browser / room. Prompted once on first
    // Netplay Lobbies visit; Change Player Name updates it.
    bool has_netplay_player_name = false; std::string netplay_player_name;
    bool has_netplay_lobby_url = false; std::string netplay_lobby_url;
    bool has_aspect_ratio   = false; int  aspect_num     = 4; // display aspect W:H
                                     int  aspect_den     = 3; // (4:3 = native)
    bool has_adaptive_view  = false; bool adaptive_view  = false;
    // [video] rewind_depth: local rewind snap-ring capacity. UI offers
    // 50 / 100 / 150 / 200 (default 50). Runtime clamps + snaps to those steps.
    bool has_rewind_depth   = false; int  rewind_depth   = 50;
    // [video] rewind_interval: frames between local rewind snaps. UI offers
    // 1 / 4 / 8 / 12 / 15 / 30 (default 15; 8 MB soft-default is 30).
    bool has_rewind_interval = false; int rewind_interval = 15;
    // [hotkeys] controller-only host shortcuts. Values use recomp-ui's
    // RECOMP_LAUNCHER_PAD_* encoding (0 = unbound, 1+button, 100+axis).
    bool has_hotkey_pad_rewind = false; int hotkey_pad_rewind = 1272; /* select+r3 */
    bool has_hotkey_pad_save_state_menu = false; int hotkey_pad_save_state_menu = 2040; /* select+r1 */
    // [audio]
    bool has_spu_hq         = false; bool spu_hq         = false;
    bool has_audio_freq     = false; int  audio_freq     = 44100;
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
    // [savestate] dir — where slot .pst files live. Savestates used to be
    // pinned to the memcard dir, which makes the two impossible to place
    // independently. Two builds of one game (e.g. a stock 2 MB build and an
    // 8 MB-mod build) want SHARED memory cards but SEPARATE savestates: the
    // .pst name embeds only entry_pc, identical across builds, so a shared
    // dir collides on filename and every cross-build load is refused by the
    // boot_state codegen_hash guard. Unset => falls back to the memcard dir,
    // preserving the historical layout for every existing project.
    bool has_savestate_dir  = false; std::filesystem::path savestate_dir;

    // [controller] — per-player input device + pad type + deadzone.
    // device is one of:
    //   "none"     — no pad in this port (port not connected)
    //   "keyboard" — driven by the keyboard map (input.ini)
    //   "<GUID>"   — an SDL game-controller GUID (SDL_JoystickGetGUIDString)
    // Modes (see PadMode): analog / digital. Hybrid is mod-only. Defaults: P1 keyboard,
    // P2–P5 none. Deadzone default is 10% (3277/32767). TOML keys:
    // pN_device / pN_mode / pN_deadzone (N=1..5). Legacy bare `deadzone`
    // still fills any slot that lacks pN_deadzone.
    static constexpr int kMaxControllerPlayers = 5;
    bool        has_p_device[kMaxControllerPlayers] = {};
    std::string p_device[kMaxControllerPlayers] = {
        "keyboard", "none", "none", "none", "none"};
    bool has_p_mode[kMaxControllerPlayers] = {};
    int  p_mode[kMaxControllerPlayers] = {
        PAD_MODE_ANALOG, PAD_MODE_ANALOG, PAD_MODE_ANALOG,
        PAD_MODE_ANALOG, PAD_MODE_ANALOG};
    bool has_p_deadzone[kMaxControllerPlayers] = {};
    int  p_deadzone[kMaxControllerPlayers] = {
        3277, 3277, 3277, 3277, 3277};  /* ~10% of 32767 */
    // Legacy global deadzone (settings.toml `deadzone=`). Applied to slots
    // that do not have an explicit pN_deadzone. Default 10%.
    bool has_deadzone  = false; int  deadzone  = 3277;
    // SCPH-1070 multitap for offline 3+ player seats (settings.toml
    // [controller] multitap). Default ON when unset. When false, offline
    // sampling caps at 2 native ports. Netplay lobbies with more than 2
    // seats always arm multitap in the runtime regardless.
    bool has_multitap_enabled = false; bool multitap_enabled = true;
    // DualShock-on-tap hack (settings.toml [controller] multitap_analog).
    // Default on when unset; game.toml / global prefs may override.
    bool has_multitap_analog = false; bool multitap_analog = true;
    // Localization: the launcher's chosen language code (feeds RuntimeConfig
    // .language / g_lang). "off"/"jp"/"" = untranslated native game. Persisted to
    // settings.toml [localization].language.
    bool has_language = false; std::string language = "en";

    bool has_parappa_timing_mode = false; std::string parappa_timing_mode = "stock";
    bool has_parappa_timing_extra_early = false; int parappa_timing_extra_early = 0;
    bool has_parappa_timing_extra_late = false; int parappa_timing_extra_late = 0;
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

// Surgical upsert of `key = true|false` under [controller] in game.toml.
// Preserves comments and unrelated keys. Creates [controller] if missing.
// Used to persist launcher multitap_analog into the title game config.
bool upsert_game_toml_controller_bool(const std::filesystem::path& path,
                                      const char* key, bool value);

// Locate the project root by walking upward from `config_path` until a
// directory containing `.gitignore`, `.git`, or `CMakeLists.txt` is found.
// Throws std::runtime_error if not found within 8 levels.
std::filesystem::path find_project_root(const std::filesystem::path& config_path);

// Load a BIOS config TOML. Throws std::runtime_error on schema violations.
BiosConfig load_bios_config(const std::filesystem::path& config_path);

// Load a game config TOML. Throws std::runtime_error on schema violations.
GameConfig load_game_config(const std::filesystem::path& config_path);

// Hash exactly the per-game settings that can change generated overlay code.
// The runtime and compile_overlays.py (via psxrecomp-game's
// --overlay-config-hash query) use this as part of the cache namespace, so a
// widescreen/patch config edit can never reuse shards emitted under the old
// rules. Runtime-only settings and comments deliberately do not participate.
uint32_t overlay_codegen_config_hash(const GameConfig& config);

} // namespace PSXRecompV4
