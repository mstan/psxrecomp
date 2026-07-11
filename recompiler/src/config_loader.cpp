// config_loader.cpp — see config_loader.h for the contract.

#include "config_loader.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>

#include "fmt/format.h"
#include "ps1_exe_parser.h"

// toml11 is header-only.
#define TOML11_USE_UNRELEASED_TOML_FEATURES
#include "toml.hpp"

namespace PSXRecompV4 {

namespace fs = std::filesystem;

// Pad mode <-> string. Accepts "hybrid"/"analog"/"digital" (case-insensitive);
// returns `fallback` for anything unrecognised so a typo never silently flips
// the pad type.
int pad_mode_from_string(const std::string& s, int fallback) {
    std::string l;
    l.reserve(s.size());
    for (char c : s) l.push_back((char)std::tolower((unsigned char)c));
    if (l == "hybrid")  return PAD_MODE_HYBRID;
    if (l == "analog")  return PAD_MODE_ANALOG;
    if (l == "digital") return PAD_MODE_DIGITAL;
    return fallback;
}

const char* pad_mode_to_string(int mode) {
    switch (mode) {
        case PAD_MODE_ANALOG:  return "analog";
        case PAD_MODE_DIGITAL: return "digital";
        default:               return "hybrid";
    }
}

// Parse a hex string ("0x...") to uint32_t. Throws on malformed input.
static uint32_t parse_hex(const std::string& s, const std::string& field) {
    try {
        return static_cast<uint32_t>(std::stoul(s, nullptr, 16));
    } catch (const std::exception& ex) {
        throw std::runtime_error(
            fmt::format("config field '{}': expected hex string, got '{}' ({})",
                        field, s, ex.what()));
    }
}

// Parse a display aspect "W:H" string (e.g. "4:3", "16:9", "21:9"). Accepts
// only aspects between native 4:3 and 32:9 — narrower than 4:3 has no
// widescreen meaning and ultra-extreme values would squash the GTE X axis
// into unusability. Returns false on malformed input or out-of-range aspect.
static bool parse_aspect_ratio(const std::string& s, int* num, int* den) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size())
        return false;
    int n = 0, d = 0;
    try {
        size_t used = 0;
        n = std::stoi(s.substr(0, colon), &used);
        if (used != colon) return false;
        const std::string rest = s.substr(colon + 1);
        d = std::stoi(rest, &used);
        if (used != rest.size()) return false;
    } catch (const std::exception&) {
        return false;
    }
    if (n <= 0 || d <= 0 || n > 99 || d > 99) return false;
    if (3 * n < 4 * d) return false;   // narrower than 4:3
    if (9 * n > 32 * d) return false;  // wider than 32:9
    *num = n; *den = d;
    return true;
}

// Parse the optional [runtime] block. All fields optional; absent fields
// leave has_* = false on the returned struct. Paths are resolved relative
// to `root` (project root).
static RuntimeConfig parse_runtime_block(const toml::value& cfg, const fs::path& root) {
    RuntimeConfig rt;
    // [localization] language = "en"  (top-level; independent of [runtime]).
    if (cfg.contains("localization")) {
        const toml::value& loc = toml::find(cfg, "localization");
        if (loc.contains("language"))
            rt.language = toml::find<std::string>(loc, "language");
        // `default` is the launcher-facing alias for the pre-selected language.
        if (loc.contains("default"))
            rt.language = toml::find<std::string>(loc, "default");
        // Optional launcher dropdown options: languages = [ {code, label}, ... ].
        if (loc.contains("languages") && toml::find(loc, "languages").is_array()) {
            for (const auto& e : toml::find(loc, "languages").as_array()) {
                if (!e.contains("code")) continue;
                RuntimeConfig::LanguageOption lo;
                lo.code  = toml::find<std::string>(e, "code");
                lo.label = e.contains("label") ? toml::find<std::string>(e, "label")
                                                : lo.code;
                if (!lo.code.empty()) rt.languages.push_back(std::move(lo));
            }
        }
    }
    if (!cfg.contains("runtime")) return rt;
    const toml::value& runtime = toml::find(cfg, "runtime");
    if (runtime.contains("language"))  // [runtime].language convenience alias
        rt.language = toml::find<std::string>(runtime, "language");

    if (runtime.contains("debug_port")) {
        const auto port = toml::find<int64_t>(runtime, "debug_port");
        if (port < 0 || port > 65535) {
            throw std::runtime_error(
                fmt::format("[runtime] debug_port out of range (0..65535): {}", port));
        }
        rt.debug_port = static_cast<uint16_t>(port);
        rt.has_debug_port = true;
    }
    if (runtime.contains("window_title")) {
        rt.window_title = toml::find<std::string>(runtime, "window_title");
        rt.has_window_title = true;
    }
    if (runtime.contains("controller")) {
        rt.controller = toml::find<std::string>(runtime, "controller");
        for (char& c : rt.controller) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (rt.controller != "digital" && rt.controller != "dualshock") {
            throw std::runtime_error(
                fmt::format("[runtime] controller must be 'digital' or 'dualshock', got '{}'",
                            rt.controller));
        }
        rt.has_controller = true;
    }
    if (runtime.contains("memcard_dir")) {
        const auto rel = toml::find<std::string>(runtime, "memcard_dir");
        rt.memcard_dir = fs::absolute(root / rel);
        rt.has_memcard_dir = true;
    }
    if (runtime.contains("disc_speed")) {
        rt.disc_speed     = toml::find<std::string>(runtime, "disc_speed");
        rt.has_disc_speed = true;
    }
    if (runtime.contains("instant_max_per_frame")) {
        const auto n = toml::find<int64_t>(runtime, "instant_max_per_frame");
        if (n < 1 || n > 4096) {
            throw std::runtime_error(fmt::format(
                "[runtime] instant_max_per_frame out of range (1..4096): {}", n));
        }
        rt.instant_max_per_frame     = static_cast<int>(n);
        rt.has_instant_max_per_frame = true;
    }
    if (runtime.contains("fast_boot")) {
        rt.fast_boot = toml::find<bool>(runtime, "fast_boot");
    }
    if (runtime.contains("bios_hle")) {
        rt.bios_hle = toml::find<bool>(runtime, "bios_hle");
    }
    if (runtime.contains("bios_hle_keep_intro")) {
        rt.bios_hle_keep_intro = toml::find<bool>(runtime, "bios_hle_keep_intro");
    }
    if (runtime.contains("hle_scheduler")) {
        rt.hle_scheduler = toml::find<bool>(runtime, "hle_scheduler");
    }
    if (runtime.contains("overlay_cache")) {
        rt.overlay_cache = toml::find<bool>(runtime, "overlay_cache");
    }
    if (runtime.contains("turbo_loads")) {
        rt.turbo_loads = toml::find<bool>(runtime, "turbo_loads");
    }
    if (runtime.contains("overlay_autocompile_cmd")) {
        rt.overlay_autocompile_cmd =
            toml::find<std::string>(runtime, "overlay_autocompile_cmd");
        rt.has_overlay_autocompile_cmd = !rt.overlay_autocompile_cmd.empty();
    }
    if (runtime.contains("overlay_autocompile_cmd_tcc")) {
        rt.overlay_autocompile_cmd_tcc =
            toml::find<std::string>(runtime, "overlay_autocompile_cmd_tcc");
        rt.has_overlay_autocompile_cmd_tcc = !rt.overlay_autocompile_cmd_tcc.empty();
    }
    if (runtime.contains("overlay_backend")) {
        rt.overlay_backend = toml::find<std::string>(runtime, "overlay_backend");
    }
    if (runtime.contains("overlay_native_block")) {
        for (const auto& a : toml::find<std::vector<std::string>>(runtime, "overlay_native_block")) {
            rt.overlay_native_block.push_back(parse_hex(a, "runtime.overlay_native_block"));
        }
    }

    // Optional [video] block — visual enhancement options. Kept on the same
    // RuntimeConfig so main.cpp consumes them alongside the other knobs.
    if (cfg.contains("video")) {
        const toml::value& video = toml::find(cfg, "video");
        if (video.contains("supersampling")) {
            const auto n = toml::find<int64_t>(video, "supersampling");
            if (n < 1 || n > 4) {
                throw std::runtime_error(fmt::format(
                    "[video] supersampling out of range (1..4): {}", n));
            }
            rt.video_supersampling = static_cast<int>(n);
        }
        if (video.contains("antialiasing")) {
            rt.video_antialiasing = toml::find<bool>(video, "antialiasing");
        }
        if (video.contains("texture_filtering")) {
            const auto mode = toml::find<std::string>(video, "texture_filtering");
            if (mode == "nearest")       rt.video_texture_filter = 0;
            else if (mode == "bilinear") rt.video_texture_filter = 1;
            else throw std::runtime_error(fmt::format(
                "[video] texture_filtering must be \"nearest\" or \"bilinear\": {}", mode));
        }
        if (video.contains("renderer")) {
            const auto mode = toml::find<std::string>(video, "renderer");
            if (mode == "software")     rt.video_renderer = 0;
            else if (mode == "opengl")  rt.video_renderer = 1;
            else if (mode == "vulkan")  rt.video_renderer = 2;
            else throw std::runtime_error(fmt::format(
                "[video] renderer must be \"software\", \"opengl\" or \"vulkan\": {}", mode));
        }
        if (video.contains("crt_filter")) {
            const auto mode = toml::find<std::string>(video, "crt_filter");
            if      (mode == "raw")       rt.video_screen_kind = 0;
            else if (mode == "crt")       rt.video_screen_kind = 1;
            else if (mode == "composite") rt.video_screen_kind = 2;
            else if (mode == "trinitron") rt.video_screen_kind = 3;
            else throw std::runtime_error(fmt::format(
                "[video] crt_filter must be \"raw\"|\"crt\"|\"composite\"|\"trinitron\": {}",
                mode));
        }
        if (video.contains("auto_skip_fmv")) {
            rt.video_auto_skip_fmv = toml::find<bool>(video, "auto_skip_fmv");
        }
        if (video.contains("fmv_skip_total_table")) {
            rt.video_fmv_skip_total_table =
                (uint32_t)toml::find<int64_t>(video, "fmv_skip_total_table");
        }
        if (video.contains("fmv_skip_movie_id")) {
            rt.video_fmv_skip_movie_id =
                (uint32_t)toml::find<int64_t>(video, "fmv_skip_movie_id");
        }
        if (video.contains("fmv_skip_end_total")) {
            rt.video_fmv_skip_end_total =
                (int)toml::find<int64_t>(video, "fmv_skip_end_total");
        }
        if (video.contains("fmv_skip_no_xa")) {
            rt.video_fmv_skip_no_xa = toml::find<bool>(video, "fmv_skip_no_xa");
        }
        if (video.contains("fmv_skip_no_xa_hold")) {
            rt.video_fmv_skip_no_xa_hold =
                (int)toml::find<int64_t>(video, "fmv_skip_no_xa_hold");
            if (rt.video_fmv_skip_no_xa_hold < 4 || rt.video_fmv_skip_no_xa_hold > 3600)
                throw std::runtime_error(
                    "[video] fmv_skip_no_xa_hold must be between 4 and 3600 vblanks");
        }
        if (video.contains("low_latency_input")) {
            rt.video_low_latency_input = toml::find<bool>(video, "low_latency_input");
        }
        if (video.contains("vsync")) {
            const auto mode = toml::find<std::string>(video, "vsync");
            if      (mode == "on"  || mode == "vsync")     rt.video_vsync = 1;
            else if (mode == "off" || mode == "immediate") rt.video_vsync = 0;
            else if (mode == "adaptive")                   rt.video_vsync = -1;
            else throw std::runtime_error(fmt::format(
                "[video] vsync must be \"on\"|\"off\"|\"immediate\"|\"adaptive\": {}", mode));
        }
        if (video.contains("aspect_ratio")) {
            const auto mode = toml::find<std::string>(video, "aspect_ratio");
            int n = 0, d = 0;
            if (!parse_aspect_ratio(mode, &n, &d))
                throw std::runtime_error(fmt::format(
                    "[video] aspect_ratio must be \"W:H\", no narrower than 4:3 "
                    "and no wider than 32:9 (e.g. \"4:3\", \"16:9\"): {}", mode));
            rt.video_aspect_num = n;
            rt.video_aspect_den = d;
        }
    }

    // Optional [audio] block.
    if (cfg.contains("audio")) {
        const toml::value& audio = toml::find(cfg, "audio");
        if (audio.contains("spu_hq")) {
            rt.audio_spu_hq = toml::find<bool>(audio, "spu_hq");
        }
    }

    // Optional [controller] block — game-declared input defaults.
    if (cfg.contains("controller")) {
        const toml::value& ct = toml::find(cfg, "controller");
        // Legacy boolean form (true->analog, false->digital), read first so the
        // new string `*_mode` keys win when both are present.
        if (ct.contains("default_analog")) {
            const int m = toml::find<bool>(ct, "default_analog")
                              ? PAD_MODE_ANALOG : PAD_MODE_DIGITAL;
            rt.default_p1_mode = rt.default_p2_mode = m;
            rt.has_default_mode = true;
        }
        if (ct.contains("p1_analog")) {
            rt.default_p1_mode = toml::find<bool>(ct, "p1_analog")
                                     ? PAD_MODE_ANALOG : PAD_MODE_DIGITAL;
            rt.has_default_mode = true;
        }
        if (ct.contains("p2_analog")) {
            rt.default_p2_mode = toml::find<bool>(ct, "p2_analog")
                                     ? PAD_MODE_ANALOG : PAD_MODE_DIGITAL;
            rt.has_default_mode = true;
        }
        // Preferred string form.
        if (ct.contains("default_mode")) {
            const int m = pad_mode_from_string(
                toml::find<std::string>(ct, "default_mode"), PAD_MODE_HYBRID);
            rt.default_p1_mode = rt.default_p2_mode = m;
            rt.has_default_mode = true;
        }
        if (ct.contains("p1_mode")) {
            rt.default_p1_mode = pad_mode_from_string(
                toml::find<std::string>(ct, "p1_mode"), PAD_MODE_HYBRID);
            rt.has_default_mode = true;
        }
        if (ct.contains("p2_mode")) {
            rt.default_p2_mode = pad_mode_from_string(
                toml::find<std::string>(ct, "p2_mode"), PAD_MODE_HYBRID);
            rt.has_default_mode = true;
        }
        if (ct.contains("allow_hybrid")) {
            rt.controller_allow_hybrid = toml::find<bool>(ct, "allow_hybrid");
        }
        if (ct.contains("lock_mode")) {
            rt.controller_lock_mode = toml::find<bool>(ct, "lock_mode");
        }
        if (ct.contains("lock_device")) {
            rt.controller_lock_device = toml::find<bool>(ct, "lock_device");
        }
        if (ct.contains("deadzone")) {
            const auto n = toml::find<int64_t>(ct, "deadzone");
            if (n < 0 || n > 32767)
                throw std::runtime_error(fmt::format(
                    "[controller] deadzone out of range (0..32767): {}", n));
            rt.deadzone = static_cast<int>(n);
            rt.has_deadzone = true;
        }
        // LEGACY per-game pad-config opt-in (default modern). Tomba sets this so
        // its launcher Hybrid mode's analog<->digital type flip doesn't make libpad
        // manufacture a disconnect; no other title is affected. Full history and
        // the removal plan live in psxrecomp runtime/src/sio.c (g_pad_legacy_cfg).
        if (ct.contains("legacy_pad_config")) {
            rt.legacy_pad_config = toml::find<bool>(ct, "legacy_pad_config");
        }
    }

    return rt;
}

fs::path find_project_root(const fs::path& config_path) {
    fs::path cur = fs::absolute(config_path).parent_path();
    const fs::path fallback = cur;
    for (int i = 0; i < 8; ++i) {
        for (const char* marker : { ".gitignore", ".git", "CMakeLists.txt" }) {
            if (fs::exists(cur / marker)) {
                return cur;
            }
        }
        const fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return fallback;
}

// Derive the output filename stem from a rom basename. Mirrors the Python
// audit_config.py logic: strip a trailing .BIN/.EXE (case-insensitive) but
// preserve dotted names like "SCUS_942.36" unchanged.
static std::string derive_out_stem(const std::string& rom_basename) {
    auto ends_with_ci = [](const std::string& s, const std::string& suffix) {
        if (s.size() < suffix.size()) return false;
        for (size_t i = 0; i < suffix.size(); ++i) {
            char a = s[s.size() - suffix.size() + i];
            char b = suffix[i];
            if (std::tolower(static_cast<unsigned char>(a)) !=
                std::tolower(static_cast<unsigned char>(b))) return false;
        }
        return true;
    };
    if (ends_with_ci(rom_basename, ".bin") || ends_with_ci(rom_basename, ".exe")) {
        return rom_basename.substr(0, rom_basename.size() - 4);
    }
    return rom_basename;
}

BiosConfig load_bios_config(const fs::path& config_path_in) {
    const fs::path config_path = fs::absolute(config_path_in);
    if (!fs::exists(config_path)) {
        throw std::runtime_error(
            fmt::format("config file not found: {}", config_path.string()));
    }

    const fs::path root = find_project_root(config_path);

    toml::value cfg;
    try {
        cfg = toml::parse(config_path);
    } catch (const toml::syntax_error& ex) {
        throw std::runtime_error(
            fmt::format("TOML syntax error in {}: {}", config_path.string(), ex.what()));
    }

    // [program] — bios.toml uses this; some legacy files use [game]
    const toml::value* prog_ptr = nullptr;
    if (cfg.contains("program")) {
        prog_ptr = &toml::find(cfg, "program");
    } else if (cfg.contains("game")) {
        prog_ptr = &toml::find(cfg, "game");
    } else {
        throw std::runtime_error(
            fmt::format("{}: missing [program] (or [game]) block",
                        config_path.string()));
    }
    const toml::value& prog = *prog_ptr;

    const std::string name = toml::find<std::string>(prog, "name");
    const std::string id   = prog.contains("id")
                                ? toml::find<std::string>(prog, "id")
                                : std::string{};

    // BIOS uses `rom`; game files use `exe`. Either is accepted here, but
    // BIOS callers should pass a bios config that has `rom`.
    std::string rom_field;
    if (prog.contains("rom")) {
        rom_field = toml::find<std::string>(prog, "rom");
    } else if (prog.contains("exe")) {
        rom_field = toml::find<std::string>(prog, "exe");
    } else {
        throw std::runtime_error(
            fmt::format("{}: [program] missing 'rom' or 'exe' field",
                        config_path.string()));
    }
    const fs::path rom_path = fs::absolute(root / rom_field);

    const uint32_t load_address =
        parse_hex(toml::find<std::string>(prog, "load_address"), "program.load_address");
    const uint32_t entry_pc =
        prog.contains("entry_pc")
            ? parse_hex(toml::find<std::string>(prog, "entry_pc"), "program.entry_pc")
            : load_address;
    const uint32_t text_size =
        parse_hex(toml::find<std::string>(prog, "text_size"), "program.text_size");

    // [recompiler]
    if (!cfg.contains("recompiler")) {
        throw std::runtime_error(
            fmt::format("{}: missing [recompiler] block", config_path.string()));
    }
    const toml::value& recomp = toml::find(cfg, "recompiler");

    if (!recomp.contains("seeds")) {
        throw std::runtime_error(
            fmt::format("{}: [recompiler] missing 'seeds' field", config_path.string()));
    }
    const std::string seeds_field = toml::find<std::string>(recomp, "seeds");
    const fs::path seeds_path = fs::absolute(root / seeds_field);

    const std::string out_dir_field =
        recomp.contains("out_dir")
            ? toml::find<std::string>(recomp, "out_dir")
            : std::string{"generated"};
    const fs::path out_dir = fs::absolute(root / out_dir_field);

    const bool strict = recomp.contains("strict")
                            ? toml::find<bool>(recomp, "strict")
                            : true;

    std::string out_stem;
    if (recomp.contains("out_stem")) {
        out_stem = toml::find<std::string>(recomp, "out_stem");
    } else {
        out_stem = derive_out_stem(fs::path(rom_field).filename().string());
    }

    // [[recompiler.bios_vectors]] — optional array of vector dispatch tables
    std::vector<BiosVectorTable> bios_vectors;
    if (recomp.contains("bios_vectors")) {
        const auto& arr = recomp.at("bios_vectors").as_array();
        for (const auto& v : arr) {
            BiosVectorTable bvt;
            bvt.ram_addr = parse_hex(
                toml::find<std::string>(v, "ram_addr"), "bios_vectors.ram_addr");
            bvt.index_reg = static_cast<int>(
                toml::find<int64_t>(v, "index_reg"));
            bvt.table_rom_addr = parse_hex(
                toml::find<std::string>(v, "table_rom_addr"), "bios_vectors.table_rom_addr");
            bvt.table_count = static_cast<uint32_t>(
                toml::find<int64_t>(v, "table_count"));
            bvt.table_ram_addr = v.contains("table_ram_addr")
                ? parse_hex(toml::find<std::string>(v, "table_ram_addr"),
                            "bios_vectors.table_ram_addr")
                : 0u;
            bios_vectors.push_back(bvt);
        }
    }

    // [[recompiler.bios_aliases]] — fixed-target RAM trampolines
    std::vector<BiosAlias> bios_aliases;
    if (recomp.contains("bios_aliases")) {
        const auto& arr = recomp.at("bios_aliases").as_array();
        for (const auto& v : arr) {
            BiosAlias ba;
            ba.ram_addr   = parse_hex(toml::find<std::string>(v, "ram_addr"),
                                      "bios_aliases.ram_addr");
            ba.target_key = parse_hex(toml::find<std::string>(v, "target_key"),
                                      "bios_aliases.target_key");
            bios_aliases.push_back(ba);
        }
    }

    return BiosConfig{
        /*config_path*/  config_path,
        /*project_root*/ root,
        /*name*/         name,
        /*id*/           id,
        /*rom_path*/     rom_path,
        /*load_address*/ load_address,
        /*entry_pc*/     entry_pc,
        /*text_size*/    text_size,
        /*seeds_path*/   seeds_path,
        /*out_dir*/      out_dir,
        /*strict*/       strict,
        /*out_stem*/     out_stem,
        /*bios_vectors*/ std::move(bios_vectors),
        /*bios_aliases*/ std::move(bios_aliases),
        /*runtime*/      parse_runtime_block(cfg, root),
    };
}

GameConfig load_game_config(const fs::path& config_path_in) {
    const fs::path config_path = fs::absolute(config_path_in);
    if (!fs::exists(config_path)) {
        throw std::runtime_error(
            fmt::format("game config not found: {}", config_path.string()));
    }
    const fs::path root = find_project_root(config_path);

    toml::value cfg;
    try {
        cfg = toml::parse(config_path);
    } catch (const toml::syntax_error& ex) {
        throw std::runtime_error(
            fmt::format("TOML syntax error in {}: {}", config_path.string(), ex.what()));
    }

    // [game] (preferred for game configs) or [program]
    const toml::value* prog_ptr = nullptr;
    if (cfg.contains("game")) {
        prog_ptr = &toml::find(cfg, "game");
    } else if (cfg.contains("program")) {
        prog_ptr = &toml::find(cfg, "program");
    } else {
        throw std::runtime_error(
            fmt::format("{}: missing [game] (or [program]) block",
                        config_path.string()));
    }
    const toml::value& game = *prog_ptr;

    const std::string name = toml::find<std::string>(game, "name");
    const std::string id   = game.contains("id")
                                ? toml::find<std::string>(game, "id")
                                : std::string{};

    std::string exe_field;
    if (game.contains("exe")) {
        exe_field = toml::find<std::string>(game, "exe");
    } else if (game.contains("rom")) {
        exe_field = toml::find<std::string>(game, "rom");
    } else {
        throw std::runtime_error(
            fmt::format("{}: [game] missing 'exe' or 'rom' field", config_path.string()));
    }
    const fs::path exe_path = fs::absolute(root / exe_field);

    // Auto-detect EXE header values for any field not explicitly set in TOML.
    // Parses the PS-X EXE header once and fills in load_address, entry_pc,
    // text_size, and stack_base from the binary. If the EXE cannot be read
    // (malformed header, file not found) and a required field is missing
    // from TOML, the error surfaces when the EXE parse itself fails.
    struct ExeHeaderAuto {
        bool ok = false;
        uint32_t load_address = 0;
        uint32_t entry_pc = 0;
        uint32_t text_size = 0;
        uint32_t stack_base = 0;
    };
    ExeHeaderAuto auto_hdr;
    // We only parse the EXE header if at least one field is missing from TOML.
    const bool need_auto =
    !game.contains("load_address") || !game.contains("entry_pc") ||
    !game.contains("text_size") || !game.contains("stack_base");
    if (need_auto) {
        std::string err;
        if (auto exe = PSXRecomp::PS1ExeParser::parse_file(exe_path, err)) {
            auto_hdr.ok = true;
            auto_hdr.load_address = exe->load_address();
            auto_hdr.entry_pc     = exe->entry_point();
            auto_hdr.text_size    = exe->code_size();
            auto_hdr.stack_base   = exe->header.stack_base;
            // Round text_size up to next 4K page if it's not already
            // page-aligned (the runtime's overlay region floor uses this
            // as a guard, and the text segment in RAM is always fully
            // page-reserved even if the EXE doesn't fill the last page).
            if (auto_hdr.text_size & 0xFFF) {
                auto_hdr.text_size = (auto_hdr.text_size + 0xFFF) & ~0xFFFu;
            }
        }
    }

    const uint32_t load_address =
        game.contains("load_address")
            ? parse_hex(toml::find<std::string>(game, "load_address"), "game.load_address")
            : (auto_hdr.ok ? auto_hdr.load_address : []() -> uint32_t {
                throw std::runtime_error(
                    "game.toml: missing 'load_address' in [game] and could not "
                    "auto-detect from EXE header");
              }());
    const uint32_t entry_pc =
        game.contains("entry_pc")
            ? parse_hex(toml::find<std::string>(game, "entry_pc"), "game.entry_pc")
            : (auto_hdr.ok ? auto_hdr.entry_pc : load_address);
    const uint32_t text_size =
        game.contains("text_size")
            ? parse_hex(toml::find<std::string>(game, "text_size"), "game.text_size")
            : (auto_hdr.ok ? auto_hdr.text_size : []() -> uint32_t {
                throw std::runtime_error(
                    "game.toml: missing 'text_size' in [game] and could not "
                    "auto-detect from EXE header");
              }());
    const uint32_t stack_base =
        game.contains("stack_base")
            ? parse_hex(toml::find<std::string>(game, "stack_base"), "game.stack_base")
            : (auto_hdr.ok && auto_hdr.stack_base != 0
                   ? auto_hdr.stack_base
                   : 0x801FFFF0u);

    // Disc paths: accept either single `disc` or array `discs`.
    std::vector<fs::path> discs;
    if (game.contains("discs")) {
        const auto& arr = toml::find<std::vector<std::string>>(game, "discs");
        for (const auto& d : arr) discs.push_back(fs::absolute(root / d));
    } else if (game.contains("disc")) {
        const auto& d = toml::find<std::string>(game, "disc");
        discs.push_back(fs::absolute(root / d));
    }

    // Optional expected disc identity (launcher verification badge).
    bool has_disc_crc = false;
    uint32_t disc_crc = 0;
    std::string disc_sha1;
    if (game.contains("disc_crc")) {
        disc_crc = parse_hex(toml::find<std::string>(game, "disc_crc"), "game.disc_crc");
        has_disc_crc = true;
    }
    if (game.contains("disc_sha1")) {
        disc_sha1 = toml::find<std::string>(game, "disc_sha1");
        // normalize to lowercase hex
        for (char& c : disc_sha1) c = (char)std::tolower((unsigned char)c);
    }

    // [recompiler]
    if (!cfg.contains("recompiler")) {
        throw std::runtime_error(
            fmt::format("{}: missing [recompiler] block", config_path.string()));
    }
    const toml::value& recomp = toml::find(cfg, "recompiler");

    if (!recomp.contains("seeds")) {
        throw std::runtime_error(
            fmt::format("{}: [recompiler] missing 'seeds' field", config_path.string()));
    }
    const fs::path seeds_path =
        fs::absolute(root / toml::find<std::string>(recomp, "seeds"));

    fs::path bios_thunks_path;
    if (recomp.contains("bios_thunks")) {
        bios_thunks_path =
            fs::absolute(root / toml::find<std::string>(recomp, "bios_thunks"));
    }

    const std::string out_dir_field =
        recomp.contains("out_dir")
            ? toml::find<std::string>(recomp, "out_dir")
            : std::string{"generated"};
    const fs::path out_dir = fs::absolute(root / out_dir_field);

    const bool strict = recomp.contains("strict")
                            ? toml::find<bool>(recomp, "strict")
                            : true;

    std::string out_stem;
    if (recomp.contains("out_stem")) {
        out_stem = toml::find<std::string>(recomp, "out_stem");
    } else {
        out_stem = derive_out_stem(fs::path(exe_field).filename().string());
    }

    // Optional [widescreen] block — per-game hooks for the widescreen hack.
    std::vector<uint32_t> ws_sprite_tag_funcs;
    uint32_t ws_sprite_anchor_addr = 0;
    bool ws_hud_sprt_squash = false;
    bool ws_full_2d = false;
    bool ws_gte_game_mode = false;
    bool ws_native_wide = true;
    bool ws_nw_hud_corners = false;
    uint32_t ws_nw_left_hud_packet_lo = 0;
    uint32_t ws_nw_left_hud_packet_hi = 0;
    bool ws_nw_backdrop = false;
    bool ws_clear_reveal = false;
    bool ws_nw_flat_backdrop = false;
    bool ws_nw_phase_backdrop = false;
    bool ws_offered = true;
    bool ws_ultrawide_offered = false;
    if (cfg.contains("widescreen")) {
        const toml::value& ws = toml::find(cfg, "widescreen");
        if (ws.contains("sprite_tag_funcs")) {
            const auto& arr = toml::find<std::vector<std::string>>(ws, "sprite_tag_funcs");
            for (const auto& a : arr)
                ws_sprite_tag_funcs.push_back(
                    parse_hex(a, "widescreen.sprite_tag_funcs"));
        }
        if (ws.contains("sprite_anchor_addr")) {
            ws_sprite_anchor_addr = parse_hex(
                toml::find<std::string>(ws, "sprite_anchor_addr"),
                "widescreen.sprite_anchor_addr");
        }
        if (!ws_sprite_tag_funcs.empty() && ws_sprite_anchor_addr == 0)
            throw std::runtime_error(fmt::format(
                "{}: [widescreen] sprite_tag_funcs requires sprite_anchor_addr",
                config_path.string()));
        if (ws.contains("hud_sprt_squash"))
            ws_hud_sprt_squash = toml::find<bool>(ws, "hud_sprt_squash");
        if (ws.contains("full_2d"))
            ws_full_2d = toml::find<bool>(ws, "full_2d");
        if (ws.contains("gte_game_mode"))
            ws_gte_game_mode = toml::find<bool>(ws, "gte_game_mode");
        if (ws.contains("native_wide"))
            ws_native_wide = toml::find<bool>(ws, "native_wide");
        if (ws.contains("nw_hud_corners"))
            ws_nw_hud_corners = toml::find<bool>(ws, "nw_hud_corners");
        const bool has_nw_left_hud_lo = ws.contains("nw_left_hud_packet_lo");
        const bool has_nw_left_hud_hi = ws.contains("nw_left_hud_packet_hi");
        if (has_nw_left_hud_lo != has_nw_left_hud_hi)
            throw std::runtime_error(fmt::format(
                "{}: [widescreen] nw_left_hud_packet_lo and "
                "nw_left_hud_packet_hi must be set together",
                config_path.string()));
        if (has_nw_left_hud_lo) {
            ws_nw_left_hud_packet_lo = parse_hex(
                toml::find<std::string>(ws, "nw_left_hud_packet_lo"),
                "widescreen.nw_left_hud_packet_lo");
            ws_nw_left_hud_packet_hi = parse_hex(
                toml::find<std::string>(ws, "nw_left_hud_packet_hi"),
                "widescreen.nw_left_hud_packet_hi");
            if (ws_nw_left_hud_packet_lo >= ws_nw_left_hud_packet_hi)
                throw std::runtime_error(fmt::format(
                    "{}: [widescreen] nw_left_hud_packet range is empty or reversed",
                    config_path.string()));
        }
        if (ws.contains("nw_backdrop"))
            ws_nw_backdrop = toml::find<bool>(ws, "nw_backdrop");
        if (ws.contains("clear_reveal"))
            ws_clear_reveal = toml::find<bool>(ws, "clear_reveal");
        if (ws.contains("nw_flat_backdrop"))
            ws_nw_flat_backdrop = toml::find<bool>(ws, "nw_flat_backdrop");
        if (ws.contains("nw_phase_backdrop"))
            ws_nw_phase_backdrop = toml::find<bool>(ws, "nw_phase_backdrop");
        if (ws.contains("offer"))
            ws_offered = toml::find<bool>(ws, "offer");
        if (ws.contains("offer_ultrawide"))
            ws_ultrawide_offered = toml::find<bool>(ws, "offer_ultrawide");
    }

    // Optional [widescreen.cull] block — world-space draw-cull widening.
    std::vector<uint32_t> ws_cull_bias_sites, ws_cull_range_sites, ws_cull_a1_sites;
    std::vector<uint32_t> ws_cull_screen_x_sites;
    std::vector<uint32_t> ws_cull_slti_sites;
    int ws_cull_guard_pixels = 0;
    // Cull-signature immediates (screen_w_imms / screen_h_imms). Defaults are
    // the original Tomba signature (320-display: 0x140/0x141 + 0xE0/0xF1); a
    // game with a different display width overrides them (Ape Escape: 0x181).
    std::vector<uint32_t> ws_cull_w_imms = { 0x140, 0x141 };
    std::vector<uint32_t> ws_cull_h_imms = { 0xE0, 0xF1 };
    bool ws_auto_screen_x_cull = false;
    bool ws_auto_backdrop_preload = false;
    if (cfg.contains("widescreen")) {
        const toml::value& ws = toml::find(cfg, "widescreen");
        if (ws.contains("cull")) {
            const toml::value& cull = toml::find(ws, "cull");
            auto load_sites = [&](const char* key, std::vector<uint32_t>& out) {
                if (!cull.contains(key)) return;
                for (const auto& a : toml::find<std::vector<std::string>>(cull, key))
                    out.push_back(parse_hex(a, fmt::format("widescreen.cull.{}", key)));
            };
            load_sites("bias_sites",  ws_cull_bias_sites);
            load_sites("range_sites", ws_cull_range_sites);
            load_sites("a1_sites",    ws_cull_a1_sites);
            load_sites("screen_x_sites", ws_cull_screen_x_sites);
            load_sites("slti_sites",  ws_cull_slti_sites);
            if (cull.contains("guard_pixels")) {
                ws_cull_guard_pixels = toml::find<int>(cull, "guard_pixels");
                if (ws_cull_guard_pixels < 0 || ws_cull_guard_pixels > 256)
                    throw std::runtime_error(fmt::format(
                        "{}: [widescreen.cull] guard_pixels must be in [0, 256]",
                        config_path.string()));
            }
            if (cull.contains("screen_w_imms")) {
                ws_cull_w_imms.clear();
                load_sites("screen_w_imms", ws_cull_w_imms);
            }
            if (cull.contains("screen_h_imms")) {
                ws_cull_h_imms.clear();
                load_sites("screen_h_imms", ws_cull_h_imms);
            }
            if (cull.contains("auto_screen_x"))
                ws_auto_screen_x_cull = toml::find<bool>(cull, "auto_screen_x");
            if (cull.contains("auto_backdrop"))
                ws_auto_backdrop_preload = toml::find<bool>(cull, "auto_backdrop");
        }
    }

    // Optional [widescreen.bg2d] block — pure-2D background tile-loop widen.
    uint32_t ws_bg2d_count_site = 0, ws_bg2d_startcol_site = 0, ws_bg2d_startx_site = 0;
    uint32_t ws_bg2d_stream_left_site = 0, ws_bg2d_stream_right_site = 0;
    uint32_t ws_bg2d_bufbase_site = 0, ws_bg2d_cap_site = 0;
    uint32_t ws_bg2d_layer_base = 0x800971F8u, ws_bg2d_ring_base = 0x800A21B8u;
    uint32_t ws_bg2d_map_size_addr = 0x800CD338u, ws_bg2d_layer_stride_addr = 0x8008EC10u;
    uint32_t ws_bg2d_ring_cols = 64, ws_bg2d_layer_count = 3;
    uint32_t ws_bg2d_layer_struct_stride = 0x54;
    uint32_t ws_bg2d_init_func = 0;
    uint32_t ws_bg2d_packet_cap = 1000;
    if (cfg.contains("widescreen")) {
        const toml::value& ws = toml::find(cfg, "widescreen");
        if (ws.contains("bg2d")) {
            const toml::value& bg = toml::find(ws, "bg2d");
            auto load1 = [&](const char* key) -> uint32_t {
                return bg.contains(key)
                    ? parse_hex(toml::find<std::string>(bg, key),
                                fmt::format("widescreen.bg2d.{}", key))
                    : 0u;
            };
            ws_bg2d_count_site    = load1("count_site");
            ws_bg2d_startcol_site = load1("startcol_site");
            ws_bg2d_startx_site   = load1("startx_site");
            ws_bg2d_stream_left_site  = load1("stream_left_site");
            ws_bg2d_stream_right_site = load1("stream_right_site");
            ws_bg2d_bufbase_site = load1("bufbase_site");
            ws_bg2d_cap_site     = load1("cap_site");
            if (bg.contains("layer_base"))
                ws_bg2d_layer_base = load1("layer_base");
            if (bg.contains("ring_base"))
                ws_bg2d_ring_base = load1("ring_base");
            if (bg.contains("map_size_addr"))
                ws_bg2d_map_size_addr = load1("map_size_addr");
            if (bg.contains("layer_stride_addr"))
                ws_bg2d_layer_stride_addr = load1("layer_stride_addr");
            auto load_positive = [&](const char* key, uint32_t current) -> uint32_t {
                if (!bg.contains(key)) return current;
                const int64_t value = toml::find<int64_t>(bg, key);
                if (value <= 0 || value > UINT32_MAX)
                    throw std::runtime_error(fmt::format(
                        "widescreen.bg2d.{} must be a positive 32-bit integer", key));
                return static_cast<uint32_t>(value);
            };
            ws_bg2d_ring_cols = load_positive("ring_cols", ws_bg2d_ring_cols);
            ws_bg2d_layer_count = load_positive("layer_count", ws_bg2d_layer_count);
            ws_bg2d_layer_struct_stride = load_positive(
                "layer_struct_stride", ws_bg2d_layer_struct_stride);
            ws_bg2d_packet_cap = load_positive("packet_cap", ws_bg2d_packet_cap);
            if ((ws_bg2d_ring_cols & (ws_bg2d_ring_cols - 1u)) != 0u)
                throw std::runtime_error(
                    "widescreen.bg2d.ring_cols must be a power of two");
            ws_bg2d_init_func    = load1("init_func");
        }
    }

    // Optional [widescreen.backdrop] block — parallax 2D backdrop screenX squash
    // (x_sites) + far-backdrop GTE un-squash (unsquash_funcs).
    std::vector<uint32_t> ws_backdrop_x_sites;
    std::vector<uint32_t> ws_backdrop_unsquash_funcs;
    std::vector<uint32_t> ws_dome_call_sites;
    if (cfg.contains("widescreen")) {
        const toml::value& ws = toml::find(cfg, "widescreen");
        if (ws.contains("backdrop")) {
            const toml::value& bd = toml::find(ws, "backdrop");
            if (bd.contains("x_sites"))
                for (const auto& a : toml::find<std::vector<std::string>>(bd, "x_sites"))
                    ws_backdrop_x_sites.push_back(
                        parse_hex(a, "widescreen.backdrop.x_sites"));
            if (bd.contains("unsquash_funcs"))
                for (const auto& a : toml::find<std::vector<std::string>>(bd, "unsquash_funcs"))
                    ws_backdrop_unsquash_funcs.push_back(
                        parse_hex(a, "widescreen.backdrop.unsquash_funcs"));
        }
    }

    // Optional [widescreen.dome] block: exact projection calls belonging to
    // finite curved background meshes that retain authored 4:3 coverage.
    if (cfg.contains("widescreen")) {
        const toml::value& ws = toml::find(cfg, "widescreen");
        if (ws.contains("dome")) {
            const toml::value& dome = toml::find(ws, "dome");
            if (dome.contains("call_sites"))
                for (const auto& a : toml::find<std::vector<std::string>>(dome, "call_sites"))
                    ws_dome_call_sites.push_back(
                        parse_hex(a, "widescreen.dome.call_sites"));
        }
    }

    return GameConfig{
        /*config_path*/      config_path,
        /*project_root*/     root,
        /*name*/             name,
        /*id*/               id,
        /*exe_path*/         exe_path,
        /*load_address*/     load_address,
        /*entry_pc*/         entry_pc,
        /*text_size*/        text_size,
        /*stack_base*/       stack_base,
        /*discs*/            discs,
        /*has_disc_crc*/     has_disc_crc,
        /*disc_crc*/         disc_crc,
        /*disc_sha1*/        disc_sha1,
        /*seeds_path*/       seeds_path,
        /*bios_thunks_path*/ bios_thunks_path,
        /*out_dir*/          out_dir,
        /*strict*/           strict,
        /*out_stem*/         out_stem,
        /*runtime*/          parse_runtime_block(cfg, root),
        /*ws_sprite_tag_funcs*/   ws_sprite_tag_funcs,
        /*ws_sprite_anchor_addr*/ ws_sprite_anchor_addr,
        /*ws_hud_sprt_squash*/    ws_hud_sprt_squash,
        /*ws_cull_bias_sites*/    ws_cull_bias_sites,
        /*ws_cull_range_sites*/   ws_cull_range_sites,
        /*ws_cull_a1_sites*/      ws_cull_a1_sites,
        /*ws_cull_screen_x_sites*/ ws_cull_screen_x_sites,
        /*ws_cull_slti_sites*/    ws_cull_slti_sites,
        /*ws_cull_guard_pixels*/  ws_cull_guard_pixels,
        /*ws_cull_w_imms*/        ws_cull_w_imms,
        /*ws_cull_h_imms*/        ws_cull_h_imms,
        /*ws_backdrop_x_sites*/   ws_backdrop_x_sites,
        /*ws_backdrop_unsquash_funcs*/ ws_backdrop_unsquash_funcs,
        /*ws_dome_call_sites*/    ws_dome_call_sites,
        /*ws_auto_screen_x_cull*/ ws_auto_screen_x_cull,
        /*ws_auto_backdrop_preload*/ ws_auto_backdrop_preload,
        /*ws_full_2d*/            ws_full_2d,
        /*ws_gte_game_mode*/      ws_gte_game_mode,
        /*ws_native_wide*/        ws_native_wide,
        /*ws_nw_hud_corners*/     ws_nw_hud_corners,
        /*ws_nw_left_hud_packet_lo*/ ws_nw_left_hud_packet_lo,
        /*ws_nw_left_hud_packet_hi*/ ws_nw_left_hud_packet_hi,
        /*ws_nw_backdrop*/        ws_nw_backdrop,
        /*ws_clear_reveal*/       ws_clear_reveal,
        /*ws_nw_flat_backdrop*/   ws_nw_flat_backdrop,
        /*ws_nw_phase_backdrop*/  ws_nw_phase_backdrop,
        /*ws_offered*/            ws_offered,
        /*ws_ultrawide_offered*/  ws_ultrawide_offered,
        /*ws_bg2d_count_site*/    ws_bg2d_count_site,
        /*ws_bg2d_startcol_site*/ ws_bg2d_startcol_site,
        /*ws_bg2d_startx_site*/   ws_bg2d_startx_site,
        /*ws_bg2d_stream_left_site*/  ws_bg2d_stream_left_site,
        /*ws_bg2d_stream_right_site*/ ws_bg2d_stream_right_site,
        /*ws_bg2d_bufbase_site*/  ws_bg2d_bufbase_site,
        /*ws_bg2d_cap_site*/      ws_bg2d_cap_site,
        /*ws_bg2d_layer_base*/    ws_bg2d_layer_base,
        /*ws_bg2d_ring_base*/     ws_bg2d_ring_base,
        /*ws_bg2d_map_size_addr*/ ws_bg2d_map_size_addr,
        /*ws_bg2d_layer_stride_addr*/ ws_bg2d_layer_stride_addr,
        /*ws_bg2d_ring_cols*/     ws_bg2d_ring_cols,
        /*ws_bg2d_layer_count*/   ws_bg2d_layer_count,
        /*ws_bg2d_layer_struct_stride*/ ws_bg2d_layer_struct_stride,
        /*ws_bg2d_init_func*/     ws_bg2d_init_func,
        /*ws_bg2d_packet_cap*/    ws_bg2d_packet_cap,
    };
}

// ---- GameOptions (game_options.toml) — the game's own native settings ----

GameOptions load_game_options(const fs::path& path) {
    GameOptions go;
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return go;

    const toml::value doc = toml::parse(path.string());
    if (!doc.contains("option")) return go;
    const auto& arr = toml::find<toml::array>(doc, "option");
    for (const auto& item : arr) {
        GameOption o;
        o.name = toml::find<std::string>(item, "name");
        o.addr = parse_hex(toml::find<std::string>(item, "addr"),
                           fmt::format("game_options [[option]] '{}' addr", o.name));
        o.size = static_cast<int>(toml::find<int64_t>(item, "size"));
        if (o.size != 1 && o.size != 2)
            throw std::runtime_error(fmt::format(
                "game_options [[option]] '{}' size must be 1 or 2: {}", o.name, o.size));
        if (item.contains("init_store_pc"))
            o.init_store_pc = parse_hex(toml::find<std::string>(item, "init_store_pc"),
                                        fmt::format("game_options '{}' init_store_pc", o.name));
        // Optional restore-time value range. `max` (with optional `min`, default 0)
        // marks the field validated; a saved value outside [min,max] is dropped.
        if (item.contains("max")) {
            o.vmax = toml::find<int64_t>(item, "max");
            o.vmin = item.contains("min") ? toml::find<int64_t>(item, "min") : 0;
            if (o.vmax < o.vmin)
                throw std::runtime_error(fmt::format(
                    "game_options '{}' max ({}) < min ({})", o.name, o.vmax, o.vmin));
            o.has_range = true;
        }
        go.options.push_back(o);
    }
    return go;
}

// ---- UserSettings (settings.toml) — launcher-written override layer ----

UserSettings load_user_settings(const fs::path& path) {
    UserSettings s;
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return s;

    toml::value doc;
    try {
        doc = toml::parse(path.string());
    } catch (const std::exception&) {
        // Malformed file: fall back to all-defaults rather than refuse to boot.
        return s;
    }

    // Each field guarded independently so one bad value can't blank the rest.
    auto try_get = [](auto&& fn) { try { fn(); } catch (const std::exception&) {} };

    if (doc.contains("video")) {
        const toml::value& v = toml::find(doc, "video");
        if (v.contains("renderer")) try_get([&]{
            const auto m = toml::find<std::string>(v, "renderer");
            if (m == "software") { s.renderer = 0; s.has_renderer = true; }
            else if (m == "opengl") { s.renderer = 1; s.has_renderer = true; }
            else if (m == "vulkan") { s.renderer = 2; s.has_renderer = true; }
        });
        if (v.contains("supersampling")) try_get([&]{
            const auto n = toml::find<int64_t>(v, "supersampling");
            if (n >= 1 && n <= 4) { s.supersampling = (int)n; s.has_supersampling = true; }
        });
        if (v.contains("window_width")) try_get([&]{
            const auto n = toml::find<int64_t>(v, "window_width");
            if (n >= 640 && n <= 3840) { s.window_width = (int)n; s.has_window_width = true; }
        });
        if (v.contains("antialiasing")) try_get([&]{
            s.antialiasing = toml::find<bool>(v, "antialiasing"); s.has_antialiasing = true;
        });
        if (v.contains("texture_filtering")) try_get([&]{
            const auto m = toml::find<std::string>(v, "texture_filtering");
            if (m == "nearest") { s.texture_filter = 0; s.has_texture_filter = true; }
            else if (m == "bilinear") { s.texture_filter = 1; s.has_texture_filter = true; }
        });
        if (v.contains("crt_filter")) try_get([&]{
            const auto m = toml::find<std::string>(v, "crt_filter");
            if (m == "raw")            { s.screen_kind = 0; s.has_screen_kind = true; }
            else if (m == "crt")       { s.screen_kind = 1; s.has_screen_kind = true; }
            else if (m == "composite") { s.screen_kind = 2; s.has_screen_kind = true; }
            else if (m == "trinitron") { s.screen_kind = 3; s.has_screen_kind = true; }
        });
        if (v.contains("auto_skip_fmv")) try_get([&]{
            s.auto_skip_fmv = toml::find<bool>(v, "auto_skip_fmv"); s.has_auto_skip_fmv = true;
        });
        if (v.contains("turbo_loads")) try_get([&]{
            s.turbo_loads = toml::find<bool>(v, "turbo_loads"); s.has_turbo_loads = true;
        });
        if (v.contains("fast_boot")) try_get([&]{
            s.fast_boot = toml::find<bool>(v, "fast_boot"); s.has_fast_boot = true;
        });
        if (v.contains("bios_hle")) try_get([&]{
            s.bios_hle = toml::find<bool>(v, "bios_hle"); s.has_bios_hle = true;
        });
        if (v.contains("fullscreen")) try_get([&]{
            s.fullscreen = toml::find<bool>(v, "fullscreen"); s.has_fullscreen = true;
        });
        if (v.contains("low_latency_input")) try_get([&]{
            s.low_latency_input = toml::find<bool>(v, "low_latency_input");
            s.has_low_latency_input = true;
        });
        if (v.contains("vsync")) try_get([&]{
            const auto m = toml::find<std::string>(v, "vsync");
            if      (m == "on"  || m == "vsync")    { s.vsync = 1;  s.has_vsync = true; }
            else if (m == "off" || m == "immediate"){ s.vsync = 0;  s.has_vsync = true; }
            else if (m == "adaptive")               { s.vsync = -1; s.has_vsync = true; }
        });
        if (v.contains("aspect_ratio")) try_get([&]{
            const auto m = toml::find<std::string>(v, "aspect_ratio");
            int n = 0, d = 0;
            if (parse_aspect_ratio(m, &n, &d)) {
                s.aspect_num = n; s.aspect_den = d; s.has_aspect_ratio = true;
            }
        });
    }
    if (doc.contains("audio")) {
        const toml::value& a = toml::find(doc, "audio");
        if (a.contains("spu_hq")) try_get([&]{
            s.spu_hq = toml::find<bool>(a, "spu_hq"); s.has_spu_hq = true;
        });
    }
    if (doc.contains("launcher")) {
        const toml::value& l = toml::find(doc, "launcher");
        if (l.contains("skip_launcher")) try_get([&]{
            s.skip_launcher = toml::find<bool>(l, "skip_launcher"); s.has_skip_launcher = true;
        });
    }
    if (doc.contains("bios")) {
        const toml::value& b = toml::find(doc, "bios");
        if (b.contains("path")) try_get([&]{
            const auto p = toml::find<std::string>(b, "path");
            if (!p.empty()) { s.bios_path = fs::path(p); s.has_bios_path = true; }
        });
    }
    if (doc.contains("disc")) {
        const toml::value& d = toml::find(doc, "disc");
        if (d.contains("path")) try_get([&]{
            const auto p = toml::find<std::string>(d, "path");
            if (!p.empty()) { s.disc_path = fs::path(p); s.has_disc_path = true; }
        });
    }
    if (doc.contains("memcard")) {
        const toml::value& m = toml::find(doc, "memcard");
        if (m.contains("dir")) try_get([&]{
            const auto p = toml::find<std::string>(m, "dir");
            if (!p.empty()) { s.memcard_dir = fs::path(p); s.has_memcard_dir = true; }
        });
        if (m.contains("card1")) try_get([&]{
            const auto p = toml::find<std::string>(m, "card1");
            if (!p.empty()) { s.memcard1_path = fs::path(p); s.has_memcard1_path = true; }
        });
        if (m.contains("card2")) try_get([&]{
            const auto p = toml::find<std::string>(m, "card2");
            if (!p.empty()) { s.memcard2_path = fs::path(p); s.has_memcard2_path = true; }
        });
        if (m.contains("enable1")) try_get([&]{
            s.memcard1_enabled = toml::find<bool>(m, "enable1"); s.has_memcard1_enabled = true;
        });
        if (m.contains("enable2")) try_get([&]{
            s.memcard2_enabled = toml::find<bool>(m, "enable2"); s.has_memcard2_enabled = true;
        });
    }
    if (doc.contains("localization")) {
        const toml::value& lc = toml::find(doc, "localization");
        if (lc.contains("language")) try_get([&]{
            const auto v = toml::find<std::string>(lc, "language");
            if (!v.empty()) { s.language = v; s.has_language = true; }
        });
    }
    if (doc.contains("controller")) {
        const toml::value& ct = toml::find(doc, "controller");
        if (ct.contains("p1_device")) try_get([&]{
            const auto v = toml::find<std::string>(ct, "p1_device");
            if (!v.empty()) { s.p1_device = v; s.has_p1_device = true; }
        });
        if (ct.contains("p2_device")) try_get([&]{
            const auto v = toml::find<std::string>(ct, "p2_device");
            if (!v.empty()) { s.p2_device = v; s.has_p2_device = true; }
        });
        // Legacy boolean form first (true->analog, false->digital); the new
        // string `*_mode` keys override when present.
        if (ct.contains("p1_analog")) try_get([&]{
            s.p1_mode = toml::find<bool>(ct, "p1_analog")
                            ? PAD_MODE_ANALOG : PAD_MODE_DIGITAL;
            s.has_p1_mode = true;
        });
        if (ct.contains("p2_analog")) try_get([&]{
            s.p2_mode = toml::find<bool>(ct, "p2_analog")
                            ? PAD_MODE_ANALOG : PAD_MODE_DIGITAL;
            s.has_p2_mode = true;
        });
        if (ct.contains("p1_mode")) try_get([&]{
            s.p1_mode = pad_mode_from_string(
                toml::find<std::string>(ct, "p1_mode"), PAD_MODE_HYBRID);
            s.has_p1_mode = true;
        });
        if (ct.contains("p2_mode")) try_get([&]{
            s.p2_mode = pad_mode_from_string(
                toml::find<std::string>(ct, "p2_mode"), PAD_MODE_HYBRID);
            s.has_p2_mode = true;
        });
        if (ct.contains("deadzone")) try_get([&]{
            const auto n = toml::find<int64_t>(ct, "deadzone");
            if (n >= 0 && n <= 32767) { s.deadzone = (int)n; s.has_deadzone = true; }
        });
    }
    return s;
}

bool save_user_settings(const fs::path& path, const UserSettings& s) {
    std::error_code ec;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), ec);  // best-effort

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;

    // TOML strings use forward slashes so backslash escaping is never an issue.
    auto fwd = [](const fs::path& p) {
        std::string str = p.generic_string();
        return str;
    };

    f << "# psxrecomp user settings - written by the launcher. Safe to hand-edit.\n";
    f << "# Overrides the bundled game.toml; the command line overrides this file.\n\n";

    f << "[video]\n";
    if (s.has_renderer)
        f << "renderer          = \""
          << (s.renderer == 2 ? "vulkan" : s.renderer == 1 ? "opengl" : "software")
          << "\"\n";
    if (s.has_supersampling)
        f << "supersampling     = " << s.supersampling << "\n";
    if (s.has_window_width)
        f << "window_width      = " << s.window_width << "\n";
    if (s.has_antialiasing)
        f << "antialiasing      = " << (s.antialiasing ? "true" : "false") << "\n";
    if (s.has_texture_filter)
        f << "texture_filtering = \"" << (s.texture_filter ? "bilinear" : "nearest") << "\"\n";
    if (s.has_screen_kind) {
        const char* k = s.screen_kind == 1 ? "crt"
                      : s.screen_kind == 2 ? "composite"
                      : s.screen_kind == 3 ? "trinitron" : "raw";
        f << "crt_filter        = \"" << k << "\"\n";
    }
    if (s.has_auto_skip_fmv)
        f << "auto_skip_fmv     = " << (s.auto_skip_fmv ? "true" : "false") << "\n";
    if (s.has_turbo_loads)
        f << "turbo_loads       = " << (s.turbo_loads ? "true" : "false") << "\n";
    if (s.has_fast_boot)
        f << "fast_boot         = " << (s.fast_boot ? "true" : "false") << "\n";
    if (s.has_bios_hle)
        f << "bios_hle          = " << (s.bios_hle ? "true" : "false") << "\n";
    if (s.has_fullscreen)
        f << "fullscreen        = " << (s.fullscreen ? "true" : "false") << "\n";
    if (s.has_low_latency_input)
        f << "low_latency_input = " << (s.low_latency_input ? "true" : "false") << "\n";
    if (s.has_vsync)
        f << "vsync             = \"" << (s.vsync == 0 ? "immediate" : s.vsync < 0 ? "adaptive" : "on") << "\"\n";
    if (s.has_aspect_ratio)
        f << "aspect_ratio      = \"" << s.aspect_num << ":" << s.aspect_den << "\"\n";
    f << "\n[audio]\n";
    if (s.has_spu_hq)
        f << "spu_hq = " << (s.spu_hq ? "true" : "false") << "\n";
    if (s.has_skip_launcher)
        f << "\n[launcher]\nskip_launcher = " << (s.skip_launcher ? "true" : "false") << "\n";
    if (s.has_bios_path)
        f << "\n[bios]\npath = \"" << fwd(s.bios_path) << "\"\n";
    if (s.has_disc_path)
        f << "\n[disc]\npath = \"" << fwd(s.disc_path) << "\"\n";
    if (s.has_memcard_dir || s.has_memcard1_path || s.has_memcard2_path ||
        s.has_memcard1_enabled || s.has_memcard2_enabled) {
        f << "\n[memcard]\n";
        if (s.has_memcard_dir)
            f << "dir     = \"" << fwd(s.memcard_dir) << "\"\n";
        if (s.has_memcard1_path)
            f << "card1   = \"" << fwd(s.memcard1_path) << "\"\n";
        if (s.has_memcard2_path)
            f << "card2   = \"" << fwd(s.memcard2_path) << "\"\n";
        if (s.has_memcard1_enabled)
            f << "enable1 = " << (s.memcard1_enabled ? "true" : "false") << "\n";
        if (s.has_memcard2_enabled)
            f << "enable2 = " << (s.memcard2_enabled ? "true" : "false") << "\n";
    }

    if (s.has_p1_device || s.has_p2_device || s.has_p1_mode || s.has_p2_mode ||
        s.has_deadzone) {
        f << "\n[controller]\n";
        if (s.has_p1_device)
            f << "p1_device = \"" << s.p1_device << "\"\n";
        if (s.has_p1_mode)
            f << "p1_mode   = \"" << pad_mode_to_string(s.p1_mode) << "\"\n";
        if (s.has_p2_device)
            f << "p2_device = \"" << s.p2_device << "\"\n";
        if (s.has_p2_mode)
            f << "p2_mode   = \"" << pad_mode_to_string(s.p2_mode) << "\"\n";
        if (s.has_deadzone)
            f << "deadzone  = " << s.deadzone << "\n";
    }

    if (s.has_language) {
        f << "\n[localization]\n";
        f << "language = \"" << s.language << "\"\n";
    }

    return f.good();
}

} // namespace PSXRecompV4
