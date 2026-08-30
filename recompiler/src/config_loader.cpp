// config_loader.cpp — see config_loader.h for the contract.

#include "config_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "bios_rom_alias.h"
#include "fmt/format.h"
#include "ps1_exe_parser.h"

// toml11 is header-only.
#define TOML11_USE_UNRELEASED_TOML_FEATURES
#include "toml.hpp"

namespace PSXRecompV4 {

namespace fs = std::filesystem;

namespace {

struct ConfigHash {
    uint32_t value = 2166136261u; // FNV-1a 32

    void byte(uint8_t v) {
        value ^= v;
        value *= 16777619u;
    }
    void u32(uint32_t v) {
        byte((uint8_t)(v >> 0));
        byte((uint8_t)(v >> 8));
        byte((uint8_t)(v >> 16));
        byte((uint8_t)(v >> 24));
    }
    void tag(const char *s) {
        while (*s) byte((uint8_t)*s++);
        byte(0);
    }
    void words(const char *name, std::vector<uint32_t> values) {
        tag(name);
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        u32((uint32_t)values.size());
        for (uint32_t value : values) u32(value);
    }
};

} // namespace

uint32_t overlay_codegen_config_hash(const GameConfig& c) {
    ConfigHash h;
    h.tag("psxrecomp-overlay-config-v1");

    h.words("sprite_tag_funcs", c.ws_sprite_tag_funcs);
    h.words("mod_function_entry_funcs", c.mod_function_entry_funcs);
    h.words("cull_bias", c.ws_cull_bias_sites);
    h.words("cull_range", c.ws_cull_range_sites);
    h.words("cull_a1", c.ws_cull_a1_sites);
    h.words("cull_screen_x", c.ws_cull_screen_x_sites);
    h.words("cull_slti", c.ws_cull_slti_sites);
    h.words("cull_slti_lower", c.ws_cull_slti_lower_sites);
    h.words("cull_bltz", c.ws_cull_bltz_sites);
    h.words("cull_negsub", c.ws_cull_negsub_sites);
    h.words("cull_vxrange", c.ws_cull_vxrange_sites);
    h.words("cull_depth", c.ws_cull_depth_sites);
    h.words("cull_plane_nx", c.ws_cull_plane_nx_sites);
    h.words("cull_xclip_load", c.ws_cull_xclip_load_sites);
    h.words("cull_nclip_keep", c.ws_cull_nclip_keep_sites);
    h.words("cull_branch_keep", c.ws_cull_branch_keep_sites);
    h.words("cull_w_imms", c.ws_cull_w_imms);
    h.words("cull_h_imms", c.ws_cull_h_imms);
    h.words("backdrop_x", c.ws_backdrop_x_sites);
    h.words("backdrop_unsquash", c.ws_backdrop_unsquash_funcs);

    h.tag("flags");
    h.u32(c.ws_auto_screen_x_cull ? 1u : 0u);
    h.u32(c.ws_auto_backdrop_preload ? 1u : 0u);
    h.u32(c.ws_bg2d_init_func);
    h.u32((uint32_t)c.ws_cull_guard_pixels);
    h.u32((uint32_t)c.ws_cull_activation_guard_pixels);

    std::vector<WidescreenSignedBoundSite> signed_sites =
        c.ws_signed_x_bound_sites;
    std::sort(signed_sites.begin(), signed_sites.end(),
              [](const auto& a, const auto& b) {
                  if (a.address != b.address) return a.address < b.address;
                  return a.expected < b.expected;
              });
    h.tag("signed_x_bounds");
    h.u32((uint32_t)signed_sites.size());
    for (const auto& site : signed_sites) {
        h.u32(site.address);
        h.u32(site.expected);
    }

    std::vector<WidescreenCullKeepSite> keep_sites = c.ws_cull_keep_sites;
    std::sort(keep_sites.begin(), keep_sites.end(),
              [](const auto& a, const auto& b) {
                  if (a.address != b.address) return a.address < b.address;
                  if (a.expected != b.expected) return a.expected < b.expected;
                  return a.result < b.result;
              });
    h.tag("cull_keep");
    h.u32((uint32_t)keep_sites.size());
    for (const auto& site : keep_sites) {
        h.u32(site.address);
        h.u32(site.expected);
        h.u32(site.result);
    }

    // Widen sites change emitted code, so they must contribute to overlay
    // cache identity exactly as keep sites do -- otherwise migrating a site
    // from keep to widen would silently reuse the pinned overlay.
    std::vector<WidescreenCullWidenSite> widen_sites = c.ws_cull_widen_sites;
    std::sort(widen_sites.begin(), widen_sites.end(),
              [](const auto& a, const auto& b) {
                  if (a.address != b.address) return a.address < b.address;
                  if (a.expected != b.expected) return a.expected < b.expected;
                  return (int)a.mode < (int)b.mode;
              });
    h.tag("cull_widen");
    h.u32((uint32_t)widen_sites.size());
    for (const auto& site : widen_sites) {
        h.u32(site.address);
        h.u32(site.expected);
        h.u32((uint32_t)site.mode);
    }

    std::vector<WidescreenAngleSite> angle_sites = c.ws_cull_angle_sites;
    std::sort(angle_sites.begin(), angle_sites.end(),
              [](const auto& a, const auto& b) {
                  if (a.address != b.address) return a.address < b.address;
                  return a.expected < b.expected;
              });
    h.tag("cull_angle");
    h.u32((uint32_t)angle_sites.size());
    for (const auto& site : angle_sites) {
        h.u32(site.address);
        h.u32(site.expected);
    }

    std::vector<WidescreenAspectConeSite> cone_sites =
        c.ws_aspect_cone.sites;
    std::sort(cone_sites.begin(), cone_sites.end(),
              [](const auto& a, const auto& b) {
                  if (a.address != b.address) return a.address < b.address;
                  return a.expected < b.expected;
              });
    h.tag("aspect_cone");
    h.u32((uint32_t)cone_sites.size());
    for (const auto& site : cone_sites) {
        h.u32(site.address);
        h.u32(site.expected);
        h.u32(site.cosine_threshold);
        h.u32(site.object_reg);
        h.u32(site.x_reg);
        h.u32(site.z_reg);
        h.u32(site.y_reg);
        h.u32(site.queue_guard ? 1u : 0u);
    }
    h.u32(c.ws_aspect_cone.forward_addr);
    h.u32(c.ws_aspect_cone.object_type_offset);
    h.u32(c.ws_aspect_cone.object_reg);
    h.u32(c.ws_aspect_cone.x_reg);
    h.u32(c.ws_aspect_cone.z_reg);
    h.u32(c.ws_aspect_cone.y_reg);
    h.u32(c.ws_aspect_cone.hysteresis_pixels);
    h.u32(c.ws_aspect_cone.queue_reserve);
    for (uint32_t value : c.ws_aspect_cone.queue_count_addrs) h.u32(value);
    for (uint32_t value : c.ws_aspect_cone.queue_capacities) h.u32(value);
    for (uint32_t value : c.ws_aspect_cone.queue_type_masks) h.u32(value);

    std::vector<RecompilerPatch> patches = c.recompiler_patches;
    std::sort(patches.begin(), patches.end(),
              [](const auto& a, const auto& b) {
                  const uint32_t aa = recompiler_patch_address_key(a.address);
                  const uint32_t ba = recompiler_patch_address_key(b.address);
                  if (aa != ba) return aa < ba;
                  if (a.expected != b.expected) return a.expected < b.expected;
                  return a.replacement < b.replacement;
              });
    h.tag("instruction_patches");
    h.u32((uint32_t)patches.size());
    for (const auto& patch : patches) {
        h.u32(recompiler_patch_address_key(patch.address));
        h.u32(patch.expected);
        h.u32(patch.replacement);
    }

    h.tag("load_charge_batch");
    h.u32(c.load_charge_batch ? 1u : 0u);
    {
        std::vector<uint32_t> lcb = c.load_charge_batch_funcs;
        std::sort(lcb.begin(), lcb.end());
        h.u32((uint32_t)lcb.size());
        for (uint32_t pc : lcb) h.u32(pc);
    }
    return h.value;
}

// Pad mode <-> string. Accepts "hybrid"/"analog"/"digital" (case-insensitive);
// returns `fallback` for anything unrecognised so a typo never silently flips
// the pad type.
int pad_mode_from_string(const std::string& s, int fallback) {
    std::string l;
    l.reserve(s.size());
    for (char c : s) l.push_back((char)std::tolower((unsigned char)c));
    /* "hybrid" is MOD-ONLY: reachable solely through
     * psx_mod_set_controller_mode_override(). A game.toml that declares it is
     * a configuration error, not something to silently coerce — coercion is
     * how this mode kept reappearing in titles that never meant to ship it. */
    if (l == "hybrid")
        throw std::runtime_error(
            "[controller] pad mode \"hybrid\" is not selectable. Hybrid is a "
            "mod-only mode, requested at runtime by a trusted game plugin via "
            "psx_mod_set_controller_mode_override(). Use \"analog\" or "
            "\"digital\" here.");
    if (l == "analog")  return PAD_MODE_ANALOG;
    if (l == "digital") return PAD_MODE_DIGITAL;
    return fallback;
}

/* Settings-file variant: a user's settings.toml may still carry a persisted
 * "hybrid" from before the mode was removed from the selector. Migrate it to
 * analog (what the old allow_hybrid clamp did) rather than refusing to
 * launch over a value the user never typed. */
int pad_mode_from_settings_string(const std::string& s, int fallback) {
    std::string l;
    l.reserve(s.size());
    for (char c : s) l.push_back((char)std::tolower((unsigned char)c));
    if (l == "hybrid")  return PAD_MODE_ANALOG;
    if (l == "analog")  return PAD_MODE_ANALOG;
    if (l == "digital") return PAD_MODE_DIGITAL;
    return fallback;
}

const char* pad_mode_to_string(int mode) {
    /* Never writes "hybrid": the mod requests it at runtime and it is not
     * persisted as a user preference. */
    switch (mode) {
        case PAD_MODE_DIGITAL: return "digital";
        default:               return "analog";
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

// Reject a config path that is not confined to the project directory.
//
// A game.toml is checked in and read on every host, so a path in it must mean
// the same thing everywhere; std::filesystem::path answers by the *host*
// grammar, so a Windows-authored escape passes on POSIX. Scan the raw bytes
// under both separator conventions instead. NUL is rejected outright because
// the runtime hands this value on as a C string — anything past a NUL would
// silently vanish between what is validated and what is used.
static void validate_project_relative(const std::string& value,
                                      const char* field) {
    if (value.find('\0') != std::string::npos) {
        throw std::runtime_error(
            fmt::format("{} must not contain a NUL byte", field));
    }
    // Rooted ("/x", "\x"), UNC ("\\server\share"), drive-qualified ("C:/x",
    // "C:\x") and drive-relative ("C:x") alike.
    const bool rooted = !value.empty() && (value[0] == '/' || value[0] == '\\');
    const bool drive  = value.size() >= 2 && value[1] == ':' &&
                        std::isalpha(static_cast<unsigned char>(value[0]));
    if (rooted || drive) {
        throw std::runtime_error(
            fmt::format("{} must be project-relative", field));
    }
    for (size_t start = 0;;) {
        const size_t sep = value.find_first_of("/\\", start);
        const size_t end = (sep == std::string::npos) ? value.size() : sep;
        if (value.compare(start, end - start, "..") == 0) {
            throw std::runtime_error(
                fmt::format("{} must stay inside the project", field));
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
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
    if (cfg.contains("runtime")) {
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
    auto parse_warm_cd_route = [&](const toml::value& route,
                                   const std::string& label) {
        if (!route.contains("arm_lba") || !route.contains("lbas")) {
            throw std::runtime_error(
                label + " arm_lba and lbas are required");
        }
        const auto arm = toml::find<int64_t>(route, "arm_lba");
        if (arm < 0 || arm > 0x7FFFFFFFll) {
            throw std::runtime_error(fmt::format(
                "{} arm_lba out of range: {}", label, arm));
        }
        RuntimeConfig::WarmCdRoute parsed;
        parsed.arm_lba = static_cast<int>(arm);
        const auto& lbas = toml::find<std::vector<int64_t>>(route, "lbas");
        if (lbas.empty() || lbas.size() > 64) {
            throw std::runtime_error(
                label + " lbas must contain 1..64 entries");
        }
        for (const auto lba : lbas) {
            if (lba < 0 || lba > 0x7FFFFFFFll) {
                throw std::runtime_error(fmt::format(
                    "{} LBA out of range: {}", label, lba));
            }
            parsed.lbas.push_back(static_cast<int>(lba));
        }
        if (route.contains("instant_max_per_frame")) {
            const auto n = toml::find<int64_t>(route, "instant_max_per_frame");
            if (n < 1 || n > 4096) {
                throw std::runtime_error(fmt::format(
                    "{} instant_max_per_frame out of range (1..4096): {}",
                    label, n));
            }
            parsed.instant_max_per_frame = static_cast<int>(n);
        }
        for (const auto& existing : rt.warm_cd_routes) {
            if (existing.arm_lba == parsed.arm_lba &&
                existing.lbas.front() == parsed.lbas.front()) {
                throw std::runtime_error(fmt::format(
                    "{} duplicates route arm {} / first LBA {}", label,
                    parsed.arm_lba, parsed.lbas.front()));
            }
        }
        rt.warm_cd_routes.push_back(std::move(parsed));
    };
    if (runtime.contains("warm_cd_route")) {
        std::fprintf(stderr,
            "psxrecomp: warning: [runtime.warm_cd_route] is deprecated; "
            "use [[runtime.warm_cd_routes]]\n");
        parse_warm_cd_route(toml::find(runtime, "warm_cd_route"),
                            "[runtime.warm_cd_route]");
    }
    if (runtime.contains("warm_cd_routes")) {
        const auto& routes = toml::find(runtime, "warm_cd_routes");
        if (!routes.is_array() || routes.as_array().empty() ||
            routes.as_array().size() > 16) {
            throw std::runtime_error(
                "[[runtime.warm_cd_routes]] must contain 1..16 routes");
        }
        size_t route_index = 0;
        for (const auto& route : routes.as_array()) {
            if (!route.is_table()) {
                throw std::runtime_error(
                    "[[runtime.warm_cd_routes]] entries must be tables");
            }
            parse_warm_cd_route(route, fmt::format(
                "[[runtime.warm_cd_routes]] entry {}", route_index++));
        }
    }
    if (runtime.contains("fast_boot")) {
        rt.fast_boot = toml::find<bool>(runtime, "fast_boot");
    }
    if (runtime.contains("ape_card_unstick")) {
        rt.ape_card_unstick = toml::find<bool>(runtime, "ape_card_unstick");
    }
    if (runtime.contains("openbios")) {
        rt.openbios = toml::find<bool>(runtime, "openbios");
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
    if (runtime.contains("overlay_capture_history")) {
        rt.overlay_capture_history =
            toml::find<bool>(runtime, "overlay_capture_history");
    }
    if (runtime.contains("overlay_capture_persist_dir")) {
        rt.overlay_capture_persist_dir =
            toml::find<std::string>(runtime, "overlay_capture_persist_dir");
        validate_project_relative(rt.overlay_capture_persist_dir,
                                  "runtime.overlay_capture_persist_dir");
    }
    // turbo_loads / offer_turbo_loads are deprecated and ignored (see
    // config_loader.h). Still parsed so old configs load, and the presence
    // flags let the runtime name the dead key in its deprecation notice.
    if (runtime.contains("turbo_loads")) {
        rt.turbo_loads = toml::find<bool>(runtime, "turbo_loads");
        rt.has_turbo_loads = true;
    }
    if (runtime.contains("offer_turbo_loads")) {
        rt.offer_turbo_loads =
            toml::find<bool>(runtime, "offer_turbo_loads");
        rt.has_offer_turbo_loads = true;
    }
    if (runtime.contains("turbo_audio_sink")) {
        rt.turbo_audio_sink = toml::find<bool>(runtime, "turbo_audio_sink");
    }
    if (runtime.contains("idle_skip")) {
        rt.idle_skip = toml::find<bool>(runtime, "idle_skip");
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
    }

    if (cfg.contains("parappa_timing")) {
        const toml::value& timing = toml::find(cfg, "parappa_timing");
        rt.has_parappa_timing = true;
        if (timing.contains("mode")) {
            rt.parappa_timing_mode = toml::find<std::string>(timing, "mode");
            for (char& c : rt.parappa_timing_mode)
                c = (char)std::tolower((unsigned char)c);
        }
        auto parse_window = [&](const char *key) -> int {
            const auto n = toml::find<int64_t>(timing, key);
            if (n < 0 || n > 60) {
                throw std::runtime_error(fmt::format(
                    "[parappa_timing] {} out of range (0..60): {}", key, n));
            }
            return (int)n;
        };
        if (timing.contains("extra_early"))
            rt.parappa_timing_extra_early = parse_window("extra_early");
        if (timing.contains("extra_late"))
            rt.parappa_timing_extra_late = parse_window("extra_late");
    }

    // Optional [video] block — visual enhancement options. Kept on the same
    // RuntimeConfig so main.cpp consumes them alongside the other knobs.
    if (cfg.contains("video")) {
        const toml::value& video = toml::find(cfg, "video");
        if (video.contains("supersampling")) {
            const auto n = toml::find<int64_t>(video, "supersampling");
            /* GL renders SSAA into an FBO rather than the software path's
             * VRAM-sized mirror, so it scales well past 4x. The runtime picks
             * the ceiling per backend and clamps again to the driver's real
             * texture limit once a context exists. */
            if (n < 1 || n > 16) {
                throw std::runtime_error(fmt::format(
                    "[video] supersampling out of range (1..16): {}", n));
            }
            rt.video_supersampling = static_cast<int>(n);
        }
        if (video.contains("crtc_refresh_multiplier")) {
            const auto n = toml::find<int64_t>(video, "crtc_refresh_multiplier");
            if (n < 1 || n > 8) {
                throw std::runtime_error(fmt::format(
                    "[video] crtc_refresh_multiplier out of range (1..8): {}", n));
            }
            rt.video_crtc_refresh_multiplier = static_cast<uint32_t>(n);
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
        if (video.contains("fmv_filter")) {
            const auto mode = toml::find<std::string>(video, "fmv_filter");
            if (!video_fmv_filter_parse(mode, &rt.video_fmv_filter))
                throw std::runtime_error(fmt::format(
                    "[video] fmv_filter must be \"nearest\", \"bilinear\", "
                    "\"sharp\" or \"bicubic\": {}", mode));
        }
        if (video.contains("renderer")) {
            const auto mode = toml::find<std::string>(video, "renderer");
            if (mode == "software")     rt.video_renderer = 0;
            else if (mode == "opengl")  rt.video_renderer = 1;
            else if (mode == "vulkan")  rt.video_renderer = 2;
            else throw std::runtime_error(fmt::format(
                "[video] renderer must be \"software\", \"opengl\" or \"vulkan\": {}", mode));
        }
        if (video.contains("offer_vulkan")) {
            rt.video_offer_vulkan = toml::find<bool>(video, "offer_vulkan");
        }
        if (video.contains("hd_textures")) {
            rt.video_hd_textures = toml::find<bool>(video, "hd_textures");
        }
        if (video.contains("hd_texture_dump")) {
            rt.video_hd_texture_dump = toml::find<bool>(video, "hd_texture_dump");
        }
        if (video.contains("cpu_overclock")) {
            rt.runtime_cpu_overclock =
                (uint32_t)toml::find<int>(video, "cpu_overclock");
        }
        if (video.contains("bezel")) {
            rt.video_bezel = toml::find<std::string>(video, "bezel");
        }
        if (video.contains("hd_texture_dir")) {
            rt.video_hd_texture_dir = toml::find<std::string>(video, "hd_texture_dir");
        }
        if (video.contains("hd_texture_exclude")) {
            rt.video_hd_texture_exclude =
                toml::find<std::vector<std::string>>(video, "hd_texture_exclude");
        }
        if (video.contains("geometry_correction")) {
            rt.video_geometry_correction =
                toml::find<bool>(video, "geometry_correction");
        }
        if (video.contains("perspective_texturing")) {
            rt.video_perspective_texturing =
                toml::find<bool>(video, "perspective_texturing");
        }
        if (video.contains("pgxp_depth")) {
            rt.video_pgxp_depth = toml::find<bool>(video, "pgxp_depth");
        }
        if (video.contains("pgxp_cpu_mode")) {
            rt.video_pgxp_cpu_mode = toml::find<bool>(video, "pgxp_cpu_mode");
        }
        if (video.contains("pgxp_tolerance")) {
            rt.video_pgxp_tolerance =
                toml::find<double>(video, "pgxp_tolerance");
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
        if (video.contains("offer_skip_fmv")) {
            rt.video_offer_skip_fmv = toml::find<bool>(video, "offer_skip_fmv");
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
        if (video.contains("frame_interpolation")) {
            rt.video_frame_interpolation =
                toml::find<bool>(video, "frame_interpolation");
        }
        if (video.contains("frame_interpolation_fps")) {
            rt.video_frame_interpolation_fps =
                toml::find<int>(video, "frame_interpolation_fps");
            if (rt.video_frame_interpolation_fps != 0 &&
                rt.video_frame_interpolation_fps < 90)
                throw std::runtime_error(
                    "[video] frame_interpolation_fps must be 0 (display) or >= 90");
        }
        if (video.contains("offer_frame_interpolation")) {
            rt.video_offer_frame_interpolation =
                toml::find<bool>(video, "offer_frame_interpolation");
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
        if (audio.contains("buffer_ms")) {
            const auto n = toml::find<int64_t>(audio, "buffer_ms");
            if (n < 30 || n > 500) {
                throw std::runtime_error(fmt::format(
                    "[audio] buffer_ms out of range (30..500): {}", n));
            }
            rt.audio_buffer_ms = static_cast<int>(n);
        }
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
                toml::find<std::string>(ct, "default_mode"), PAD_MODE_ANALOG);
            rt.default_p1_mode = rt.default_p2_mode = m;
            rt.has_default_mode = true;
        }
        if (ct.contains("p1_mode")) {
            rt.default_p1_mode = pad_mode_from_string(
                toml::find<std::string>(ct, "p1_mode"), PAD_MODE_ANALOG);
            rt.has_default_mode = true;
        }
        if (ct.contains("p2_mode")) {
            rt.default_p2_mode = pad_mode_from_string(
                toml::find<std::string>(ct, "p2_mode"), PAD_MODE_ANALOG);
            rt.has_default_mode = true;
        }
        if (ct.contains("p1_device")) {
            rt.default_p1_device = toml::find<std::string>(ct, "p1_device");
            if (!rt.default_p1_device.empty())
                rt.has_default_p1_device = true;
        }
        if (ct.contains("p2_device")) {
            rt.default_p2_device = toml::find<std::string>(ct, "p2_device");
            if (!rt.default_p2_device.empty())
                rt.has_default_p2_device = true;
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
        if (ct.contains("multitap_port")) {
            const auto n = toml::find<int64_t>(ct, "multitap_port");
            if (n != 1 && n != 2)
                throw std::runtime_error(fmt::format(
                    "[controller] multitap_port must be 1 or 2, got {}", n));
            rt.multitap_port = static_cast<int>(n);
            rt.has_multitap_port = true;
        }
        if (ct.contains("multitap_analog")) {
            rt.multitap_analog = toml::find<bool>(ct, "multitap_analog");
            rt.has_multitap_analog = true;
        }
        // Prefer string key; accept legacy bool alias.
        if (ct.contains("legacy_pad_config")) {
            rt.legacy_pad_config = toml::find<bool>(ct, "legacy_pad_config");
        }
        if (ct.contains("anti_deadzone")) {
            const auto n = toml::find<int64_t>(ct, "anti_deadzone");
            if (n < 0 || n > 32767)
                throw std::runtime_error(fmt::format(
                    "[controller] anti_deadzone out of range (0..32767): {}", n));
            rt.anti_deadzone = static_cast<int>(n);
            rt.has_anti_deadzone = true;
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

    // A BIOS profile describes an IMAGE — facts about bytes — never a runtime
    // preference. This block used to be parsed into BiosConfig::runtime and
    // read by nothing, which let bios/OpenBIOS.toml carry a `bios_hle = false`
    // that looked authoritative and was inert. Fail loud instead of ignoring:
    // a preference put here would never take effect, and believing it had is
    // how the OpenBIOS boot-skip regression got explained away.
    if (cfg.contains("runtime")) {
        throw std::runtime_error(fmt::format(
            "{}: a BIOS profile has no [runtime] block — it describes an image, "
            "not a preference. Runtime options belong in game.toml / "
            "settings.toml. (The BIOS-dependent HLE gating is expressed by "
            "[recompiler.runtime_exports]: shell_entry_phys enables the boot-"
            "skip, deliver_event_ret enables the kernel-call HLE tier.)",
            config_path.string()));
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
    // Tolerate either BIOS filename convention (bare "SCPH1001.BIN" vs
    // region-qualified "US-PSX-SCPH1001.BIN") — see bios_rom_alias.h. A no-op
    // for game [program].exe fields, which never match a BIOS model token.
    const fs::path rom_path = resolve_bios_rom(fs::absolute(root / rom_field));

    const uint32_t load_address =
        parse_hex(toml::find<std::string>(prog, "load_address"), "program.load_address");
    const uint32_t entry_pc =
        prog.contains("entry_pc")
            ? parse_hex(toml::find<std::string>(prog, "entry_pc"), "program.entry_pc")
            : load_address;
    const uint32_t text_size =
        parse_hex(toml::find<std::string>(prog, "text_size"), "program.text_size");

    // [program.image] — declared identity (optional)
    std::string image_sha256;
    bool image_redistributable = false;
    if (prog.contains("image")) {
        const toml::value& img = toml::find(prog, "image");
        if (img.contains("sha256"))
            image_sha256 = toml::find<std::string>(img, "sha256");
        if (img.contains("redistributable"))
            image_redistributable = toml::find<bool>(img, "redistributable");
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

    // [recompiler.address_model] — the BIOS's boot-time ROM->RAM code copies.
    // Optional: absent (or an empty copy list) means the BIOS runs entirely
    // from ROM and normalization degenerates to the KSEG mask. Semantic
    // invariants (window overlap, bless uniqueness, alignment) are enforced
    // in BiosAddressModel::from_config, not here.
    std::vector<BiosAddrCopy> address_copies;
    if (recomp.contains("address_model")) {
        const toml::value& am = toml::find(recomp, "address_model");
        if (am.contains("normalize_mask")) {
            const uint32_t mask = parse_hex(
                toml::find<std::string>(am, "normalize_mask"),
                "address_model.normalize_mask");
            // Only the KSEG strip is implemented; refuse anything else rather
            // than silently mis-normalizing (RELOCATION_MANIFEST_FORMAT.md
            // echo-and-check).
            if (mask != 0x1FFFFFFFu) {
                throw std::runtime_error(fmt::format(
                    "{}: address_model.normalize_mask must be 0x1FFFFFFF",
                    config_path.string()));
            }
        }
        if (am.contains("copy")) {
            for (const auto& v : am.at("copy").as_array()) {
                BiosAddrCopy c;
                c.name   = toml::find<std::string>(v, "name");
                c.rom_lo = parse_hex(toml::find<std::string>(v, "rom_lo"),
                                     "address_model.copy.rom_lo");
                c.rom_hi = parse_hex(toml::find<std::string>(v, "rom_hi"),
                                     "address_model.copy.rom_hi");
                c.ram_lo = parse_hex(toml::find<std::string>(v, "ram_lo"),
                                     "address_model.copy.ram_lo");
                c.runtime_base = parse_hex(
                    toml::find<std::string>(v, "runtime_base"),
                    "address_model.copy.runtime_base");
                const std::string key =
                    toml::find<std::string>(v, "dispatch_key");
                if (key == "ram")      c.key_is_ram = true;
                else if (key == "rom") c.key_is_ram = false;
                else {
                    throw std::runtime_error(fmt::format(
                        "{}: address_model.copy '{}': dispatch_key must be "
                        "\"ram\" or \"rom\", got '{}'",
                        config_path.string(), c.name, key));
                }
                if (v.contains("kernel_bless"))
                    c.kernel_bless = toml::find<bool>(v, "kernel_bless");
                address_copies.push_back(std::move(c));
            }
        }
    }

    // [[recompiler.install_slots]] — kernel-RAM PCs the BIOS overwrites with
    // dispatch stubs at runtime.
    std::vector<uint32_t> install_slots;
    if (recomp.contains("install_slots")) {
        for (const auto& v : recomp.at("install_slots").as_array()) {
            install_slots.push_back(parse_hex(
                toml::find<std::string>(v, "ram_addr"),
                "install_slots.ram_addr"));
        }
    }

    // [recompiler.runtime_exports] — per-image HLE anchors couriered into the
    // generated C. Absent keys stay 0 = structurally unavailable.
    uint32_t shell_entry_phys = 0, deliver_event_ret = 0;
    if (recomp.contains("runtime_exports")) {
        const toml::value& rx = toml::find(recomp, "runtime_exports");
        if (rx.contains("shell_entry_phys"))
            shell_entry_phys = parse_hex(
                toml::find<std::string>(rx, "shell_entry_phys"),
                "runtime_exports.shell_entry_phys");
        if (rx.contains("deliver_event_ret"))
            deliver_event_ret = parse_hex(
                toml::find<std::string>(rx, "deliver_event_ret"),
                "runtime_exports.deliver_event_ret");
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
        /*image_sha256*/ image_sha256,
        /*image_redistributable*/ image_redistributable,
        /*seeds_path*/   seeds_path,
        /*out_dir*/      out_dir,
        /*strict*/       strict,
        /*out_stem*/     out_stem,
        /*bios_vectors*/ std::move(bios_vectors),
        /*bios_aliases*/ std::move(bios_aliases),
        /*address_copies*/ std::move(address_copies),
        /*install_slots*/  std::move(install_slots),
        /*shell_entry_phys*/  shell_entry_phys,
        /*deliver_event_ret*/ deliver_event_ret,
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
    const std::string region = game.contains("region")
                                ? toml::find<std::string>(game, "region")
                                : std::string{};
    const int players = game.contains("players")
                                ? (int)toml::find<int64_t>(game, "players")
                                : 1;

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

    // [netplay] — mount / TOC policy for online (see DiscIdentity::disc_fp).
    bool netplay_require_cue = false;
    int netplay_required_tracks = 0;
    bool has_netplay_required_leadout = false;
    uint32_t netplay_required_leadout_lba = 0;
    std::string netplay_required_disc_fp;
    std::string netplay_local_viewport;
    std::string netplay_local_viewport_aspect;
    if (cfg.contains("netplay")) {
        const toml::value& np = toml::find(cfg, "netplay");
        if (np.contains("require_cue"))
            netplay_require_cue = toml::find<bool>(np, "require_cue");
        if (np.contains("required_tracks"))
            netplay_required_tracks = toml::find<int>(np, "required_tracks");
        if (np.contains("required_leadout_lba")) {
            netplay_required_leadout_lba =
                (uint32_t)toml::find<int64_t>(np, "required_leadout_lba");
            has_netplay_required_leadout = true;
        }
        if (np.contains("required_disc_fp")) {
            netplay_required_disc_fp = toml::find<std::string>(np, "required_disc_fp");
            for (char& c : netplay_required_disc_fp)
                c = (char)std::tolower((unsigned char)c);
        }
        if (np.contains("local_viewport")) {
            netplay_local_viewport = toml::find<std::string>(np, "local_viewport");
            for (char& c : netplay_local_viewport)
                c = (char)std::tolower((unsigned char)c);
            if (!netplay_local_viewport.empty() &&
                netplay_local_viewport != "vertical_split") {
                throw std::runtime_error(fmt::format(
                    "[netplay] local_viewport must be \"vertical_split\", got '{}'",
                    netplay_local_viewport));
            }
        }
        if (np.contains("local_viewport_aspect")) {
            netplay_local_viewport_aspect =
                toml::find<std::string>(np, "local_viewport_aspect");
            for (char& c : netplay_local_viewport_aspect)
                c = (char)std::tolower((unsigned char)c);
            if (!netplay_local_viewport_aspect.empty() &&
                netplay_local_viewport_aspect != "16:9" &&
                netplay_local_viewport_aspect != "21:9" &&
                netplay_local_viewport_aspect != "adaptive") {
                throw std::runtime_error(fmt::format(
                    "[netplay] local_viewport_aspect must be \"16:9\", "
                    "\"21:9\", or \"adaptive\", got '{}'",
                    netplay_local_viewport_aspect));
            }
            if (!netplay_local_viewport_aspect.empty() &&
                netplay_local_viewport.empty()) {
                throw std::runtime_error(
                    "[netplay] local_viewport_aspect requires local_viewport");
            }
        }
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

    // [recompiler] bios_config — the BIOS profile this game is built
    // against. Optional: main_psx falls back to the SCPH1001 profile
    // (bios/SCPH1001.toml, in-repo or under the psxrecomp/ submodule).
    fs::path bios_config_path;
    if (recomp.contains("bios_config")) {
        bios_config_path =
            fs::absolute(root / toml::find<std::string>(recomp, "bios_config"));
    }

    const std::string out_dir_field =
        recomp.contains("out_dir")
            ? toml::find<std::string>(recomp, "out_dir")
            : std::string{"generated"};
    const fs::path out_dir = fs::absolute(root / out_dir_field);

    const bool strict = recomp.contains("strict")
                            ? toml::find<bool>(recomp, "strict")
                            : true;

    const std::string discovery =
        recomp.contains("discovery")
            ? toml::find<std::string>(recomp, "discovery")
            : std::string{"whole-image"};
    if (discovery != "whole-image" && discovery != "reachable") {
        throw std::runtime_error(fmt::format(
            "{}: [recompiler].discovery must be 'whole-image' or 'reachable'",
            config_path.string()));
    }

    std::string out_stem;
    if (recomp.contains("out_stem")) {
        out_stem = toml::find<std::string>(recomp, "out_stem");
    } else {
        out_stem = derive_out_stem(fs::path(exe_field).filename().string());
    }

    std::vector<RecompilerPatch> recompiler_patches;
    if (recomp.contains("patch")) {
        const auto& patches = toml::find<toml::array>(recomp, "patch");
        std::set<std::string> seen_ids;
        std::set<uint32_t> seen_addresses;
        for (const auto& item : patches) {
            RecompilerPatch patch;
            patch.id = toml::find<std::string>(item, "id");
            patch.address = parse_hex(toml::find<std::string>(item, "address"),
                                      "recompiler.patch.address");
            patch.expected = parse_hex(toml::find<std::string>(item, "expected"),
                                       "recompiler.patch.expected");
            patch.replacement = parse_hex(
                toml::find<std::string>(item, "replacement"),
                "recompiler.patch.replacement");
            if (item.contains("note")) {
                patch.note = toml::find<std::string>(item, "note");
            }

            if (patch.id.empty()) {
                throw std::runtime_error(
                    "recompiler.patch.id must not be empty");
            }
            if ((patch.address & 3u) != 0u) {
                throw std::runtime_error(fmt::format(
                    "{}: recompiler patch '{}' address 0x{:08X} is not "
                    "instruction-aligned",
                    config_path.string(), patch.id, patch.address));
            }
            if (!seen_ids.insert(patch.id).second) {
                throw std::runtime_error(fmt::format(
                    "{}: duplicate [[recompiler.patch]] id '{}'",
                    config_path.string(), patch.id));
            }
            const uint32_t address_key =
                recompiler_patch_address_key(patch.address);
            if (!seen_addresses.insert(address_key).second) {
                throw std::runtime_error(fmt::format(
                    "{}: duplicate [[recompiler.patch]] physical address "
                    "0x{:08X} (KUSEG/KSEG aliases are one site)",
                    config_path.string(), address_key));
            }
            recompiler_patches.push_back(std::move(patch));
        }
    }

    // Optional [widescreen] block — per-game hooks for the widescreen hack.
    std::vector<uint32_t> ws_sprite_tag_funcs;
    uint32_t ws_sprite_anchor_addr = 0;
    bool ws_hud_sprt_squash = false;
    bool ws_auto_ui_squash = false;
    bool ws_full_2d = false;
    bool ws_gte_game_mode = false;
    bool ws_fixed_outer_aspect = false;
    bool ws_precise_nclip = false;
    uint32_t ws_gameplay_state_addr = 0;
    std::vector<uint32_t> ws_gameplay_state_values;
    bool ws_native_wide = true;
    bool ws_nw_hud_corners = false;
    uint32_t ws_nw_left_hud_packet_lo = 0;
    uint32_t ws_nw_left_hud_packet_hi = 0;
    bool ws_nw_backdrop = false;
    bool ws_clear_reveal = false;
    bool ws_nw_flat_backdrop = false;
    bool ws_nw_phase_backdrop = false;
    bool ws_nw_textured_edges = false;
    int ws_nw_textured_edge_scale = 0;
    bool ws_nw_full_mirror = false;
    std::vector<WidescreenSignedBoundSite> ws_signed_x_bound_sites;
    bool ws_offered = true;
    bool vulkan_offered = false;
    bool ws_adaptive_view = false;
    bool ws_ultrawide_offered = false;
    if (cfg.contains("video")) {
        const toml::value& video = toml::find(cfg, "video");
        if (video.contains("offer_vulkan"))
            vulkan_offered = toml::find<bool>(video, "offer_vulkan");
    }
    // Optional [data_shards] block — memoized pure-function replay hooks.
    std::vector<uint32_t> data_shard_funcs;
    if (cfg.contains("data_shards")) {
        const toml::value& dsv = toml::find(cfg, "data_shards");
        if (dsv.contains("funcs")) {
            const auto& arr = toml::find<std::vector<std::string>>(dsv, "funcs");
            for (const auto& a : arr)
                data_shard_funcs.push_back(parse_hex(a, "data_shards.funcs"));
        }
    }
    // Optional [recompiler] hot_funcs — __attribute__((hot)) on emitted C.
    // Optional trusted, statically linked game-mod entry hooks.
    std::vector<uint32_t> mod_function_entry_funcs;
    if (recomp.contains("mod_function_entry_funcs")) {
        const auto& arr = toml::find<std::vector<std::string>>(
            recomp, "mod_function_entry_funcs");
        for (const auto& a : arr)
            mod_function_entry_funcs.push_back(
                parse_hex(a, "recompiler.mod_function_entry_funcs"));
    }
    std::vector<uint32_t> hot_funcs;
    if (recomp.contains("hot_funcs")) {
        const auto& arr = toml::find<std::vector<std::string>>(recomp, "hot_funcs");
        for (const auto& a : arr)
            hot_funcs.push_back(parse_hex(a, "recompiler.hot_funcs"));
    }
    // Optional emitter-level load-charge batching for VLC leaves.
    bool load_charge_batch = false;
    std::vector<uint32_t> load_charge_batch_funcs;
    if (recomp.contains("load_charge_batch"))
        load_charge_batch = toml::find<bool>(recomp, "load_charge_batch");
    if (recomp.contains("load_charge_batch_funcs")) {
        const auto& arr =
            toml::find<std::vector<std::string>>(recomp, "load_charge_batch_funcs");
        for (const auto& a : arr)
            load_charge_batch_funcs.push_back(
                parse_hex(a, "recompiler.load_charge_batch_funcs"));
    }
    if (load_charge_batch && load_charge_batch_funcs.empty())
        load_charge_batch_funcs = hot_funcs;
    uint32_t vsync_query_func = 0;
    uint32_t vsync_counter_addr = 0;
    uint32_t vsync_gpustat_ptr_addr = 0;
    uint32_t vsync_timer1_ptr_addr = 0;
    uint32_t vsync_timer1_cache_addr = 0;
    std::vector<uint32_t> vsync_event_horizon_sites;
    std::vector<uint32_t> vsync_event_horizon_extra_sites;
    bool vsync_event_horizon_any = false;
    if (cfg.contains("load_accel")) {
        const toml::value& lav = toml::find(cfg, "load_accel");
        if (lav.contains("vsync_query")) {
            const toml::value& vq = toml::find(lav, "vsync_query");
            const bool has_func = vq.contains("func");
            const bool has_counter = vq.contains("counter_addr");
            const bool has_gpu_ptr = vq.contains("gpustat_ptr_addr");
            const bool has_timer_ptr = vq.contains("timer1_ptr_addr");
            const bool has_timer_cache = vq.contains("timer1_cache_addr");
            const int present = (int)has_func + (int)has_counter + (int)has_gpu_ptr +
                                (int)has_timer_ptr + (int)has_timer_cache;
            if (present != 0 && present != 5)
                throw std::runtime_error(fmt::format(
                    "{}: [load_accel.vsync_query] func, counter_addr, gpustat_ptr_addr, "
                    "timer1_ptr_addr, and timer1_cache_addr must be set together",
                    config_path.string()));
            if (has_func) {
                vsync_query_func = parse_hex(
                    toml::find<std::string>(vq, "func"),
                    "load_accel.vsync_query.func");
                vsync_counter_addr = parse_hex(
                    toml::find<std::string>(vq, "counter_addr"),
                    "load_accel.vsync_query.counter_addr");
                vsync_gpustat_ptr_addr = parse_hex(
                    toml::find<std::string>(vq, "gpustat_ptr_addr"),
                    "load_accel.vsync_query.gpustat_ptr_addr");
                vsync_timer1_ptr_addr = parse_hex(
                    toml::find<std::string>(vq, "timer1_ptr_addr"),
                    "load_accel.vsync_query.timer1_ptr_addr");
                vsync_timer1_cache_addr = parse_hex(
                    toml::find<std::string>(vq, "timer1_cache_addr"),
                    "load_accel.vsync_query.timer1_cache_addr");
                if (vq.contains("event_horizon_sites")) {
                    const auto& arr = toml::find<std::vector<std::string>>(
                        vq, "event_horizon_sites");
                    for (const auto& a : arr)
                        vsync_event_horizon_sites.push_back(parse_hex(
                            a, "load_accel.vsync_query.event_horizon_sites"));
                }
                if (vq.contains("event_horizon_extra_sites")) {
                    const auto& arr = toml::find<std::vector<std::string>>(
                        vq, "event_horizon_extra_sites");
                    for (const auto& a : arr)
                        vsync_event_horizon_extra_sites.push_back(parse_hex(
                            a, "load_accel.vsync_query.event_horizon_extra_sites"));
                }
                if (vq.contains("event_horizon_any"))
                    vsync_event_horizon_any = toml::find<bool>(vq, "event_horizon_any");
            }
        }
    }
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
        if (ws.contains("auto_ui_squash"))
            ws_auto_ui_squash = toml::find<bool>(ws, "auto_ui_squash");
        if (ws.contains("full_2d"))
            ws_full_2d = toml::find<bool>(ws, "full_2d");
        if (ws.contains("gte_game_mode"))
            ws_gte_game_mode = toml::find<bool>(ws, "gte_game_mode");
        if (ws.contains("fixed_outer_aspect"))
            ws_fixed_outer_aspect = toml::find<bool>(
                ws, "fixed_outer_aspect");
        if (ws.contains("precise_nclip"))
            ws_precise_nclip = toml::find<bool>(ws, "precise_nclip");
        const bool has_gameplay_state_addr = ws.contains("gameplay_state_addr");
        const bool has_gameplay_state_values = ws.contains("gameplay_state_values");
        if (has_gameplay_state_addr != has_gameplay_state_values)
            throw std::runtime_error(fmt::format(
                "{}: [widescreen] gameplay_state_addr and "
                "gameplay_state_values must be set together",
                config_path.string()));
        if (has_gameplay_state_addr) {
            ws_gameplay_state_addr = parse_hex(
                toml::find<std::string>(ws, "gameplay_state_addr"),
                "widescreen.gameplay_state_addr");
            const auto& arr = toml::find<std::vector<std::string>>(
                ws, "gameplay_state_values");
            for (const auto& value : arr)
                ws_gameplay_state_values.push_back(parse_hex(
                    value, "widescreen.gameplay_state_values"));
            if (ws_gameplay_state_values.empty())
                throw std::runtime_error(fmt::format(
                    "{}: [widescreen] gameplay_state_values must not be empty",
                    config_path.string()));
        }
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
        if (ws.contains("nw_textured_edges"))
            ws_nw_textured_edges = toml::find<bool>(ws, "nw_textured_edges");
        if (ws.contains("nw_textured_edge_scale")) {
            ws_nw_textured_edge_scale = toml::find<int>(ws, "nw_textured_edge_scale");
            if (ws_nw_textured_edge_scale != 0 &&
                (ws_nw_textured_edge_scale < 100 || ws_nw_textured_edge_scale > 400))
                throw std::runtime_error(fmt::format(
                    "{}: [widescreen] nw_textured_edge_scale must be 0 or in [100, 400]",
                    config_path.string()));
        }
        if (ws.contains("nw_full_mirror"))
            ws_nw_full_mirror = toml::find<bool>(ws, "nw_full_mirror");
        if (ws.contains("signed_x_bound")) {
            std::set<uint32_t> seen;
            for (const auto& item : toml::find<toml::array>(ws, "signed_x_bound")) {
                WidescreenSignedBoundSite site;
                site.address = parse_hex(toml::find<std::string>(item, "address"),
                                         "widescreen.signed_x_bound.address");
                site.expected = parse_hex(toml::find<std::string>(item, "expected"),
                                          "widescreen.signed_x_bound.expected");
                if ((site.expected >> 26) != 0x0Fu)
                    throw std::runtime_error(fmt::format(
                        "{}: [[widescreen.signed_x_bound]] expected must be LUI",
                        config_path.string()));
                if (!seen.insert(site.address & 0x1FFFFFFFu).second)
                    throw std::runtime_error(fmt::format(
                        "{}: duplicate [[widescreen.signed_x_bound]] address 0x{:08X}",
                        config_path.string(), site.address));
                ws_signed_x_bound_sites.push_back(site);
            }
        }
        if (ws.contains("offer"))
            ws_offered = toml::find<bool>(ws, "offer");
        if (ws.contains("offer_ultrawide"))
            ws_ultrawide_offered = toml::find<bool>(ws, "offer_ultrawide");
        if (ws.contains("adaptive_view"))
            ws_adaptive_view = toml::find<bool>(ws, "adaptive_view");
    }

    // Optional [widescreen.cull] block — world-space draw-cull widening.
    std::vector<uint32_t> ws_cull_bias_sites, ws_cull_range_sites, ws_cull_a1_sites;
    std::vector<uint32_t> ws_cull_screen_x_sites;
    std::vector<uint32_t> ws_cull_slti_sites;
    std::vector<uint32_t> ws_cull_slti_lower_sites;
    std::vector<uint32_t> ws_cull_bltz_sites;
    std::vector<uint32_t> ws_cull_negsub_sites;
    std::vector<uint32_t> ws_cull_vxrange_sites;
    std::vector<uint32_t> ws_cull_depth_sites;
    std::vector<uint32_t> ws_cull_plane_nx_sites;
    std::vector<uint32_t> ws_cull_xclip_load_sites;
    std::vector<uint32_t> ws_cull_nclip_keep_sites;
    std::vector<uint32_t> ws_cull_branch_keep_sites;
    std::vector<WidescreenCullKeepSite> ws_cull_keep_sites;
    std::vector<WidescreenCullWidenSite> ws_cull_widen_sites;
    std::vector<WidescreenAngleSite> ws_cull_angle_sites;
    WidescreenAspectConeConfig ws_aspect_cone;
    int ws_cull_guard_pixels = 0;
    int ws_cull_activation_guard_pixels = 0;
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
            load_sites("slti_lower_sites", ws_cull_slti_lower_sites);
            load_sites("bltz_sites",  ws_cull_bltz_sites);
            load_sites("negsub_sites", ws_cull_negsub_sites);
            load_sites("vxrange_sites", ws_cull_vxrange_sites);
            load_sites("depth_sites", ws_cull_depth_sites);
            load_sites("plane_nx_sites", ws_cull_plane_nx_sites);
            load_sites("xclip_load_sites", ws_cull_xclip_load_sites);
            load_sites("nclip_keep_sites", ws_cull_nclip_keep_sites);
            load_sites("branch_keep_sites", ws_cull_branch_keep_sites);
            if (cull.contains("keep")) {
                std::set<uint32_t> seen;
                for (const auto& item : toml::find<toml::array>(cull, "keep")) {
                    WidescreenCullKeepSite site;
                    site.address = parse_hex(toml::find<std::string>(item, "address"),
                                             "widescreen.cull.keep.address");
                    site.expected = parse_hex(toml::find<std::string>(item, "expected"),
                                              "widescreen.cull.keep.expected");
                    site.result = (uint32_t)toml::find<int>(item, "result");
                    const uint32_t op = site.expected >> 26;
                    const uint32_t fn = site.expected & 0x3Fu;
                    if (!((op == 0x0Au) || (op == 0x0Bu) ||
                          (op == 0u && (fn == 0x2Au || fn == 0x2Bu)))) {
                        throw std::runtime_error(fmt::format(
                            "{}: [[widescreen.cull.keep]] expected must be "
                            "SLT/SLTU/SLTI/SLTIU",
                            config_path.string()));
                    }
                    if (site.result > 1u) {
                        throw std::runtime_error(fmt::format(
                            "{}: [[widescreen.cull.keep]] result must be 0 or 1",
                            config_path.string()));
                    }
                    if (!seen.insert(site.address & 0x1FFFFFFFu).second) {
                        throw std::runtime_error(fmt::format(
                            "{}: duplicate [[widescreen.cull.keep]] address 0x{:08X}",
                            config_path.string(), site.address));
                    }
                    ws_cull_keep_sites.push_back(site);
                }
            }
            if (cull.contains("widen")) {
                std::set<uint32_t> seen;
                for (const auto& item : toml::find<toml::array>(cull, "widen")) {
                    WidescreenCullWidenSite site;
                    site.address = parse_hex(toml::find<std::string>(item, "address"),
                                             "widescreen.cull.widen.address");
                    site.expected = parse_hex(toml::find<std::string>(item, "expected"),
                                              "widescreen.cull.widen.expected");
                    const std::string mode =
                        toml::find<std::string>(item, "mode");
                    const uint32_t op = site.expected >> 26;
                    const uint32_t fn = site.expected & 0x3Fu;
                    const bool is_imm = (op == 0x0Au) || (op == 0x0Bu);
                    const bool is_reg = (op == 0u && (fn == 0x2Au || fn == 0x2Bu));
                    if (!is_imm && !is_reg) {
                        throw std::runtime_error(fmt::format(
                            "{}: [[widescreen.cull.widen]] expected must be "
                            "SLT/SLTU/SLTI/SLTIU",
                            config_path.string()));
                    }
                    if (mode == "imm_upper")      site.mode = WsCullWidenMode::ImmUpper;
                    else if (mode == "imm_lower") site.mode = WsCullWidenMode::ImmLower;
                    else if (mode == "bound_rt")  site.mode = WsCullWidenMode::BoundRt;
                    else if (mode == "bound_rs")  site.mode = WsCullWidenMode::BoundRs;
                    else {
                        throw std::runtime_error(fmt::format(
                            "{}: [[widescreen.cull.widen]] mode must be one of "
                            "imm_upper, imm_lower, bound_rt, bound_rs",
                            config_path.string()));
                    }
                    // The mode has to match the instruction shape: an
                    // immediate mode on a register compare (or vice versa)
                    // would widen an operand that does not exist.
                    const bool mode_is_imm =
                        site.mode == WsCullWidenMode::ImmUpper ||
                        site.mode == WsCullWidenMode::ImmLower;
                    if (mode_is_imm != is_imm) {
                        throw std::runtime_error(fmt::format(
                            "{}: [[widescreen.cull.widen]] 0x{:08X} mode '{}' "
                            "does not match the instruction form (imm modes are "
                            "for SLTI/SLTIU, bound modes for SLT/SLTU)",
                            config_path.string(), site.address, mode));
                    }
                    if (!seen.insert(site.address & 0x1FFFFFFFu).second) {
                        throw std::runtime_error(fmt::format(
                            "{}: duplicate [[widescreen.cull.widen]] address 0x{:08X}",
                            config_path.string(), site.address));
                    }
                    ws_cull_widen_sites.push_back(site);
                }
            }
            if (cull.contains("angle")) {
                std::set<uint32_t> seen;
                for (const auto& item :
                     toml::find<toml::array>(cull, "angle")) {
                    WidescreenAngleSite site;
                    site.address = parse_hex(
                        toml::find<std::string>(item, "address"),
                        "widescreen.cull.angle.address");
                    site.expected = parse_hex(
                        toml::find<std::string>(item, "expected"),
                        "widescreen.cull.angle.expected");
                    const uint32_t op = site.expected >> 26;
                    const uint32_t rs = (site.expected >> 21) & 31u;
                    const int32_t imm = (int16_t)(site.expected & 0xFFFFu);
                    if ((op != 0x08u && op != 0x09u) || rs != 0u ||
                        imm <= 0 || imm >= 1024) {
                        throw std::runtime_error(fmt::format(
                            "{}: [[widescreen.cull.angle]] expected must be "
                            "ADDI/ADDIU rt,zero,imm with imm in [1, 1023]",
                            config_path.string()));
                    }
                    if ((site.address & 3u) != 0)
                        throw std::runtime_error(fmt::format(
                            "{}: cull-angle address 0x{:08X} is not "
                            "instruction-aligned",
                            config_path.string(), site.address));
                    if (!seen.insert(site.address & 0x1FFFFFFFu).second)
                        throw std::runtime_error(fmt::format(
                            "{}: duplicate cull-angle address 0x{:08X}",
                            config_path.string(), site.address));
                    ws_cull_angle_sites.push_back(site);
                }
            }
            if (cull.contains("aspect_cone")) {
                const toml::value& cone = toml::find(cull, "aspect_cone");
                auto cone_hex = [&](const char *key) {
                    return parse_hex(toml::find<std::string>(cone, key),
                                     fmt::format("widescreen.cull.aspect_cone.{}", key));
                };
                ws_aspect_cone.forward_addr = cone_hex("forward_addr");
                const int object_type_offset =
                    toml::find<int>(cone, "object_type_offset");
                const int object_reg = toml::find<int>(cone, "object_reg");
                const int x_reg = toml::find<int>(cone, "x_reg");
                const int z_reg = toml::find<int>(cone, "z_reg");
                const int y_reg = toml::find<int>(cone, "y_reg");
                const int hysteresis_pixels =
                    toml::find_or<int>(cone, "hysteresis_pixels", 0);
                const int queue_reserve =
                    toml::find_or<int>(cone, "queue_reserve", 0);
                if (object_type_offset < 0 || object_type_offset > 0xFFFF) {
                    throw std::runtime_error(fmt::format(
                        "{}: [widescreen.cull.aspect_cone] "
                        "object_type_offset must be in [0, 65535]",
                        config_path.string()));
                }
                if (object_reg < 0 || object_reg > 31 ||
                    x_reg < 0 || x_reg > 31 ||
                    z_reg < 0 || z_reg > 31 ||
                    y_reg < 0 || y_reg > 31) {
                    throw std::runtime_error(fmt::format(
                        "{}: [widescreen.cull.aspect_cone] register indices "
                        "must be in [0, 31]", config_path.string()));
                }
                if (hysteresis_pixels < 0 || hysteresis_pixels > 256 ||
                    queue_reserve < 0 || queue_reserve > 256) {
                    throw std::runtime_error(fmt::format(
                        "{}: [widescreen.cull.aspect_cone] hysteresis_pixels "
                        "and queue_reserve must be in [0, 256]",
                        config_path.string()));
                }
                if ((ws_aspect_cone.forward_addr & 1u) != 0) {
                    throw std::runtime_error(fmt::format(
                        "{}: [widescreen.cull.aspect_cone] forward_addr "
                        "must be halfword-aligned", config_path.string()));
                }
                ws_aspect_cone.object_type_offset =
                    (uint32_t)object_type_offset;
                ws_aspect_cone.object_reg = (uint32_t)object_reg;
                ws_aspect_cone.x_reg = (uint32_t)x_reg;
                ws_aspect_cone.z_reg = (uint32_t)z_reg;
                ws_aspect_cone.y_reg = (uint32_t)y_reg;
                ws_aspect_cone.hysteresis_pixels =
                    (uint32_t)hysteresis_pixels;
                ws_aspect_cone.queue_reserve = (uint32_t)queue_reserve;
                auto load_cone_hex_array =
                    [&](const char *key, std::array<uint32_t, 3>& out) {
                        const auto values =
                            toml::find<std::vector<std::string>>(cone, key);
                        if (values.size() != out.size())
                            throw std::runtime_error(fmt::format(
                                "{}: [widescreen.cull.aspect_cone] {} must "
                                "contain exactly three values",
                                config_path.string(), key));
                        for (size_t i = 0; i < out.size(); i++)
                            out[i] = parse_hex(
                                values[i],
                                fmt::format("widescreen.cull.aspect_cone.{}",
                                            key));
                    };
                auto load_cone_int_array =
                    [&](const char *key, std::array<uint32_t, 3>& out) {
                        const auto values =
                            toml::find<std::vector<int>>(cone, key);
                        if (values.size() != out.size())
                            throw std::runtime_error(fmt::format(
                                "{}: [widescreen.cull.aspect_cone] {} must "
                                "contain exactly three values",
                                config_path.string(), key));
                        for (size_t i = 0; i < out.size(); i++) {
                            if (values[i] < 0)
                                throw std::runtime_error(fmt::format(
                                    "{}: [widescreen.cull.aspect_cone] {} "
                                    "values must be non-negative",
                                    config_path.string(), key));
                            out[i] = (uint32_t)values[i];
                        }
                    };
                load_cone_hex_array("queue_count_addrs",
                                    ws_aspect_cone.queue_count_addrs);
                load_cone_int_array("queue_capacities",
                                    ws_aspect_cone.queue_capacities);
                load_cone_hex_array("queue_type_masks",
                                    ws_aspect_cone.queue_type_masks);
                uint32_t used_type_mask = 0;
                for (size_t i = 0; i < 3; i++) {
                    if ((ws_aspect_cone.queue_count_addrs[i] & 1u) != 0)
                        throw std::runtime_error(fmt::format(
                            "{}: [widescreen.cull.aspect_cone] "
                            "queue_count_addrs values must be "
                            "halfword-aligned", config_path.string()));
                    if (ws_aspect_cone.queue_capacities[i] == 0 ||
                        ws_aspect_cone.queue_capacities[i] > 0x7FFFu)
                        throw std::runtime_error(fmt::format(
                            "{}: [widescreen.cull.aspect_cone] "
                            "queue_capacities values must be in [1, 32767]",
                            config_path.string()));
                    if (ws_aspect_cone.queue_reserve >=
                        ws_aspect_cone.queue_capacities[i])
                        throw std::runtime_error(fmt::format(
                            "{}: [widescreen.cull.aspect_cone] "
                            "queue_reserve must be smaller than every "
                            "queue capacity", config_path.string()));
                    if ((used_type_mask &
                         ws_aspect_cone.queue_type_masks[i]) != 0)
                        throw std::runtime_error(fmt::format(
                            "{}: [widescreen.cull.aspect_cone] "
                            "queue_type_masks must not overlap",
                            config_path.string()));
                    used_type_mask |= ws_aspect_cone.queue_type_masks[i];
                }
                if (!cone.contains("sites"))
                    throw std::runtime_error(fmt::format(
                        "{}: [widescreen.cull.aspect_cone] requires at least "
                        "one [[...sites]] entry", config_path.string()));
                std::set<uint32_t> seen;
                for (const auto& item :
                     toml::find<toml::array>(cone, "sites")) {
                    WidescreenAspectConeSite site;
                    site.address = parse_hex(
                        toml::find<std::string>(item, "address"),
                        "widescreen.cull.aspect_cone.sites.address");
                    site.expected = parse_hex(
                        toml::find<std::string>(item, "expected"),
                        "widescreen.cull.aspect_cone.sites.expected");
                    const uint32_t opcode = site.expected >> 26;
                    const bool is_slti = opcode == 0x0Au;
                    const bool is_slt =
                        opcode == 0u &&
                        (site.expected & 0x3Fu) == 0x2Au &&
                        ((site.expected >> 6) & 0x1Fu) == 0u;
                    if (!is_slti && !is_slt)
                        throw std::runtime_error(fmt::format(
                            "{}: [[widescreen.cull.aspect_cone.sites]] "
                            "expected must be signed SLTI or SLT",
                            config_path.string()));
                    const int threshold = toml::find_or<int>(
                        item, "cosine_threshold",
                        is_slti ? (int)(site.expected & 0xFFFFu) : -1);
                    if (threshold < 1 || threshold > 1023)
                        throw std::runtime_error(fmt::format(
                            "{}: [[widescreen.cull.aspect_cone.sites]] "
                            "cosine_threshold must be in [1, 1023] "
                            "(and is required for SLT)",
                            config_path.string()));
                    site.cosine_threshold = (uint32_t)threshold;
                    auto site_reg = [&](const char *key, int fallback) {
                        const int value = item.contains(key)
                            ? toml::find<int>(item, key) : fallback;
                        if (value < 0 || value > 31)
                            throw std::runtime_error(fmt::format(
                                "{}: [[widescreen.cull.aspect_cone.sites]] "
                                "{} must be in [0, 31]",
                                config_path.string(), key));
                        return (uint32_t)value;
                    };
                    site.object_reg = site_reg("object_reg", object_reg);
                    site.x_reg = site_reg("x_reg", x_reg);
                    site.z_reg = site_reg("z_reg", z_reg);
                    site.y_reg = site_reg("y_reg", y_reg);
                    site.queue_guard =
                        toml::find_or<bool>(item, "queue_guard", true);
                    if ((site.address & 3u) != 0)
                        throw std::runtime_error(fmt::format(
                            "{}: aspect-cone address 0x{:08X} is not "
                            "instruction-aligned",
                            config_path.string(), site.address));
                    if (!seen.insert(site.address & 0x1FFFFFFFu).second)
                        throw std::runtime_error(fmt::format(
                            "{}: duplicate aspect-cone address 0x{:08X}",
                            config_path.string(), site.address));
                    ws_aspect_cone.sites.push_back(site);
                }
            }
            if (cull.contains("guard_pixels")) {
                ws_cull_guard_pixels = toml::find<int>(cull, "guard_pixels");
                if (ws_cull_guard_pixels < 0 || ws_cull_guard_pixels > 256)
                    throw std::runtime_error(fmt::format(
                        "{}: [widescreen.cull] guard_pixels must be in [0, 256]",
                        config_path.string()));
            }
            if (cull.contains("activation_guard_pixels")) {
                ws_cull_activation_guard_pixels =
                    toml::find<int>(cull, "activation_guard_pixels");
                if (ws_cull_activation_guard_pixels < 0 ||
                    ws_cull_activation_guard_pixels > 256)
                    throw std::runtime_error(fmt::format(
                        "{}: [widescreen.cull] activation_guard_pixels must "
                        "be in [0, 256]",
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
        /*region*/           region,
        /*players*/          players,
        /*exe_path*/         exe_path,
        /*load_address*/     load_address,
        /*entry_pc*/         entry_pc,
        /*text_size*/        text_size,
        /*stack_base*/       stack_base,
        /*discs*/            discs,
        /*has_disc_crc*/     has_disc_crc,
        /*disc_crc*/         disc_crc,
        /*disc_sha1*/        disc_sha1,
        /*netplay_require_cue*/ netplay_require_cue,
        /*netplay_required_tracks*/ netplay_required_tracks,
        /*has_netplay_required_leadout*/ has_netplay_required_leadout,
        /*netplay_required_leadout_lba*/ netplay_required_leadout_lba,
        /*netplay_required_disc_fp*/ netplay_required_disc_fp,
        /*netplay_local_viewport*/ netplay_local_viewport,
        /*netplay_local_viewport_aspect*/ netplay_local_viewport_aspect,
        /*seeds_path*/       seeds_path,
        /*bios_thunks_path*/ bios_thunks_path,
        /*bios_config_path*/ bios_config_path,
        /*out_dir*/          out_dir,
        /*strict*/           strict,
        /*discovery*/        discovery,
        /*out_stem*/         out_stem,
        /*recompiler_patches*/ recompiler_patches,
        /*runtime*/          parse_runtime_block(cfg, root),
        /*ws_sprite_tag_funcs*/   ws_sprite_tag_funcs,
        /*ws_sprite_anchor_addr*/ ws_sprite_anchor_addr,
        /*ws_hud_sprt_squash*/    ws_hud_sprt_squash,
        /*ws_auto_ui_squash*/      ws_auto_ui_squash,
        /*data_shard_funcs*/      data_shard_funcs,
        /*mod_function_entry_funcs*/ mod_function_entry_funcs,
        /*hot_funcs*/             hot_funcs,
        /*load_charge_batch*/     load_charge_batch,
        /*load_charge_batch_funcs*/ load_charge_batch_funcs,
        /*vsync_query_func*/      vsync_query_func,
        /*vsync_counter_addr*/    vsync_counter_addr,
        /*vsync_gpustat_ptr_addr*/ vsync_gpustat_ptr_addr,
        /*vsync_timer1_ptr_addr*/ vsync_timer1_ptr_addr,
        /*vsync_timer1_cache_addr*/ vsync_timer1_cache_addr,
        /*vsync_event_horizon_sites*/ vsync_event_horizon_sites,
        /*vsync_event_horizon_extra_sites*/ vsync_event_horizon_extra_sites,
        /*vsync_event_horizon_any*/   vsync_event_horizon_any,
        /*ws_cull_bias_sites*/    ws_cull_bias_sites,
        /*ws_cull_range_sites*/   ws_cull_range_sites,
        /*ws_cull_a1_sites*/      ws_cull_a1_sites,
        /*ws_cull_screen_x_sites*/ ws_cull_screen_x_sites,
        /*ws_cull_slti_sites*/    ws_cull_slti_sites,
        /*ws_cull_slti_lower_sites*/ ws_cull_slti_lower_sites,
        /*ws_cull_bltz_sites*/    ws_cull_bltz_sites,
        /*ws_cull_negsub_sites*/  ws_cull_negsub_sites,
        /*ws_cull_vxrange_sites*/ ws_cull_vxrange_sites,
        /*ws_cull_depth_sites*/   ws_cull_depth_sites,
        /*ws_cull_plane_nx_sites*/ ws_cull_plane_nx_sites,
        /*ws_cull_xclip_load_sites*/ ws_cull_xclip_load_sites,
        /*ws_cull_nclip_keep_sites*/ ws_cull_nclip_keep_sites,
        /*ws_cull_branch_keep_sites*/ ws_cull_branch_keep_sites,
        /*ws_cull_keep_sites*/    ws_cull_keep_sites,
        /*ws_cull_widen_sites*/   ws_cull_widen_sites,
        /*ws_cull_angle_sites*/   ws_cull_angle_sites,
        /*ws_aspect_cone*/         ws_aspect_cone,
        /*ws_cull_guard_pixels*/  ws_cull_guard_pixels,
        /*ws_cull_activation_guard_pixels*/ ws_cull_activation_guard_pixels,
        /*ws_cull_w_imms*/        ws_cull_w_imms,
        /*ws_cull_h_imms*/        ws_cull_h_imms,
        /*ws_backdrop_x_sites*/   ws_backdrop_x_sites,
        /*ws_backdrop_unsquash_funcs*/ ws_backdrop_unsquash_funcs,
        /*ws_dome_call_sites*/    ws_dome_call_sites,
        /*ws_auto_screen_x_cull*/ ws_auto_screen_x_cull,
        /*ws_auto_backdrop_preload*/ ws_auto_backdrop_preload,
        /*ws_full_2d*/            ws_full_2d,
        /*ws_gte_game_mode*/      ws_gte_game_mode,
        /*ws_fixed_outer_aspect*/ ws_fixed_outer_aspect,
        /*ws_precise_nclip*/      ws_precise_nclip,
        /*ws_gameplay_state_addr*/ ws_gameplay_state_addr,
        /*ws_gameplay_state_values*/ ws_gameplay_state_values,
        /*ws_native_wide*/        ws_native_wide,
        /*ws_nw_hud_corners*/     ws_nw_hud_corners,
        /*ws_nw_left_hud_packet_lo*/ ws_nw_left_hud_packet_lo,
        /*ws_nw_left_hud_packet_hi*/ ws_nw_left_hud_packet_hi,
        /*ws_nw_backdrop*/        ws_nw_backdrop,
        /*ws_clear_reveal*/       ws_clear_reveal,
        /*ws_nw_flat_backdrop*/   ws_nw_flat_backdrop,
        /*ws_nw_phase_backdrop*/  ws_nw_phase_backdrop,
        /*ws_nw_textured_edges*/ ws_nw_textured_edges,
        /*ws_nw_textured_edge_scale*/ ws_nw_textured_edge_scale,
        /*ws_nw_full_mirror*/ ws_nw_full_mirror,
        /*ws_signed_x_bound_sites*/ ws_signed_x_bound_sites,
        /*ws_offered*/            ws_offered,
        /*vulkan_offered*/        vulkan_offered,
        /*ws_adaptive_view*/      ws_adaptive_view,
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

static bool valid_user_audio_freq(int n) {
    return n == 32040 || n == 32000 || n == 44100 || n == 48000;
}

UserSettings load_user_settings(const fs::path& path) {
    UserSettings s;
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return s;

    toml::value doc;
    try {
        doc = toml::parse(path.string());
    } catch (const std::exception&) {
        // Malformed file: fall back to all-defaults rather than refuse to boot,
        // but tell the caller so the user hears about it (and so the file can
        // be preserved before any launcher save overwrites it with defaults).
        s.parse_error = true;
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
            if (n >= 1 && n <= 16) { s.supersampling = (int)n; s.has_supersampling = true; }
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
        if (v.contains("fmv_filter")) try_get([&]{
            const auto m = toml::find<std::string>(v, "fmv_filter");
            if (video_fmv_filter_parse(m, &s.fmv_filter)) s.has_fmv_filter = true;
        });
        if (v.contains("hd_textures")) try_get([&]{
            s.hd_textures = toml::find<bool>(v, "hd_textures"); s.has_hd_textures = true;
        });
        if (v.contains("hd_texture_pack")) try_get([&]{
            s.hd_texture_pack = fs::path(toml::find<std::string>(v, "hd_texture_pack"));
            s.has_hd_texture_pack = true;
        });
        if (v.contains("geometry_correction")) try_get([&]{
            s.geometry_correction = toml::find<bool>(v, "geometry_correction");
            s.has_geometry_correction = true;
        });
        if (v.contains("perspective_texturing")) try_get([&]{
            s.perspective_texturing = toml::find<bool>(v, "perspective_texturing");
            s.has_perspective_texturing = true;
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
        // Deprecated and ignored: read only so the runtime can report that a
        // stale value was found (and so the next save drops it). Never applied
        // — see UserSettings::turbo_loads in config_loader.h.
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
            // Tri-state (0 off / 1 borderless / 2 exclusive). Back-compat: a
            // settings.toml written before the tri-state migration stores this
            // as a bool (true meant borderless desktop fullscreen).
            try {
                s.fullscreen = toml::find<int>(v, "fullscreen");
            } catch (const std::exception&) {
                s.fullscreen = toml::find<bool>(v, "fullscreen") ? 1 : 0;
            }
            if (s.fullscreen < 0 || s.fullscreen > 2) s.fullscreen = 0;
            s.has_fullscreen = true;
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
        if (v.contains("frame_interpolation")) try_get([&]{
            s.frame_interpolation = toml::find<bool>(v, "frame_interpolation");
            s.has_frame_interpolation = true;
        });
        if (v.contains("frame_interpolation_fps")) try_get([&]{
            s.frame_interpolation_fps = toml::find<int>(v, "frame_interpolation_fps");
            if (s.frame_interpolation_fps == 0 || s.frame_interpolation_fps >= 90)
                s.has_frame_interpolation_fps = true;
        });
        if (v.contains("aspect_ratio")) try_get([&]{
            const auto m = toml::find<std::string>(v, "aspect_ratio");
            int n = 0, d = 0;
            if (parse_aspect_ratio(m, &n, &d)) {
                s.aspect_num = n; s.aspect_den = d; s.has_aspect_ratio = true;
            }
        });
        if (v.contains("adaptive_view")) try_get([&]{
            s.adaptive_view = toml::find<bool>(v, "adaptive_view");
            s.has_adaptive_view = true;
        });
        if (v.contains("rewind_depth")) try_get([&]{
            int d = toml::find<int>(v, "rewind_depth");
            static const int opts[4] = {50, 100, 150, 200};
            int best = opts[0];
            int best_d = d > best ? d - best : best - d;
            for (int i = 1; i < 4; ++i) {
                int dd = d > opts[i] ? d - opts[i] : opts[i] - d;
                if (dd < best_d) { best_d = dd; best = opts[i]; }
            }
            s.rewind_depth = best;
            s.has_rewind_depth = true;
        });
        if (v.contains("rewind_interval")) try_get([&]{
            int d = toml::find<int>(v, "rewind_interval");
            static const int opts[6] = {1, 4, 8, 12, 15, 30};
            int best = opts[0];
            int best_d = d > best ? d - best : best - d;
            for (int i = 1; i < 6; ++i) {
                int dd = d > opts[i] ? d - opts[i] : opts[i] - d;
                if (dd < best_d) { best_d = dd; best = opts[i]; }
            }
            s.rewind_interval = best;
            s.has_rewind_interval = true;
        });
    }
    if (doc.contains("audio")) {
        const toml::value& a = toml::find(doc, "audio");
        if (a.contains("frequency")) try_get([&]{
            const auto n = toml::find<int64_t>(a, "frequency");
            if (valid_user_audio_freq((int)n)) {
                s.audio_freq = (int)n;
                s.has_audio_freq = true;
            }
        });
        if (a.contains("spu_hq")) try_get([&]{
            s.spu_hq = toml::find<bool>(a, "spu_hq"); s.has_spu_hq = true;
        });
    }
    if (doc.contains("hotkeys")) {
        const toml::value& h = toml::find(doc, "hotkeys");
        if (h.contains("rewind_pad")) try_get([&]{
            const auto n = toml::find<int64_t>(h, "rewind_pad");
            if (pad_bind_value_ok(n)) {
                s.hotkey_pad_rewind = (int)n;
                s.has_hotkey_pad_rewind = true;
            }
        });
        if (h.contains("save_state_menu_pad")) try_get([&]{
            const auto n = toml::find<int64_t>(h, "save_state_menu_pad");
            if (pad_bind_value_ok(n)) {
                s.hotkey_pad_save_state_menu = (int)n;
                s.has_hotkey_pad_save_state_menu = true;
            }
        });
    }
    if (doc.contains("launcher")) {
        const toml::value& l = toml::find(doc, "launcher");
        if (l.contains("skip_launcher")) try_get([&]{
            s.skip_launcher = toml::find<bool>(l, "skip_launcher"); s.has_skip_launcher = true;
        });
    }
    if (doc.contains("netplay")) {
        const toml::value& n = toml::find(doc, "netplay");
        if (n.contains("player_name")) try_get([&]{
            const auto v = toml::find<std::string>(n, "player_name");
            if (!v.empty()) { s.netplay_player_name = v; s.has_netplay_player_name = true; }
        });
        if (n.contains("lobby_url")) try_get([&]{
            const auto v = toml::find<std::string>(n, "lobby_url");
            if (!v.empty()) { s.netplay_lobby_url = v; s.has_netplay_lobby_url = true; }
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
    if (doc.contains("savestate")) {
        const toml::value& ss = toml::find(doc, "savestate");
        if (ss.contains("dir")) try_get([&]{
            const auto p = toml::find<std::string>(ss, "dir");
            if (!p.empty()) { s.savestate_dir = fs::path(p); s.has_savestate_dir = true; }
        });
    }
    if (doc.contains("localization")) {
        const toml::value& lc = toml::find(doc, "localization");
        if (lc.contains("language")) try_get([&]{
            const auto v = toml::find<std::string>(lc, "language");
            if (!v.empty()) { s.language = v; s.has_language = true; }
        });
    }
    if (doc.contains("parappa_timing")) {
        const toml::value& timing = toml::find(doc, "parappa_timing");
        if (timing.contains("mode")) try_get([&]{
            auto mode = toml::find<std::string>(timing, "mode");
            for (char& c : mode) c = (char)std::tolower((unsigned char)c);
            if (mode == "stock" || mode == "off" || mode == "medium" ||
                mode == "permissive" || mode == "easy" || mode == "custom") {
                s.parappa_timing_mode = mode;
                s.has_parappa_timing_mode = true;
            }
        });
        if (timing.contains("extra_early")) try_get([&]{
            const auto n = toml::find<int64_t>(timing, "extra_early");
            if (n >= 0 && n <= 60) {
                s.parappa_timing_extra_early = (int)n;
                s.has_parappa_timing_extra_early = true;
            }
        });
        if (timing.contains("extra_late")) try_get([&]{
            const auto n = toml::find<int64_t>(timing, "extra_late");
            if (n >= 0 && n <= 60) {
                s.parappa_timing_extra_late = (int)n;
                s.has_parappa_timing_extra_late = true;
            }
        });
    }
    if (doc.contains("controller")) {
        const toml::value& ct = toml::find(doc, "controller");
        static const char* kDevKeys[] = {
            "p1_device", "p2_device", "p3_device", "p4_device", "p5_device"};
        static const char* kModeKeys[] = {
            "p1_mode", "p2_mode", "p3_mode", "p4_mode", "p5_mode"};
        static const char* kDzKeys[] = {
            "p1_deadzone", "p2_deadzone", "p3_deadzone", "p4_deadzone",
            "p5_deadzone"};
        static const char* kAnalogKeys[] = {
            "p1_analog", "p2_analog", "p3_analog", "p4_analog", "p5_analog"};
        for (int i = 0; i < UserSettings::kMaxControllerPlayers; ++i) {
            if (ct.contains(kDevKeys[i])) try_get([&]{
                const auto v = toml::find<std::string>(ct, kDevKeys[i]);
                if (!v.empty()) {
                    s.p_device[i] = v;
                    s.has_p_device[i] = true;
                }
            });
            // Legacy boolean form first (true->analog, false->digital); the
            // string `*_mode` keys override when present.
            if (ct.contains(kAnalogKeys[i])) try_get([&]{
                s.p_mode[i] = toml::find<bool>(ct, kAnalogKeys[i])
                                  ? PAD_MODE_ANALOG : PAD_MODE_DIGITAL;
                s.has_p_mode[i] = true;
            });
            if (ct.contains(kModeKeys[i])) try_get([&]{
                s.p_mode[i] = pad_mode_from_settings_string(
                    toml::find<std::string>(ct, kModeKeys[i]), PAD_MODE_ANALOG);
                s.has_p_mode[i] = true;
            });
            if (ct.contains(kDzKeys[i])) try_get([&]{
                const auto n = toml::find<int64_t>(ct, kDzKeys[i]);
                if (n >= 0 && n <= 32767) {
                    s.p_deadzone[i] = (int)n;
                    s.has_p_deadzone[i] = true;
                }
            });
        }
        if (ct.contains("multitap")) try_get([&]{
            s.multitap_enabled = toml::find<bool>(ct, "multitap");
            s.has_multitap_enabled = true;
        });
        if (ct.contains("multitap_analog")) try_get([&]{
            s.multitap_analog = toml::find<bool>(ct, "multitap_analog");
            s.has_multitap_analog = true;
        });
        if (ct.contains("deadzone")) try_get([&]{
            const auto n = toml::find<int64_t>(ct, "deadzone");
            if (n >= 0 && n <= 32767) {
                s.deadzone = (int)n;
                s.has_deadzone = true;
                /* Legacy global: fill any slot that was not given pN_deadzone. */
                for (int i = 0; i < UserSettings::kMaxControllerPlayers; ++i) {
                    if (!s.has_p_deadzone[i]) {
                        s.p_deadzone[i] = s.deadzone;
                        s.has_p_deadzone[i] = true;
                    }
                }
            }
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
    if (s.has_fmv_filter)
        f << "fmv_filter        = \"" << video_fmv_filter_name(s.fmv_filter) << "\"\n";
    if (s.has_hd_textures)
        f << "hd_textures       = " << (s.hd_textures ? "true" : "false") << "\n";
    if (s.has_hd_texture_pack)
        f << "hd_texture_pack   = \"" << fwd(s.hd_texture_pack) << "\"\n";
    if (s.has_geometry_correction)
        f << "geometry_correction   = "
          << (s.geometry_correction ? "true" : "false") << "\n";
    if (s.has_perspective_texturing)
        f << "perspective_texturing = "
          << (s.perspective_texturing ? "true" : "false") << "\n";
    if (s.has_screen_kind) {
        const char* k = s.screen_kind == 1 ? "crt"
                      : s.screen_kind == 2 ? "composite"
                      : s.screen_kind == 3 ? "trinitron" : "raw";
        f << "crt_filter        = \"" << k << "\"\n";
    }
    if (s.has_auto_skip_fmv)
        f << "auto_skip_fmv     = " << (s.auto_skip_fmv ? "true" : "false") << "\n";
    /* turbo_loads is deliberately NOT written back: it is deprecated and no
     * longer restored, so re-emitting it would preserve a dead row that looks
     * authoritative. Omitting it lets an existing settings.toml self-clean on
     * the first save after the update. */
    if (s.has_fast_boot)
        f << "fast_boot         = " << (s.fast_boot ? "true" : "false") << "\n";
    if (s.has_bios_hle)
        f << "bios_hle          = " << (s.bios_hle ? "true" : "false") << "\n";
    if (s.has_fullscreen)
        f << "fullscreen        = " << s.fullscreen << "\n";
    if (s.has_low_latency_input)
        f << "low_latency_input = " << (s.low_latency_input ? "true" : "false") << "\n";
    if (s.has_vsync)
        f << "vsync             = \"" << (s.vsync == 0 ? "immediate" : s.vsync < 0 ? "adaptive" : "on") << "\"\n";
    if (s.has_frame_interpolation)
        f << "frame_interpolation = " << (s.frame_interpolation ? "true" : "false") << "\n";
    if (s.has_frame_interpolation_fps)
        f << "frame_interpolation_fps = " << s.frame_interpolation_fps << "\n";
    if (s.has_aspect_ratio)
        f << "aspect_ratio      = \"" << s.aspect_num << ":" << s.aspect_den << "\"\n";
    if (s.has_adaptive_view)
        f << "adaptive_view     = " << (s.adaptive_view ? "true" : "false") << "\n";
    if (s.has_rewind_depth)
        f << "rewind_depth      = " << s.rewind_depth << "\n";
    if (s.has_rewind_interval)
        f << "rewind_interval   = " << s.rewind_interval << "\n";
    f << "\n[audio]\n";
    if (s.has_audio_freq)
        f << "frequency = " << s.audio_freq << "\n";
    if (s.has_spu_hq)
        f << "spu_hq = " << (s.spu_hq ? "true" : "false") << "\n";
    if (s.has_hotkey_pad_rewind || s.has_hotkey_pad_save_state_menu) {
        f << "\n[hotkeys]\n";
        if (s.has_hotkey_pad_rewind)
            f << "rewind_pad = " << s.hotkey_pad_rewind << "\n";
        if (s.has_hotkey_pad_save_state_menu)
            f << "save_state_menu_pad = "
              << s.hotkey_pad_save_state_menu << "\n";
    }
    if (s.has_skip_launcher)
        f << "\n[launcher]\nskip_launcher = " << (s.skip_launcher ? "true" : "false") << "\n";
    if ((s.has_netplay_player_name && !s.netplay_player_name.empty()) ||
        (s.has_netplay_lobby_url && !s.netplay_lobby_url.empty())) {
        f << "\n[netplay]\n";
        if (s.has_netplay_player_name && !s.netplay_player_name.empty())
            f << "player_name = \"" << s.netplay_player_name << "\"\n";
        if (s.has_netplay_lobby_url && !s.netplay_lobby_url.empty())
            f << "lobby_url = \"" << s.netplay_lobby_url << "\"\n";
    }
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
    if (s.has_savestate_dir)
        f << "\n[savestate]\ndir = \"" << fwd(s.savestate_dir) << "\"\n";

    {
        bool any_ctrl = s.has_deadzone || s.has_multitap_enabled ||
                        s.has_multitap_analog;
        for (int i = 0; i < UserSettings::kMaxControllerPlayers; ++i) {
            if (s.has_p_device[i] || s.has_p_mode[i] || s.has_p_deadzone[i])
                any_ctrl = true;
        }
        if (any_ctrl) {
            static const char* kDevKeys[] = {
                "p1_device", "p2_device", "p3_device", "p4_device", "p5_device"};
            static const char* kModeKeys[] = {
                "p1_mode", "p2_mode", "p3_mode", "p4_mode", "p5_mode"};
            static const char* kDzKeys[] = {
                "p1_deadzone", "p2_deadzone", "p3_deadzone", "p4_deadzone",
                "p5_deadzone"};
            f << "\n[controller]\n";
            for (int i = 0; i < UserSettings::kMaxControllerPlayers; ++i) {
                if (s.has_p_device[i])
                    f << kDevKeys[i] << " = \"" << s.p_device[i] << "\"\n";
                if (s.has_p_mode[i])
                    f << kModeKeys[i] << "   = \""
                      << pad_mode_to_string(s.p_mode[i]) << "\"\n";
                if (s.has_p_deadzone[i])
                    f << kDzKeys[i] << " = " << s.p_deadzone[i] << "\n";
            }
            if (s.has_multitap_enabled)
                f << "multitap  = " << (s.multitap_enabled ? "true" : "false")
                  << "\n";
            if (s.has_multitap_analog)
                f << "multitap_analog = "
                  << (s.multitap_analog ? "true" : "false") << "\n";
            /* Keep a global deadzone= for older readers (mirrors P1). */
            if (s.has_deadzone || s.has_p_deadzone[0])
                f << "deadzone  = "
                  << (s.has_p_deadzone[0] ? s.p_deadzone[0] : s.deadzone)
                  << "\n";
        }
    }

    if (s.has_language) {
        f << "\n[localization]\n";
        f << "language = \"" << s.language << "\"\n";
    }
    if (s.has_parappa_timing_mode || s.has_parappa_timing_extra_early ||
        s.has_parappa_timing_extra_late) {
        f << "\n[parappa_timing]\n";
        if (s.has_parappa_timing_mode)
            f << "mode = \"" << s.parappa_timing_mode << "\"\n";
        if (s.has_parappa_timing_extra_early)
            f << "extra_early = " << s.parappa_timing_extra_early << "\n";
        if (s.has_parappa_timing_extra_late)
            f << "extra_late = " << s.parappa_timing_extra_late << "\n";
    }

    return f.good();
}

bool upsert_game_toml_controller_bool(const std::filesystem::path& path,
                                      const char* key, bool value)
{
    if (!key || !key[0]) return false;
    std::string text;
    {
        std::ifstream in(path);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
    }
    const std::string val = value ? "true" : "false";
    const std::string assign = std::string(key) + " = " + val;

    auto is_section = [](const std::string& line) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        return i < line.size() && line[i] == '[';
    };
    auto section_name = [](const std::string& line) -> std::string {
        size_t a = line.find('[');
        size_t b = line.find(']');
        if (a == std::string::npos || b == std::string::npos || b <= a + 1)
            return {};
        return line.substr(a + 1, b - a - 1);
    };
    auto key_prefix = [&](const std::string& line) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i < line.size() && line[i] == '#') return false;
        const std::string rest = line.substr(i);
        return rest.rfind(std::string(key) + " =", 0) == 0 ||
               rest.rfind(std::string(key) + "=", 0) == 0;
    };

    std::istringstream ls(text);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ls, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }

    int ctrl_start = -1, ctrl_end = -1;
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (is_section(lines[i]) && section_name(lines[i]) == "controller") {
            ctrl_start = i;
            ctrl_end = (int)lines.size();
            for (int j = i + 1; j < (int)lines.size(); ++j) {
                if (is_section(lines[j])) {
                    ctrl_end = j;
                    break;
                }
            }
            break;
        }
    }

    if (ctrl_start < 0) {
        if (!text.empty() && text.back() != '\n') text.push_back('\n');
        text += "\n[controller]\n";
        text += assign;
        text += "\n";
    } else {
        int replace_at = -1;
        for (int i = ctrl_start + 1; i < ctrl_end; ++i) {
            if (key_prefix(lines[i])) {
                replace_at = i;
                break;
            }
        }
        if (replace_at >= 0) {
            lines[replace_at] = assign;
        } else {
            int insert_at = ctrl_end;
            while (insert_at > ctrl_start + 1 &&
                   lines[insert_at - 1].find_first_not_of(" \t") ==
                       std::string::npos)
                --insert_at;
            lines.insert(lines.begin() + insert_at, assign);
        }
        std::ostringstream out;
        for (size_t i = 0; i < lines.size(); ++i) {
            out << lines[i];
            if (i + 1 < lines.size() || (!text.empty() && text.back() == '\n'))
                out << '\n';
        }
        text = out.str();
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return f.good();
}

} // namespace PSXRecompV4
